/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file integration/errors/errors-sqlstate.cpp
 * @brief Live integration tests for PostgreSQL server-error propagation by SQLSTATE.
 *
 * Verifies that the client surfaces server errors with the correct structured
 * `db_error::sqlstate` enum (and the matching 5-char `code`) across the major SQLSTATE
 * classes — syntax, undefined-table/column, the constraint-violation family, numeric /
 * string data exceptions, division-by-zero, and the failed-transaction state. Both the
 * callback transport (`execute(sql, ok, err)` + `await()`) and the coroutine transport
 * (`co_await db_->query(...)` -> `Reply`) are exercised and both assert the EXACT
 * SQLSTATE, never a message-substring-OR-code disjunction.
 *
 * Every field of a server error reaches `db_error` (constraint, table, schema, column, data type,
 * position, internal query and position, context, hint, the non-localized severity, the server's
 * source location), and `on_notice` receives every notice (Huly QB-108).
 *
 * Tier: integration (REQUIRES live postgres), skip-not-fail via `PgIntegrationTest`.
 *
 * The connect-to-dead-host / DNS-resolve-failure case that used to live here belongs in
 * the network-timing tier and was moved to `system/connection/connect-timeout.cpp`.
 *
 * @ingroup Pgsql
 */
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
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

/**
 * @brief Error-handling fixture: connect-or-skip, then provision a constrained table.
 *
 * `test_errors` carries a NOT NULL column and a UNIQUE column so constraint violations can
 * be triggered deterministically. Built on the shared skip-not-fail base.
 */
class PostgreSQLErrorHandlingTest : public qb::pg::test::PgIntegrationTest {
protected:
    void
    SetUp() override {
        qb::pg::test::PgIntegrationTest::SetUp(); // connect-or-skip
        if (IsSkipped())
            return;
        (void) db_->execute("DROP TABLE IF EXISTS test_errors", discard_query, discard_error).await();
        ASSERT_TRUE(db_->execute("CREATE TABLE test_errors ("
                                 "  id SERIAL PRIMARY KEY,"
                                 "  value TEXT NOT NULL,"
                                 "  unique_value TEXT UNIQUE"
                                 ")",
                                 discard_query, discard_error)
                        .await());
        ASSERT_TRUE(
            db_->execute("INSERT INTO test_errors (value, unique_value) VALUES ('test1', 'unique1')", discard_query, discard_error).await());
    }

    void
    TearDown() override {
        // `&& !IsSkipped()`: when SetUp skipped because no PostgreSQL was reachable, `db_` is
        // still a live object that was never connected. Issuing a statement on it and .await()ing
        // the reply waits for a response that can never arrive — the binary spins its event loop
        // until ctest kills it (measured: 300.01 s wall, ~117 s CPU), instead of the ~20 s clean
        // skip every sibling suite reports. The same guard is already on the TearDowns of
        // transaction-advanced.cpp and prepared-statements.cpp, the other two integration suites
        // that issue a statement here; this one was the odd member out.
        if (db_ && !IsSkipped()) {
            (void) db_->execute("DROP TABLE IF EXISTS test_errors", discard_query, discard_error).await();
            db_->disconnect();
        }
        db_.reset();
    }
};

// Syntax error via the callback transport: the error callback fires with EXACTLY 42601.
TEST_F(PostgreSQLErrorHandlingTest, SyntaxError) {
    bool           error_caught = false;
    sqlstate::code state{};
    std::string    code;
    (void) db_
        ->execute(
            "INVALID SQL STATEMENT", [](Transaction &, results) { FAIL() << "Query should have failed"; },
            [&](error::db_error err) {
                error_caught = true;
                state        = err.sqlstate;
                code         = err.code;
            })
        .await();
    EXPECT_TRUE(error_caught);
    EXPECT_EQ(state, sqlstate::syntax_error);
    EXPECT_EQ(code, "42601");
}

// Syntax error via the coroutine transport: the failed Reply carries EXACTLY 42601.
TEST_F(PostgreSQLErrorHandlingTest, SyntaxError_Coroutine) {
    bool           failed = false;
    sqlstate::code state{};
    std::string    code;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply = co_await db_->query("INVALID SQL STATEMENT");
        failed     = !reply.ok();
        if (failed) {
            state = reply.error().sqlstate;
            code  = reply.error().code;
        }
    }());
    ASSERT_TRUE(failed);
    EXPECT_EQ(state, sqlstate::syntax_error);
    EXPECT_EQ(code, "42601");
}

// Undefined table via the callback transport: EXACTLY 42P01.
TEST_F(PostgreSQLErrorHandlingTest, TableNotFound) {
    bool           error_caught = false;
    sqlstate::code state{};
    std::string    code;
    (void) db_
        ->execute(
            "SELECT * FROM non_existent_table", [](Transaction &, results) { FAIL() << "Query should have failed"; },
            [&](error::db_error err) {
                error_caught = true;
                state        = err.sqlstate;
                code         = err.code;
            })
        .await();
    EXPECT_TRUE(error_caught);
    EXPECT_EQ(state, sqlstate::undefined_table);
    EXPECT_EQ(code, "42P01");
}

// Undefined table via the coroutine transport: EXACTLY 42P01 (was: only `!ok()`).
TEST_F(PostgreSQLErrorHandlingTest, TableNotFound_Coroutine) {
    bool           failed = false;
    sqlstate::code state{};
    std::string    code;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply = co_await db_->query("SELECT * FROM non_existent_table");
        failed     = !reply.ok();
        if (failed) {
            state = reply.error().sqlstate;
            code  = reply.error().code;
        }
    }());
    ASSERT_TRUE(failed);
    EXPECT_EQ(state, sqlstate::undefined_table);
    EXPECT_EQ(code, "42P01");
}

// Undefined column via the callback transport: EXACTLY 42703.
TEST_F(PostgreSQLErrorHandlingTest, ColumnNotFound) {
    bool           error_caught = false;
    sqlstate::code state{};
    std::string    code;
    (void) db_
        ->execute(
            "SELECT non_existent_column FROM test_errors", [](Transaction &, results) { FAIL() << "Query should have failed"; },
            [&](error::db_error err) {
                error_caught = true;
                state        = err.sqlstate;
                code         = err.code;
            })
        .await();
    EXPECT_TRUE(error_caught);
    EXPECT_EQ(state, sqlstate::undefined_column);
    EXPECT_EQ(code, "42703");
}

/**
 * @brief Typed SQLSTATE enum mapping across the constraint + data-exception families.
 *
 * Single canonical home for the constraint-violation matrix (the standalone
 * UniqueViolation / NotNullViolation callback tests were folded in here — D7). Each
 * statement auto-commits, so a failure rolls back its own implicit transaction and the
 * next statement starts clean. Every assertion pins the exact `sqlstate::code` enum AND,
 * for the non-ambiguous codes, the 5-char string.
 */
TEST_F(PostgreSQLErrorHandlingTest, SqlStateEnumMapping) {
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->query("CREATE TEMP TABLE sse_parent (id INT PRIMARY KEY)");
        (void) co_await db_->query("CREATE TEMP TABLE sse ("
                                   "  id INT UNIQUE NOT NULL,"
                                   "  pid INT REFERENCES sse_parent(id),"
                                   "  amount INT CHECK (amount >= 0))");
        (void) co_await db_->query("INSERT INTO sse_parent (id) VALUES (1)");
        (void) co_await db_->query("INSERT INTO sse (id, pid, amount) VALUES (10, 1, 5)");

        // 23505 unique_violation (duplicate id) — folds the old UniqueViolation test.
        auto uv = co_await db_->query("INSERT INTO sse (id, pid, amount) VALUES (10, 1, 5)");
        EXPECT_FALSE(uv.ok());
        EXPECT_EQ(uv.error().sqlstate, sqlstate::unique_violation);
        EXPECT_EQ(uv.error().code, "23505");

        // 23502 not_null_violation (NULL id) — folds the old NotNullViolation test.
        auto nn = co_await db_->query("INSERT INTO sse (id, pid, amount) VALUES (NULL, 1, 5)");
        EXPECT_FALSE(nn.ok());
        EXPECT_EQ(nn.error().sqlstate, sqlstate::not_null_violation);
        EXPECT_EQ(nn.error().code, "23502");

        // 23503 foreign_key_violation (no such parent).
        auto fk = co_await db_->query("INSERT INTO sse (id, pid, amount) VALUES (11, 999, 5)");
        EXPECT_FALSE(fk.ok());
        EXPECT_EQ(fk.error().sqlstate, sqlstate::foreign_key_violation);
        EXPECT_EQ(fk.error().code, "23503");

        // 23514 check_violation (amount < 0) — NEW.
        auto cv = co_await db_->query("INSERT INTO sse (id, pid, amount) VALUES (12, 1, -1)");
        EXPECT_FALSE(cv.ok());
        EXPECT_EQ(cv.error().sqlstate, sqlstate::check_violation);
        EXPECT_EQ(cv.error().code, "23514");

        // 42P01 undefined_table.
        auto ut = co_await db_->query("SELECT * FROM sse_no_such_table");
        EXPECT_FALSE(ut.ok());
        EXPECT_EQ(ut.error().sqlstate, sqlstate::undefined_table);
        EXPECT_EQ(ut.error().code, "42P01");

        // 42601 syntax_error.
        auto se = co_await db_->query("INVALID SQL");
        EXPECT_FALSE(se.ok());
        EXPECT_EQ(se.error().sqlstate, sqlstate::syntax_error);
        EXPECT_EQ(se.error().code, "42601");

        // 22012 division_by_zero.
        auto dz = co_await db_->query("SELECT 1 / 0");
        EXPECT_FALSE(dz.ok());
        EXPECT_EQ(dz.error().sqlstate, sqlstate::division_by_zero);
        EXPECT_EQ(dz.error().code, "22012");
        co_return;
    }());
}

/**
 * @brief Numeric + string data-exception SQLSTATEs (22xxx class) — NEW.
 *
 * - 22003 numeric_value_out_of_range: an int4 cast overflow.
 * - 22001 string_data_right_truncation: a value too long for a bounded varchar. Note the
 *   sqlstates table maps BOTH 01004 and 22001 onto `string_data_right_truncation`, so the
 *   enum is asserted alongside the unambiguous 5-char code.
 *
 * Ground-truthed against PostgreSQL 18: an *explicit cast* to a length-limited type
 * (`'abcdef'::varchar(3)`) silently TRUNCATES to `abc` with no error — 22001 is only raised on
 * an *assignment* (INSERT/UPDATE) into a bounded column. The truncation case therefore inserts
 * into a `varchar(3)` column rather than casting. (The previous cast-based version asserted an
 * error that PostgreSQL never raises.)
 */
TEST_F(PostgreSQLErrorHandlingTest, DataExceptionSqlStates) {
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE sse_trunc (v varchar(3))", discard_query, discard_error).await());
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto oor = co_await db_->query("SELECT 99999999999::int");
        EXPECT_FALSE(oor.ok());
        EXPECT_EQ(oor.error().sqlstate, sqlstate::numeric_value_out_of_range);
        EXPECT_EQ(oor.error().code, "22003");

        // Assignment into a bounded varchar raises 22001 (a bare cast would silently truncate).
        auto trunc = co_await db_->query("INSERT INTO sse_trunc (v) VALUES ('abcdef')");
        EXPECT_FALSE(trunc.ok());
        EXPECT_EQ(trunc.error().sqlstate, sqlstate::string_data_right_truncation);
        EXPECT_EQ(trunc.error().code, "22001");
        co_return;
    }());
}

/**
 * @brief 25P02 in_failed_sql_transaction — NEW.
 *
 * After a statement fails inside an explicit transaction block, the block enters the
 * aborted state; any subsequent statement (other than ROLLBACK) is rejected with 25P02
 * until the transaction is rolled back. The connection survives afterward.
 */
TEST_F(PostgreSQLErrorHandlingTest, InFailedTransactionRejectsFurtherStatements) {
    bool           survived = false;
    sqlstate::code state{};
    std::string    code;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->begin();
        auto bad = co_await db_->query("SELECT * FROM qb_no_such_table_25p02"); // 42P01, poisons the block
        EXPECT_FALSE(bad.ok());

        auto blocked = co_await db_->query("SELECT 1"); // rejected: aborted block
        EXPECT_FALSE(blocked.ok());
        state = blocked.error().sqlstate;
        code  = blocked.error().code;

        (void) co_await db_->rollback();
        auto good = co_await db_->query("SELECT 1");
        survived  = good.ok();
        co_return;
    }());
    EXPECT_EQ(state, sqlstate::in_failed_sql_transaction);
    EXPECT_EQ(code, "25P02");
    EXPECT_TRUE(survived) << "connection must be usable after ROLLBACK of the aborted block";
}

/**
 * @brief Prepared-statement bind-arity error pins an exact SQLSTATE.
 *
 * Preparing INSERT(...) with two text params then executing with one bound parameter is a
 * protocol-level bind error; PostgreSQL reports 08P01 (protocol_violation). Asserted as
 * the exact enum/code rather than a "parameter" message substring.
 */
TEST_F(PostgreSQLErrorHandlingTest, PreparedStatementParameterError) {
    ASSERT_TRUE(db_->prepare("test_prepare", "INSERT INTO test_errors (value, unique_value) VALUES ($1, $2)",
                             type_oid_sequence{oid::text, oid::text}, discard_prepare, discard_error)
                    .await());

    bool           error_caught = false;
    sqlstate::code state{};
    std::string    code;
    (void) db_
        ->execute(
            "test_prepare", {std::string("test_value")}, // missing second parameter
            [](Transaction &, results) { FAIL() << "Execute should have failed due to missing parameter"; },
            [&](error::db_error err) {
                error_caught = true;
                state        = err.sqlstate;
                code         = err.code;
            })
        .await();
    EXPECT_TRUE(error_caught);
    EXPECT_EQ(state, sqlstate::protocol_violation);
    EXPECT_EQ(code, "08P01");
}

// ---------------------------------------------------------------------------
// Every field of a server error, and the notices (Huly QB-108)
// ---------------------------------------------------------------------------

namespace errors_sqlstate_test {

/// The error a failing callback-form execute(sql) reports; false when it did not fail.
bool
execute_error(qb::pg::tcp::database &db, std::string const &sql, std::optional<error::db_error> &out) {
    (void) db.execute(sql, [](Transaction &, results) {}, [&out](error::db_error const &e) { out = e; }).await();
    return out.has_value();
}

/// What a notice handler saw, copied out of the call.
struct seen_notice {
    std::string severity;
    std::string severity_nonlocalized;
    std::string sqlstate;
    std::string message;
    std::string hint;
};

} // namespace errors_sqlstate_test

/**
 * @brief A unique violation names its constraint, table and schema -- the fields a handler of
 *        23505 needs, which the client used to drop -- and keeps every other field the server sent.
 */
TEST_F(PostgreSQLErrorHandlingTest, UniqueViolationCarriesTheConstraintTableAndSchema) {
    std::optional<error::db_error> err;
    ASSERT_TRUE(errors_sqlstate_test::execute_error(*db_, "INSERT INTO test_errors (value, unique_value) VALUES ('x', 'unique1')", err));
    EXPECT_EQ(err->sqlstate, sqlstate::unique_violation);
    ASSERT_NE(err->server_fields(), nullptr);
    EXPECT_EQ(err->constraint_name(), "test_errors_unique_value_key");
    EXPECT_EQ(err->table_name(), "test_errors");
    EXPECT_FALSE(err->schema_name().empty());
    EXPECT_EQ(err->severity_nonlocalized(), "ERROR");
    EXPECT_NE(err->detail.find("unique1"), std::string::npos) << err->detail;
    EXPECT_EQ(err->server_fields()->detail, err->detail);
    EXPECT_FALSE(err->file_name().empty()) << "the server always says where it raised the error";
    EXPECT_FALSE(err->line().empty());
    EXPECT_FALSE(err->routine().empty());

    // A copy shares the fields; it does not duplicate them.
    const error::db_error copy = *err;
    EXPECT_EQ(copy.server_fields(), err->server_fields());
}

/// A NOT NULL violation names its column -- through the coroutine Reply as well.
TEST_F(PostgreSQLErrorHandlingTest, NotNullViolationCarriesTheColumn_Coroutine) {
    std::optional<error::db_error> err;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply = co_await db_->query("INSERT INTO test_errors (value) VALUES (NULL)");
        if (!reply.ok())
            err = reply.error();
    }());
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->sqlstate, sqlstate::not_null_violation);
    EXPECT_EQ(err->column_name(), "value");
    EXPECT_EQ(err->table_name(), "test_errors");
}

/// A syntax error says where in the query; an undefined column says what the server suggests.
TEST_F(PostgreSQLErrorHandlingTest, SyntaxErrorCarriesThePositionAndUndefinedColumnTheHint) {
    std::optional<error::db_error> syntax;
    ASSERT_TRUE(errors_sqlstate_test::execute_error(*db_, "SELEC 1", syntax));
    EXPECT_EQ(syntax->sqlstate, sqlstate::syntax_error);
    EXPECT_EQ(syntax->position(), "1");

    std::optional<error::db_error> column;
    ASSERT_TRUE(errors_sqlstate_test::execute_error(*db_, "SELECT valuee FROM test_errors", column));
    EXPECT_EQ(column->sqlstate, sqlstate::undefined_column);
    EXPECT_NE(column->hint().find("value"), std::string_view::npos) << "hint: " << column->hint();
    EXPECT_EQ(column->position(), "8");
}

/**
 * @brief Every field a statement can set reaches the error: RAISE ... USING sets them all, the
 *        PL/pgSQL frame is the context, and a failing dynamic statement is the internal query.
 */
TEST_F(PostgreSQLErrorHandlingTest, EveryRaiseFieldReachesTheError) {
    std::optional<error::db_error> err;
    ASSERT_TRUE(errors_sqlstate_test::execute_error(
        *db_,
        "DO $$ BEGIN RAISE EXCEPTION 'boom' USING ERRCODE = 'P0001', DETAIL = 'the detail', HINT = 'the hint', "
        "COLUMN = 'col_x', CONSTRAINT = 'con_x', DATATYPE = 'type_x', TABLE = 'tab_x', SCHEMA = 'sch_x'; END $$",
        err));
    EXPECT_STREQ(err->what(), "boom");
    EXPECT_EQ(err->code, "P0001");
    EXPECT_EQ(err->detail, "the detail");
    EXPECT_EQ(err->hint(), "the hint");
    EXPECT_EQ(err->column_name(), "col_x");
    EXPECT_EQ(err->constraint_name(), "con_x");
    EXPECT_EQ(err->data_type_name(), "type_x");
    EXPECT_EQ(err->table_name(), "tab_x");
    EXPECT_EQ(err->schema_name(), "sch_x");
    EXPECT_NE(err->where().find("PL/pgSQL function"), std::string_view::npos) << "where: " << err->where();

    std::optional<error::db_error> dynamic;
    ASSERT_TRUE(errors_sqlstate_test::execute_error(*db_, "DO $$ BEGIN EXECUTE 'SELEC 1'; END $$", dynamic));
    EXPECT_EQ(dynamic->sqlstate, sqlstate::syntax_error);
    EXPECT_EQ(dynamic->internal_query(), "SELEC 1");
    EXPECT_EQ(dynamic->internal_position(), "1");
}

/// An error the client raised has no server fields: every accessor is empty.
TEST_F(PostgreSQLErrorHandlingTest, AClientErrorHasNoServerFields) {
    std::optional<error::db_error> err;
    (void) db_->execute(
                  "never_prepared_qb108", params{1}, [](Transaction &, results) {}, [&err](error::db_error const &e) { err = e; })
        .await();
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(err->server_fields(), nullptr);
    EXPECT_TRUE(err->hint().empty());
    EXPECT_TRUE(err->constraint_name().empty());
    EXPECT_TRUE(err->where().empty());
}

/**
 * @brief on_notice receives every notice with all its fields; without a handler a notice is only
 *        logged; a throwing handler is contained.
 */
TEST_F(PostgreSQLErrorHandlingTest, OnNoticeReceivesEveryNoticeWithItsFields) {
    using errors_sqlstate_test::seen_notice;
    std::vector<seen_notice> seen;
    db_->on_notice([&seen](qb::pg::notice const &n) { seen.push_back({n.severity, n.severity_nonlocalized, n.sqlstate, n.message, n.hint}); });
    ASSERT_TRUE(
        db_->execute("DO $$ BEGIN RAISE NOTICE 'note %', 42 USING HINT = 'nh'; RAISE WARNING 'warn'; END $$", discard_query, discard_error)
            .await());
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].severity_nonlocalized, "NOTICE");
    EXPECT_EQ(seen[0].sqlstate, "00000");
    EXPECT_EQ(seen[0].message, "note 42");
    EXPECT_EQ(seen[0].hint, "nh");
    EXPECT_EQ(seen[1].severity_nonlocalized, "WARNING");
    EXPECT_EQ(seen[1].sqlstate, "01000");
    EXPECT_EQ(seen[1].message, "warn");

    db_->on_notice({});
    ASSERT_TRUE(db_->execute("DO $$ BEGIN RAISE NOTICE 'unheard'; END $$", discard_query, discard_error).await());
    EXPECT_EQ(seen.size(), 2u) << "a removed handler hears nothing";

    int calls = 0;
    db_->on_notice([&calls](qb::pg::notice const &) {
        ++calls;
        throw std::runtime_error("intentional on_notice throw");
    });
    ASSERT_TRUE(db_->execute("DO $$ BEGIN RAISE NOTICE 'thrown at'; END $$", discard_query, discard_error).await());
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(db_->execute("SELECT 1", discard_query, discard_error).await()) << "the connection survives a throwing handler";
    db_->on_notice({});
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
