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
 * Tier: integration (REQUIRES live postgres), skip-not-fail via `PgIntegrationTest`.
 *
 * The connect-to-dead-host / DNS-resolve-failure case that used to live here belongs in
 * the network-timing tier and was moved to `system/connection/connect-timeout.cpp`.
 *
 * @ingroup Pgsql
 */
#include <gtest/gtest.h>
#include <string>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/test_config.hpp"
#include "../pgsql.h"

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
        if (db_) {
            (void) db_->execute("DROP TABLE IF EXISTS test_errors", discard_query, discard_error).await();
            db_->disconnect();
            db_.reset();
        }
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

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
