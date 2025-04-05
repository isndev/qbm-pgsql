#include <chrono>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <stdexcept>
#include <thread>
#include "../pgsql.h"
#include "../src/transaction.h"

using namespace qb::pg;

class PostgreSQLErrorHandlingTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        // Configuration pour la connexion à la base de données de test
        std::string connection_string =
            "host=localhost port=5432 dbname=postgres user=postgres password=postgres";

        // Création de la connexion
        db_ = std::make_shared<database<qb::io::transport::tcp>>(connection_string);

        // Se connecter à la base de données
        bool connected = false;
        db_->connect([&connected](auto) { connected = true; }, [](auto) {});

        // Attendre la connexion
        for (int i = 0; i < 100 && !connected; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Vérifier que la connexion a réussi
        ASSERT_TRUE(connected) << "La connexion à PostgreSQL a échoué";

        // Créer la table de test
        bool table_created = false;
        auto transaction = db_->create_transaction();
        transaction->execute(
            "DROP TABLE IF EXISTS test_errors", [&table_created](auto) { table_created = true; },
            [](auto) {});

        // Attendre la création de la table
        for (int i = 0; i < 100 && !table_created; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        table_created = false;
        transaction->execute(
            "CREATE TABLE IF NOT EXISTS test_errors ("
            "  id SERIAL PRIMARY KEY,"
            "  value TEXT NOT NULL,"
            "  unique_value TEXT UNIQUE"
            ")",
            [&table_created](auto) { table_created = true; }, [](auto) {});

        // Attendre la création de la table
        for (int i = 0; i < 100 && !table_created; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Insérer une valeur initiale
        bool value_inserted = false;
        transaction->execute(
            "INSERT INTO test_errors (value, unique_value) VALUES ('test1', 'unique1')",
            [&value_inserted](auto) { value_inserted = true; }, [](auto) {});

        // Attendre l'insertion
        for (int i = 0; i < 100 && !value_inserted; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void
    TearDown() override {
        // Nettoyer la base de données
        if (db_) {
            bool cleaned = false;
            auto transaction = db_->create_transaction();
            transaction->execute(
                "DROP TABLE IF EXISTS test_errors", [&cleaned](auto) { cleaned = true; },
                [](auto) {});

            // Attendre le nettoyage
            for (int i = 0; i < 100 && !cleaned; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Fermer la connexion
            db_->disconnect();
        }
    }

    std::shared_ptr<database<qb::io::transport::tcp>> db_;
};

TEST_F(PostgreSQLErrorHandlingTest, ConnectionError) {
    auto invalid_db = std::make_shared<database<qb::io::transport::tcp>>(
        "host=invalid_host port=5432 dbname=postgres user=postgres password=postgres");
    ASSERT_FALSE(invalid_db->connect());
}

TEST_F(PostgreSQLErrorHandlingTest, AuthenticationError) {
    auto invalid_db = std::make_shared<database<qb::io::transport::tcp>>(
        "host=localhost port=5432 dbname=postgres user=invalid password=invalid");
    ASSERT_FALSE(invalid_db->connect());
}

TEST_F(PostgreSQLErrorHandlingTest, SyntaxError) {
    auto transaction = db_->create_transaction();
    ASSERT_FALSE(transaction->execute("INVALID SQL STATEMENT"));
}

TEST_F(PostgreSQLErrorHandlingTest, TableNotFound) {
    auto transaction = db_->create_transaction();
    ASSERT_FALSE(transaction->execute("SELECT * FROM non_existent_table"));
}

TEST_F(PostgreSQLErrorHandlingTest, ColumnNotFound) {
    auto transaction = db_->create_transaction();
    ASSERT_FALSE(transaction->execute("SELECT non_existent_column FROM test_errors"));
}

TEST_F(PostgreSQLErrorHandlingTest, NotNullViolation) {
    auto transaction = db_->create_transaction();
    ASSERT_FALSE(transaction->execute("INSERT INTO test_errors (value) VALUES (NULL)"));
}

TEST_F(PostgreSQLErrorHandlingTest, UniqueViolation) {
    // Insert first row
    auto transaction = db_->create_transaction();
    ASSERT_TRUE(transaction->execute(
        "INSERT INTO test_errors (value, unique_value) VALUES ('test1', 'unique1')"));

    // Try to insert duplicate unique value
    ASSERT_FALSE(transaction->execute(
        "INSERT INTO test_errors (value, unique_value) VALUES ('test2', 'unique1')"));
}

TEST_F(PostgreSQLErrorHandlingTest, TransactionError) {
    // Start transaction
    auto transaction = db_->create_transaction();
    ASSERT_TRUE(transaction->begin());

    // Insert valid data
    ASSERT_TRUE(transaction->execute(
        "INSERT INTO test_errors (value, unique_value) VALUES ('test1', 'unique1')"));

    // Try to insert invalid data (should fail)
    ASSERT_FALSE(transaction->execute("INSERT INTO test_errors (value) VALUES (NULL)"));

    // Try to commit (should fail)
    ASSERT_FALSE(transaction->commit());

    // Verify no data was committed
    auto result = transaction->execute("SELECT COUNT(*) FROM test_errors");
    ASSERT_EQ(result.get<int>(0, 0), 0);
}

TEST_F(PostgreSQLErrorHandlingTest, PreparedStatementError) {
    // Try to prepare invalid statement
    auto transaction = db_->create_transaction();
    auto invalid_stmt = transaction->prepare("INVALID SQL STATEMENT");
    ASSERT_FALSE(invalid_stmt);

    // Try to prepare statement with wrong number of parameters
    auto wrong_params_stmt =
        transaction->prepare("INSERT INTO test_errors (value, unique_value) VALUES ($1, $2, $3)");
    ASSERT_FALSE(wrong_params_stmt);

    // Prepare valid statement but execute with wrong parameters
    auto valid_stmt =
        transaction->prepare("INSERT INTO test_errors (value, unique_value) VALUES ($1, $2)");
    ASSERT_TRUE(valid_stmt);
    ASSERT_FALSE(valid_stmt->execute("test1")); // Missing second parameter
}

TEST_F(PostgreSQLErrorHandlingTest, ConnectionTimeout) {
    // Set a very short connection timeout
    auto timeout_db = std::make_shared<database<qb::io::transport::tcp>>(
        "host=invalid_host port=5432 dbname=postgres user=postgres password=postgres "
        "connect_timeout=1");

    auto start = std::chrono::steady_clock::now();
    ASSERT_FALSE(timeout_db->connect());
    auto end = std::chrono::steady_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);
    ASSERT_LT(duration.count(), 5); // Should timeout within 5 seconds
}

TEST_F(PostgreSQLErrorHandlingTest, QueryTimeout) {
    // Set a very short statement timeout
    ASSERT_TRUE(db_->execute("SET statement_timeout = 1000")); // 1 second

    // Try to execute a long-running query
    ASSERT_FALSE(db_->execute("SELECT pg_sleep(2)")); // Should timeout
}

TEST_F(PostgreSQLErrorHandlingTest, ConnectionLoss) {
    ASSERT_TRUE(db_->connect());

    // Simulate connection loss
    db_->disconnect();

    // Try to execute query (should fail)
    ASSERT_FALSE(db_->execute("SELECT * FROM test_errors"));

    // Try to reconnect
    ASSERT_TRUE(db_->connect());

    // Verify connection is working again
    ASSERT_TRUE(db_->execute("SELECT * FROM test_errors"));
}

TEST_F(PostgreSQLErrorHandlingTest, ErrorRecovery) {
    // Start transaction
    auto transaction = db_->create_transaction();
    ASSERT_TRUE(transaction->begin());

    // Insert some data
    ASSERT_TRUE(transaction->execute(
        "INSERT INTO test_errors (value, unique_value) VALUES ('test1', 'unique1')"));

    // Simulate error
    ASSERT_FALSE(transaction->execute("INSERT INTO test_errors (value) VALUES (NULL)"));

    // Rollback transaction
    ASSERT_TRUE(transaction->rollback());

    // Verify data was rolled back
    auto result = transaction->execute("SELECT COUNT(*) FROM test_errors");
    ASSERT_EQ(result.get<int>(0, 0), 0);

    // Try new transaction
    ASSERT_TRUE(transaction->begin());
    ASSERT_TRUE(transaction->execute(
        "INSERT INTO test_errors (value, unique_value) VALUES ('test1', 'unique1')"));
    ASSERT_TRUE(transaction->commit());

    // Verify new data was committed
    result = transaction->execute("SELECT COUNT(*) FROM test_errors");
    ASSERT_EQ(result.get<int>(0, 0), 1);
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}