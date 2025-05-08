/**
 * @file test-queries.cpp
 * @brief Unit tests for PostgreSQL query execution functionality
 *
 * This file implements tests for the query execution capabilities of the PostgreSQL
 * client module. It verifies the client's ability to correctly execute various types
 * of SQL queries and process their results, including:
 *
 * - Basic SELECT operations
 * - Conditional queries with WHERE clauses
 * - JOIN operations across tables
 * - Aggregation functions and GROUP BY queries
 * - Subqueries and complex query constructs
 * - Common Table Expressions (CTEs)
 * - Query performance characteristics
 *
 * The implementation validates query execution with different complexities,
 * ensuring results are correctly retrieved and processed. It also tests
 * bulk operations and different parameter passing approaches.
 *
 * Key features tested:
 * - Query result handling
 * - Query parameterization
 * - Complex query execution
 * - Vector parameter serialization
 * - Query performance
 * - Bulk operations
 *
 * @see qb::pg::tcp::database
 * @see qb::pg::detail::Transaction
 * @see qb::pg::detail::results
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include "../pgsql.h"
#include <filesystem>
#include <fstream>

using namespace qb::pg;
using namespace qb::io;

constexpr std::string_view PGSQL_CONNECTION_STR = "tcp://test:test@localhost:5432[test]";

/**
 * @brief Test fixture for PostgreSQL query functionality
 *
 * Sets up a test environment with database tables and sample data
 * for comprehensive query testing.
 */
class PostgreSQLQueryTest : public ::testing::Test {
protected:
    /**
     * @brief Set up the test environment
     *
     * Creates a database connection and sets up test tables with sample data
     * for query testing.
     */
    void
    SetUp() override {
        db_ = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(db_->connect(PGSQL_CONNECTION_STR.data()));

        // Create temporary test tables
        bool success = false;
        auto status =
            db_->execute(
                   R"(
            CREATE TEMP TABLE test_users (
                id SERIAL PRIMARY KEY,
                name VARCHAR(50),
                age INTEGER,
                email VARCHAR(100),
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )",
                   [&success](transaction &tr, results result) { success = true; },
                   [](error::db_error error) {
                       ASSERT_TRUE(false) << "Create table failed: " << error.code;
                   })
                .await();
        ASSERT_TRUE(success);

        success = false;
        status  = db_->execute(
                        R"(
            CREATE TEMP TABLE test_orders (
                id SERIAL PRIMARY KEY,
                user_id INTEGER REFERENCES test_users(id),
                amount DECIMAL(10,2),
                status VARCHAR(20),
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        )",
                        [&success](transaction &tr, results result) { success = true; },
                        [](error::db_error error) {
                            ASSERT_TRUE(false) << "Create table failed: " << error.code;
                        })
                     .await();
        ASSERT_TRUE(success);

        // Insert test data
        success = false;
        status  = db_->execute(
                        R"(
            INSERT INTO test_users (name, age, email) VALUES
            ('John Doe', 30, 'john@example.com'),
            ('Jane Smith', 25, 'jane@example.com'),
            ('Bob Wilson', 45, 'bob@example.com')
        )",
                        [&success](transaction &tr, results result) { success = true; },
                        [](error::db_error error) {
                            ASSERT_TRUE(false) << "Insert failed: " << error.code;
                        })
                     .await();
        ASSERT_TRUE(success);

        success = false;
        status  = db_->execute(
                        R"(
            INSERT INTO test_orders (user_id, amount, status) VALUES
            (1, 100.50, 'completed'),
            (1, 200.75, 'pending'),
            (2, 150.25, 'completed'),
            (3, 300.00, 'cancelled')
        )",
                        [&success](transaction &tr, results result) { success = true; },
                        [](error::db_error error) {
                            ASSERT_TRUE(false) << "Insert failed: " << error.code;
                        })
                     .await();
        ASSERT_TRUE(success);
    }

    /**
     * @brief Clean up after tests
     *
     * Drops temporary tables and disconnects the database.
     */
    void
    TearDown() override {
        if (db_) {
            bool success = false;
            auto status =
                db_->execute("DROP TABLE IF EXISTS test_orders")
                    .execute(
                        "DROP TABLE IF EXISTS test_users",
                        [&success](transaction &tr, results result) { success = true; },
                        [](error::db_error error) {
                            ASSERT_TRUE(false) << "Drop table failed: " << error.code;
                        })
                    .await();
            ASSERT_TRUE(success);
            db_->disconnect();
            db_.reset();
        }
    }

    std::unique_ptr<qb::pg::tcp::database> db_;
};

/**
 * @brief Test basic SELECT query functionality
 *
 * Verifies that a simple SELECT query can retrieve all rows
 * from a table with correct data.
 */
TEST_F(PostgreSQLQueryTest, BasicSelect) {
    bool success = false;
    auto status =
        db_->execute(
               "SELECT * FROM test_users",
               [&success](transaction &tr, results result) {
                   ASSERT_EQ(result.size(), 3);

                   // Check first row
                   ASSERT_EQ(result[0][1].as<std::string>(), "John Doe");
                   ASSERT_EQ(result[0][2].as<int>(), 30);
                   ASSERT_EQ(result[0][3].as<std::string>(), "john@example.com");
                   success = true;
               },
               [](error::db_error error) {
                   ASSERT_TRUE(false) << "Query failed: " << error.code;
               })
            .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test conditional queries with WHERE clause
 *
 * Verifies that a SELECT query with a WHERE clause correctly
 * filters results based on specified conditions.
 */
TEST_F(PostgreSQLQueryTest, WhereClause) {
    bool success = false;
    auto status  = db_->execute(
                         "SELECT * FROM test_users WHERE age > 30",
                         [&success](transaction &tr, results result) {
                             ASSERT_EQ(result.size(), 1);
                             ASSERT_EQ(result[0][1].as<std::string>(), "Bob Wilson");
                             success = true;
                         },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Query failed: " << error.code;
                         })
                      .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test JOIN query functionality
 *
 * Verifies that JOIN operations correctly combine data from
 * multiple tables based on specified relationships.
 */
TEST_F(PostgreSQLQueryTest, JoinQuery) {
    bool success = false;
    auto status  = db_->execute(
                         R"(
        SELECT u.name, o.amount, o.status
        FROM test_users u
        JOIN test_orders o ON u.id = o.user_id
        WHERE o.status = 'completed'
    )",
                         [&success](transaction &tr, results result) {
                             ASSERT_EQ(result.size(), 2);

                             // Check first completed order
                             ASSERT_EQ(result[0][0].as<std::string>(), "John Doe");
                             ASSERT_EQ(result[0][1].as<std::string>(), "100.50");
                             ASSERT_EQ(result[0][2].as<std::string>(), "completed");
                             success = true;
                         },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Query failed: " << error.code;
                         })
                      .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test aggregation query functionality
 *
 * Verifies that aggregation functions (COUNT, SUM) and GROUP BY clauses
 * work correctly to produce summarized results.
 */
TEST_F(PostgreSQLQueryTest, Aggregation) {
    bool success = false;
    auto status  = db_->execute(
                         R"(
        SELECT u.name, COUNT(o.id) as order_count, SUM(o.amount) as total_amount
        FROM test_users u
        LEFT JOIN test_orders o ON u.id = o.user_id
        GROUP BY u.id, u.name
        ORDER BY total_amount DESC
    )",
                         [&success](transaction &tr, results result) {
                             ASSERT_EQ(result.size(), 3);

                             // Check first row (John Doe with highest total)
                             ASSERT_EQ(result[0][0].as<std::string>(), "John Doe");
                             ASSERT_EQ(result[0][1].as<int>(), 2);
                             ASSERT_EQ(result[0][2].as<std::string>(), "301.25");
                             success = true;
                         },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Query failed: " << error.code;
                         })
                      .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test subquery functionality
 *
 * Verifies that subqueries can be correctly executed and their
 * results incorporated into the main query.
 */
TEST_F(PostgreSQLQueryTest, Subquery) {
    bool success = false;
    auto status  = db_->execute(
                         R"(
        SELECT name, age
        FROM test_users
        WHERE id IN (
            SELECT user_id
            FROM test_orders
            WHERE status = 'pending'
        )
    )",
                         [&success](transaction &tr, results result) {
                             ASSERT_EQ(result.size(), 1);
                             ASSERT_EQ(result[0][0].as<std::string>(), "John Doe");
                             ASSERT_EQ(result[0][1].as<int>(), 30);
                             success = true;
                         },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Query failed: " << error.code;
                         })
                      .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test complex query with CTE and multiple operations
 *
 * Verifies that complex queries with Common Table Expressions (CTEs),
 * multiple joins, aggregations, and conditional logic execute correctly.
 */
TEST_F(PostgreSQLQueryTest, ComplexQuery) {
    bool success = false;
    auto status =
        db_->execute(
               R"(
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
               [&success](transaction &tr, results result) {
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
               })
            .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test query performance and parameter handling
 *
 * Verifies query performance with different parameter passing methods,
 * including single parameters, multiple explicit parameters, and
 * vector parameters for batch operations.
 */
TEST_F(PostgreSQLQueryTest, QueryPerformance) {
    // Create test table
    bool success = false;
    auto status  = db_->execute(
                         R"(
        CREATE TEMP TABLE test_performance (
            id SERIAL PRIMARY KEY,
            value TEXT,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    )",
                         [&success](transaction &tr, results result) { success = true; },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Create table failed: " << error.code;
                         })
                      .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(success);

    // Prepare the queries
    ASSERT_TRUE(
        db_->prepare("simple_insert", "INSERT INTO test_performance (value) VALUES ($1)")
            .prepare("batch_insert",
                     "INSERT INTO test_performance (value) VALUES ($1),($2),($3),($4)")
            .prepare("multi_insert",
                     "INSERT INTO test_performance (value) VALUES ($1),($2),($3),($4)")
            .await());

    // METHOD 1: Simple insertion with a single parameter
    ASSERT_TRUE(
        db_->execute("simple_insert", params{std::string("Test value single")}).await());

    // METHOD 2: Insertion with multiple explicit parameters
    ASSERT_TRUE(
        db_->execute("batch_insert", params{std::string("Test value explicit 1"),
                                            std::string("Test value explicit 2"),
                                            std::string("Test value explicit 3"),
                                            std::string("Test value explicit 4")})
            .await());

    // METHOD 3: Insertion with a vector of strings
    std::vector<std::string> values;
    for (int i = 1; i <= 4; ++i) {
        values.push_back("Test value vector " + std::to_string(i));
    }
    ASSERT_TRUE(db_->execute("multi_insert", params{values}).await());

    // Verify that all insertions worked
    success = false;
    status  = db_->execute(
                    "SELECT value FROM test_performance ORDER BY id",
                    [&success](transaction &tr, results result) {
                        // We should have 9 rows in total (1 + 4 + 4)
                        ASSERT_EQ(result.size(), 9);

                        // Verify simple insertion
                        ASSERT_EQ(result[0][0].as<std::string>(), "Test value single");

                        // Verify insertion with explicit parameters
                        for (int i = 1; i <= 4; ++i) {
                            ASSERT_EQ(result[i][0].as<std::string>(),
                                       "Test value explicit " + std::to_string(i));
                        }

                        // Verify insertion with string vector
                        for (int i = 1; i <= 4; ++i) {
                            ASSERT_EQ(result[i + 4][0].as<std::string>(),
                                       "Test value vector " + std::to_string(i));
                        }

                        success = true;
                    },
                    [](error::db_error error) {
                        ASSERT_TRUE(false) << "Query failed: " << error.code;
                    })
                 .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(success);

    // Performance test with a large number of parameters
    std::vector<std::string> large_values;
    for (int i = 1; i <= 100; ++i) {
        large_values.push_back("Performance test value " + std::to_string(i));
    }

    // Dynamically build a multi-value insert query
    std::string insert_query = "INSERT INTO test_performance (value) VALUES ";
    for (int i = 1; i <= 100; ++i) {
        if (i > 1)
            insert_query += ",";
        insert_query += "($" + std::to_string(i) + ")";
    }

    // Mass insertion with a vector of 100 strings
    success = false;
    status  = db_->prepare("mass_insert", insert_query)
                 .execute(
                     "mass_insert", params{large_values},
                     [&success](transaction &tr, results result) { success = true; },
                     [](error::db_error error) {
                         ASSERT_TRUE(false) << "Insert failed: " << error.code;
                     })
                 .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(success);

    // Measure query performance
    auto start = std::chrono::steady_clock::now();
    success    = false;
    status =
        db_->execute(
               "SELECT COUNT(*) FROM test_performance WHERE value LIKE 'Performance "
               "test%'",
               [&success](transaction &tr, results result) {
                   ASSERT_EQ(result.size(), 1);
                   ASSERT_EQ(result[0][0].as<int>(), 100); // We should find 100 results
                   success = true;
               },
               [](error::db_error error) {
                   ASSERT_TRUE(false) << "Query failed: " << error.code;
               })
            .await();
    auto end = std::chrono::steady_clock::now();

    ASSERT_TRUE(status);
    ASSERT_TRUE(success);
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    ASSERT_LT(duration.count(), 100); // Should complete within 100ms
}

/**
 * @brief Test execution of SQL queries from files
 *
 * Verifies that the execute_file function can correctly load and execute
 * SQL queries from external files. This tests the convenience API for
 * managing large or complex SQL statements in separate files.
 */
TEST_F(PostgreSQLQueryTest, ExecuteFromFile) {
    // Define a temporary file path
    std::filesystem::path temp_file = std::filesystem::temp_directory_path() / "test_query.sql";
    
    // Create a temporary SQL file with a test query
    {
        std::ofstream file(temp_file);
        ASSERT_TRUE(file.is_open());
        file << "SELECT name, email FROM test_users WHERE age > 25";
        ASSERT_TRUE(file.good());
        file.close();
    }

    // Execute query from file with explicit callbacks
    bool success = false;
    auto status = db_->execute_file(
        temp_file,
        [&success](transaction &tr, results result) {
            ASSERT_EQ(result.size(), 2); // Should find 2 users older than 25
            
            // Verify results contain expected data
            bool found_john = false;
            bool found_bob = false;
            
            for (size_t i = 0; i < result.size(); ++i) {
                std::string name = result[i][0].as<std::string>();
                if (name == "John Doe") {
                    found_john = true;
                    ASSERT_EQ(result[i][1].as<std::string>(), "john@example.com");
                } else if (name == "Bob Wilson") {
                    found_bob = true;
                    ASSERT_EQ(result[i][1].as<std::string>(), "bob@example.com");
                }
            }
            
            ASSERT_TRUE(found_john);
            ASSERT_TRUE(found_bob);
            success = true;
        },
        [](error::db_error const& err) {
            ASSERT_TRUE(false) << "Query execution failed: " << err.code << " - " << err.what();
        }
    ).await();
    
    ASSERT_TRUE(status);
    ASSERT_TRUE(success);

    // Execute query from file with only success callback
    success = false;
    status = db_->execute_file(
        temp_file,
        [&success](transaction &tr, results result) {
            ASSERT_EQ(result.size(), 2);
            success = true;
        }
    ).await();
    
    ASSERT_TRUE(status);
    ASSERT_TRUE(success);

    // Execute query from file without callbacks
    status = db_->execute_file(temp_file).await();
    ASSERT_TRUE(status);

    // Test with non-existent file (should throw an exception)
    bool error_caught = false;
    bool exception_caught = false;
    try {
        status = db_->execute_file(
            std::filesystem::temp_directory_path() / "nonexistent.sql",
            [](transaction &tr, results result) {
                ASSERT_TRUE(false) << "Should not succeed with non-existent file";
            },
            [&error_caught](error::db_error const& err) {
                error_caught = true;
                std::cout << "Error on non-existent file (expected): " << err.what() << std::endl;
            }
        ).await();
        
        ASSERT_TRUE(false) << "Should have thrown an exception for non-existent file";
    } catch (const error::query_error& e) {
        exception_caught = true;
        std::cout << "Exception caught as expected: " << e.what() << std::endl;
    }
    
    ASSERT_TRUE(error_caught) << "Error callback should have been called";
    ASSERT_TRUE(exception_caught) << "Exception should have been thrown";

    // Clean up temporary file
    std::filesystem::remove(temp_file);
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}