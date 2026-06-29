/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file integration/api/database-api-extra.cpp
 * @brief Live integration tests for the still-uncovered Database / Transaction surface.
 *
 * The coroutine API (`coro-api.cpp`), the callback transactions (`transaction-*.cpp`) and
 * the error/SQLSTATE matrix (`errors-sqlstate.cpp`) already cover the happy + most error
 * paths. This file fills the remaining holes, asserting REAL database outcomes:
 *
 *  - Connection introspection accessors that no other test drives:
 *    `is_connection_alive()`, `server_version()`, `parameter_status()`, `backend_pid()`,
 *    `enable_keepalive()` (+ the `apply_keepalive_settings()` setsockopt path).
 *  - The fluent callback combinators `then()` / `success()` / `error()` (transaction.inl),
 *    untouched by any neighbour.
 *  - Callback-mode `begin`/`execute`/`commit`/`rollback` lifecycle with a decoded round-trip.
 *  - A syntax error mid-transaction puts the block into the failed (E) state; ROLLBACK clears
 *    it and a follow-up query succeeds (the failed-block recovery path).
 *  - `reconnect after a forced server error` (55P02-free re-handshake on a fresh backend).
 *
 * Tier: integration (REQUIRES live postgres). Uses the shared skip-not-fail fixture; when
 * the daemon is unreachable the binary is `GTEST_SKIP`-ped, never hard-failed.
 *
 * @ingroup Pgsql
 */
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/test_config.hpp"
#include "../pgsql.h"

using namespace qb::pg;
using namespace qb::pg::detail;
using qb::pg::test::dsn_tcp_string;

namespace {

constexpr std::string_view kExtraTable = "qb_pgsql_db_api_extra_t";

} // namespace

/**
 * @brief Connect-or-skip, then provision a per-TU work table (callback+await transport).
 */
class PgsqlDbApiExtra : public qb::pg::test::PgIntegrationTest {
protected:
    void
    SetUp() override {
        qb::pg::test::PgIntegrationTest::SetUp(); // connect-or-skip
        if (IsSkipped())
            return;
        ASSERT_TRUE(db_->execute(std::string("DROP TABLE IF EXISTS ") + std::string(kExtraTable), qb::pg::discard_query, qb::pg::discard_error)
                        .await());
        ASSERT_TRUE(db_->execute(std::string("CREATE TEMP TABLE ") + std::string(kExtraTable) + " (id SERIAL PRIMARY KEY, v TEXT NOT NULL)",
                                 qb::pg::discard_query, qb::pg::discard_error)
                        .await());
    }

    void
    TearDown() override {
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
    }
};

// --------------------------------------------------------------------------------------
// Connection introspection accessors (no other test drives these)
// --------------------------------------------------------------------------------------

// is_connection_alive() is true on a live socket and false after disconnect(); it reads the
// SO_ERROR socket state (the getsockopt path) and the is_connected_ flag.
TEST_F(PgsqlDbApiExtra, IsConnectionAliveTrueThenFalseAfterDisconnect) {
    EXPECT_TRUE(db_->is_connection_alive()) << "a freshly connected session must report alive";
    db_->disconnect();
    EXPECT_FALSE(db_->is_connection_alive()) << "a disconnected session must report not-alive";
}

// server_version() parses the server-reported ParameterStatus into a libpq-style integer
// (16.x -> 16xxxx). parameter_status("server_version") returns the raw string the int was
// derived from. Both must be consistent and the int must name a real, supported major.
TEST_F(PgsqlDbApiExtra, ServerVersionAndParameterStatus) {
    const int ver = db_->server_version();
    EXPECT_GE(ver, 90000) << "server_version() should be a libpq-style int >= 9.0.0 (90000)";

    auto raw = db_->parameter_status("server_version");
    ASSERT_TRUE(raw.has_value()) << "server_version ParameterStatus must have been reported at connect";
    EXPECT_FALSE(raw->empty());

    // The major encoded in the int must match the major printed in the raw string.
    const int major_from_int = ver / 10000;
    EXPECT_GT(major_from_int, 0);
    EXPECT_EQ(std::to_string(major_from_int), std::string(raw->substr(0, raw->find('.'))));

    // A never-reported key returns nullopt (the find()==end() branch).
    EXPECT_FALSE(db_->parameter_status("qb_no_such_parameter_zzz").has_value());

    // client_encoding is always reported by the backend.
    EXPECT_TRUE(db_->parameter_status("client_encoding").has_value());
}

// backend_pid() is captured from BackendKeyData and matches pg_backend_pid() observed
// server-side over the SAME connection.
TEST_F(PgsqlDbApiExtra, BackendPidMatchesServerSide) {
    const int captured = db_->backend_pid();
    ASSERT_GT(captured, 0);

    int server_side = -1;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await db_->query("SELECT pg_backend_pid()");
        if (r.ok() && r.result().size() == 1)
            server_side = r.result()[0][0].as<int>();
        co_return;
    }());
    EXPECT_EQ(captured, server_side) << "BackendKeyData PID must equal the server's pg_backend_pid()";
}

// enable_keepalive() on a connected session runs apply_keepalive_settings() (the live
// setsockopt path). It must not disturb the connection: a query still succeeds.
TEST_F(PgsqlDbApiExtra, EnableKeepaliveKeepsConnectionUsable) {
    db_->enable_keepalive(/*interval=*/5, /*idle=*/30, /*probes=*/2);
    EXPECT_TRUE(db_->is_connection_alive());

    int decoded = -1;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await db_->query("SELECT 7 AS n");
        if (r.ok() && r.result().size() == 1)
            decoded = r.result()[0][0].as<int>();
        co_return;
    }());
    EXPECT_EQ(decoded, 7) << "a keepalive-enabled connection must remain queryable";
}

// --------------------------------------------------------------------------------------
// Fluent callback combinators then() / success() / error() (transaction.inl)
// --------------------------------------------------------------------------------------

// then(): a follow-up success callback runs after the prior operation; the chain drains
// with await() and the row it inserted is observable.
TEST_F(PgsqlDbApiExtra, CallbackThenChainRunsAndCommits) {
    bool first_ran = false, then_ran = false;
    auto st = db_->execute(
                     std::string("INSERT INTO ") + std::string(kExtraTable) + " (v) VALUES ('then_a')",
                     [&](Transaction &, results) { first_ran = true; }, qb::pg::discard_error)
                  .then([&](Transaction &) { then_ran = true; })
                  .await();
    EXPECT_TRUE(static_cast<bool>(st));
    EXPECT_TRUE(first_ran);
    EXPECT_TRUE(then_ran) << "then() success callback must run after the preceding execute";

    int count = -1;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kExtraTable) + " WHERE v = 'then_a'");
        if (q.ok() && q.result().size() == 1)
            count = q.result()[0][0].as<int>();
        co_return;
    }());
    EXPECT_EQ(count, 1);
}

// success() registers a standalone success callback on the transaction; it fires once the
// queued work drains successfully.
TEST_F(PgsqlDbApiExtra, CallbackSuccessRunsOnDrain) {
    bool success_ran = false;
    auto st          = db_->execute(std::string("INSERT INTO ") + std::string(kExtraTable) + " (v) VALUES ('succ_b')", qb::pg::discard_query,
                                    qb::pg::discard_error)
                           .success([&](Transaction &) { success_ran = true; })
                           .await();
    EXPECT_TRUE(static_cast<bool>(st));
    EXPECT_TRUE(success_ran) << "success() callback must run when the batch drains OK";
}

// error(): a standalone error callback fires when an operation in the chain fails, and
// await() reports the failed status. The connection stays usable afterwards.
TEST_F(PgsqlDbApiExtra, CallbackErrorRunsOnFailure) {
    bool           error_ran = false;
    sqlstate::code seen      = sqlstate::unknown_code;
    auto           st        = db_->execute("THIS IS NOT VALID SQL", qb::pg::discard_query, qb::pg::discard_error)
                                   .error([&](error::db_error const &e) {
                      error_ran = true;
                      seen      = e.sqlstate;
                                   })
                                   .await();
    EXPECT_FALSE(static_cast<bool>(st)) << "await() must report failure for the invalid SQL";
    EXPECT_TRUE(error_ran) << "error() callback must fire on the failed operation";
    EXPECT_EQ(seen, sqlstate::syntax_error); // 42601

    int decoded = -1;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await db_->query("SELECT 1 AS one");
        if (r.ok() && r.result().size() == 1)
            decoded = r.result()[0][0].as<int>();
        co_return;
    }());
    EXPECT_EQ(decoded, 1) << "connection must survive a failed callback chain";
}

// --------------------------------------------------------------------------------------
// Callback-mode begin / execute / commit lifecycle (decoded round-trip)
// --------------------------------------------------------------------------------------

// A full callback transaction: begin -> execute INSERT (inside the begin success cb) ->
// commit, all via the callback transport, then verify the committed row coroutine-side.
TEST_F(PgsqlDbApiExtra, CallbackBeginExecuteCommitLifecycle) {
    bool insert_ran = false;
    auto st         = db_->begin(
                             [&](Transaction &t) {
                         t.execute(
                             std::string("INSERT INTO ") + std::string(kExtraTable) + " (v) VALUES ('cb_commit')",
                             [&](Transaction &, results) { insert_ran = true; }, qb::pg::discard_error);
                             },
                             [](error::db_error const &e) { ADD_FAILURE() << "begin failed: " << e.what(); })
                          .await();
    ASSERT_TRUE(static_cast<bool>(st));
    ASSERT_TRUE(insert_ran);

    int count = -1;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kExtraTable) + " WHERE v = 'cb_commit'");
        if (q.ok() && q.result().size() == 1)
            count = q.result()[0][0].as<int>();
        co_return;
    }());
    EXPECT_EQ(count, 1) << "the callback-committed row must be durable";
}

// --------------------------------------------------------------------------------------
// Error mid-transaction keeps the connection usable after ROLLBACK
// --------------------------------------------------------------------------------------

// Inside an explicit transaction a failing statement poisons the block (ReadyForQuery 'E');
// a manual ROLLBACK clears it and a follow-up query succeeds. Exercises the failed-block
// recovery + on_error_query propagation paths.
TEST_F(PgsqlDbApiExtra, ErrorInsideTransactionThenRollbackRecovers) {
    bool failed_block = false, recovered = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto b = co_await db_->begin();
        if (!b.ok())
            co_return;
        auto bad     = co_await db_->execute("SELECT * FROM qb_no_such_db_api_extra_table");
        failed_block = !bad.ok() && db_->in_transaction(); // 'E' failed-block state
        auto rb      = co_await db_->rollback();
        if (!rb.ok())
            co_return;
        auto good = co_await db_->query("SELECT 1 AS one");
        recovered = good.ok() && good.result().size() == 1 && good.result()[0][0].as<int>() == 1;
        co_return;
    }());
    EXPECT_TRUE(failed_block) << "a failed statement must put the block into the failed (E) state";
    EXPECT_TRUE(recovered) << "ROLLBACK must clear the failed block and restore usability";
}

// --------------------------------------------------------------------------------------
// Reconnect after a forced server error
// --------------------------------------------------------------------------------------

// A query that errors does NOT poison the connection (outside a transaction); but here we
// force a hard error, then disconnect + prepare_reconnect + connect onto a fresh backend and
// confirm the new session works. Covers the disconnect/prepare_reconnect/re-handshake cycle.
TEST_F(PgsqlDbApiExtra, ReconnectAfterForcedErrorLandsFreshBackend) {
    const int first_pid = db_->backend_pid();
    ASSERT_GT(first_pid, 0);

    // Force a server error (does not poison an idle connection).
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto bad = co_await db_->query("SELECT * FROM qb_no_such_reconnect_table_xyz");
        EXPECT_FALSE(bad.ok());
        co_return;
    }());

    db_->disconnect();
    db_->prepare_reconnect();
    ASSERT_TRUE(qb::io::async::run_sync(db_->connect(dsn_tcp_string()))) << "reconnect after a forced error failed";

    const int second_pid = db_->backend_pid();
    EXPECT_GT(second_pid, 0);
    EXPECT_NE(second_pid, first_pid) << "reconnect must land on a new backend process";

    int decoded = -1;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await db_->query("SELECT 99 AS n");
        if (r.ok() && r.result().size() == 1)
            decoded = r.result()[0][0].as<int>();
        co_return;
    }());
    EXPECT_EQ(decoded, 99) << "the reconnected session must be fully usable";
}

// --------------------------------------------------------------------------------------
// Query after disconnect() fails fast (does not hang) — REGRESSION
// --------------------------------------------------------------------------------------

// A query/execute/prepare submitted AFTER disconnect() must FAIL FAST with a connection
// error, never hang. Before the is_connection_usable() guard (+ disconnect() clearing
// is_connected_ synchronously), the command was enqueued on a dead connection and its
// coroutine awaiter never resolved — run_sync blocked forever. If this regresses, the
// run_sync calls below never return and the test times out.
TEST_F(PgsqlDbApiExtra, QueryAfterDisconnectFailsFastInsteadOfHanging) {
    ASSERT_TRUE(db_->is_connection_alive());
    db_->disconnect();
    ASSERT_FALSE(db_->is_connection_alive()) << "disconnect() must clear the connected state synchronously";

    bool resolved = false;
    bool ok       = true;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r   = co_await db_->query("SELECT 1"); // MUST resolve, not hang
        resolved = true;
        ok       = r.ok();
        co_return;
    }());
    EXPECT_TRUE(resolved) << "the awaiter must resolve on a disconnected handle (no hang)";
    EXPECT_FALSE(ok) << "a query after disconnect() must return a failed Reply";

    // execute() and prepare() on a dead handle fail fast the same way.
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto e = co_await db_->execute(std::string_view("SELECT 2"));
        EXPECT_FALSE(e.ok()) << "execute() after disconnect() must fail fast";
        auto p = co_await db_->prepare(std::string_view("p_dead"), std::string_view("SELECT $1::int"), type_oid_sequence{23});
        EXPECT_FALSE(p.ok()) << "prepare() after disconnect() must fail fast";
        // The prepared-name + params execute() coroutine overload guards on the
        // same not-connected predicate and must also fail fast (not hang).
        auto ep = co_await db_->execute(std::string_view("p_dead"), params{1});
        EXPECT_FALSE(ep.ok()) << "execute(name, params) after disconnect() must fail fast";
        co_return;
    }());
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
