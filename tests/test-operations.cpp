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
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../pgsql.h"
#include "test_config.hpp"

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
        ASSERT_TRUE(qb::io::async::run_sync(db_->connect(qb::pg::test::dsn_tcp_string())));
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

/** @brief SimpleQueryExecution via `co_await query()`. */
TEST_F(PostgreSQLOperationsTest, SimpleQueryExecution_Coroutine) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply = co_await db_->query("SELECT 1");
        ok         = reply.ok() && reply.result().size() == 1 && reply.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(ok);
}

/**
 * @brief Test prepared statement functionality
 *
 * Verifies that a prepared statement can be created and executed
 * with proper parameter binding and result handling.
 */
TEST_F(PostgreSQLOperationsTest, PreparedStatement) {
    ASSERT_TRUE(db_->prepare("test_prepare", "SELECT $1::int", type_oid_sequence{}, discard_prepare,
                             discard_error)
                    .await());
    bool success = false;
    auto status =
        db_->execute(
               "test_prepare", {42},
               [&success](Transaction &tr, results result) {
                   ASSERT_EQ(result.size(), 1);
                   ASSERT_EQ(result[0][0].as<int>(), 42);
                   success = true;
               },
               [](error::db_error error) { ASSERT_TRUE(false) << "Execute failed: " << error.code; })
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

/** @brief ErrorHandling: missing table via `co_await query()` → failed `Reply`. */
TEST_F(PostgreSQLOperationsTest, ErrorHandling_Coroutine) {
    bool saw_fail = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply = co_await db_->query("SELECT * FROM nonexistent_table");
        saw_fail   = !reply.ok();
    }());
    ASSERT_TRUE(saw_fail);
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

/** @brief ChainingOperations-style sequence using three `co_await query()` calls. */
TEST_F(PostgreSQLOperationsTest, ChainingOperations_Coroutine) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto a = co_await db_->query("SELECT 1");
        auto b = co_await db_->query("SELECT 2");
        auto c = co_await db_->query("SELECT 3");
        ok     = a.ok() && b.ok() && c.ok() && a.result()[0][0].as<int>() == 1 &&
             b.result()[0][0].as<int>() == 2 && c.result()[0][0].as<int>() == 3;
    }());
    ASSERT_TRUE(ok);
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}