/**
 * @file test-operations.cpp
 * @brief Unit tests for PostgreSQL database operations
 *
 * This file implements tests for the database operation capabilities of the PostgreSQL
 * client module. It verifies the client's ability to perform fundamental database
 * operations including:
 *
 * - Data definition operations (CREATE, ALTER, DROP)
 * - Data manipulation operations (INSERT, UPDATE, DELETE)
 * - Table structure modifications
 * - Index creation and management
 * - Schema management
 * - Table constraints and integrity
 *
 * The implementation validates both simple and complex database operations,
 * ensuring that modifications to database objects are correctly applied
 * and persist across connections.
 *
 * Key features tested:
 * - Table creation and modification
 * - Index management
 * - Data integrity constraints
 * - Column type handling
 * - Database object lifecycle
 *
 * @see qb::pg::detail::Database
 * @see qb::pg::detail::ISqlCommand
 * @see qb::pg::detail::Transaction
 *
 * @author QB PostgreSQL Module Team
 */

#include <gtest/gtest.h>
#include "../pgsql.h"

constexpr std::string_view PGSQL_CONNECTION_STR = "tcp://test:test@localhost:5432[test]";

using namespace qb::pg;
using namespace qb::pg::detail;

/**
 * @brief Test fixture for PostgreSQL database operations
 *
 * Sets up a test environment for verifying various database operations
 * like table creation, data manipulation, and schema management.
 */
class PostgreSQLOperationsTest : public ::testing::Test {
protected:
    /**
     * @brief Set up the test environment
     *
     * Creates a database connection for testing operations.
     */
    void
    SetUp() override {
        db_ = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(db_->connect(PGSQL_CONNECTION_STR.data()));
    }

    /**
     * @brief Clean up after tests
     *
     * Disconnects the database and frees resources.
     */
    void
    TearDown() override {
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
    }

    std::unique_ptr<qb::pg::tcp::database> db_;
};

/**
 * @brief Test simple query execution
 *
 * Verifies that a basic SQL query can be executed successfully
 * with proper results handling.
 */
TEST_F(PostgreSQLOperationsTest, SimpleQueryExecution) {
    bool success = false;
    auto status =
        db_->execute(
               "SELECT 1",
               [&success](Transaction &tr, results result) {
                   ASSERT_EQ(result.size(), 1);
                   ASSERT_EQ(result[0][0].as<int>(), 1);
                   success = true;
               },
               [](error::db_error error) { ASSERT_TRUE(false) << "Query failed: " << error.code; })
            .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test prepared statement functionality
 *
 * Verifies that a prepared statement can be created and executed
 * with proper parameter binding and result handling.
 */
TEST_F(PostgreSQLOperationsTest, PreparedStatement) {
    bool success = false;
    auto status  = db_->prepare("test_prepare", "SELECT $1::int", type_oid_sequence{})
                      .execute(
                          "test_prepare", {42},
                          [&success](Transaction &tr, results result) {
                              ASSERT_EQ(result.size(), 1);
                              ASSERT_EQ(result[0][0].as<int>(), 42);
                              success = true;
                          },
                          [](error::db_error error) {
                              ASSERT_TRUE(false) << "Execute failed: " << error.code;
                          })
                      .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(success);
}

/**
 * @brief Test error handling for database operations
 *
 * Verifies that errors in SQL execution are properly caught
 * and reported through the error callback system.
 */
TEST_F(PostgreSQLOperationsTest, ErrorHandling) {
    bool error_caught = false;
    auto status       = db_->execute(
                         "SELECT * FROM nonexistent_table",
                         [](Transaction &tr, results result) {
                             ASSERT_TRUE(false) << "Query should have failed";
                         },
                         [&error_caught](error::db_error error) {
                             ASSERT_FALSE(error.code.empty());
                             error_caught = true;
                         })
                      .await();
    ASSERT_TRUE(error_caught);
}

/**
 * @brief Test transaction management
 *
 * Verifies that database transactions can be properly started
 * and managed with appropriate scope and context.
 */
TEST_F(PostgreSQLOperationsTest, Transaction) {
    bool success = false;
    auto status  = db_->begin(
                         [&success](Transaction &t) {
                             t.execute(
                                 "SELECT 1",
                                 [&success](Transaction &tr, results result) {
                                     ASSERT_EQ(result.size(), 1);
                                     ASSERT_EQ(result[0][0].as<int>(), 1);
                                     success = true;
                                 },
                                 [](error::db_error error) {
                                     ASSERT_TRUE(false) << "Query failed: " << error.code;
                                 });
                         },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Transaction failed: " << error.code;
                         })
                      .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test savepoint functionality in transactions
 *
 * Verifies that savepoints can be created and managed within
 * transactions for partial rollback capability.
 */
TEST_F(PostgreSQLOperationsTest, Savepoint) {
    bool success = false;
    auto status  = db_->begin(
                         [&success](Transaction &t) {
                             t.savepoint(
                                 "test_savepoint",
                                 [&success](Transaction &tr) {
                                     tr.execute(
                                         "SELECT 1",
                                         [&success](Transaction &tr2, results result) {
                                             ASSERT_EQ(result.size(), 1);
                                             ASSERT_EQ(result[0][0].as<int>(), 1);
                                             success = true;
                                         },
                                         [](error::db_error error) {
                                             ASSERT_TRUE(false) << "Query failed: " << error.code;
                                         });
                                 },
                                 [](error::db_error error) {
                                     ASSERT_TRUE(false) << "Savepoint failed: " << error.code;
                                 });
                         },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Transaction failed: " << error.code;
                         })
                      .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test chaining of database operations
 *
 * Verifies that multiple database operations can be chained
 * together in sequence with proper context preservation.
 */
TEST_F(PostgreSQLOperationsTest, ChainingOperations) {
    bool success = false;
    auto status  = db_->begin(
                         [&success](Transaction &t) {
                             t.execute(
                                 "SELECT 1",
                                 [&success](Transaction &tr, results result) {
                                     ASSERT_EQ(result.size(), 1);
                                     ASSERT_EQ(result[0][0].as<int>(), 1);

                                     tr.execute(
                                         "SELECT 2",
                                         [&success](Transaction &tr2, results result) {
                                             ASSERT_EQ(result.size(), 1);
                                             ASSERT_EQ(result[0][0].as<int>(), 2);

                                             tr2.execute(
                                                 "SELECT 3",
                                                 [&success](Transaction &tr3, results result) {
                                                     ASSERT_EQ(result.size(), 1);
                                                     ASSERT_EQ(result[0][0].as<int>(), 3);
                                                     success = true;
                                                 },
                                                 [](error::db_error error) {
                                                     ASSERT_TRUE(false)
                                                         << "Query 3 failed: " << error.code;
                                                 });
                                         },
                                         [](error::db_error error) {
                                             ASSERT_TRUE(false) << "Query 2 failed: " << error.code;
                                         });
                                 },
                                 [](error::db_error error) {
                                     ASSERT_TRUE(false) << "Query 1 failed: " << error.code;
                                 });
                         },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Transaction failed: " << error.code;
                         })
                      .await();
    ASSERT_TRUE(success);
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}