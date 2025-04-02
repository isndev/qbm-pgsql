#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <thread>
#include <chrono>
#include <stdexcept>
#include "../pgsql.h"

using namespace qb::pg;

class PostgreSQLErrorHandlingTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<tcp::database>("host=localhost port=5432 dbname=postgres user=postgres password=postgres");
        ASSERT_TRUE(db_->connect());
        
        // Create a temporary test table
        db_->execute(R"(
            CREATE TEMP TABLE test_errors (
                id SERIAL PRIMARY KEY,
                value TEXT NOT NULL,
                unique_value TEXT UNIQUE
            )
        )");
    }

    void TearDown() override {
        if (db_) {
            db_->execute("DROP TABLE IF EXISTS test_errors");
            db_.reset();
        }
    }

    std::unique_ptr<tcp::database> db_;
};

TEST_F(PostgreSQLErrorHandlingTest, ConnectionError) {
    auto invalid_db = std::make_unique<tcp::database>("host=invalid_host port=5432 dbname=postgres user=postgres password=postgres");
    ASSERT_FALSE(invalid_db->connect());
}

TEST_F(PostgreSQLErrorHandlingTest, AuthenticationError) {
    auto invalid_db = std::make_unique<tcp::database>("host=localhost port=5432 dbname=postgres user=invalid password=invalid");
    ASSERT_FALSE(invalid_db->connect());
}

TEST_F(PostgreSQLErrorHandlingTest, SyntaxError) {
    ASSERT_FALSE(db_->execute("INVALID SQL STATEMENT"));
}

TEST_F(PostgreSQLErrorHandlingTest, TableNotFound) {
    ASSERT_FALSE(db_->execute("SELECT * FROM non_existent_table"));
}

TEST_F(PostgreSQLErrorHandlingTest, ColumnNotFound) {
    ASSERT_FALSE(db_->execute("SELECT non_existent_column FROM test_errors"));
}

TEST_F(PostgreSQLErrorHandlingTest, NotNullViolation) {
    ASSERT_FALSE(db_->execute("INSERT INTO test_errors (value) VALUES (NULL)"));
}

TEST_F(PostgreSQLErrorHandlingTest, UniqueViolation) {
    // Insert first row
    ASSERT_TRUE(db_->execute("INSERT INTO test_errors (value, unique_value) VALUES ('test1', 'unique1')"));
    
    // Try to insert duplicate unique value
    ASSERT_FALSE(db_->execute("INSERT INTO test_errors (value, unique_value) VALUES ('test2', 'unique1')"));
}

TEST_F(PostgreSQLErrorHandlingTest, TransactionError) {
    // Start transaction
    ASSERT_TRUE(db_->begin());
    
    // Insert valid data
    ASSERT_TRUE(db_->execute("INSERT INTO test_errors (value, unique_value) VALUES ('test1', 'unique1')"));
    
    // Try to insert invalid data (should fail)
    ASSERT_FALSE(db_->execute("INSERT INTO test_errors (value) VALUES (NULL)"));
    
    // Try to commit (should fail)
    ASSERT_FALSE(db_->commit());
    
    // Verify no data was committed
    auto result = db_->query("SELECT COUNT(*) FROM test_errors");
    ASSERT_EQ(result->get<int>(0, 0), 0);
}

TEST_F(PostgreSQLErrorHandlingTest, PreparedStatementError) {
    // Try to prepare invalid statement
    auto invalid_stmt = db_->prepare("INVALID SQL STATEMENT");
    ASSERT_FALSE(invalid_stmt);
    
    // Try to prepare statement with wrong number of parameters
    auto wrong_params_stmt = db_->prepare("INSERT INTO test_errors (value, unique_value) VALUES ($1, $2, $3)");
    ASSERT_FALSE(wrong_params_stmt);
    
    // Prepare valid statement but execute with wrong parameters
    auto valid_stmt = db_->prepare("INSERT INTO test_errors (value, unique_value) VALUES ($1, $2)");
    ASSERT_TRUE(valid_stmt);
    ASSERT_FALSE(valid_stmt->execute("test1")); // Missing second parameter
}

TEST_F(PostgreSQLErrorHandlingTest, ConnectionTimeout) {
    // Set a very short connection timeout
    auto timeout_db = std::make_unique<tcp::database>("host=invalid_host port=5432 dbname=postgres user=postgres password=postgres");
    
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
    ASSERT_TRUE(db_->begin());
    
    // Insert some data
    ASSERT_TRUE(db_->execute("INSERT INTO test_errors (value, unique_value) VALUES ('test1', 'unique1')"));
    
    // Simulate error
    ASSERT_FALSE(db_->execute("INSERT INTO test_errors (value) VALUES (NULL)"));
    
    // Rollback transaction
    ASSERT_TRUE(db_->rollback());
    
    // Verify data was rolled back
    auto result = db_->query("SELECT COUNT(*) FROM test_errors");
    ASSERT_EQ(result->get<int>(0, 0), 0);
    
    // Try new transaction
    ASSERT_TRUE(db_->begin());
    ASSERT_TRUE(db_->execute("INSERT INTO test_errors (value, unique_value) VALUES ('test1', 'unique1')"));
    ASSERT_TRUE(db_->commit());
    
    // Verify new data was committed
    result = db_->query("SELECT COUNT(*) FROM test_errors");
    ASSERT_EQ(result->get<int>(0, 0), 1);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 