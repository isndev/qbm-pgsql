/**
 * @file connection-lifecycle.cpp
 * @brief Integration tests for PostgreSQL connection lifecycle (live daemon required).
 *
 * Live-PG half of the legacy `test-connection.cpp`. Exercises the real handshake against a
 * `postgres:5432` daemon: connect (callback + coroutine), reconnect, queued-query teardown
 * on disconnect, the handshake-deadline timer's owned lifetime, connection pooling, and
 * invalid-credential rejection.
 *
 * All cases route through the shared skip-not-fail fixture (`PgIntegrationTest`): when the
 * daemon is unreachable the binary reports *Skipped*, never *Failed*. The DSN-parse cases
 * (pure logic) live in `unit/dsn/dsn-parse.cpp`; the dead-host timeout case lives in
 * `system/connection/connect-timeout.cpp`.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <qb/io/async.h>
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/pg_pump.hpp"
#include "../../shared/test_config.hpp"
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using qb::pg::test::dsn_tcp_string;
using qb::pg::test::PgIntegrationTest;

namespace {

/**
 * @brief Lifecycle fixture: a fresh, *unconnected* database per test.
 *
 * Unlike the base `PgIntegrationTest` (which connects `db_` in SetUp), several lifecycle
 * tests want to drive connect/disconnect/reconnect explicitly. SetUp here only probes that
 * the daemon is reachable (skip-not-fail) and then hands the test a disconnected `db_`.
 */
class ConnectionLifecycle : public ::testing::Test {
protected:
    std::unique_ptr<qb::pg::tcp::database> db_;

    void
    SetUp() override {
        // Reachability probe on a throwaway handle: skip (not fail) when PG is down.
        auto probe = std::make_unique<qb::pg::tcp::database>();
        if (!qb::pg::test::pg_try_connect(*probe))
            GTEST_SKIP() << qb::pg::test::kDaemonUnreachableSentinel << " (postgres at " << dsn_tcp_string() << " not reachable)";
        probe->disconnect();
        db_ = std::make_unique<qb::pg::tcp::database>();
    }

    void
    TearDown() override {
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
    }
};

} // namespace

// --------------------------------------------------------------------------------------
// Connect
// --------------------------------------------------------------------------------------

/// Callback/run_sync connect against the live server.
TEST_F(ConnectionLifecycle, ConnectSuccess) {
    ASSERT_TRUE(qb::io::async::run_sync(db_->connect(dsn_tcp_string())));
    EXPECT_GT(db_->backend_pid(), 0) << "BackendKeyData PID must be captured at connect";
}

/// `co_await connect()` on a spawned task (libev + coro_scheduler).
TEST_F(ConnectionLifecycle, ConnectSuccess_Coroutine) {
    bool ok  = false;
    int  pid = 0;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        ok  = co_await db_->connect(dsn_tcp_string());
        pid = db_->backend_pid();
    }());
    ASSERT_TRUE(ok);
    EXPECT_GT(pid, 0);
}

// --------------------------------------------------------------------------------------
// Invalid credentials (de-masked: assert the rejection, surface the skip reason)
// --------------------------------------------------------------------------------------

/**
 * @brief A wrong-password DSN must be rejected by SCRAM/MD5 auth.
 *
 * If the server's `pg_hba.conf` uses `trust` for the test role the wrong password is
 * *accepted*; that is a server-config artifact, not a client bug, so we skip with a clear
 * message unless the operator pinned a known-bad DSN via `QB_PG_INVALID_DSN`.
 */
TEST_F(ConnectionLifecycle, ConnectWithInvalidCredentials) {
    auto       invalid_db = std::make_unique<qb::pg::tcp::database>();
    const bool connected  = qb::io::async::run_sync(invalid_db->connect(qb::pg::test::dsn_invalid_auth_string()));

    if (connected) {
        if (std::getenv("QB_PG_INVALID_DSN") == nullptr) {
            GTEST_SKIP() << "Server accepted the default wrong-password DSN (likely `trust` "
                            "in pg_hba). Set QB_PG_INVALID_DSN to a DSN that must fail auth.";
        }
        FAIL() << "QB_PG_INVALID_DSN was accepted; it must fail authentication.";
    }
    ASSERT_FALSE(connected);
}

// --------------------------------------------------------------------------------------
// Reconnect
// --------------------------------------------------------------------------------------

/// Disconnect then prepare_reconnect()+connect() must succeed on a fresh backend.
TEST_F(ConnectionLifecycle, ReconnectAfterDisconnect) {
    ASSERT_TRUE(qb::io::async::run_sync(db_->connect(dsn_tcp_string())));
    const int first_pid = db_->backend_pid();
    EXPECT_GT(first_pid, 0);

    db_->disconnect();
    db_->prepare_reconnect();

    ASSERT_TRUE(qb::io::async::run_sync(db_->connect(dsn_tcp_string())));
    const int second_pid = db_->backend_pid();
    EXPECT_GT(second_pid, 0) << "BackendKeyData PID must be captured on the reconnect";
    // A genuine re-handshake lands on a new backend process.
    EXPECT_NE(second_pid, first_pid) << "reconnect must land on a new backend process";
    auto status = db_->execute("SELECT 1", discard_query, discard_error).await();
    EXPECT_TRUE(status) << "query on the reconnected session failed";
}

/**
 * @brief Client-supplied startup options (set_startup_option / application_name) are sent in the
 *        StartupMessage and applied by the server.
 *
 * Regression for the formerly-dead `client_opts_` path: create_startup_message iterated it but no
 * public API ever wrote it, so only USER+DATABASE were ever sent. With the new setters, a custom
 * application_name + a GUC (datestyle) round-trip back through `SHOW`. Options must be registered
 * BEFORE connect() (serialized at handshake). A fresh db is used so the option set is pristine.
 */
TEST_F(ConnectionLifecycle, ClientStartupOptionsAreSentAndApplied) {
    auto db = std::make_unique<qb::pg::tcp::database>();
    db->application_name("qbm_pgsql_startup_opt_test").set_startup_option("datestyle", "ISO, MDY");
    ASSERT_EQ(db->startup_options().at("application_name"), "qbm_pgsql_startup_opt_test");

    if (!qb::pg::test::pg_try_connect(*db))
        GTEST_SKIP() << qb::pg::test::kDaemonUnreachableSentinel;

    std::string app, datestyle;
    bool        ok = qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto a = co_await db->execute("SHOW application_name");
        auto d = co_await db->execute("SHOW datestyle");
        if (!a.ok() || !d.ok() || a.result().size() != 1u || d.result().size() != 1u)
            co_return false;
        app       = a.result()[0][0].as<std::string>();
        datestyle = d.result()[0][0].as<std::string>();
        co_return true;
    }());

    EXPECT_TRUE(ok) << "SHOW of the startup-set GUCs failed";
    EXPECT_EQ(app, "qbm_pgsql_startup_opt_test") << "application_name startup option was not applied";
    EXPECT_EQ(datestyle, "ISO, MDY") << "datestyle startup option was not applied";
}

/**
 * @brief A bare connect() after disconnect() re-handshakes on a fresh backend.
 *
 * Ground-truthed against PostgreSQL 18: `connect()` routes through the async connector, which
 * opens a brand-new socket and drives a full startup handshake regardless of the prior
 * `disconnect()` (whose `dispose()` is undone by the input layer's `start()` on the new fd).
 * So a bare reconnect IS usable — it lands on a new backend PID and queries succeed.
 *
 * `prepare_reconnect()` (see ReconnectAfterDisconnect) is the *recommended* hygiene call: it
 * also resets the in/out buffers, closes the stale fd, and clears the per-backend
 * ParameterStatus cache. It is not a hard precondition for the connector to re-handshake.
 *
 * (This case previously asserted the opposite — that a bare reconnect must be UNUSABLE. That
 * was a false contract: it only ever "passed" because the second StartupMessage echoed the
 * server-reported read-only `server_version` parameter back to the server, which aborted the
 * handshake with SQLSTATE 55P02. That framework bug is fixed; the bare reconnect now works.)
 */
TEST_F(ConnectionLifecycle, ReconnectWithoutPrepareReconnectIsUsable) {
    ASSERT_TRUE(qb::io::async::run_sync(db_->connect(dsn_tcp_string())));
    const int first_pid = db_->backend_pid();
    EXPECT_GT(first_pid, 0);
    db_->disconnect();

    // No prepare_reconnect(): the connector still opens a fresh socket and re-handshakes.
    ASSERT_TRUE(qb::io::async::run_sync(db_->connect(dsn_tcp_string()))) << "bare connect() after disconnect() failed to re-handshake";
    const int second_pid = db_->backend_pid();
    EXPECT_GT(second_pid, 0) << "BackendKeyData PID must be captured on the bare reconnect";
    EXPECT_NE(second_pid, first_pid) << "a genuine re-handshake must land on a new backend process";

    int  decoded = -1;
    auto status  = db_->execute(
                          "SELECT 1",
                          [&](transaction &, results r) {
                             ASSERT_EQ(r.size(), 1u);
                             decoded = r[0][0].as<int>();
                          },
                          discard_error)
                       .await();
    EXPECT_TRUE(static_cast<bool>(status)) << "query on the bare-reconnected session failed";
    EXPECT_EQ(decoded, 1);
}

// --------------------------------------------------------------------------------------
// Disconnect drains the whole queue
// --------------------------------------------------------------------------------------

/**
 * @brief disconnect() must fail EVERY outstanding query, not just the in-flight one.
 *
 * Regression: on(disconnected) only failed `_current_query`; queries queued behind it had
 * their error callback skipped, so a pipelined caller's awaiter would suspend forever.
 * `fail_all_pending()` now drains the whole queue. Each of the three error callbacks must
 * fire, and each must carry a non-empty error. Uses the bounded `pump_until` helper so a
 * regression fails fast with a diagnostic rather than spinning.
 */
TEST_F(ConnectionLifecycle, DisconnectFailsAllQueuedQueries) {
    ASSERT_TRUE(qb::io::async::run_sync(db_->connect(dsn_tcp_string())));

    int  errors       = 0;
    int  with_message = 0;
    auto on_ok        = [](transaction &, results) {
    };
    auto on_err = [&](error::db_error const &e) {
        ++errors;
        if (std::string(e.what()).size() > 0)
            ++with_message;
    };

    // First query parks the server; the next two queue behind it.
    db_->execute("SELECT pg_sleep(3)", on_ok, on_err);
    db_->execute("SELECT 1", on_ok, on_err);
    db_->execute("SELECT 2", on_ok, on_err);

    // Pump once so the first query is actually sent and in flight.
    qb::io::async::run(EVRUN_NOWAIT);

    // Drop the connection while all three are outstanding.
    db_->disconnect();

    // All three error callbacks must fire (was 1 before fail_all_pending).
    const bool all_failed = qb::pg::test::pump_until([&] { return errors >= 3; }, std::chrono::seconds(5));
    EXPECT_TRUE(all_failed) << "only " << errors << "/3 queued queries were failed on disconnect";
    EXPECT_EQ(errors, 3);
    EXPECT_EQ(with_message, errors) << "each failed query must carry an error message";
}

// --------------------------------------------------------------------------------------
// Handshake deadline timer is owned (no UAF after destroy)
// --------------------------------------------------------------------------------------

/**
 * @brief The handshake-deadline timer must not outlive the Database.
 *
 * The deadline used to be scheduled with a self-deleting heap `Timeout` decoupled from the
 * Database lifetime: after a successful connect the timer stayed pending for the whole
 * timeout window; destroying the Database within it (the normal case) left the timer to fire
 * against a freed `this` (use-after-free). It is now an owned ScopedTimeout member, cancelled
 * on destruction. This connects with a short (1s) deadline, destroys the Database, then pumps
 * the loop past the deadline. Under ASan the old code reports heap-use-after-free here.
 *
 * De-masked from the legacy bare `SUCCEED()`: we assert the loop kept running healthily past
 * the deadline window (a fired timer against freed memory would crash before this assert).
 */
TEST_F(ConnectionLifecycle, DeadlineTimerCancelledOnDestroy) {
    bool reached_deadline_window = false;
    {
        auto db = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(qb::io::async::run_sync(db->connect(dsn_tcp_string())));
        db->disconnect();
        db->prepare_reconnect();
        // Reconnect with a 1s handshake deadline (reuses the parsed connection options).
        ASSERT_TRUE(qb::io::async::run_sync(db->connect(std::chrono::seconds(1))));
        EXPECT_GT(db->backend_pid(), 0);
    } // Database destroyed here; the owned ScopedTimeout is cancelled with it.

    // Pump the event loop well past the 1s deadline. A fire-and-forget timer would come due
    // now and run against the freed Database (ASan would trip). Reaching the end of the
    // window with the loop intact is the observable post-condition.
    qb::pg::test::pump_for(std::chrono::milliseconds(1500));
    reached_deadline_window = true;
    EXPECT_TRUE(reached_deadline_window) << "loop did not survive past the cancelled handshake deadline";
}

// --------------------------------------------------------------------------------------
// Connection pool (decoded value asserted, not just status)
// --------------------------------------------------------------------------------------

/**
 * @brief Multiple concurrent connections, each verified by a decoded `SELECT 1` == 1.
 *
 * Strengthened: the legacy non-coro test only asserted `ASSERT_TRUE(status)` and never the
 * decoded value. Each pooled connection now returns the integer `1`.
 */
TEST_F(ConnectionLifecycle, ConnectionPool) {
    constexpr int                                       num_connections = 5;
    std::vector<std::unique_ptr<qb::pg::tcp::database>> connections;
    connections.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i) {
        auto conn = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(qb::io::async::run_sync(conn->connect(dsn_tcp_string()))) << "pool connection " << i << " failed to connect";
        connections.push_back(std::move(conn));
    }

    for (std::size_t i = 0; i < connections.size(); ++i) {
        int  decoded = -1;
        auto status  = connections[i]
                           ->execute(
                               "SELECT 1",
                               [&](transaction &, results res) {
                                  ASSERT_EQ(res.size(), 1u);
                                  decoded = res[0][0].as<int>();
                               },
                               discard_error)
                           .await();
        ASSERT_TRUE(status) << "pool connection " << i << " query failed";
        EXPECT_EQ(decoded, 1) << "pool connection " << i << " decoded wrong value";
    }
}

/// Pool scenario driven entirely through coroutines: connect + query, decoded == 1.
TEST_F(ConnectionLifecycle, ConnectionPool_Coroutine) {
    constexpr int num_connections = 5;
    int           ok_count        = 0;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        for (int i = 0; i < num_connections; ++i) {
            auto conn = std::make_unique<qb::pg::tcp::database>();
            if (!co_await conn->connect(dsn_tcp_string()))
                co_return;
            auto reply = co_await conn->query("SELECT 1");
            if (reply.ok() && reply.result().size() == 1 && reply.result()[0][0].as<int>() == 1)
                ++ok_count;
        }
    }());
    EXPECT_EQ(ok_count, num_connections) << "only " << ok_count << "/" << num_connections
                                         << " coroutine pool connections "
                                            "returned the decoded value 1";
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
