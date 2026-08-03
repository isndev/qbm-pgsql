/**
 * @file integration/prepared/prepared-statements.cpp
 * @brief Live-PostgreSQL integration tests for prepared-statement functionality.
 *
 * Exercises the full prepared-statement lifecycle against a live `postgres:5432`
 * (DSN from `QB_PG_DSN`, default `tcp://test:test@localhost:5432[test]`):
 *
 * - Parse/Bind/Execute round-trips with text, integer, boolean and NULL parameters.
 * - The success-callback-only and coroutine (`co_await`) overloads of `prepare`/`execute`.
 * - Wire-level guard rails (Parse rejects >32767 parameter types) and server-reported
 *   error paths (bind-arity → `08P01`, missing/DEALLOCATE'd statement → `26000`,
 *   nonexistent relation at execute time → `42P01`).
 * - `prepare_file` from a writable temp dir (callback, success-only, discard, and the
 *   throwing non-existent-file path).
 * - Statement reuse, batch prepare+execute, SELECTs (LIKE filter, empty result set),
 *   large result sets, and SQL-level transaction visibility (COMMIT / ROLLBACK).
 *
 * Down-daemon policy: derives from `qb::pg::test::PgIntegrationTest`, which connects
 * or `GTEST_SKIP()`s — it never hard-fails on an unreachable database.
 *
 * The pure-logic `PreparedStorage` LRU / `NameCache` unit tests live in
 * `unit/prepared/prepared-storage-lru.cpp`; the prepared-vs-adhoc timing comparison
 * lives in `benchmark/wire/prepared-throughput-bench.cpp`. This file is integration-only
 * and asserts correctness (rows / decoded values / exact SQLSTATE), never wall-clock time.
 *
 * @see qb::pg::detail::Transaction::prepare
 * @see qb::pg::detail::Transaction::execute
 * @see qb::pg::detail::PreparedQuery
 * @see qb::pg::detail::QueryParams (aliased as qb::pg::params)
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
#include <limits>
#include <string>
#include <vector>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/test_config.hpp"
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using namespace qb::pg::detail;

namespace {

/**
 * @brief Integration fixture for prepared-statement tests.
 *
 * Inherits the connect-or-skip contract from `qb::pg::test::PgIntegrationTest` (never
 * `ASSERT_TRUE(connect)`), then provisions a fresh `TEMP` working table. The TEMP table
 * is dropped with the session, so even a mid-test crash leaves no residue. We additionally
 * `DROP` it in `TearDown` for tests that recreate it with a different schema.
 */
class PreparedStatementsIntegration : public qb::pg::test::PgIntegrationTest {
protected:
    void
    SetUp() override {
        // Connect-or-skip (GTEST_SKIP in the base when the daemon is unreachable).
        PgIntegrationTest::SetUp();
        if (IsSkipped())
            return;

        auto status = db_->execute("CREATE TEMP TABLE test_prepared (id SERIAL PRIMARY KEY, value TEXT)", discard_query, discard_error).await();
        ASSERT_TRUE(status) << "Failed to create TEMP table test_prepared";
    }

    void
    TearDown() override {
        // The TEMP table dies with the session; this DROP keeps the (shared-lock) session
        // tidy between tests that recreate test_prepared with a different schema. Skip when
        // the daemon was unreachable — db_ exists but is not connected.
        if (db_ && !IsSkipped())
            (void) db_->execute("DROP TABLE IF EXISTS test_prepared", discard_query, discard_error).await();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Prepare lifecycle: basic, callback-only, coroutine
// ---------------------------------------------------------------------------

/**
 * @brief A basic prepared statement can be created successfully.
 */
TEST_F(PreparedStatementsIntegration, BasicPrepare) {
    auto status =
        db_->prepare("test_prepare", "INSERT INTO test_prepared (value) VALUES ($1)", type_oid_sequence{}, discard_prepare, discard_error)
            .await();
    ASSERT_TRUE(status);
}

/**
 * @brief A Parse with more parameter types than the int16 count field can hold must be
 *        rejected client-side, not desynchronize the wire stream.
 *
 * The Parse message encodes the parameter-type count as an int16 while writing every OID
 * entry. >32767 types would truncate the count but still emit all entries. `ParseQuery::is_valid()`
 * rejects it (the Parse-side twin of the Bind guard), so it surfaces as a clean error and the
 * connection stays usable.
 */
TEST_F(PreparedStatementsIntegration, PrepareRejectsTooManyParamTypes) {
    type_oid_sequence too_many(static_cast<std::size_t>(std::numeric_limits<smallint>::max()) + 1, oid::int4); // 32768 > 32767
    auto              status = db_->prepare("too_many_param_types", "SELECT 1", std::move(too_many), discard_prepare, discard_error).await();
    EXPECT_FALSE(status);

    // The connection must still be usable afterwards (it was never corrupted): a real
    // round-trip, not just a status bool.
    bool round_trip_ok = false;
    auto ok            = db_->execute(
                                "SELECT 1",
                                [&round_trip_ok](Transaction &, results r) {
                         ASSERT_EQ(r.size(), 1U);
                         EXPECT_EQ(r[0][0].as<int>(), 1);
                         round_trip_ok = true;
                                },
                                [](error::db_error const &e) { ADD_FAILURE() << "post-rejection SELECT 1 failed: " << e.what(); })
                             .await();
    EXPECT_TRUE(ok);
    EXPECT_TRUE(round_trip_ok);
}

/**
 * @brief The success-callback-only prepare overload (no on_error) instantiates and runs.
 *
 * Regression: the 4-arg overload forwarded its named `type_oid_sequence&&` parameter as an
 * lvalue into the 5-arg prepare, which was ill-formed — the API simply did not compile when
 * used. This forces its instantiation and verifies the prepared statement is usable.
 */
TEST_F(PreparedStatementsIntegration, PrepareSuccessCallbackOnly) {
    bool prepared = false;
    auto status   = db_->prepare("test_prepare_success_only", "INSERT INTO test_prepared (value) VALUES ($1)", type_oid_sequence{oid::text},
                                 [&prepared](Transaction &, PreparedQuery const &q) {
                                   EXPECT_EQ(q.name, "test_prepare_success_only");
                                   prepared = true;
                                 })
                        .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(prepared);

    // It must be executable.
    bool inserted = false;
    ASSERT_TRUE(db_->execute(
                       "test_prepare_success_only", params{std::string("cb_only")}, [&inserted](Transaction &, results) { inserted = true; },
                       [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                    .await());
    ASSERT_TRUE(inserted);
}

// ---------------------------------------------------------------------------
// Prepare + execute (callback, coroutine-verify, full-coroutine)
// ---------------------------------------------------------------------------

/**
 * @brief A prepared statement executed multiple times with different parameter values
 *        stores each row; verified with exact decoded values via the callback path.
 */
TEST_F(PreparedStatementsIntegration, PrepareAndExecute) {
    ASSERT_TRUE(db_->prepare("test_prepare", "INSERT INTO test_prepared (value) VALUES ($1)", type_oid_sequence{oid::text}, discard_prepare,
                             discard_error)
                    .await());
    ASSERT_TRUE(db_->execute("test_prepare", params{std::string("test1")}, discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("test_prepare", params{std::string("test2")}, discard_query, discard_error).await());

    bool verify_success = false;
    auto status         = db_->execute(
                                 "SELECT value FROM test_prepared ORDER BY id",
                                 [&verify_success](Transaction &, results result) {
                             ASSERT_EQ(result.size(), 2U);
                             EXPECT_EQ(result[0][0].as<std::string>(), "test1");
                             EXPECT_EQ(result[1][0].as<std::string>(), "test2");
                             verify_success = true;
                                 },
                                 [](error::db_error const &e) { ADD_FAILURE() << "Failed to verify data: " << e.what(); })
                              .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(verify_success);
}

/**
 * @brief Same inserts as PrepareAndExecute; row check via `co_await query()` (spawned task).
 *        Surfaces the reply error and asserts exact decoded values per row.
 */
TEST_F(PreparedStatementsIntegration, PrepareAndExecute_CoroutineVerify) {
    ASSERT_TRUE(db_->prepare("test_prepare_coro", "INSERT INTO test_prepared (value) VALUES ($1)", type_oid_sequence{oid::text},
                             discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->execute("test_prepare_coro", params{std::string("c1")}, discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("test_prepare_coro", params{std::string("c2")}, discard_query, discard_error).await());

    bool        ok = false;
    std::string err;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply = co_await db_->query("SELECT value FROM test_prepared ORDER BY id");
        if (!reply.ok()) {
            err = reply.error().what();
            co_return;
        }
        const auto &rs = reply.result();
        if (rs.size() != 2U) {
            err = "expected 2 rows, got " + std::to_string(rs.size());
            co_return;
        }
        ok = rs[0][0].as<std::string>() == "c1" && rs[1][0].as<std::string>() == "c2";
    }());
    ASSERT_TRUE(ok) << err;
}

/**
 * @brief `prepare` + `execute` + `query` entirely through the coroutine (no-callback) path.
 */
TEST_F(PreparedStatementsIntegration, PrepareCoroExecutePreparedCoro) {
    bool        ok = false;
    std::string err;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto pr = co_await db_->prepare("prep_coro_only", "INSERT INTO test_prepared (value) VALUES ($1)", type_oid_sequence{oid::text});
        if (!pr) {
            err = "prepare failed: " + std::string(pr.error().what());
            co_return;
        }
        auto ex = co_await db_->execute("prep_coro_only", params{std::string("coro_prep")});
        if (!ex) {
            err = "execute failed: " + std::string(ex.error().what());
            co_return;
        }
        auto q = co_await db_->query("SELECT value FROM test_prepared WHERE value = 'coro_prep'");
        if (!q.ok()) {
            err = "select failed: " + std::string(q.error().what());
            co_return;
        }
        if (q.result().size() != 1U) {
            err = "expected 1 row, got " + std::to_string(q.result().size());
            co_return;
        }
        ok = q.result()[0][0].as<std::string>() == "coro_prep";
    }());
    ASSERT_TRUE(ok) << err;
}

// ---------------------------------------------------------------------------
// Parameters: multiple, NULL, type variety
// ---------------------------------------------------------------------------

/**
 * @brief A prepared statement correctly binds multiple parameters in one execution.
 */
TEST_F(PreparedStatementsIntegration, MultipleParameters) {
    ASSERT_TRUE(db_->prepare("test_prepare_multi", "INSERT INTO test_prepared (value) VALUES ($1 || ' - ' || $2 || ' - ' || $3)",
                             type_oid_sequence{oid::text, oid::text, oid::text}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->execute("test_prepare_multi", params{std::string("string"), std::string("42"), std::string("3.14159")}, discard_query,
                             discard_error)
                    .await());

    bool verify_success = false;
    auto status         = db_->execute(
                                 "SELECT value FROM test_prepared ORDER BY id",
                                 [&verify_success](Transaction &, results result) {
                             ASSERT_EQ(result.size(), 1U);
                             EXPECT_EQ(result[0][0].as<std::string>(), "string - 42 - 3.14159");
                             verify_success = true;
                                 },
                                 [](error::db_error const &e) { ADD_FAILURE() << "Failed to verify data: " << e.what(); })
                              .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(verify_success);
}

/**
 * @brief A prepared statement handles NULL parameters and stores them as SQL NULL.
 */
TEST_F(PreparedStatementsIntegration, NullParameters) {
    // Recreate with a nullable integer column.
    ASSERT_TRUE(db_->execute("DROP TABLE IF EXISTS test_prepared", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE test_prepared (id SERIAL PRIMARY KEY, value TEXT, optional_value INTEGER)", discard_query,
                             discard_error)
                    .await());

    // Literal NULL in the SQL (no $2 bound).
    ASSERT_TRUE(db_->prepare("test_null_param", "INSERT INTO test_prepared (value, optional_value) VALUES ($1, NULL)",
                             type_oid_sequence{oid::text}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->execute(
                       "test_null_param", params{std::string("with_null")}, [](Transaction &, results) {},
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed with NULL value: " << e.what(); })
                    .await());

    // Bound non-NULL value.
    ASSERT_TRUE(db_->prepare("test_value_param", "INSERT INTO test_prepared (value, optional_value) VALUES ($1, $2)",
                             type_oid_sequence{oid::text, oid::int4}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->execute(
                       "test_value_param", params{std::string("with_value"), 42}, [](Transaction &, results) {},
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed with non-null insert: " << e.what(); })
                    .await());

    // NULL row decodes to is_null().
    bool null_verified = false;
    ASSERT_TRUE(db_->execute(
                       "SELECT value, optional_value FROM test_prepared WHERE value = 'with_null'",
                       [&null_verified](Transaction &, results result) {
                           ASSERT_EQ(result.size(), 1U);
                           EXPECT_EQ(result[0][0].as<std::string>(), "with_null");
                           EXPECT_TRUE(result[0][1].is_null());
                           null_verified = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to verify null data: " << e.what(); })
                    .await());
    ASSERT_TRUE(null_verified);

    // Non-NULL row decodes to its value.
    bool value_verified = false;
    ASSERT_TRUE(db_->execute(
                       "SELECT value, optional_value FROM test_prepared WHERE value = 'with_value'",
                       [&value_verified](Transaction &, results result) {
                           ASSERT_EQ(result.size(), 1U);
                           EXPECT_EQ(result[0][0].as<std::string>(), "with_value");
                           ASSERT_FALSE(result[0][1].is_null());
                           EXPECT_EQ(result[0][1].as<int>(), 42);
                           value_verified = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to verify non-null data: " << e.what(); })
                    .await());
    ASSERT_TRUE(value_verified);
}

/**
 * @brief A prepared statement binds integer, text and boolean parameters and they decode
 *        back to the exact C++ values.
 */
TEST_F(PreparedStatementsIntegration, VariousDataTypes) {
    ASSERT_TRUE(db_->execute("DROP TABLE IF EXISTS test_types", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE test_types (id SERIAL PRIMARY KEY, int_val INTEGER, text_val TEXT, bool_val BOOLEAN)",
                             discard_query, discard_error)
                    .await());

    ASSERT_TRUE(db_->prepare("test_types_insert", "INSERT INTO test_types (int_val, text_val, bool_val) VALUES ($1, $2, $3)",
                             type_oid_sequence{oid::int4, oid::text, oid::boolean}, discard_prepare, discard_error)
                    .await());

    ASSERT_TRUE(db_->execute(
                       "test_types_insert", params{42, std::string("text value"), true}, [](Transaction &, results) {},
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to insert data: " << e.what(); })
                    .await());

    bool verify_success = false;
    auto status         = db_->execute(
                                 "SELECT int_val, text_val, bool_val FROM test_types LIMIT 1",
                                 [&verify_success](Transaction &, results result) {
                             ASSERT_EQ(result.size(), 1U);
                             EXPECT_EQ(result[0][0].as<int>(), 42);
                             EXPECT_EQ(result[0][1].as<std::string>(), "text value");
                             EXPECT_EQ(result[0][2].as<bool>(), true);
                             verify_success = true;
                                 },
                                 [](error::db_error const &e) { ADD_FAILURE() << "Failed to verify data: " << e.what(); })
                              .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(verify_success);

    (void) db_->execute("DROP TABLE IF EXISTS test_types", discard_query, discard_error).await();
}

// ---------------------------------------------------------------------------
// Parameter-count behavior — assert the exact PostgreSQL bind-arity SQLSTATE
// ---------------------------------------------------------------------------

/**
 * @brief Binding the wrong number of parameters is a PostgreSQL protocol violation (08P01).
 *
 * The prepared statement requires exactly two parameters. Supplying one (too few) or three
 * (too many) must surface a server-reported error with SQLSTATE `08P01`
 * (`sqlstate::protocol_violation`) — "bind message supplies N parameters, but prepared
 * statement '...' requires M". (Fixes the original test, which only `std::cout`-logged the
 * outcome and asserted nothing.)
 */
TEST_F(PreparedStatementsIntegration, ParameterCountBehavior) {
    ASSERT_TRUE(db_->prepare("two_params", "INSERT INTO test_prepared (value) VALUES ($1 || ' - ' || $2)",
                             type_oid_sequence{oid::text, oid::text}, discard_prepare, discard_error)
                    .await());

    // Correct arity succeeds.
    bool success = false;
    ASSERT_TRUE(db_->execute(
                       "two_params", params{std::string("param1"), std::string("param2")},
                       [&success](Transaction &, results) { success = true; },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed with correct params: " << e.what(); })
                    .await());
    ASSERT_TRUE(success);

    // Too few parameters → 08P01.
    {
        sqlstate::code seen = sqlstate::unknown_code;
        std::string    msg;
        auto           status = db_->execute(
                                       "two_params", params{std::string("only one param")},
                                       [](Transaction &, results) { ADD_FAILURE() << "execute with too few params should not succeed"; },
                                       [&seen, &msg](error::db_error const &e) {
                                 seen = e.sqlstate;
                                 msg  = e.what();
                                       })
                                    .await();
        EXPECT_FALSE(status);
        EXPECT_EQ(seen, sqlstate::protocol_violation) << "too-few-params error: " << msg;
    }

    // Too many parameters → 08P01. The connection must still be usable, so this runs after
    // the previous failure on the same connection.
    {
        sqlstate::code seen = sqlstate::unknown_code;
        std::string    msg;
        auto           status = db_->execute(
                                       "two_params", params{std::string("param1"), std::string("param2"), std::string("extra")},
                                       [](Transaction &, results) { ADD_FAILURE() << "execute with too many params should not succeed"; },
                                       [&seen, &msg](error::db_error const &e) {
                                 seen = e.sqlstate;
                                 msg  = e.what();
                                       })
                                    .await();
        EXPECT_FALSE(status);
        EXPECT_EQ(seen, sqlstate::protocol_violation) << "too-many-params error: " << msg;
    }
}

// ---------------------------------------------------------------------------
// Error paths: nonexistent table at execute, missing / DEALLOCATE'd statement
// ---------------------------------------------------------------------------

/**
 * @brief Preparing against a nonexistent relation may defer validation, but executing it
 *        must fail with `42P01` (undefined_table).
 *
 * Ground-truthed against PostgreSQL 18: the relation is validated during the extended-protocol
 * `Parse` step, so the error is reported at PREPARE time (not deferred to execute) with SQLSTATE
 * `42P01`. The prepare therefore fails and the statement is never stored; we pin the SQLSTATE on
 * the prepare error path. (The previous version assumed deferred validation and asserted on the
 * execute, which only ever saw a client-side "unknown statement" error because the prepare had
 * already failed.)
 */
TEST_F(PreparedStatementsIntegration, PrepareNonexistentTable) {
    sqlstate::code prep_seen = sqlstate::unknown_code;
    std::string    prep_msg;
    auto           prep_status = db_->prepare("prep_nonexistent", "INSERT INTO nonexistent_relation_xyz (value) VALUES ($1)",
                                              type_oid_sequence{oid::text}, discard_prepare,
                                              [&prep_seen, &prep_msg](error::db_error const &e) {
                                        prep_seen = e.sqlstate;
                                        prep_msg  = e.what();
                                              })
                                     .await();
    EXPECT_FALSE(prep_status) << "PREPARE on a nonexistent table must fail at parse time";
    EXPECT_EQ(prep_seen, sqlstate::undefined_table) << "error: " << prep_msg;

    // The failed prepare stored nothing: executing the name is a client-side unknown-statement
    // error (no wire round-trip), distinct from the server-side 42P01 reported above.
    auto exec_status =
        db_->execute(
               "prep_nonexistent", params{std::string("test")},
               [](Transaction &, results) { ADD_FAILURE() << "execute against a never-stored statement must not succeed"; }, discard_error)
            .await();
    EXPECT_FALSE(exec_status);
}

/**
 * @brief Executing a statement name that was never prepared fails (client-side, not hang).
 *
 * The client rejects an unknown name before any wire traffic (`ExecuteQuery::is_valid()`),
 * so this is a client error rather than a server SQLSTATE. We assert the failure is observed
 * via the error callback and the call does not succeed.
 */
TEST_F(PreparedStatementsIntegration, NonExistentPreparedStatement) {
    bool error_detected = false;
    auto status         = db_->execute(
                                 "never_prepared_statement", params{std::string("value")},
                                 [](Transaction &, results) { ADD_FAILURE() << "Should not succeed with non-existent statement"; },
                                 [&error_detected](error::db_error const &) { error_detected = true; })
                              .await();
    ASSERT_FALSE(status);
    ASSERT_TRUE(error_detected);
}

/**
 * @brief After SQL DEALLOCATE, execute-by-name fails with `26000` until re-prepared.
 *
 * The name is still cached client-side (so the Bind is sent), but the backend has dropped it
 * → `invalid_sql_statement_name` (26000). The connection must remain usable for a subsequent
 * re-prepare + execute that decodes the new expression's result.
 */
TEST_F(PreparedStatementsIntegration, ExecuteAfterDeallocateFailsThenReprepareWorks) {
    constexpr char const *name = "p_qb_dealloc_session";

    ASSERT_TRUE(db_->prepare(name, "SELECT $1::int", type_oid_sequence{oid::int4}, discard_prepare, discard_error).await());

    bool first_ok = false;
    ASSERT_TRUE(db_->execute(
                       name, params{7},
                       [&first_ok](Transaction &, results r) {
                           ASSERT_EQ(r.size(), 1U);
                           EXPECT_EQ(r[0][0].as<int>(), 7);
                           first_ok = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                    .await());
    ASSERT_TRUE(first_ok);

    ASSERT_TRUE(db_->execute("DEALLOCATE " + std::string(name), discard_query, discard_error).await());

    sqlstate::code seen = sqlstate::unknown_code;
    std::string    msg;
    auto           status = db_->execute(
                                   name, params{1}, [](Transaction &, results) { ADD_FAILURE() << "execute after DEALLOCATE should not succeed"; },
                                   [&seen, &msg](error::db_error const &e) {
                             seen = e.sqlstate;
                             msg  = e.what();
                                   })
                                .await();
    EXPECT_FALSE(status);
    EXPECT_EQ(seen, sqlstate::invalid_sql_statement_name) << "error: " << msg;

    // Re-prepare under the same name with a different expression and confirm it runs.
    ASSERT_TRUE(db_->prepare(name, "SELECT $1::int + 1", type_oid_sequence{oid::int4}, discard_prepare, discard_error).await());
    bool second_ok = false;
    ASSERT_TRUE(db_->execute(
                       name, params{10},
                       [&second_ok](Transaction &, results r) {
                           ASSERT_EQ(r.size(), 1U);
                           EXPECT_EQ(r[0][0].as<int>(), 11);
                           second_ok = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                    .await());
    ASSERT_TRUE(second_ok);
}

// ---------------------------------------------------------------------------
// Statement reuse, prepared SELECT, batch, large result set
// ---------------------------------------------------------------------------

/**
 * @brief Two distinct prepared-statement names coexist and each inserts its own row.
 */
TEST_F(PreparedStatementsIntegration, StatementNameReuse) {
    ASSERT_TRUE(db_->execute("DELETE FROM test_prepared", discard_query, discard_error).await());

    ASSERT_TRUE(db_->prepare("test_stmt_reuse", "INSERT INTO test_prepared (value) VALUES ('first')", type_oid_sequence{}, discard_prepare,
                             discard_error)
                    .await());
    ASSERT_TRUE(db_->execute(
                       "test_stmt_reuse", params{}, [](Transaction &, results) {},
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to execute first: " << e.what(); })
                    .await());

    ASSERT_TRUE(db_->prepare("test_stmt_reuse_second", "INSERT INTO test_prepared (value) VALUES ('second')", type_oid_sequence{},
                             discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->execute(
                       "test_stmt_reuse_second", params{}, [](Transaction &, results) {},
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to execute second: " << e.what(); })
                    .await());

    bool verified = false;
    ASSERT_TRUE(db_->execute(
                       "SELECT value FROM test_prepared WHERE value IN ('first', 'second') ORDER BY value",
                       [&verified](Transaction &, results result) {
                           ASSERT_EQ(result.size(), 2U);
                           EXPECT_EQ(result[0][0].as<std::string>(), "first");
                           EXPECT_EQ(result[1][0].as<std::string>(), "second");
                           verified = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to verify data: " << e.what(); })
                    .await());
    ASSERT_TRUE(verified);
}

/**
 * @brief A prepared SELECT with a LIKE parameter returns the matching rows, and a
 *        non-matching parameter returns an empty result set.
 */
TEST_F(PreparedStatementsIntegration, PreparedSelect) {
    ASSERT_TRUE(db_->execute("INSERT INTO test_prepared (value) VALUES ('select_test_1')", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("INSERT INTO test_prepared (value) VALUES ('select_test_2')", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("INSERT INTO test_prepared (value) VALUES ('other_value')", discard_query, discard_error).await());

    ASSERT_TRUE(db_->prepare("test_select", "SELECT id, value FROM test_prepared WHERE value LIKE $1 ORDER BY id", type_oid_sequence{oid::text},
                             discard_prepare, discard_error)
                    .await());

    bool select_success = false;
    ASSERT_TRUE(db_->execute(
                       "test_select", params{std::string("select\\_test\\_%")},
                       [&select_success](Transaction &, results result) {
                           ASSERT_EQ(result.size(), 2U);
                           EXPECT_FALSE(result[0][0].is_null());
                           EXPECT_EQ(result[0][1].as<std::string>(), "select_test_1");
                           EXPECT_FALSE(result[1][0].is_null());
                           EXPECT_EQ(result[1][1].as<std::string>(), "select_test_2");
                           select_success = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to execute select: " << e.what(); })
                    .await());
    ASSERT_TRUE(select_success);

    bool empty_success = false;
    ASSERT_TRUE(db_->execute(
                       "test_select", params{std::string("nonexistent\\_%")},
                       [&empty_success](Transaction &, results result) {
                           EXPECT_EQ(result.size(), 0U);
                           empty_success = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to execute empty select: " << e.what(); })
                    .await());
    ASSERT_TRUE(empty_success);
}

/**
 * @brief Multiple statements can be prepared and executed back to back; all rows land.
 */
TEST_F(PreparedStatementsIntegration, BatchPrepareAndExecute) {
    ASSERT_TRUE(db_->prepare("batch_insert_1", "INSERT INTO test_prepared (value) VALUES ('batch_1')", type_oid_sequence{}, discard_prepare,
                             discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("batch_insert_2", "INSERT INTO test_prepared (value) VALUES ('batch_2')", type_oid_sequence{}, discard_prepare,
                             discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("batch_insert_3", "INSERT INTO test_prepared (value) VALUES ('batch_3')", type_oid_sequence{}, discard_prepare,
                             discard_error)
                    .await());

    ASSERT_TRUE(db_->execute("batch_insert_1", params{}, discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("batch_insert_2", params{}, discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("batch_insert_3", params{}, discard_query, discard_error).await());

    bool verified = false;
    ASSERT_TRUE(db_->execute(
                       "SELECT COUNT(*) FROM test_prepared WHERE value IN ('batch_1', 'batch_2', 'batch_3')",
                       [&verified](Transaction &, results result) {
                           ASSERT_EQ(result.size(), 1U);
                           EXPECT_EQ(result[0][0].as<int>(), 3);
                           verified = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to verify batch data: " << e.what(); })
                    .await());
    ASSERT_TRUE(verified);
}

/**
 * @brief A prepared SELECT returns a large result set with correct ordering and content.
 */
TEST_F(PreparedStatementsIntegration, LargeResultSet) {
    ASSERT_TRUE(db_->execute("DROP TABLE IF EXISTS test_large_results", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE test_large_results (id SERIAL PRIMARY KEY, value TEXT)", discard_query, discard_error).await());

    constexpr int num_rows = 100;
    for (int i = 0; i < num_rows; i += 10) {
        std::string batch = "INSERT INTO test_large_results (value) VALUES ";
        for (int j = 0; j < 10; ++j) {
            if (j > 0)
                batch += ", ";
            batch += "('large_row_" + std::to_string(i + j) + "')";
        }
        ASSERT_TRUE(db_->execute(batch, discard_query, discard_error).await());
    }

    ASSERT_TRUE(db_->prepare("select_large", "SELECT id, value FROM test_large_results ORDER BY id", type_oid_sequence{}, discard_prepare,
                             discard_error)
                    .await());

    bool large_success = false;
    ASSERT_TRUE(db_->execute(
                       "select_large", params{},
                       [&large_success](Transaction &, results result) {
                           ASSERT_EQ(result.size(), static_cast<std::size_t>(num_rows));
                           EXPECT_EQ(result[0][1].as<std::string>(), "large_row_0");
                           EXPECT_EQ(result[num_rows / 2][1].as<std::string>(), "large_row_" + std::to_string(num_rows / 2));
                           EXPECT_EQ(result[num_rows - 1][1].as<std::string>(), "large_row_" + std::to_string(num_rows - 1));
                           large_success = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to execute large select: " << e.what(); })
                    .await());
    ASSERT_TRUE(large_success);

    (void) db_->execute("DROP TABLE IF EXISTS test_large_results", discard_query, discard_error).await();
}

// ---------------------------------------------------------------------------
// Transaction visibility (SQL-level BEGIN/COMMIT/ROLLBACK around prepared inserts)
// ---------------------------------------------------------------------------

/**
 * @brief Prepared inserts inside an explicit SQL transaction respect COMMIT and ROLLBACK.
 */
TEST_F(PreparedStatementsIntegration, SqlTransactionBehavior) {
    ASSERT_TRUE(db_->execute("DELETE FROM test_prepared", discard_query, discard_error).await());

    // COMMIT path.
    ASSERT_TRUE(db_->execute("BEGIN", discard_query, discard_error).await());
    ASSERT_TRUE(
        db_->prepare("tx_insert", "INSERT INTO test_prepared (value) VALUES ($1)", type_oid_sequence{oid::text}, discard_prepare, discard_error)
            .await());
    ASSERT_TRUE(db_->execute("tx_insert", params{std::string("sql_transaction_test")}, discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("COMMIT", discard_query, discard_error).await());

    bool committed_visible = false;
    ASSERT_TRUE(db_->execute(
                       "SELECT value FROM test_prepared WHERE value = 'sql_transaction_test'",
                       [&committed_visible](Transaction &, results result) {
                           ASSERT_EQ(result.size(), 1U);
                           EXPECT_EQ(result[0][0].as<std::string>(), "sql_transaction_test");
                           committed_visible = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to verify committed data: " << e.what(); })
                    .await());
    ASSERT_TRUE(committed_visible);

    // ROLLBACK path.
    ASSERT_TRUE(db_->execute("BEGIN", discard_query, discard_error).await());
    ASSERT_TRUE(db_->prepare("tx_rollback_insert", "INSERT INTO test_prepared (value) VALUES ($1)", type_oid_sequence{oid::text},
                             discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->execute("tx_rollback_insert", params{std::string("should_be_rolled_back")}, discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("ROLLBACK", discard_query, discard_error).await());

    bool rollback_verified = false;
    ASSERT_TRUE(db_->execute(
                       "SELECT value FROM test_prepared WHERE value = 'should_be_rolled_back'",
                       [&rollback_verified](Transaction &, results result) {
                           EXPECT_EQ(result.size(), 0U);
                           rollback_verified = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to verify rolled back data: " << e.what(); })
                    .await());
    ASSERT_TRUE(rollback_verified);
}

// ---------------------------------------------------------------------------
// Parameter edge cases — fail loudly on a failed verify (no silent continue)
// ---------------------------------------------------------------------------

/**
 * @brief Prepared parameters survive a battery of edge-case text values intact.
 *
 * Covers empty strings, special/SQL-injection characters, long strings, and unicode. Every
 * round-trip must succeed AND decode back to the exact inserted value. A failed verify query
 * raises `ADD_FAILURE()` (the original swallowed it via `verify_success=false; continue;`).
 */
TEST_F(PreparedStatementsIntegration, ParameterTypeEdgeCases) {
    ASSERT_TRUE(db_->execute("DROP TABLE IF EXISTS test_param_types", discard_query, discard_error).await());
    ASSERT_TRUE(
        db_->execute("CREATE TEMP TABLE test_param_types (id SERIAL PRIMARY KEY, int_val INTEGER, text_val TEXT)", discard_query, discard_error)
            .await());

    ASSERT_TRUE(db_->prepare("insert_types", "INSERT INTO test_param_types (int_val, text_val) VALUES ($1, $2)",
                             type_oid_sequence{oid::int4, oid::text}, discard_prepare, discard_error)
                    .await());
    // Named execute() takes a prepared-statement name, so the verify query is prepared too.
    ASSERT_TRUE(db_->prepare("verify_by_int", "SELECT text_val FROM test_param_types WHERE int_val = $1", type_oid_sequence{oid::int4},
                             discard_prepare, discard_error)
                    .await());

    const std::vector<std::pair<int, std::string>> test_cases = {
        {-1000000, "Large negative value"},
        {1000000, "Large positive value"},
        {42, ""},                                                                       // empty string
        {43, "Special chars: !@#$%^&*(){}[]<>?/\\|'\"`~"},                              // special characters
        {44, std::string(1000, 'x')},                                                   // long string
        {45, "Unicode: \xC3\xA1\xC3\xA9\xC3\xAD\xC3\xB3\xC3\xBA\xC3\xB1 \xE2\x82\xAC"}, // unicode (UTF-8)
        {46, "'; DROP TABLE students; --"}                                              // SQL-injection attempt (must be inert)
    };

    for (const auto &test_case : test_cases) {
        ASSERT_TRUE(db_->execute(
                           "insert_types", params{test_case.first, test_case.second}, [](Transaction &, results) {},
                           [&test_case](error::db_error const &e) {
                               ADD_FAILURE() << "Failed to insert test case " << test_case.first << ": " << e.what();
                           })
                        .await())
            << "insert status false for test case " << test_case.first;
    }

    for (const auto &test_case : test_cases) {
        bool verify_success = false;
        auto verify_status  = db_->execute(
                                     "verify_by_int", params{test_case.first},
                                     [&verify_success, &test_case](Transaction &, results result) {
                                        ASSERT_EQ(result.size(), 1U);
                                        EXPECT_EQ(result[0][0].as<std::string>(), test_case.second);
                                        verify_success = true;
                                     },
                                     [&test_case](error::db_error const &e) {
                                        ADD_FAILURE() << "Failed to verify test case " << test_case.first << ": " << e.what();
                                     })
                                  .await();
        EXPECT_TRUE(verify_status) << "verify query status false for test case " << test_case.first;
        EXPECT_TRUE(verify_success) << "verify did not run for test case " << test_case.first;
    }

    (void) db_->execute("DROP TABLE IF EXISTS test_param_types", discard_query, discard_error).await();
}

// ---------------------------------------------------------------------------
// prepare_file — from a writable temp dir
// ---------------------------------------------------------------------------

/**
 * @brief prepare_file loads SQL from a file and the prepared statement executes correctly.
 *
 * Exercises all three prepare_file overloads (callback pair, success-only, discard) plus the
 * throwing non-existent-file path. SQL files are created under
 * `std::filesystem::temp_directory_path()` and removed afterward.
 */
TEST_F(PreparedStatementsIntegration, PrepareFromFile) {
    const std::filesystem::path temp_file  = std::filesystem::temp_directory_path() / "qbm_pgsql_test_query.sql";
    const std::filesystem::path temp_file2 = std::filesystem::temp_directory_path() / "qbm_pgsql_test_query2.sql";
    const std::filesystem::path temp_file3 = std::filesystem::temp_directory_path() / "qbm_pgsql_test_query3.sql";

    ASSERT_TRUE(db_->execute("DELETE FROM test_prepared", discard_query, discard_error).await());

    // 1) Callback pair overload — INSERT ... RETURNING.
    {
        std::ofstream sql_file(temp_file);
        ASSERT_TRUE(sql_file.is_open());
        sql_file << "-- This is a test SQL file for prepare_file\n"
                 << "INSERT INTO test_prepared (value) VALUES ($1)\n"
                 << "RETURNING id, value";
    }
    ASSERT_TRUE(std::filesystem::exists(temp_file));

    bool prepare_success = false;
    ASSERT_TRUE(db_->prepare_file(
                       "file_prepared_stmt", temp_file, {oid::text},
                       [&prepare_success](Transaction &, PreparedQuery const &query) {
                           EXPECT_EQ(query.name, "file_prepared_stmt");
                           EXPECT_NE(query.expression.find("INSERT INTO test_prepared"), std::string::npos);
                           prepare_success = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to prepare from file: " << e.what(); })
                    .await());
    ASSERT_TRUE(prepare_success);

    bool execute_success = false;
    ASSERT_TRUE(db_->execute(
                       "file_prepared_stmt", params{std::string("from_file_test")},
                       [&execute_success](Transaction &, results result) {
                           ASSERT_EQ(result.size(), 1U);
                           EXPECT_EQ(result[0][1].as<std::string>(), "from_file_test");
                           execute_success = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to execute file-prepared statement: " << e.what(); })
                    .await());
    ASSERT_TRUE(execute_success);

    // 2) Success-only overload.
    {
        std::ofstream sql_file(temp_file2);
        ASSERT_TRUE(sql_file.is_open());
        sql_file << "SELECT value FROM test_prepared WHERE value = $1";
    }
    bool prepare2_success = false;
    ASSERT_TRUE(db_->prepare_file("file_select_stmt", temp_file2, {oid::text},
                                  [&prepare2_success](Transaction &, PreparedQuery const &) { prepare2_success = true; })
                    .await());
    ASSERT_TRUE(prepare2_success);

    bool select_via_file = false;
    ASSERT_TRUE(db_->execute(
                       "file_select_stmt", params{std::string("from_file_test")},
                       [&select_via_file](Transaction &, results result) {
                           ASSERT_EQ(result.size(), 1U);
                           EXPECT_EQ(result[0][0].as<std::string>(), "from_file_test");
                           select_via_file = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to execute file_select_stmt: " << e.what(); })
                    .await());
    ASSERT_TRUE(select_via_file);

    // 3) Discard overload (fire-and-forget prepare + callback execute).
    {
        std::ofstream sql_file(temp_file3);
        ASSERT_TRUE(sql_file.is_open());
        sql_file << "SELECT COUNT(*) FROM test_prepared";
    }
    ASSERT_TRUE(db_->prepare_file("file_count_stmt", temp_file3, type_oid_sequence{}, discard_prepare, discard_error).await());

    bool count_success = false;
    ASSERT_TRUE(db_->execute(
                       "file_count_stmt", params{},
                       [&count_success](Transaction &, results result) {
                           ASSERT_EQ(result.size(), 1U);
                           EXPECT_GT(result[0][0].as<int>(), 0); // at least the row we inserted
                           count_success = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to execute file_count_stmt: " << e.what(); })
                    .await());
    ASSERT_TRUE(count_success);

    // 4) Non-existent file: prepare_file throws (and fires the error callback first).
    bool error_caught = false;
    try {
        db_->prepare_file(
            "nonexistent_file", std::filesystem::temp_directory_path() / "qbm_pgsql_nonexistent.sql", {oid::text},
            [](Transaction &, PreparedQuery const &) { ADD_FAILURE() << "Should not succeed with non-existent file"; },
            [&error_caught](error::db_error const &) { error_caught = true; });
        ADD_FAILURE() << "prepare_file should have thrown for a non-existent file";
    } catch (const error::db_error &) {
        // Expected.
    }
    EXPECT_TRUE(error_caught) << "Error callback should have fired before the exception was thrown";

    std::filesystem::remove(temp_file);
    std::filesystem::remove(temp_file2);
    std::filesystem::remove(temp_file3);
}

// ---------------------------------------------------------------------------
// Batched prepared execute inside one transaction (correctness only — no timing).
// Preserves the unique async-batch coverage from the retired AsyncPerformanceComparison;
// the prepared-vs-adhoc timing comparison lives in benchmark/wire/prepared-throughput-bench.cpp.
// ---------------------------------------------------------------------------

/**
 * @brief Many prepared inserts queued inside a single `begin` transaction all land.
 *
 * Queues `kBatch` prepared executes against one transaction, awaits once, and asserts the
 * exact committed row count — exercising the batched Bind/Execute pipeline without any
 * wall-clock assertion.
 */
TEST_F(PreparedStatementsIntegration, AsyncBatchedPreparedInsertsAllLand) {
    static constexpr int kBatch = 100;

    ASSERT_TRUE(db_->execute("DELETE FROM test_prepared", discard_query, discard_error).await());
    ASSERT_TRUE(db_->prepare("async_batch_insert", "INSERT INTO test_prepared (value) VALUES ($1)", type_oid_sequence{oid::text},
                             discard_prepare, discard_error)
                    .await());

    bool queued = false;
    auto status = db_->begin(
                         [&queued](Transaction &tr) {
                             for (int i = 0; i < kBatch; ++i) {
                                 tr.execute(
                                     "async_batch_insert", params{std::string("prepared_") + std::to_string(i)}, [](Transaction &, results) {},
                                     [i](error::db_error const &e) { ADD_FAILURE() << "Failed at " << i << ": " << e.what(); });
                             }
                             queued = true;
                         },
                         [](error::db_error const &e) { ADD_FAILURE() << "Transaction failed: " << e.what(); })
                      .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(queued);

    bool counted = false;
    ASSERT_TRUE(db_->execute(
                       "SELECT COUNT(*) FROM test_prepared",
                       [&counted](Transaction &, results result) {
                           ASSERT_EQ(result.size(), 1U);
                           EXPECT_EQ(result[0][0].as<int>(), kBatch);
                           counted = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << "Failed to count rows: " << e.what(); })
                    .await());
    ASSERT_TRUE(counted);
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
