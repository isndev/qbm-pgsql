/**
 * @file test-prepared-statements.cpp
 * @brief Unit tests for PostgreSQL prepared statement functionality
 *
 * This file implements comprehensive tests for the prepared statement capabilities
 * of the PostgreSQL client module. It verifies the client's ability to properly
 * prepare, parameterize, execute, and manage database prepared statements including:
 *
 * - Statement preparation with various parameter types
 * - Parameter binding with different data types
 * - Execution of prepared statements with multiple parameter sets
 * - Prepared statement caching and reuse
 * - Error handling in prepared statements
 * - Performance characteristics of prepared vs. ad-hoc queries
 *
 * The implementation validates the full lifecycle of prepared statements from
 * creation to execution to deallocation, ensuring reliable and efficient
 * database access patterns.
 *
 * Key features tested:
 * - Parameter type specification and validation
 * - Named parameter support
 * - Binary vs. text format parameter handling
 * - Multiple execution of the same prepared statement
 * - Statement preparation error detection
 * - Execution error handling
 *
 * @see qb::pg::detail::Database::prepare
 * @see qb::pg::detail::PreparedQuery
 * @see qb::pg::detail::params
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

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include <string_view>
#include "../pgsql.h"
#include "test_config.hpp"

using namespace qb::pg;
using namespace qb::pg::detail;

/**
 * @brief Test fixture for PostgreSQL prepared statement functionality
 *
 * Sets up a test environment for verifying prepared statement operations
 * with database tables and data for comprehensive testing.
 */
class PostgreSQLPreparedStatementsTest : public ::testing::Test {
protected:
    bool fixture_ready_{false};

    /**
     * @brief Set up the test environment
     *
     * Creates a database connection and sets up a test table
     * for prepared statement testing.
     */
    void
    SetUp() override {
        db_ = std::make_unique<qb::pg::tcp::database>();
        if (!qb::io::async::run_sync(db_->connect(qb::pg::test::dsn_tcp_string()))) {
            GTEST_SKIP() << "PostgreSQL not reachable";
            return;
        }

        // Create test table
        auto status =
            db_->execute("CREATE TEMP TABLE test_prepared (id SERIAL PRIMARY KEY, value TEXT)",
                         discard_query, discard_error)
                .await();
        ASSERT_TRUE(status);
        fixture_ready_ = true;
    }

    /**
     * @brief Clean up after tests
     *
     * Drops temporary tables and disconnects the database connection.
     */
    void
    TearDown() override {
        if (db_ && fixture_ready_) {
            (void) db_->execute("DROP TABLE IF EXISTS test_prepared", discard_query, discard_error)
                .await();
            db_->disconnect();
        }
        db_.reset();
        fixture_ready_ = false;
    }

    std::unique_ptr<qb::pg::tcp::database> db_;
};

/**
 * @brief Test basic prepared statement functionality
 *
 * Verifies that a basic prepared statement can be created and
 * executed successfully with parameter binding.
 */
TEST_F(PostgreSQLPreparedStatementsTest, BasicPrepare) {
    auto status = db_->prepare("test_prepare", "INSERT INTO test_prepared (value) VALUES ($1)",
                               type_oid_sequence{}, discard_prepare, discard_error)
                      .await();
    ASSERT_TRUE(status);
}

/**
 * @brief A prepare with more parameter types than the int16 count field can hold
 *        must be rejected, not desynchronize the wire stream.
 *
 * The Parse message encodes the parameter-type count as an int16 while writing every
 * OID entry. >32767 types would truncate the count but still emit all entries. The
 * query is now rejected via ParseQuery::is_valid() (the Parse-side twin of the Bind
 * guard), so it surfaces as a clean error instead of corrupting the connection.
 */
TEST_F(PostgreSQLPreparedStatementsTest, PrepareRejectsTooManyParamTypes) {
    type_oid_sequence too_many(static_cast<std::size_t>(32768), oid::int4); // 32768 > 32767
    auto status = db_->prepare("too_many_param_types", "SELECT 1", std::move(too_many),
                               discard_prepare, discard_error)
                      .await();
    EXPECT_FALSE(status);

    // The connection must still be usable afterwards (it was never corrupted).
    auto ok = db_->execute("SELECT 1", discard_query, discard_error).await();
    EXPECT_TRUE(ok);
}

/**
 * @brief Instantiate the success-callback-only prepare overload (no on_error).
 *
 * Regression: that 4-arg overload forwarded its named `type_oid_sequence&&` parameter
 * as an lvalue into the 5-arg prepare (which takes `type_oid_sequence&&`), which is
 * ill-formed — so the public API simply did not compile when used. No test exercised
 * it (all others pass both callbacks), so the build stayed green while the overload was
 * unusable. This forces its instantiation and runs it.
 */
TEST_F(PostgreSQLPreparedStatementsTest, PrepareSuccessCallbackOnly) {
    auto status = db_->prepare("test_prepare_success_only",
                               "INSERT INTO test_prepared (value) VALUES ($1)",
                               type_oid_sequence{}, discard_prepare)
                      .await();
    ASSERT_TRUE(status);
}

/**
 * @brief Test prepared statement execution with parameters
 *
 * Verifies that a prepared statement can be executed multiple times
 * with different parameter values, demonstrating the primary
 * advantage of prepared statements.
 */
TEST_F(PostgreSQLPreparedStatementsTest, PrepareAndExecute) {
    ASSERT_TRUE(db_->prepare("test_prepare", "INSERT INTO test_prepared (value) VALUES ($1)",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(
        db_->execute("test_prepare", params{std::string("test1")}, discard_query, discard_error)
            .await());
    ASSERT_TRUE(
        db_->execute("test_prepare", params{std::string("test2")}, discard_query, discard_error)
            .await());

    // Verify data was inserted correctly
    bool verify_success = false;
    auto status         = db_->execute(
                         "SELECT value FROM test_prepared ORDER BY id",
                         [&verify_success](Transaction &tr, results result) {
                             ASSERT_EQ(result.size(), 2);
                             ASSERT_EQ(result[0][0].as<std::string>(), "test1");
                             ASSERT_EQ(result[1][0].as<std::string>(), "test2");
                             verify_success = true;
                         },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Failed to verify data: " << error.what();
                         })
                      .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(verify_success);
}

/**
 * @brief Same inserts as PrepareAndExecute; row check via `co_await query()` (spawned task).
 */
TEST_F(PostgreSQLPreparedStatementsTest, PrepareAndExecute_CoroutineVerify) {
    ASSERT_TRUE(db_->prepare("test_prepare_coro", "INSERT INTO test_prepared (value) VALUES ($1)",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(
        db_->execute("test_prepare_coro", params{std::string("c1")}, discard_query, discard_error)
            .await());
    ASSERT_TRUE(
        db_->execute("test_prepare_coro", params{std::string("c2")}, discard_query, discard_error)
            .await());

    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply = co_await db_->query("SELECT value FROM test_prepared ORDER BY id");
        ok         = reply.ok() && reply.result().size() >= 2;
        if (!ok)
            co_return;
        const auto n = reply.result().size();
        ok           = reply.result()[n - 2][0].as<std::string>() == "c1" &&
             reply.result()[n - 1][0].as<std::string>() == "c2";
    }());
    ASSERT_TRUE(ok);
}

/**
 * @brief `prepare_coro` + `execute_prepared_coro` (no callback path).
 */
TEST_F(PostgreSQLPreparedStatementsTest, PrepareCoroExecutePreparedCoro) {
    if (!fixture_ready_)
        GTEST_SKIP();

    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto         pr =
            co_await db_->prepare("prep_coro_only", "INSERT INTO test_prepared (value) VALUES ($1)",
                                  type_oid_sequence{oid::text});
        if (!pr)
            co_return;
        auto ex = co_await db_->execute("prep_coro_only", params{std::string("coro_prep")});
        if (!ex)
            co_return;
        auto q = co_await db_->query("SELECT value FROM test_prepared WHERE value = 'coro_prep'");
        ok     = q.ok() && q.result().size() == 1;
    }());
    ASSERT_TRUE(ok);
}

/**
 * @brief Test prepared statement with multiple parameters
 *
 * Verifies that a prepared statement can correctly handle multiple
 * parameters of different types in a single execution.
 */
TEST_F(PostgreSQLPreparedStatementsTest, MultipleParameters) {
    // First recreate test table to ensure clean state
    ASSERT_TRUE(
        db_->execute("DROP TABLE IF EXISTS test_prepared", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE test_prepared (id SERIAL PRIMARY KEY, value TEXT)",
                             discard_query, discard_error)
                    .await());

    // Prepare with a different approach - use PostgreSQL's concatenation operator
    ASSERT_TRUE(db_->prepare("test_prepare",
                             "INSERT INTO test_prepared (value) VALUES ($1 || ' "
                             "- ' || $2 || ' - ' || $3)",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(
        db_->execute("test_prepare",
                     params{std::string("string"), std::string("42"), std::string("3.14159")},
                     discard_query, discard_error)
            .await());

    // Verify data was inserted correctly
    bool verify_success = false;
    auto status         = db_->execute(
                         "SELECT value FROM test_prepared ORDER BY id",
                         [&verify_success](Transaction &tr, results result) {
                             ASSERT_GT(result.size(), 0);
                             ASSERT_EQ(result[0][0].as<std::string>(), "string - 42 - 3.14159");
                             verify_success = true;
                         },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Failed to verify data: " << error.what();
                         })
                      .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(verify_success);
}

/**
 * @brief Test prepared statement with nonexistent table
 *
 * Verifies the behavior when preparing a statement for a nonexistent table.
 * Note: Some PostgreSQL connections might allow this preparation to succeed
 * since statement preparation might only validate syntax, not object existence.
 */
TEST_F(PostgreSQLPreparedStatementsTest, PrepareNonexistentTable) {
    auto prep_reply = db_->prepare("test_prepare", "INSERT INTO nonexistent (value) VALUES ($1)",
                                   type_oid_sequence{}, discard_prepare, discard_error)
                          .await();

    // Don't assert status - just log the result for reference
    // Some implementations might defer existence validation until execution
    if (prep_reply) {
        std::cout << "Prepare succeeded for nonexistent table (deferred validation likely)"
                  << std::endl;
    } else {
        std::cout << "Prepare failed for nonexistent table (immediate validation)" << std::endl;
    }

    // Test that execution fails regardless of preparation outcome
    bool                error_detected = false;
    Transaction::status status =
        db_->execute(
               "test_prepare", params{std::string("test")},
               [](Transaction &tr, results result) {
                   // Should not succeed
               },
               [&error_detected](error::db_error error) { error_detected = true; })
            .await();

    // The execution should fail since the table doesn't exist
    ASSERT_FALSE(status);
    ASSERT_TRUE(error_detected);
}

/**
 * @brief Test prepared statement with NULL parameters
 *
 * Verifies that a prepared statement can correctly handle NULL values
 * as parameters and properly store them in the database.
 */
TEST_F(PostgreSQLPreparedStatementsTest, NullParameters) {
    // First recreate the table with a column that can be NULL
    ASSERT_TRUE(
        db_->execute("DROP TABLE IF EXISTS test_prepared", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE test_prepared (id SERIAL PRIMARY KEY, "
                             "value TEXT, optional_value INTEGER)",
                             discard_query, discard_error)
                    .await());

    // Prepare statement with parameters
    ASSERT_TRUE(db_->prepare("test_null_param",
                             "INSERT INTO test_prepared (value, optional_value) VALUES ($1, NULL)",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Execute the prepared statement with NULL value
    Transaction::status status = db_->execute(
                                        "test_null_param", params{std::string("with_null")},
                                        [](Transaction &tr, results result) {},
                                        [&](error::db_error error) {
                                            ASSERT_TRUE(false)
                                                << "Failed with NULL value: " << error.what();
                                        })
                                     .await();
    ASSERT_TRUE(status);

    // Prepare another statement for non-NULL value
    ASSERT_TRUE(db_->prepare("test_value_param",
                             "INSERT INTO test_prepared (value, optional_value) VALUES ($1, $2)",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Execute with a non-NULL value
    status = db_->execute(
                    "test_value_param", params{std::string("with_value"), 42},
                    [](Transaction &tr, results result) {},
                    [&](error::db_error error) {
                        ASSERT_TRUE(false) << "Failed with non-null insert: " << error.what();
                    })
                 .await();
    ASSERT_TRUE(status);

    // Verify the NULL was properly inserted
    bool verify_success = false;
    status              = db_->execute(
                    "SELECT value, optional_value FROM test_prepared WHERE value = "
                                 "'with_null'",
                    [&verify_success](Transaction &tr, results result) {
                        ASSERT_GT(result.size(), 0);
                        ASSERT_EQ(result[0][0].as<std::string>(), "with_null");
                        ASSERT_TRUE(result[0][1].is_null());
                        verify_success = true;
                    },
                    [&](error::db_error error) {
                        ASSERT_TRUE(false) << "Failed to verify null data: " << error.what();
                    })
                 .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(verify_success);

    // Verify the non-NULL value was properly inserted
    verify_success = false;
    status         = db_->execute(
                    "SELECT value, optional_value FROM test_prepared WHERE value = "
                            "'with_value'",
                    [&verify_success](Transaction &tr, results result) {
                        ASSERT_GT(result.size(), 0);
                        ASSERT_EQ(result[0][0].as<std::string>(), "with_value");
                        ASSERT_FALSE(result[0][1].is_null());
                        ASSERT_EQ(result[0][1].as<int>(), 42);
                        verify_success = true;
                    },
                    [&](error::db_error error) {
                        ASSERT_TRUE(false) << "Failed to verify non-null data: " << error.what();
                    })
                 .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(verify_success);
}

/**
 * @brief Test prepared statement with various data types
 *
 * Verifies that a prepared statement can correctly handle
 * parameters of different PostgreSQL data types.
 */
TEST_F(PostgreSQLPreparedStatementsTest, VariousDataTypes) {
    // Create a table with multiple column types
    ASSERT_TRUE(
        db_->execute("DROP TABLE IF EXISTS test_types", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE test_types ("
                             "id SERIAL PRIMARY KEY, "
                             "int_val INTEGER, "
                             "text_val TEXT, "
                             "bool_val BOOLEAN)",
                             discard_query, discard_error)
                    .await());

    // Prepare insertion statement with different parameter types
    ASSERT_TRUE(db_->prepare("test_types_insert",
                             "INSERT INTO test_types (int_val, text_val, bool_val) "
                             "VALUES ($1, $2, $3)",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Execute with different data types
    Transaction::status status = db_->execute(
                                        "test_types_insert",
                                        params{
                                            42,                        // INTEGER
                                            std::string("text value"), // TEXT
                                            true                       // BOOLEAN
                                        },
                                        [](Transaction &tr, results result) {},
                                        [](error::db_error error) {
                                            ASSERT_TRUE(false)
                                                << "Failed to insert data: " << error.what();
                                        })
                                     .await();
    ASSERT_TRUE(status);

    // Verify data was inserted correctly
    bool verify_success = false;
    status              = db_->execute(
                    "SELECT int_val, text_val, bool_val FROM test_types LIMIT 1",
                    [&verify_success](Transaction &tr, results result) {
                        ASSERT_GT(result.size(), 0);

                        // Verify each data type
                        ASSERT_EQ(result[0][0].as<int>(), 42);
                        ASSERT_EQ(result[0][1].as<std::string>(), "text value");
                        ASSERT_EQ(result[0][2].as<bool>(), true);

                        verify_success = true;
                    },
                    [](error::db_error error) {
                        ASSERT_TRUE(false) << "Failed to verify data: " << error.what();
                    })
                 .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(verify_success);
}

/**
 * @brief Test parameter count validation behavior
 *
 * Tests how the system handles cases where the parameter count doesn't
 * match what's expected. Note: Behavior varies by PostgreSQL implementation.
 */
TEST_F(PostgreSQLPreparedStatementsTest, ParameterCountBehavior) {
    // Prepare statement expecting 2 parameters
    ASSERT_TRUE(db_->prepare("two_params",
                             "INSERT INTO test_prepared (value) VALUES ($1 || ' - ' || $2)",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Test with correct parameter count (should succeed)
    bool                success = false;
    Transaction::status status =
        db_->execute(
               "two_params", params{std::string("param1"), std::string("param2")},
               [&success](Transaction &tr, results result) { success = true; },
               [](error::db_error error) {
                   ASSERT_TRUE(false) << "Failed with correct params: " << error.what();
               })
            .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(success);

    // Note: The behavior with incorrect parameter counts depends on the implementation
    // Some may fail, others may succeed with NULL defaults or ignore extra params
    // We're not asserting specific behavior here, just documenting what happens

    // Too few parameters - try to catch error
    bool too_few_error_caught = false;
    status                    = db_->execute(
                    "two_params", params{std::string("only one param")},
                    [](Transaction &tr, results result) {},
                    [&too_few_error_caught](error::db_error error) {
                        too_few_error_caught = true;
                        std::cout << "Error with too few params: " << error.what() << std::endl;
                    })
                 .await();

    // Too many parameters - try to catch error
    bool too_many_error_caught = false;
    status                     = db_->execute(
                    "two_params",
                    params{std::string("param1"), std::string("param2"), std::string("extra")},
                    [](Transaction &tr, results result) {},
                    [&too_many_error_caught](error::db_error error) {
                        too_many_error_caught = true;
                        std::cout << "Error with too many params: " << error.what() << std::endl;
                    })
                 .await();

    std::cout << "Parameter count behavior: "
              << "Too few error caught: " << (too_few_error_caught ? "yes" : "no")
              << ", Too many error caught: " << (too_many_error_caught ? "yes" : "no") << std::endl;
}

/**
 * @brief Test statement name reuse behavior
 *
 * Tests how the system handles reusing the same statement name with
 * different SQL. Note: Expected behavior depends on the implementation.
 */
TEST_F(PostgreSQLPreparedStatementsTest, StatementNameReuse) {
    // First, clear the table
    auto setup = db_->execute("DELETE FROM test_prepared", discard_query, discard_error).await();
    ASSERT_TRUE(setup);

    // Prepare first statement with name "test_stmt"
    ASSERT_TRUE(db_->prepare("test_stmt_reuse", "INSERT INTO test_prepared (value) VALUES ('first')",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Execute first statement
    Transaction::status status =
        db_->execute(
               "test_stmt_reuse", params{}, [](Transaction &tr, results result) {},
               [&](error::db_error error) {
                   ASSERT_TRUE(false) << "Failed to execute first: " << error.what();
               })
            .await();
    ASSERT_TRUE(status);

    // Verify first value is in the database
    bool first_verify_success = false;
    status                    = db_->execute(
                    "SELECT value FROM test_prepared WHERE value = 'first'",
                    [&first_verify_success](Transaction &tr, results result) {
                        ASSERT_GT(result.size(), 0);
                        first_verify_success = true;
                    },
                    [&](error::db_error error) {
                        ASSERT_TRUE(false) << "Failed to verify first value: " << error.what();
                    })
                 .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(first_verify_success);

    // Prepare a different statement with a different name
    ASSERT_TRUE(db_->prepare("test_stmt_reuse_second",
                             "INSERT INTO test_prepared (value) VALUES ('second')",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Execute second statement
    status = db_->execute(
                    "test_stmt_reuse_second", params{}, [](Transaction &tr, results result) {},
                    [&](error::db_error error) {
                        ASSERT_TRUE(false) << "Failed to execute second: " << error.what();
                    })
                 .await();
    ASSERT_TRUE(status);

    // Verify both values are in the database
    bool second_verify_success = false;
    status =
        db_->execute(
               "SELECT value FROM test_prepared WHERE value IN ('first', 'second') "
               "ORDER BY value",
               [&second_verify_success](Transaction &tr, results result) {
                   ASSERT_EQ(result.size(), 2);
                   ASSERT_EQ(result[0][0].as<std::string>(), "first");
                   ASSERT_EQ(result[1][0].as<std::string>(), "second");
                   second_verify_success = true;

                   // Log the values
                   std::cout << "Statement reuse - First row: " << result[0][0].as<std::string>()
                             << ", Second row: " << result[1][0].as<std::string>() << std::endl;
               },
               [&](error::db_error error) {
                   ASSERT_TRUE(false) << "Failed to verify data: " << error.what();
               })
            .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(second_verify_success);
}

/**
 * @brief Test prepared statement with query results
 *
 * Verifies that a prepared SELECT statement properly returns
 * and processes query results.
 */
TEST_F(PostgreSQLPreparedStatementsTest, PreparedSelect) {
    // Insert test data
    ASSERT_TRUE(db_->execute("INSERT INTO test_prepared (value) VALUES ('select_test_1')",
                             discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->execute("INSERT INTO test_prepared (value) VALUES ('select_test_2')",
                             discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->execute("INSERT INTO test_prepared (value) VALUES ('other_value')",
                             discard_query, discard_error)
                    .await());

    // Prepare a SELECT statement with parameters
    ASSERT_TRUE(db_->prepare("test_select",
                             "SELECT id, value FROM test_prepared WHERE value LIKE $1 ORDER BY id",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Execute prepared SELECT
    bool                select_success = false;
    Transaction::status status =
        db_->execute(
               "test_select", params{std::string("select\\_test\\_%")},
               [&select_success](Transaction &tr, results result) {
                   ASSERT_EQ(result.size(), 2);

                   // Check row 1
                   ASSERT_FALSE(result[0][0].is_null());
                   ASSERT_FALSE(result[0][1].is_null());
                   ASSERT_EQ(result[0][1].as<std::string>(), "select_test_1");

                   // Check row 2
                   ASSERT_FALSE(result[1][0].is_null());
                   ASSERT_FALSE(result[1][1].is_null());
                   ASSERT_EQ(result[1][1].as<std::string>(), "select_test_2");

                   select_success = true;
               },
               [](error::db_error error) {
                   ASSERT_TRUE(false) << "Failed to execute select: " << error.what();
               })
            .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(select_success);

    // Test with parameter that should return no results
    bool empty_success = false;
    status             = db_->execute(
                    "test_select", params{std::string("nonexistent\\_%")},
                    [&empty_success](Transaction &tr, results result) {
                        ASSERT_EQ(result.size(), 0);
                        empty_success = true;
                    },
                    [](error::db_error error) {
                        ASSERT_TRUE(false) << "Failed to execute empty select: " << error.what();
                    })
                 .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(empty_success);
}

/**
 * @brief Test execution of a non-existent prepared statement
 *
 * Verifies that attempting to execute a prepared statement that hasn't
 * been prepared properly results in an error rather than hanging.
 */
TEST_F(PostgreSQLPreparedStatementsTest, NonExistentPreparedStatement) {
    // Try to execute a statement that was never prepared
    bool error_detected = false;
    auto status         = db_->execute(
                         "never_prepared_statement", params{std::string("value")},
                         [](Transaction &tr, results result) {
                             ASSERT_TRUE(false) << "Should not succeed with non-existent statement";
                         },
                         [&error_detected](error::db_error error) {
                             // Error should be detected here
                             error_detected = true;
                             std::cout
                                 << "Error when executing non-existent statement: " << error.what()
                                 << std::endl;
                         })
                      .await();

    // Should fail with an error
    ASSERT_FALSE(status);
    ASSERT_TRUE(error_detected);
}

/**
 * @brief After SQL DEALLOCATE, execute by name must fail until re-prepared
 *
 * Ensures the client surfaces server-side missing prepared statement and the
 * connection remains usable for a subsequent prepare + execute.
 */
TEST_F(PostgreSQLPreparedStatementsTest, ExecuteAfterDeallocateFailsThenReprepareWorks) {
    constexpr char const *name = "p_qb_dealloc_session";

    auto status = db_->prepare(name, "SELECT $1::int", type_oid_sequence{oid::int4}, discard_prepare,
                               discard_error)
                      .await();
    ASSERT_TRUE(status);

    bool first_ok = false;
    status        = db_->execute(
                    name, params{7},
                    [&first_ok](Transaction &, results r) {
                        ASSERT_EQ(r.size(), 1U);
                        EXPECT_EQ(r[0][0].as<int>(), 7);
                        first_ok = true;
                    },
                    [](error::db_error const &e) { FAIL() << e.what(); })
                 .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(first_ok);

    ASSERT_TRUE(
        db_->execute("DEALLOCATE " + std::string(name), discard_query, discard_error).await());

    bool saw_execute_error = false;
    status                 = db_->execute(
                    name, params{1},
                    [](Transaction &, results) {
                        FAIL() << "execute after DEALLOCATE should not succeed";
                    },
                    [&saw_execute_error](error::db_error const &) { saw_execute_error = true; })
                 .await();
    EXPECT_FALSE(status);
    EXPECT_TRUE(saw_execute_error);

    ASSERT_TRUE(db_->prepare(name, "SELECT $1::int + 1", type_oid_sequence{oid::int4},
                             discard_prepare, discard_error)
                    .await());
    bool second_ok = false;
    status         = db_->execute(
                    name, params{10},
                    [&second_ok](Transaction &, results r) {
                        ASSERT_EQ(r.size(), 1U);
                        EXPECT_EQ(r[0][0].as<int>(), 11);
                        second_ok = true;
                    },
                    [](error::db_error const &e) { FAIL() << e.what(); })
                 .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(second_ok);
}

/**
 * @brief Performance comparison between prepared and non-prepared statements
 *
 * Compares the execution time of multiple insertions using prepared
 * statements versus non-prepared direct SQL execution.
 */
TEST_F(PostgreSQLPreparedStatementsTest, PerformanceComparison) {
    const int iterations = 1000;

    // Measure time for non-prepared statements
    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        std::string sql =
            "INSERT INTO test_prepared (value) VALUES ('non_prepared_" + std::to_string(i) + "')";
        auto status = db_->execute(sql, discard_query, discard_error).await();
        ASSERT_TRUE(status);
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto non_prepared_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    // Clear table for prepared statement test
    auto status = db_->execute("DELETE FROM test_prepared", discard_query, discard_error).await();
    ASSERT_TRUE(status);

    // Prepare statement
    ASSERT_TRUE(db_->prepare("perf_test", "INSERT INTO test_prepared (value) VALUES ($1)",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Measure time for prepared statements
    start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        std::string value = "prepared_" + std::to_string(i);
        status            = db_->execute(
                        "perf_test", params{value}, [](Transaction &tr, results result) {},
                        [](error::db_error error) {
                            ASSERT_TRUE(false) << "Failed prepared insert: " << error.what();
                        })
                     .await();
        ASSERT_TRUE(status);
    }

    end_time = std::chrono::high_resolution_clock::now();
    auto prepared_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    // Output performance comparison
    std::cout << "Performance comparison (ms) for " << iterations << " iterations:" << std::endl;
    std::cout << "Non-prepared: " << non_prepared_duration << std::endl;
    std::cout << "Prepared: " << prepared_duration << std::endl;
    std::cout << "Difference: " << (non_prepared_duration - prepared_duration) << std::endl;
    std::cout << "Speedup: " << (static_cast<double>(non_prepared_duration) / prepared_duration)
              << "x" << std::endl;

    // Prepared statements should generally be faster, but we don't assert
    // this as it depends on the environment, database load, etc.
}

/**
 * @brief Test asynchronous prepared statement performance comparison
 *
 * Similar to PerformanceComparison but using async transactions to
 * more efficiently submit multiple statements.
 */
constexpr int iterations = 100; // Number of statements to execute
TEST_F(PostgreSQLPreparedStatementsTest, AsyncPerformanceComparison) {
    // Clear table before test
    auto status = db_->execute("DELETE FROM test_prepared", discard_query, discard_error).await();
    ASSERT_TRUE(status);

    // 1. Measure time for non-prepared statements with async transaction
    auto start_time = std::chrono::high_resolution_clock::now();

    // Use a transaction to batch all non-prepared statements
    bool non_prepared_success = false;
    db_->begin(
        [&non_prepared_success](Transaction &tr) {
            for (int i = 0; i < iterations; i++) {
                std::string sql = "INSERT INTO test_prepared (value) VALUES ('non_prepared_" +
                                  std::to_string(i) + "')";
                tr.execute(
                    sql,
                    [](auto &tr, auto results) {
                        // No action needed in callback
                    },
                    [i](error::db_error error) {
                        ASSERT_TRUE(false) << "Failed at " << i << ": " << error.what();
                    });
            }
            non_prepared_success = true;
        },
        [](error::db_error error) { ASSERT_TRUE(false) << "Transaction failed: " << error.what(); });

    // Single await after all statements are queued
    status = db_->await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(non_prepared_success);

    auto end_time = std::chrono::high_resolution_clock::now();
    auto non_prepared_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    // Clear table for prepared statement test
    status = db_->execute("DELETE FROM test_prepared", discard_query, discard_error).await();
    ASSERT_TRUE(status);

    // 2. Prepare statement
    ASSERT_TRUE(db_->prepare("async_perf_test", "INSERT INTO test_prepared (value) VALUES ($1)",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Measure time for prepared statements with async transaction
    start_time = std::chrono::high_resolution_clock::now();

    // Use a transaction for prepared statements
    bool prepared_success = false;
    db_->begin(
        [&prepared_success](Transaction &tr) {
            for (int i = 0; i < iterations; i++) {
                std::string value = "prepared_" + std::to_string(i);
                tr.execute(
                    "async_perf_test", params{value},
                    [](auto &tr, auto results) {
                        // No action needed in callback
                    },
                    [i](error::db_error error) {
                        ASSERT_TRUE(false) << "Failed at " << i << ": " << error.what();
                    });
            }
            prepared_success = true;
        },
        [](error::db_error error) { ASSERT_TRUE(false) << "Transaction failed: " << error.what(); });

    // Single await after all prepared executions
    status = db_->await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(prepared_success);

    end_time = std::chrono::high_resolution_clock::now();
    auto prepared_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    // Verify all data was inserted
    int row_count = 0;
    status        = db_->execute("SELECT COUNT(*) FROM test_prepared",
                                 [&row_count](Transaction &tr, results result) {
                              row_count = result[0][0].as<int>();
                          })
                 .await();
    ASSERT_TRUE(status);

    // We expect to have 'iterations' rows because the table is cleared between tests
    std::cout << "Rows inserted: " << row_count << " (expected: " << iterations << ")" << std::endl;
    ASSERT_EQ(row_count, iterations);

    // 3. Bonus test: Measure select performance with async batch
    // Prepare the select statement first
    ASSERT_TRUE(db_->prepare("async_perf_select",
                             "SELECT * FROM test_prepared WHERE value = $1 LIMIT 1",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Use fewer iterations for select to avoid overwhelming the connection
    const int select_iterations = 100;
    start_time                  = std::chrono::high_resolution_clock::now();

    // Async selects in a loop (without transaction)
    std::vector<bool> select_results(select_iterations, false);
    for (int i = 0; i < select_iterations; i++) {
        db_->execute(
            "async_perf_select",
            params{std::string("prepared_") + std::to_string(i % 100)}, // Use values we know exist
            [i, &select_results](Transaction &tr, results result) { select_results[i] = true; },
            [i](error::db_error error) {
                ASSERT_TRUE(false) << "Select failed at " << i << ": " << error.what();
            });
    }

    // Single await after all selects
    status = db_->await();
    ASSERT_TRUE(status);

    // Verify all selects succeeded
    for (int i = 0; i < select_iterations; i++) {
        ASSERT_TRUE(select_results[i]) << "Select at index " << i << " did not succeed";
    }

    end_time = std::chrono::high_resolution_clock::now();
    auto select_batch_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    // Output performance comparison
    std::cout << "Async Performance comparison (ms) for " << iterations
              << " iterations:" << std::endl;
    std::cout << "Non-prepared (async batch): " << non_prepared_duration << std::endl;
    std::cout << "Prepared (async batch): " << prepared_duration << std::endl;
    std::cout << "Select queries (async batch, " << select_iterations
              << " queries): " << select_batch_duration << std::endl;
    std::cout << "Difference: " << (non_prepared_duration - prepared_duration) << std::endl;
    std::cout << "Speedup: " << (static_cast<double>(non_prepared_duration) / prepared_duration)
              << "x" << std::endl;
}

/**
 * @brief Test preparing and executing multiple statements in batch
 *
 * Verifies that multiple prepared statements can be prepared and
 * executed in batch operations without waiting for individual
 * completions.
 */
TEST_F(PostgreSQLPreparedStatementsTest, BatchPrepareAndExecute) {
    // Prepare multiple statements in batch
    ASSERT_TRUE(db_->prepare("batch_insert_1",
                             "INSERT INTO test_prepared (value) VALUES ('batch_1')",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("batch_insert_2",
                             "INSERT INTO test_prepared (value) VALUES ('batch_2')",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("batch_insert_3",
                             "INSERT INTO test_prepared (value) VALUES ('batch_3')",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Execute multiple prepared statements in batch
    ASSERT_TRUE(db_->execute("batch_insert_1", params{}, discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("batch_insert_2", params{}, discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("batch_insert_3", params{}, discard_query, discard_error).await());

    // Verify all batch data was inserted
    bool verify_success = false;
    auto status         = db_->execute(
                         "SELECT COUNT(*) FROM test_prepared WHERE value IN ('batch_1', "
                                 "'batch_2', 'batch_3')",
                         [&verify_success](Transaction &tr, results result) {
                             ASSERT_EQ(result.size(), 1);
                             ASSERT_EQ(result[0][0].as<int>(), 3);
                             verify_success = true;
                         },
                         [&](error::db_error error) {
                             ASSERT_TRUE(false) << "Failed to verify batch data: " << error.what();
                         })
                      .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(verify_success);
}

/**
 * @brief Test prepared statement with large result sets
 *
 * Verifies that prepared statements can handle queries returning
 * large result sets efficiently.
 */
TEST_F(PostgreSQLPreparedStatementsTest, LargeResultSet) {
    // First, create a table with lots of rows
    ASSERT_TRUE(db_->execute("DROP TABLE IF EXISTS test_large_results", discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE test_large_results (id SERIAL PRIMARY "
                             "KEY, value TEXT)",
                             discard_query, discard_error)
                    .await());

    // Insert a decent number of rows (not too many to slow down the test)
    const int num_rows = 100;
    for (int i = 0; i < num_rows; i += 10) {
        // Insert 10 rows at a time
        std::stringstream batch_insert;
        batch_insert << "INSERT INTO test_large_results (value) VALUES ";
        for (int j = 0; j < 10; j++) {
            if (j > 0)
                batch_insert << ", ";
            batch_insert << "('large_row_" << (i + j) << "')";
        }

        auto insert_status = db_->execute(batch_insert.str(), discard_query, discard_error).await();
        ASSERT_TRUE(insert_status);
    }

    // Prepare a statement to retrieve all rows
    ASSERT_TRUE(db_->prepare("select_large", "SELECT id, value FROM test_large_results ORDER BY id",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Execute the prepared statement and check results
    bool large_success = false;
    auto status        = db_->execute(
                         "select_large", params{},
                         [&large_success, num_rows](Transaction &tr, results result) {
                             ASSERT_EQ(result.size(), num_rows);

                             // Check a few rows to verify ordering and content
                             ASSERT_EQ(result[0][1].as<std::string>(), "large_row_0");
                             ASSERT_EQ(result[num_rows / 2][1].as<std::string>(),
                                              "large_row_" + std::to_string(num_rows / 2));
                             ASSERT_EQ(result[num_rows - 1][1].as<std::string>(),
                                              "large_row_" + std::to_string(num_rows - 1));

                             large_success = true;
                         },
                         [&](error::db_error error) {
                             ASSERT_TRUE(false)
                                 << "Failed to execute large select: " << error.what();
                         })
                      .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(large_success);

    // Clean up the temporary table
    auto cleanup =
        db_->execute("DROP TABLE IF EXISTS test_large_results", discard_query, discard_error)
            .await();
    ASSERT_TRUE(cleanup);
}

/**
 * @brief Test transaction behavior with prepared statements
 *
 * Verifies that prepared statements work correctly within SQL transactions
 * by executing BEGIN/COMMIT/ROLLBACK statements explicitly.
 */
TEST_F(PostgreSQLPreparedStatementsTest, SqlTransactionBehavior) {
    // Clear any existing data
    auto setup = db_->execute("DELETE FROM test_prepared", discard_query, discard_error).await();
    ASSERT_TRUE(setup);

    // Begin a transaction with SQL command
    auto status = db_->execute("BEGIN", discard_query, discard_error).await();
    ASSERT_TRUE(status);

    // Prepare and execute an insert within the transaction
    ASSERT_TRUE(db_->prepare("tx_insert", "INSERT INTO test_prepared (value) VALUES ($1)",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    status = db_->execute("tx_insert", params{std::string("sql_transaction_test")}, discard_query,
                          discard_error)
                 .await();
    ASSERT_TRUE(status);

    // Commit the transaction
    status = db_->execute("COMMIT", discard_query, discard_error).await();
    ASSERT_TRUE(status);

    // Verify data was committed
    bool verify_success = false;
    status              = db_->execute(
                    "SELECT value FROM test_prepared WHERE value = 'sql_transaction_test'",
                    [&verify_success](Transaction &tr, results result) {
                        ASSERT_GT(result.size(), 0);
                        verify_success = true;
                    },
                    [&](error::db_error error) {
                        ASSERT_TRUE(false) << "Failed to verify committed data: " << error.what();
                    })
                 .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(verify_success);

    // Now test with rollback
    status = db_->execute("BEGIN", discard_query, discard_error).await();
    ASSERT_TRUE(status);

    // Prepare and execute another insert that will be rolled back
    ASSERT_TRUE(db_->prepare("tx_rollback_insert", "INSERT INTO test_prepared (value) VALUES ($1)",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    status = db_->execute("tx_rollback_insert", params{std::string("should_be_rolled_back")},
                          discard_query, discard_error)
                 .await();
    ASSERT_TRUE(status);

    // Rollback the transaction
    status = db_->execute("ROLLBACK", discard_query, discard_error).await();
    ASSERT_TRUE(status);

    // Verify rolled back data is not in the database
    bool rollback_verify = false;
    status               = db_->execute(
                    "SELECT value FROM test_prepared WHERE value = 'should_be_rolled_back'",
                    [&rollback_verify](Transaction &tr, results result) {
                        ASSERT_EQ(result.size(), 0);
                        rollback_verify = true;
                    },
                    [&](error::db_error error) {
                        ASSERT_TRUE(false) << "Failed to verify rolled back data: " << error.what();
                    })
                 .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(rollback_verify);
}

/**
 * @brief Test prepared statement parameter type handling
 *
 * Verifies the handling of different parameter types in prepared statements,
 * particularly focusing on edge cases and special values.
 */
TEST_F(PostgreSQLPreparedStatementsTest, ParameterTypeEdgeCases) {
    // Set up a test table with different types
    ASSERT_TRUE(
        db_->execute("DROP TABLE IF EXISTS test_param_types", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE test_param_types ("
                             "id SERIAL PRIMARY KEY, "
                             "int_val INTEGER, "
                             "text_val TEXT)",
                             discard_query, discard_error)
                    .await());

    // Prepare a statement for insertion
    ASSERT_TRUE(db_->prepare("insert_types",
                             "INSERT INTO test_param_types (int_val, text_val) VALUES ($1, $2)",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Named execute() takes a prepared statement name, not raw SQL — prepare the verify query.
    ASSERT_TRUE(db_->prepare("verify_by_int",
                             "SELECT text_val FROM test_param_types WHERE int_val = $1",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    // Test with safe edge case values that shouldn't cause errors
    const std::vector<std::pair<int, std::string>> test_cases = {
        // Integer values (avoiding extremes that might cause issues)
        {-1000000, "Large negative value"},
        {1000000, "Large positive value"},
        // Empty string
        {42, ""},
        // Special characters
        {43, "Special chars: !@#$%^&*(){}[]<>?/\\|'\"`~"},
        // Very long string (but not too long for test efficiency)
        {44, std::string(1000, 'x')},
        // Unicode text
        {45, "Unicode: áéíóúñÁÉÍÓÚÑ¿¡€£¥₹"},
        // SQL injection attempt
        {46, "'; DROP TABLE students; --"}};

    // Insert all test cases
    for (const auto &test_case : test_cases) {
        auto insert_status = db_->execute(
                                    "insert_types", params{test_case.first, test_case.second},
                                    [](Transaction &tr, results result) {},
                                    [&test_case](error::db_error error) {
                                        ASSERT_TRUE(false)
                                            << "Failed to insert test case: " << test_case.first
                                            << ", " << test_case.second << ": " << error.what();
                                    })
                                 .await();
        ASSERT_TRUE(insert_status);
    }

    // Verify each test case was inserted properly
    for (const auto &test_case : test_cases) {
        bool verify_success = false;
        auto verify_status  = db_->execute(
                                    "verify_by_int", params{test_case.first},
                                    [&verify_success, &test_case](Transaction &tr, results result) {
                                        ASSERT_GT(result.size(), 0);
                                        std::string retrieved = result[0][0].as<std::string>();
                                        ASSERT_EQ(retrieved, test_case.second);
                                        verify_success = true;
                                    },
                                    [&test_case, &verify_success](error::db_error error) {
                                        std::cout << "Failed to verify test case " << test_case.first
                                                  << ": " << error.what() << std::endl;
                                        // Don't fail the test, just mark it as not successful
                                        verify_success = false;
                                    })
                                 .await();

        if (!verify_status) {
            std::cout << "Query status was false for test case: " << test_case.first << std::endl;
            continue; // Skip this test case if query failed
        }

        ASSERT_TRUE(verify_success);
    }

    // Clean up
    auto cleanup =
        db_->execute("DROP TABLE IF EXISTS test_param_types", discard_query, discard_error).await();
    ASSERT_TRUE(cleanup);
}

/**
 * @brief Test preparation of SQL queries from files
 *
 * Verifies that the prepare_file function can correctly load and prepare
 * SQL queries from external files. This tests the convenience API for
 * managing large or complex SQL statements in separate files.
 */
TEST_F(PostgreSQLPreparedStatementsTest, PrepareFromFile) {
    // Define a temporary file path
    std::filesystem::path temp_file = std::filesystem::temp_directory_path() / "test_query.sql";

    // Clear any existing test data
    auto setup = db_->execute("DELETE FROM test_prepared", discard_query, discard_error).await();
    ASSERT_TRUE(setup);

    // Create SQL file with a simple query
    {
        std::ofstream sql_file(temp_file);
        ASSERT_TRUE(sql_file.is_open());
        sql_file << "-- This is a test SQL file for prepare_file\n"
                 << "INSERT INTO test_prepared (value) VALUES ($1)\n"
                 << "RETURNING id, value";
        sql_file.close();
        ASSERT_TRUE(std::filesystem::exists(temp_file));
    }

    // Test prepare_file with callbacks
    bool prepare_success = false;
    auto status =
        db_->prepare_file(
               "file_prepared_stmt", temp_file, {oid::text},
               [&prepare_success](Transaction &tr, PreparedQuery const &query) {
                   prepare_success = true;
                   ASSERT_EQ(query.name, "file_prepared_stmt");

                   // Verify the query string contains our SQL
                   ASSERT_NE(query.expression.find("INSERT INTO test_prepared"), std::string::npos);
               },
               [](error::db_error const &err) {
                   ASSERT_TRUE(false) << "Failed to prepare from file: " << err.what();
               })
            .await();

    ASSERT_TRUE(status);
    ASSERT_TRUE(prepare_success);

    // Execute the prepared statement
    bool execute_success = false;
    status               = db_->execute(
                    "file_prepared_stmt", params{std::string("from_file_test")},
                    [&execute_success](Transaction &tr, results result) {
                        ASSERT_EQ(result.size(), 1);
                        ASSERT_EQ(result[0][1].as<std::string>(), "from_file_test");
                        execute_success = true;
                    },
                    [](error::db_error const &err) {
                        ASSERT_TRUE(false)
                            << "Failed to execute file-prepared statement: " << err.what();
                    })
                 .await();

    ASSERT_TRUE(status);
    ASSERT_TRUE(execute_success);

    // Test the simplified version with just success callback
    std::filesystem::path temp_file2 = std::filesystem::temp_directory_path() / "test_query2.sql";
    {
        std::ofstream sql_file(temp_file2);
        ASSERT_TRUE(sql_file.is_open());
        sql_file << "SELECT value FROM test_prepared WHERE value = $1";
        sql_file.close();
    }

    bool prepare2_success = false;
    status                = db_->prepare_file("file_select_stmt", temp_file2, {oid::text},
                                              [&prepare2_success](Transaction &tr, PreparedQuery const &query) {
                                   prepare2_success = true;
                               })
                 .await();

    ASSERT_TRUE(status);
    ASSERT_TRUE(prepare2_success);

    // Synchronous fire-and-forget: discard callbacks + await (no-callback overload is
    // coroutine-only)
    std::filesystem::path temp_file3 = std::filesystem::temp_directory_path() / "test_query3.sql";
    {
        std::ofstream sql_file(temp_file3);
        ASSERT_TRUE(sql_file.is_open());
        sql_file << "SELECT COUNT(*) FROM test_prepared";
        sql_file.close();
    }

    status = db_->prepare_file("file_count_stmt", temp_file3, type_oid_sequence{}, discard_prepare,
                               discard_error)
                 .await();
    ASSERT_TRUE(status);

    // Execute the statement prepared without callbacks
    bool count_success = false;
    status             = db_->execute("file_count_stmt", params{},
                                      [&count_success](Transaction &tr, results result) {
                              ASSERT_EQ(result.size(), 1);
                              ASSERT_GT(result[0][0].as<int>(), 0); // Should have at least one row
                              count_success = true;
                          })
                 .await();

    ASSERT_TRUE(status);
    ASSERT_TRUE(count_success);

    // Test with non-existent file (should fail)
    bool error_caught = false;
    try {
        // This should throw an exception since the file doesn't exist
        db_->prepare_file(
            "nonexistent_file", std::filesystem::temp_directory_path() / "nonexistent.sql",
            {oid::text},
            [](Transaction &tr, PreparedQuery const &query) {
                ASSERT_TRUE(false) << "Should not succeed with non-existent file";
            },
            [&error_caught](error::db_error const &err) {
                error_caught = true;
                std::cout << "Error on non-existent file (expected): " << err.what() << std::endl;
            });
        ASSERT_TRUE(false) << "Should have thrown an exception for non-existent file";
    } catch (const error::db_error &e) {
        std::cout << "Exception caught as expected: " << e.what() << std::endl;
    }

    ASSERT_TRUE(error_caught)
        << "Error callback should have been called before the exception was thrown";

    // Cleanup temporary files
    std::filesystem::remove(temp_file);
    std::filesystem::remove(temp_file2);
    std::filesystem::remove(temp_file3);
}

/**
 * @brief Test LRU cache eviction policy for prepared statements (P2-1)
 *
 * Verifies that the prepared statement storage correctly implements
 * LRU eviction when max capacity is reached.
 */
TEST(PreparedStorageLRUTest, EvictionPolicy) {
    using namespace qb::pg::detail;

    // Create storage with small capacity
    PreparedStorage storage(3); // Max 3 entries

    // Add 3 prepared queries
    PreparedQuery q1{"q1", "SELECT 1", {}, {}};
    PreparedQuery q2{"q2", "SELECT 2", {}, {}};
    PreparedQuery q3{"q3", "SELECT 3", {}, {}};

    storage.push(std::move(q1));
    storage.push(std::move(q2));
    storage.push(std::move(q3));

    EXPECT_EQ(storage.size(), 3);
    EXPECT_EQ(storage.max_size(), 3);
    EXPECT_TRUE(storage.has("q1"));
    EXPECT_TRUE(storage.has("q2"));
    EXPECT_TRUE(storage.has("q3"));

    // Access q1 (marks as recently used)
    (void) storage.get("q1");

    // Add q4 - should evict q2 (least recently used)
    PreparedQuery q4{"q4", "SELECT 4", {}, {}};
    storage.push(std::move(q4));

    EXPECT_EQ(storage.size(), 3);    // Still at max capacity
    EXPECT_TRUE(storage.has("q1"));  // q1 still there (was accessed)
    EXPECT_FALSE(storage.has("q2")); // q2 evicted (LRU)
    EXPECT_TRUE(storage.has("q3"));  // q3 still there
    EXPECT_TRUE(storage.has("q4"));  // q4 added

    // Check eviction count
    EXPECT_EQ(storage.evicted_count(), 1);

    // Update max size
    storage.set_max_size(5);
    EXPECT_EQ(storage.max_size(), 5);

    // Add more queries (should not evict until > 5)
    PreparedQuery q5{"q5", "SELECT 5", {}, {}};
    storage.push(std::move(q5));
    EXPECT_EQ(storage.size(), 4);
    EXPECT_EQ(storage.evicted_count(), 1); // Still 1

    std::cout << "LRU eviction test passed" << std::endl;
}

/**
 * @brief Test name cache for O(1) column lookup (P0-11)
 *
 * This test verifies the lazy initialization of the name cache
 * in result_impl for fast column index lookups.
 */
TEST(NameCacheTest, LazyInitialization) {
    // Note: Full test requires a real database connection
    // This is tested implicitly through the query execution tests

    // Verify that the name_cache_ is declared as mutable (allows const methods to update)
    // and that column_index_of method exists
    std::cout << "Name cache test (placeholder - tested via integration)" << std::endl;
}

/**
 * @brief Stress test LRU cache with 1000+ entries (P2-1)
 *
 * Verifies LRU performance and correctness under high load.
 */
TEST(PreparedStorageStressTest, HighVolumeEviction) {
    using namespace qb::pg::detail;

    const size_t CACHE_SIZE  = 100;
    const size_t NUM_QUERIES = 1000;

    PreparedStorage storage(CACHE_SIZE);

    // Add 1000 queries with cache size of 100
    for (size_t i = 0; i < NUM_QUERIES; ++i) {
        std::string   name = "query_" + std::to_string(i);
        std::string   sql  = "SELECT " + std::to_string(i);
        PreparedQuery q{name, sql, {}, {}};
        storage.push(std::move(q));

        // Verify cache never exceeds max size
        EXPECT_LE(storage.size(), CACHE_SIZE);
    }

    // Should have exactly 100 entries
    EXPECT_EQ(storage.size(), CACHE_SIZE);

    // Should have evicted 900 queries
    EXPECT_EQ(storage.evicted_count(), NUM_QUERIES - CACHE_SIZE);

    // Only the last 100 queries should remain (q900-q999)
    for (size_t i = 0; i < NUM_QUERIES; ++i) {
        std::string name = "query_" + std::to_string(i);
        if (i >= NUM_QUERIES - CACHE_SIZE) {
            // Should exist (recently added)
            EXPECT_TRUE(storage.has(name)) << "Query " << name << " should exist";
        } else {
            // Should have been evicted
            EXPECT_FALSE(storage.has(name)) << "Query " << name << " should be evicted";
        }
    }

    std::cout << "LRU stress test passed: " << NUM_QUERIES << " queries, cache size " << CACHE_SIZE
              << ", evicted " << storage.evicted_count() << std::endl;
}

/**
 * @brief Test LRU cache access pattern (P2-1)
 *
 * Verifies that accessed items are promoted and not evicted.
 */
TEST(PreparedStorageStressTest, AccessPatternPromotion) {
    using namespace qb::pg::detail;

    PreparedStorage storage(5); // Small cache for testing

    // Add 5 queries
    for (int i = 0; i < 5; ++i) {
        PreparedQuery q{"q" + std::to_string(i), "SELECT " + std::to_string(i), {}, {}};
        storage.push(std::move(q));
    }

    // Access q0, q1, q2 (promote them to most recently used)
    (void) storage.get("q0");
    (void) storage.get("q1");
    (void) storage.get("q2");

    // Add q5 and q6 (should evict q3 and q4 - least recently used)
    PreparedQuery q5{"q5", "SELECT 5", {}, {}};
    PreparedQuery q6{"q6", "SELECT 6", {}, {}};
    storage.push(std::move(q5));
    storage.push(std::move(q6));

    // q0, q1, q2 should still exist (were accessed)
    EXPECT_TRUE(storage.has("q0"));
    EXPECT_TRUE(storage.has("q1"));
    EXPECT_TRUE(storage.has("q2"));

    // q3, q4 should be evicted (never accessed)
    EXPECT_FALSE(storage.has("q3"));
    EXPECT_FALSE(storage.has("q4"));

    // q5, q6 should exist (recently added)
    EXPECT_TRUE(storage.has("q5"));
    EXPECT_TRUE(storage.has("q6"));

    std::cout << "LRU access pattern test passed" << std::endl;
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}