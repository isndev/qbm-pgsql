#include <gtest/gtest.h>
#include <pgsql/pgsql.h>
#include <thread>
#include <chrono>
#include <vector>
#include "../detail/transaction.h"
#include "../detail/queries.h"
#include <iostream>

using namespace qb::pg;
using namespace qb::io;

constexpr std::string_view PGSQL_CONNECTION_STR = "tcp://test:test@localhost:5432[test]";

class PostgreSQLQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(db_->connect(PGSQL_CONNECTION_STR.data()));
        
        // Create temporary test tables
        bool success = false;
        auto status = db_->execute(R"(
            CREATE TEMP TABLE test_users (
                id SERIAL PRIMARY KEY,
                name VARCHAR(50),
                age INTEGER,
                email VARCHAR(100),
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )",
            [&success](transaction& tr, results result) {
                success = true;
            },
            [](error::db_error error) {
                ASSERT_TRUE(false) << "Create table failed: " << error.code;
            }
        ).await();
        ASSERT_TRUE(success);
        
        success = false;
        status = db_->execute(R"(
            CREATE TEMP TABLE test_orders (
                id SERIAL PRIMARY KEY,
                user_id INTEGER REFERENCES test_users(id),
                amount DECIMAL(10,2),
                status VARCHAR(20),
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )",
            [&success](transaction& tr, results result) {
                success = true;
            },
            [](error::db_error error) {
                ASSERT_TRUE(false) << "Create table failed: " << error.code;
            }
        ).await();
        ASSERT_TRUE(success);
        
        // Insert test data
        success = false;
        status = db_->execute(R"(
            INSERT INTO test_users (name, age, email) VALUES
            ('John Doe', 30, 'john@example.com'),
            ('Jane Smith', 25, 'jane@example.com'),
            ('Bob Wilson', 45, 'bob@example.com')
        )",
            [&success](transaction& tr, results result) {
                success = true;
            },
            [](error::db_error error) {
                ASSERT_TRUE(false) << "Insert failed: " << error.code;
            }
        ).await();
        ASSERT_TRUE(success);
        
        success = false;
        status = db_->execute(R"(
            INSERT INTO test_orders (user_id, amount, status) VALUES
            (1, 100.50, 'completed'),
            (1, 200.75, 'pending'),
            (2, 150.25, 'completed'),
            (3, 300.00, 'cancelled')
        )",
            [&success](transaction& tr, results result) {
                success = true;
            },
            [](error::db_error error) {
                ASSERT_TRUE(false) << "Insert failed: " << error.code;
            }
        ).await();
        ASSERT_TRUE(success);
    }

    void TearDown() override {
        if (db_) {
            bool success = false;
            auto status = db_->execute("DROP TABLE IF EXISTS test_orders")
                    .execute("DROP TABLE IF EXISTS test_users",
                        [&success](transaction& tr, results result) {
                            success = true;
                        },
                        [](error::db_error error) {
                            ASSERT_TRUE(false) << "Drop table failed: " << error.code;
                        }
                    ).await();
            ASSERT_TRUE(success);
            db_->disconnect();
            db_.reset();
        }
    }

    std::unique_ptr<qb::pg::tcp::database> db_;
};

TEST_F(PostgreSQLQueryTest, BasicSelect) {
    bool success = false;
    auto status = db_->execute("SELECT * FROM test_users",
        [&success](transaction& tr, results result) {
            ASSERT_EQ(result.size(), 3);
            
            // Check first row
            ASSERT_EQ(result[0][1].as<std::string>(), "John Doe");
            ASSERT_EQ(result[0][2].as<int>(), 30);
            ASSERT_EQ(result[0][3].as<std::string>(), "john@example.com");
            success = true;
        },
        [](error::db_error error) {
            ASSERT_TRUE(false) << "Query failed: " << error.code;
        }
    ).await();
    ASSERT_TRUE(success);
}

TEST_F(PostgreSQLQueryTest, WhereClause) {
    bool success = false;
    auto status = db_->execute("SELECT * FROM test_users WHERE age > 30",
        [&success](transaction& tr, results result) {
            ASSERT_EQ(result.size(), 1);
            ASSERT_EQ(result[0][1].as<std::string>(), "Bob Wilson");
            success = true;
        },
        [](error::db_error error) {
            ASSERT_TRUE(false) << "Query failed: " << error.code;
        }
    ).await();
    ASSERT_TRUE(success);
}

TEST_F(PostgreSQLQueryTest, JoinQuery) {
    bool success = false;
    auto status = db_->execute(R"(
        SELECT u.name, o.amount, o.status
        FROM test_users u
        JOIN test_orders o ON u.id = o.user_id
        WHERE o.status = 'completed'
    )",
        [&success](transaction& tr, results result) {
            ASSERT_EQ(result.size(), 2);
            
            // Check first completed order
            ASSERT_EQ(result[0][0].as<std::string>(), "John Doe");
            ASSERT_EQ(result[0][1].as<std::string>(), "100.50");
            ASSERT_EQ(result[0][2].as<std::string>(), "completed");
            success = true;
        },
        [](error::db_error error) {
            ASSERT_TRUE(false) << "Query failed: " << error.code;
        }
    ).await();
    ASSERT_TRUE(success);
}

TEST_F(PostgreSQLQueryTest, Aggregation) {
    bool success = false;
    auto status = db_->execute(R"(
        SELECT u.name, COUNT(o.id) as order_count, SUM(o.amount) as total_amount
        FROM test_users u
        LEFT JOIN test_orders o ON u.id = o.user_id
        GROUP BY u.id, u.name
        ORDER BY total_amount DESC
    )",
        [&success](transaction& tr, results result) {
            ASSERT_EQ(result.size(), 3);
            
            // Check first row (John Doe with highest total)
            ASSERT_EQ(result[0][0].as<std::string>(), "John Doe");
            ASSERT_EQ(result[0][1].as<int>(), 2);
            ASSERT_EQ(result[0][2].as<std::string>(), "301.25");
            success = true;
        },
        [](error::db_error error) {
            ASSERT_TRUE(false) << "Query failed: " << error.code;
        }
    ).await();
    ASSERT_TRUE(success);
}

TEST_F(PostgreSQLQueryTest, Subquery) {
    bool success = false;
    auto status = db_->execute(R"(
        SELECT name, age
        FROM test_users
        WHERE id IN (
            SELECT user_id
            FROM test_orders
            WHERE status = 'pending'
        )
    )",
        [&success](transaction& tr, results result) {
            ASSERT_EQ(result.size(), 1);
            ASSERT_EQ(result[0][0].as<std::string>(), "John Doe");
            ASSERT_EQ(result[0][1].as<int>(), 30);
            success = true;
        },
        [](error::db_error error) {
            ASSERT_TRUE(false) << "Query failed: " << error.code;
        }
    ).await();
    ASSERT_TRUE(success);
}

TEST_F(PostgreSQLQueryTest, ComplexQuery) {
    bool success = false;
    auto status = db_->execute(R"(
        WITH user_stats AS (
            SELECT 
                u.id,
                u.name,
                COUNT(o.id) as order_count,
                SUM(o.amount) as total_amount,
                AVG(o.amount) as avg_amount
            FROM test_users u
            LEFT JOIN test_orders o ON u.id = o.user_id
            GROUP BY u.id, u.name
        )
        SELECT 
            name,
            order_count,
            total_amount,
            avg_amount,
            CASE 
                WHEN total_amount > 200 THEN 'High Value'
                WHEN total_amount > 100 THEN 'Medium Value'
                ELSE 'Low Value'
            END as customer_category
        FROM user_stats
        ORDER BY total_amount DESC NULLS LAST
    )",
        [&success](transaction& tr, results result) {
            ASSERT_EQ(result.size(), 3);
            
            // Check first row (John Doe with highest total)
            ASSERT_EQ(result[0][0].as<std::string>(), "John Doe");
            ASSERT_EQ(result[0][1].as<int>(), 2);
            ASSERT_EQ(result[0][2].as<std::string>(), "301.25");
            ASSERT_NEAR(std::stod(result[0][3].as<std::string>()), 150.63, 0.01);
            ASSERT_EQ(result[0][4].as<std::string>(), "High Value");
            success = true;
        },
        [](error::db_error error) {
            ASSERT_TRUE(false) << "Query failed: " << error.code;
        }
    ).await();
    ASSERT_TRUE(success);
}

TEST_F(PostgreSQLQueryTest, QueryPerformance) {
    // Create test table
    bool success = false;
    auto status = db_->execute(R"(
        CREATE TEMP TABLE test_performance (
            id SERIAL PRIMARY KEY,
            value TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    )",
        [&success](transaction& tr, results result) {
            success = true;
        },
        [](error::db_error error) {
            ASSERT_TRUE(false) << "Create table failed: " << error.code;
        }
    ).await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(success);

    // Préparer les requêtes
    ASSERT_TRUE(db_->prepare("simple_insert", "INSERT INTO test_performance (value) VALUES ($1)")
        .prepare("batch_insert", "INSERT INTO test_performance (value) VALUES ($1),($2),($3),($4)")
        .prepare("multi_insert", "INSERT INTO test_performance (value) VALUES ($1),($2),($3),($4)")
        .await());

    // MÉTHODE 1: Insertion simple avec un seul paramètre
    ASSERT_TRUE(db_->execute("simple_insert", params{std::string("Test value single")}).await());

    // MÉTHODE 2: Insertion avec plusieurs paramètres explicites
    ASSERT_TRUE(db_->execute("batch_insert", 
        params{
            std::string("Test value explicit 1"), 
            std::string("Test value explicit 2"), 
            std::string("Test value explicit 3"), 
            std::string("Test value explicit 4")
        }).await());

    // MÉTHODE 3: Insertion avec un vecteur de chaînes
    std::vector<std::string> values;
    for (int i = 1; i <= 4; ++i) {
        values.push_back("Test value vector " + std::to_string(i));
    }
    ASSERT_TRUE(db_->execute("multi_insert", params{values}).await());

    // Vérifier que toutes les insertions ont fonctionné
    success = false;
    status = db_->execute("SELECT value FROM test_performance ORDER BY id",
        [&success](transaction& tr, results result) {
            // Nous devrions avoir 9 lignes au total (1 + 4 + 4)
            ASSERT_EQ(result.size(), 9);
            
            // Vérifier l'insertion simple
            ASSERT_EQ(result[0][0].as<std::string>(), "Test value single");
            
            // Vérifier l'insertion avec paramètres explicites
            for (int i = 1; i <= 4; ++i) {
                ASSERT_EQ(result[i][0].as<std::string>(), "Test value explicit " + std::to_string(i));
            }
            
            // Vérifier l'insertion avec vecteur de chaînes
            for (int i = 1; i <= 4; ++i) {
                ASSERT_EQ(result[i+4][0].as<std::string>(), "Test value vector " + std::to_string(i));
            }
            
            success = true;
        },
        [](error::db_error error) {
            ASSERT_TRUE(false) << "Query failed: " << error.code;
        }
    ).await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(success);

    // Test de performance avec un grand nombre de paramètres
    std::vector<std::string> large_values;
    for (int i = 1; i <= 100; ++i) {
        large_values.push_back("Performance test value " + std::to_string(i));
    }

    // Construire une requête d'insertion multivaluée dynamiquement
    std::string insert_query = "INSERT INTO test_performance (value) VALUES ";
    for (int i = 1; i <= 100; ++i) {
        if (i > 1) insert_query += ",";
        insert_query += "($" + std::to_string(i) + ")";
    }

    // Insertion de masse avec un vecteur de 100 chaînes
    success = false;
    status = db_->prepare("mass_insert", insert_query)
            .execute("mass_insert",
                     params{large_values},
                     [&success](transaction &tr, results result) {
                         success = true;
                     },
                     [](error::db_error error) {
                         ASSERT_TRUE(false) << "Insert failed: " << error.code;
                     }
            ).await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(success);

    // Mesurer les performances d'une requête
    auto start = std::chrono::steady_clock::now();
    success = false;
    status = db_->execute("SELECT COUNT(*) FROM test_performance WHERE value LIKE 'Performance test%'",
        [&success](transaction& tr, results result) {
            ASSERT_EQ(result.size(), 1);
            ASSERT_EQ(result[0][0].as<int>(), 100); // On doit trouver 100 résultats
            success = true;
        },
        [](error::db_error error) {
            ASSERT_TRUE(false) << "Query failed: " << error.code;
        }
    ).await();
    auto end = std::chrono::steady_clock::now();

    ASSERT_TRUE(status);
    ASSERT_TRUE(success);
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    ASSERT_LT(duration.count(), 100); // Should complete within 100ms
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 