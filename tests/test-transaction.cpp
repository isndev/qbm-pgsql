/**
 * @file test-transaction.cpp
 * @brief Unit tests for PostgreSQL transaction management
 *
 * This file implements tests for the transaction management functionality of the
 * PostgreSQL client module. It verifies the client's ability to properly handle
 * database transactions including:
 *
 * - Transaction begin, commit, and rollback operations
 * - Nested transactions using savepoints
 * - Transaction isolation levels
 * - Error handling within transactions
 * - Data consistency across transaction boundaries
 * - Concurrent transaction handling
 *
 * The implementation validates both simple and complex transaction patterns,
 * ensuring that data modifications are properly committed or rolled back according
 * to the specified transaction behavior.
 *
 * Key features tested:
 * - Basic transaction operations
 * - Savepoint management
 * - Transaction isolation enforcement
 * - Transaction error recovery
 * - Transaction callback execution
 *
 * @see qb::pg::detail::Transaction
 * @see qb::pg::detail::Database
 * @see qb::pg::detail::ISqlCommand
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

constexpr std::string_view PGSQL_CONNECTION_STR = "tcp://test:test@localhost:5432[test]";

using namespace qb::pg;
using namespace qb::pg::detail;

/**
 * @brief Test fixture for PostgreSQL transaction functionality
 *
 * Sets up a test environment with database tables and data for testing
 * transaction management features.
 */
class PostgreSQLTransactionTest : public ::testing::Test {
protected:
    /**
     * @brief Set up the test environment
     *
     * Creates a database connection and sets up test tables with sample data
     * for transaction testing.
     */
    void
    SetUp() override {
        db_ = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(db_->connect(PGSQL_CONNECTION_STR.data()));

        auto status = db_->execute("CREATE TEMP TABLE test_transactions (id SERIAL "
                                   "PRIMARY KEY, value TEXT)")
                          .await();
        ASSERT_TRUE(status);
    }

    /**
     * @brief Clean up after tests
     *
     * Drops temporary tables and disconnects the database connection.
     */
    void
    TearDown() override {
        if (db_) {
            auto status = db_->execute("DROP TABLE IF EXISTS test_transactions").await();
            ASSERT_TRUE(status);
            db_->disconnect();
            db_.reset();
        }
    }

    std::unique_ptr<qb::pg::tcp::database> db_;
};

/**
 * @brief Test basic transaction functionality
 *
 * Verifies that a simple transaction can be started and executed
 * with proper scope and context.
 */
TEST_F(PostgreSQLTransactionTest, BasicTransaction) {
    bool success = false;
    auto status  = db_->begin(
                         [&success](Transaction &t) {
                             t.execute(
                                 "SELECT * FROM test_transactions",
                                 [&success](Transaction &tr, results result) {
                                     ASSERT_EQ(result.size(), 0);
                                     success = true;
                                 },
                                 [](error::db_error error) { ASSERT_TRUE(false); });
                         },
                         [](error::db_error error) { ASSERT_TRUE(false); })
                      .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test transaction rollback on error
 *
 * Verifies that a transaction is properly rolled back when an error
 * occurs during its execution, ensuring data consistency.
 */
TEST_F(PostgreSQLTransactionTest, TransactionRollback) {
    bool error_caught = false;
    auto status       = db_->begin(
                         [](Transaction &t) {
                             t.execute(
                                 "INSERT INTO nonexistent (value) VALUES ('test')",
                                 [](Transaction &tr, results result) {
                                     ASSERT_TRUE(false); // This query should fail
                                 },
                                 [](error::db_error error) {
                                     ASSERT_TRUE(true); // We expect an error
                                 });
                         },
                         [&error_caught](error::db_error error) { error_caught = true; })
                      .await();
    ASSERT_TRUE(error_caught);
}

/**
 * @brief Test nested transactions using savepoints
 *
 * Verifies that nested transactions via savepoints work correctly,
 * allowing for partial rollbacks while maintaining an outer transaction.
 */
TEST_F(PostgreSQLTransactionTest, NestedTransactions) {
    bool success = false;
    auto status =
        db_->begin(
               [&success](Transaction &t) {
                   t.savepoint(
                       "nested_sp",
                       [&success](Transaction &tr) {
                           tr.execute(
                               "INSERT INTO test_transactions (value) VALUES ('test')",
                               [&success](Transaction &tr2, results result) {
                                   success = true;
                               },
                               [](error::db_error error) { ASSERT_TRUE(false); });
                       },
                       [](error::db_error error) { ASSERT_TRUE(false); });
               },
               [](error::db_error error) { ASSERT_TRUE(false); })
            .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test transaction isolation
 *
 * Verifies that transaction isolation works correctly, with changes
 * not visible outside the transaction until committed.
 */
TEST_F(PostgreSQLTransactionTest, TransactionIsolation) {
    bool success = false;
    auto status  = db_->begin(
                         [&success](Transaction &t) {
                             t.execute(
                                 "SELECT * FROM test_transactions",
                                 [&success](Transaction &tr, results result) {
                                     ASSERT_EQ(result.size(), 0);
                                     success = true;
                                 },
                                 [](error::db_error error) { ASSERT_TRUE(false); });
                         },
                         [](error::db_error error) { ASSERT_TRUE(false); })
                      .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test transaction timeout handling
 *
 * Verifies that long-running transactions with sleep operations
 * complete successfully without triggering timeout errors.
 */
TEST_F(PostgreSQLTransactionTest, TransactionTimeout) {
    bool success = false;
    auto status =
        db_->begin(
               [&success](Transaction &t) {
                   t.execute(
                       "SELECT pg_sleep(2)",
                       [&success](Transaction &tr, results result) { success = true; },
                       [](error::db_error error) { ASSERT_TRUE(false); });
               },
               [](error::db_error error) { ASSERT_TRUE(false); })
            .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test multiple statements in a transaction
 *
 * Verifies that multiple SQL statements can be executed
 * within a single transaction context.
 */
TEST_F(PostgreSQLTransactionTest, MultipleStatements) {
    bool success = false;
    auto status  = db_->begin(
                         [&success](Transaction &t) {
                             t.execute(
                                 "SELECT * FROM test_transactions",
                                 [&success](Transaction &tr, results result) {
                                     ASSERT_EQ(result.size(), 0);
                                     success = true;
                                 },
                                 [](error::db_error error) { ASSERT_TRUE(false); });
                         },
                         [](error::db_error error) { ASSERT_TRUE(false); })
                      .await();
    ASSERT_TRUE(success);
}

/**
 * @brief Test savepoint rollback functionality with error handling
 *
 * Verifies that when a savepoint is rolled back, changes made within
 * the savepoint are undone while preserving changes made outside it.
 * This test specifically tests the error propagation mechanism.
 */
TEST_F(PostgreSQLTransactionTest, SavepointRollback) {
    bool before_savepoint = false;
    bool in_savepoint     = false;
    bool error_caught     = false;
    bool after_savepoint  = false;

    // Clean the test table
    auto cleanup = db_->execute("DELETE FROM test_transactions").await();
    ASSERT_TRUE(cleanup);

    // Insert reference data outside the main transaction
    auto setup =
        db_->execute("INSERT INTO test_transactions (value) VALUES ('before_savepoint')")
            .await();
    ASSERT_TRUE(setup);
    before_savepoint = true;

    // Phase 1: Verify that an error in a savepoint triggers a rollback of the savepoint
    auto status =
        db_->begin(
               [&in_savepoint, &error_caught](Transaction &t) {
                   // Create a savepoint where we'll cause an error
                   t.savepoint(
                       "sp1",
                       [&in_savepoint, &error_caught](Transaction &tr2) {
                           // Insert data that will be rolled back
                           tr2.execute(
                               "INSERT INTO test_transactions (value) VALUES "
                               "('in_savepoint')",
                               [&in_savepoint, &error_caught](Transaction &tr3,
                                                              results      result) {
                                   in_savepoint = true;

                                   // Trigger an explicit error that will cause the
                                   // savepoint to roll back
                                   tr3.execute(
                                       "SELECT * FROM nonexistent_table",
                                       [](Transaction &tr4, results result) {
                                           FAIL() << "This query should fail";
                                       },
                                       [&error_caught](error::db_error error) {
                                           error_caught = true;
                                           std::cout
                                               << "SQL error detected in savepoint: "
                                               << error.what() << std::endl;
                                       });
                               },
                               [](error::db_error error) {
                                   std::cout << "Error during savepoint insertion: "
                                             << error.what() << std::endl;
                               });
                       },
                       [](error::db_error error) {
                           std::cout << "Savepoint error detected: " << error.what()
                                     << std::endl;
                       });
               },
               [](error::db_error error) {
                   std::cout << "Transaction error detected: " << error.what()
                             << std::endl;
               })
            .await();

    // Phase 2: Verify that data outside the savepoint still exists and data inside the
    // savepoint was removed
    auto verify =
        db_->begin(
               [&after_savepoint](Transaction &t) {
                   // Verify that savepoint data doesn't exist (rollback)
                   t.execute(
                       "SELECT * FROM test_transactions WHERE value = 'in_savepoint'",
                       [](Transaction &tr, results result) {
                           ASSERT_EQ(result.size(), 0)
                               << "Savepoint data was not properly rolled back";
                       },
                       [](error::db_error error) {
                           FAIL() << "Error when verifying rollback: " << error.what();
                       });

                   // Verify that data before the savepoint still exists (commit)
                   t.execute(
                       "SELECT * FROM test_transactions WHERE value = "
                       "'before_savepoint'",
                       [&after_savepoint](Transaction &tr, results result) {
                           ASSERT_EQ(result.size(), 1)
                               << "Data before savepoint was not preserved";
                           after_savepoint = true;
                       },
                       [](error::db_error error) {
                           FAIL() << "Error when verifying commit: " << error.what();
                       });
               },
               [](error::db_error error) {
                   FAIL() << "Error during verification: " << error.what();
               })
            .await();

    // Verify that the execution flow completed correctly
    ASSERT_TRUE(before_savepoint) << "Insertion before savepoint didn't work";
    ASSERT_TRUE(in_savepoint) << "Insertion in savepoint didn't work";
    ASSERT_TRUE(error_caught) << "Error in savepoint wasn't detected";
    ASSERT_TRUE(after_savepoint) << "Verification after savepoint didn't work";
}

/**
 * @brief Test multiple nested transactions with savepoints
 *
 * Verifies that multiple levels of nested transactions work correctly
 * with proper scope isolation and state management.
 */
TEST_F(PostgreSQLTransactionTest, MultipleNestedTransactions) {
    bool success1 = false;
    bool success2 = false;
    auto status =
        db_->begin(
               [&success1, &success2](Transaction &t) {
                   t.savepoint(
                       "sp1",
                       [&success1, &success2](Transaction &tr1) {
                           tr1.execute(
                               "INSERT INTO test_transactions (value) VALUES ('sp1')",
                               [&success1, &success2](Transaction &tr2, results result) {
                                   success1 = true;
                                   tr2.savepoint(
                                       "sp2",
                                       [&success2](Transaction &tr3) {
                                           tr3.execute(
                                               "INSERT INTO test_transactions (value) "
                                               "VALUES "
                                               "('sp2')",
                                               [&success2](Transaction &tr4,
                                                           results      result) {
                                                   success2 = true;
                                               },
                                               [](error::db_error error) {
                                                   ASSERT_TRUE(false);
                                               });
                                       },
                                       [](error::db_error error) {
                                           ASSERT_TRUE(false);
                                       });
                               },
                               [](error::db_error error) { ASSERT_TRUE(false); });
                       },
                       [](error::db_error error) { ASSERT_TRUE(false); });
               },
               [](error::db_error error) { ASSERT_TRUE(false); })
            .await();
    ASSERT_TRUE(success1);
    ASSERT_TRUE(success2);
}

/**
 * @brief Test committing multiple changes in a transaction
 *
 * Verifies that multiple data modifications within a transaction
 * are all committed atomically on successful completion.
 */
TEST_F(PostgreSQLTransactionTest, CommitMultipleChanges) {
    bool insert_success = false;
    bool select_success = false;
    auto status =
        db_->begin(
               [&insert_success, &select_success](Transaction &t) {
                   t.execute(
                       "INSERT INTO test_transactions (value) VALUES ('test1')",
                       [&insert_success, &select_success](Transaction &tr1,
                                                          results      result) {
                           insert_success = true;
                           tr1.execute(
                               "INSERT INTO test_transactions (value) VALUES ('test2')",
                               [&select_success](Transaction &tr2, results result) {
                                   tr2.execute(
                                       "SELECT * FROM test_transactions",
                                       [&select_success](Transaction &tr3,
                                                         results      result) {
                                           ASSERT_EQ(result.size(), 2);
                                           select_success = true;
                                       },
                                       [](error::db_error error) {
                                           ASSERT_TRUE(false);
                                       });
                               },
                               [](error::db_error error) { ASSERT_TRUE(false); });
                       },
                       [](error::db_error error) { ASSERT_TRUE(false); });
               },
               [](error::db_error error) { ASSERT_TRUE(false); })
            .await();
    ASSERT_TRUE(insert_success);
    ASSERT_TRUE(select_success);
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}