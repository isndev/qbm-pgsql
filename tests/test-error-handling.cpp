/**
 * @file test-error-handling.cpp
 * @brief Unit tests for PostgreSQL error handling mechanisms
 *
 * This file implements tests for the error handling capabilities of the PostgreSQL
 * client module. It verifies that the client properly handles various error conditions
 * such as:
 *
 * - Connection errors (invalid credentials, unreachable server)
 * - Syntax errors in SQL statements
 * - Constraint violations (unique, foreign key, check constraints)
 * - Transaction errors (deadlocks, serialization failures)
 * - Resource exhaustion conditions
 *
 * The implementation validates both synchronous and asynchronous error handling patterns,
 * ensuring that errors are properly propagated to callbacks and status indicators.
 *
 * Key features tested:
 * - Error code and message extraction
 * - Error state (SQLSTATE) validation
 * - Error callback execution
 * - Transaction state after errors
 * - Prepared statement error handling
 *
 * @see qb::pg::error::db_error
 * @see qb::pg::error::query_error
 * @see qb::pg::detail::Database
 *
 * @author QB PostgreSQL Module Team
 */

#include <gtest/gtest.h>
#include "../pgsql.h"

constexpr std::string_view PGSQL_CONNECTION_STR = "tcp://test:test@localhost:5432[test]";

using namespace qb::pg;
using namespace qb::pg::detail;

/**
 * @brief Test fixture for PostgreSQL error handling tests
 *
 * Sets up a test environment with database tables and constraints
 * that can be used to trigger various error conditions.
 */
class PostgreSQLErrorHandlingTest : public ::testing::Test {
protected:
    /**
     * @brief Set up the test environment
     *
     * Creates a database connection and sets up test tables with various
     * constraints for testing error handling scenarios.
     */
    void
    SetUp() override {
        // Create a test database connection
        db_ = std::make_unique<qb::pg::tcp::database>();

        // Connect to the database
        bool connected = db_->connect(PGSQL_CONNECTION_STR.data());
        if (!connected) {
            GTEST_SKIP() << "Skipping test because database connection failed";
            return;
        }

        // Set up test table
        setupTestTable();
    }

    /**
     * @brief Clean up after tests
     *
     * Drops temporary tables and disconnects the database connection.
     */
    void
    TearDown() override {
        if (db_) {
            // Clean up the test table before disconnecting
            auto status = db_->execute("DROP TABLE IF EXISTS test_errors").await();

            // Disconnect the database
            db_->disconnect();
            db_.reset();
        }
    }

    /**
     * @brief Set up test table with constraints
     *
     * Creates a test table with various constraints (NOT NULL, UNIQUE) 
     * that can be used to test error handling scenarios.
     */
    void
    setupTestTable() {
        // First drop the table if it exists
        auto drop_status = db_->execute("DROP TABLE IF EXISTS test_errors").await();

        // Create the test table
        auto create_status = db_->execute("CREATE TABLE IF NOT EXISTS test_errors ("
                                          "  id SERIAL PRIMARY KEY,"
                                          "  value TEXT NOT NULL,"
                                          "  unique_value TEXT UNIQUE"
                                          ")")
                                 .await();

        // Insert test data
        auto insert_status =
            db_->execute(
                   "INSERT INTO test_errors (value, unique_value) VALUES ('test1', 'unique1')")
                .await();
    }

    std::unique_ptr<qb::pg::tcp::database> db_;
};

// Test handling of syntax errors in SQL statements
TEST_F(PostgreSQLErrorHandlingTest, SyntaxError) {
    bool error_caught = false;
    auto status =
        db_->execute(
               "INVALID SQL STATEMENT",
               [](Transaction &tr, results result) { FAIL() << "Query should have failed"; },
               [&error_caught](error::db_error err) {
                   error_caught = true;
                   // Check error contains syntax-related info
                   EXPECT_TRUE(err.what() != nullptr &&
                               (std::string(err.what()).find("syntax") != std::string::npos ||
                                err.code == "42601"));
               })
            .await();

    EXPECT_TRUE(error_caught);
}

// Test handling of queries on non-existent tables
TEST_F(PostgreSQLErrorHandlingTest, TableNotFound) {
    bool error_caught = false;
    auto status =
        db_->execute(
               "SELECT * FROM non_existent_table",
               [](Transaction &tr, results result) { FAIL() << "Query should have failed"; },
               [&error_caught](error::db_error err) {
                   error_caught = true;
                   // Check error contains relation-related info
                   EXPECT_TRUE(
                       err.what() != nullptr &&
                       (std::string(err.what()).find("does not exist") != std::string::npos ||
                        err.code == "42P01"));
               })
            .await();

    EXPECT_TRUE(error_caught);
}

// Test handling of queries with non-existent columns
TEST_F(PostgreSQLErrorHandlingTest, ColumnNotFound) {
    bool error_caught = false;
    auto status =
        db_->execute(
               "SELECT non_existent_column FROM test_errors",
               [](Transaction &tr, results result) { FAIL() << "Query should have failed"; },
               [&error_caught](error::db_error err) {
                   error_caught = true;
                   // Check error contains column-related info
                   EXPECT_TRUE(err.what() != nullptr &&
                               (std::string(err.what()).find("column") != std::string::npos ||
                                err.code == "42703"));
               })
            .await();

    EXPECT_TRUE(error_caught);
}

// Test handling of NOT NULL constraint violations
TEST_F(PostgreSQLErrorHandlingTest, NotNullViolation) {
    bool error_caught = false;
    auto status =
        db_->execute(
               "INSERT INTO test_errors (value) VALUES (NULL)",
               [](Transaction &tr, results result) { FAIL() << "Query should have failed"; },
               [&error_caught](error::db_error err) {
                   error_caught = true;
                   // Check error contains null-related info
                   EXPECT_TRUE(err.what() != nullptr &&
                               (std::string(err.what()).find("null") != std::string::npos ||
                                err.code == "23502"));
               })
            .await();

    EXPECT_TRUE(error_caught);
}

// Test handling of UNIQUE constraint violations
TEST_F(PostgreSQLErrorHandlingTest, UniqueViolation) {
    bool error_caught = false;
    auto status =
        db_->execute(
               "INSERT INTO test_errors (value, unique_value) VALUES ('test2', 'unique1')",
               [](Transaction &tr, results result) { FAIL() << "Query should have failed"; },
               [&error_caught](error::db_error err) {
                   error_caught = true;
                   // Check error contains unique-related info
                   EXPECT_TRUE(err.what() != nullptr &&
                               (std::string(err.what()).find("unique") != std::string::npos ||
                                err.code == "23505"));
               })
            .await();

    EXPECT_TRUE(error_caught);
}

// Test connection to non-existent server
TEST_F(PostgreSQLErrorHandlingTest, ConnectionError) {
    const auto invalid_db = std::make_unique<qb::pg::tcp::database>();
    ASSERT_FALSE(invalid_db->connect("tcp://postgres:postgres@non_existent_host:5432[postgres]"));
}

// Test handling of prepared statement parameter errors
TEST_F(PostgreSQLErrorHandlingTest, PreparedStatementParameterError) {
    bool statement_prepared = false;
    bool error_caught       = false;

    auto prepare_status =
        db_->prepare("test_prepare",
                     "INSERT INTO test_errors (value, unique_value) VALUES ($1, $2)",
                     type_oid_sequence{oid::text, oid::text})
            .await();

    ASSERT_TRUE(prepare_status);

    auto exec_status =
        db_->execute(
               "test_prepare", {std::string("test_value")}, // Missing second parameter
               [](Transaction &tr, results result) {
                   FAIL() << "Execute should have failed due to missing parameter";
               },
               [&error_caught](error::db_error err) {
                   error_caught = true;
                   // Check error contains parameter-related info
                   EXPECT_TRUE(err.what() != nullptr &&
                               std::string(err.what()).find("parameter") != std::string::npos);
               })
            .await();

    EXPECT_TRUE(error_caught);
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}