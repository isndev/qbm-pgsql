/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file reconnect-contract.cpp
 * @brief Integration: the reconnect contract on a live server (Huly QB-111; `qb/io/reconnect.h`).
 *
 * The connection's side of the contract, case by case: `connect()` resolves at `ready` -- after the
 * protocol handshake, the hook installed with `set_handshake()` and the LISTEN replay -- and
 * `is_connected()` means exactly that; work is refused outside `ready` except through the hook's own
 * `handshake_session`, which goes dead with its handshake; a refusal (SQLSTATE class 28, 3D000, or the
 * hook) is terminal and never retried; a lost ready connection is recovered only by an armed run, and
 * an explicit `disconnect()` is terminal whatever the policy; the hooks report each transition once.
 *
 * A loss is provoked the way a server restart or an administrator does it: `pg_terminate_backend()`
 * of the connection's backend from a second connection, which then stays the observer. Waits are
 * bounded `pump_until` loops at test-body scope (never inside a `run_sync` coroutine). Skips (never
 * fails) when the daemon is unreachable.
 */

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/pg_pump.hpp"
#include "../../shared/test_config.hpp"
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using qb::io::connection_state;
using qb::pg::test::dsn_tcp_string;
using qb::pg::test::pump_until;

namespace reconnect_contract_test {

constexpr auto kDeadline = std::chrono::seconds(10);

/// Short, deterministic backoff: the run reconnects in milliseconds.
RetryPolicy
quick_policy(int max_attempts = -1) {
    return RetryPolicy{}
        .with_max_attempts(max_attempts)
        .with_initial_delay(std::chrono::milliseconds(20))
        .with_max_delay(std::chrono::milliseconds(50))
        .with_jitter(false);
}

/// `dsn` with its database replaced by one that does not exist.
std::string
dsn_with_database(std::string const &dsn, std::string const &database) {
    const auto open = dsn.rfind('[');
    return dsn.substr(0, open) + "[" + database + "]";
}

/// One statement on `db`, from test-body scope; true when it succeeded.
bool
run_ok(qb::pg::tcp::database &db, std::string const &sql) {
    return qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto r = co_await db.execute(sql);
        co_return r.ok();
    }());
}

/// The single text value of `sql` on `db`, or nullopt when it failed.
std::optional<std::string>
scalar(qb::pg::tcp::database &db, std::string const &sql) {
    return qb::io::async::run_sync([&]() -> qb::io::async::task<std::optional<std::string>> {
        auto r = co_await db.query(sql);
        if (!r.ok() || r.result().size() != 1)
            co_return std::nullopt;
        co_return r.result()[0][0].as<std::string>();
    }());
}

/// Observer fixture: `observer_` is a second connection that terminates backends and reads server state.
class ReconnectContract : public ::testing::Test {
protected:
    std::unique_ptr<qb::pg::tcp::database> observer_;

    void
    SetUp() override {
        observer_ = std::make_unique<qb::pg::tcp::database>();
        if (!qb::io::async::run_sync(observer_->connect(dsn_tcp_string())))
            GTEST_SKIP() << qb::pg::test::kDaemonUnreachableSentinel << " (postgres at " << dsn_tcp_string() << " not reachable)";
    }

    void
    TearDown() override {
        if (observer_) {
            observer_->disconnect();
            observer_.reset();
        }
    }

    /// Terminate `pid`'s backend, as an administrator or a server restart would.
    void
    terminate(int pid) {
        ASSERT_GT(pid, 0);
        ASSERT_TRUE(run_ok(*observer_, "SELECT pg_terminate_backend(" + std::to_string(pid) + ")"));
    }
};

} // namespace reconnect_contract_test

using namespace reconnect_contract_test;

// connect() resolves at ready: the hook ran on the new session first, and its effect is visible to the
// first user query. The hook's queries go through the session; the connection itself refuses them.
TEST_F(ReconnectContract, ConnectResolvesAfterTheHookOnTheNewSession) {
    qb::pg::tcp::database db;
    int                   hook_runs = 0;
    bool                  db_refused_in_hook{};
    db.set_handshake([&](handshake_session &hs) -> qb::io::async::task<bool> {
        ++hook_runs;
        auto refused       = co_await db.execute("SELECT 1"); // the connection is not ready yet
        db_refused_in_hook = !refused.ok();
        auto set           = co_await hs.execute("SET application_name = 'qb_reconnect_contract'");
        co_return set.ok();
    });
    ASSERT_TRUE(qb::io::async::run_sync(db.connect(dsn_tcp_string())));
    EXPECT_EQ(db.state(), connection_state::ready);
    EXPECT_TRUE(db.is_connected());
    EXPECT_EQ(hook_runs, 1);
    EXPECT_TRUE(db_refused_in_hook) << "a query through the connection itself slipped past the handshake";
    EXPECT_EQ(scalar(db, "SHOW application_name"), std::optional<std::string>{"qb_reconnect_contract"});
    db.disconnect();
}

// The session writes only while its own handshake runs: kept past the hook, it fails "handshake over".
TEST_F(ReconnectContract, ASessionKeptPastItsHandshakeIsDead) {
    qb::pg::tcp::database            db;
    std::optional<handshake_session> leaked;
    db.set_handshake([&](handshake_session &hs) -> qb::io::async::task<bool> {
        leaked.emplace(hs);
        co_return true;
    });
    ASSERT_TRUE(qb::io::async::run_sync(db.connect(dsn_tcp_string())));
    ASSERT_TRUE(leaked.has_value());
    EXPECT_FALSE(leaked->live());
    std::string error;
    const bool  ok = qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto r = co_await leaked->execute("SELECT 1");
        if (!r.ok())
            error = r.error().what();
        co_return r.ok();
    }());
    EXPECT_FALSE(ok) << "a leaked handshake session still wrote on the link";
    EXPECT_EQ(error, "handshake over");
    db.disconnect();
}

// A hook returning false refuses the connection for good: one attempt, no retry, handshake_refused.
TEST_F(ReconnectContract, AHookThatRefusesIsTerminal) {
    qb::pg::tcp::database db;
    int                   hook_runs = 0, retries = 0;
    db.set_handshake([&](handshake_session &) -> qb::io::async::task<bool> {
        ++hook_runs;
        co_return false;
    });
    std::vector<qb::io::down_reason> downs;
    db.on_down([&](qb::io::down_reason r) { downs.push_back(r); });
    auto policy = quick_policy(5).with_on_retry([&](int, qb::duration) { ++retries; });
    EXPECT_FALSE(qb::io::async::run_sync(db.connect_with_retry(dsn_tcp_string(), policy)));
    EXPECT_EQ(db.state(), connection_state::down);
    EXPECT_EQ(db.down_reason(), qb::io::down_reason::handshake_refused);
    EXPECT_EQ(hook_runs, 1) << "a refusal was retried";
    EXPECT_EQ(retries, 0);
    EXPECT_EQ(downs, std::vector<qb::io::down_reason>{qb::io::down_reason::handshake_refused});
    EXPECT_FALSE(db.is_connected());
}

// A wrong password (SQLSTATE class 28) is a refusal: terminal, visible, never retried.
TEST_F(ReconnectContract, AWrongPasswordIsARefusalNotARetry) {
    qb::pg::tcp::database db;
    int                   retries = 0;
    auto                  policy  = quick_policy(5).with_on_retry([&](int, qb::duration) { ++retries; });
    EXPECT_FALSE(qb::io::async::run_sync(db.connect_with_retry(qb::pg::test::dsn_invalid_auth_string(), policy)));
    EXPECT_EQ(db.state(), connection_state::down);
    EXPECT_EQ(db.down_reason(), qb::io::down_reason::handshake_refused);
    EXPECT_EQ(retries, 0) << "a refused authentication was retried";
    EXPECT_EQ(db.error().code.substr(0, 2), "28") << db.error().what();
}

// A database that does not exist (3D000) is a refusal too: the server answers it after AuthenticationOk,
// so a connect() that resolved at authentication used to report this connection established.
TEST_F(ReconnectContract, AMissingDatabaseIsARefusal) {
    qb::pg::tcp::database db;
    EXPECT_FALSE(qb::io::async::run_sync(db.connect(dsn_with_database(dsn_tcp_string(), "qb_reconnect_contract_no_such_db"))));
    EXPECT_EQ(db.state(), connection_state::down);
    EXPECT_EQ(db.down_reason(), qb::io::down_reason::handshake_refused);
    EXPECT_EQ(db.error().code, "3D000") << db.error().what();
    EXPECT_FALSE(db.is_connected());
}

// Without a run, a lost ready connection is down(peer): on_disconnected then on_down, once each, and
// work is refused from then on.
TEST_F(ReconnectContract, APeerLossWithoutAPolicyIsDown) {
    qb::pg::tcp::database            db;
    std::vector<qb::io::down_reason> lost, downs;
    db.on_disconnected([&](qb::io::down_reason r) { lost.push_back(r); });
    db.on_down([&](qb::io::down_reason r) { downs.push_back(r); });
    ASSERT_TRUE(qb::io::async::run_sync(db.connect(dsn_tcp_string())));
    terminate(db.backend_pid());
    ASSERT_TRUE(pump_until([&] { return db.state() == connection_state::down; }, kDeadline)) << "the loss was never noticed";
    EXPECT_EQ(db.down_reason(), qb::io::down_reason::peer);
    EXPECT_EQ(lost, std::vector<qb::io::down_reason>{qb::io::down_reason::peer});
    EXPECT_EQ(downs, std::vector<qb::io::down_reason>{qb::io::down_reason::peer});
    EXPECT_FALSE(db.is_reconnecting());
    EXPECT_FALSE(run_ok(db, "SELECT 1")) << "work on a down connection must be refused";
}

// With a run armed, a lost ready connection comes back by itself: the hook runs again on the new
// session, the hooks alternate, the work in flight failed once, and stats() counts the reconnection.
TEST_F(ReconnectContract, APeerLossWithAPolicyReconnectsThroughTheHandshake) {
    qb::pg::tcp::database            db;
    int                              hook_runs = 0, connected = 0;
    std::vector<qb::io::down_reason> lost;
    db.set_handshake([&](handshake_session &hs) -> qb::io::async::task<bool> {
        ++hook_runs;
        auto set = co_await hs.execute("SET application_name = 'qb_reconnect_run'");
        co_return set.ok();
    });
    db.on_connected([&] { ++connected; });
    db.on_disconnected([&](qb::io::down_reason r) { lost.push_back(r); });
    db.enable_auto_reconnect(quick_policy());
    ASSERT_TRUE(qb::io::async::run_sync(db.connect(dsn_tcp_string())));
    const int first_pid = db.backend_pid();

    // A query in flight when the backend dies fails once, with the server's reason, and is not replayed.
    int  in_flight_failures = 0, in_flight_successes = 0;
    auto sleeper = [&]() -> qb::io::async::task<void> {
        auto r = co_await db.query("SELECT pg_sleep(5)");
        (r.ok() ? in_flight_successes : in_flight_failures) += 1;
    };
    qb::io::async::coro_scheduler().spawn(sleeper());
    ASSERT_TRUE(pump_until(
        [&] {
            auto n = scalar(*observer_, "SELECT count(*)::text FROM pg_stat_activity WHERE pid = " + std::to_string(first_pid)
                                            + " AND query LIKE 'SELECT pg_sleep%' AND state = 'active'");
            return n && *n == "1";
        },
        kDeadline))
        << "the sleeping query never reached the server";
    terminate(first_pid);

    ASSERT_TRUE(pump_until([&] { return connected == 2; }, kDeadline)) << "the run never reconnected";
    EXPECT_EQ(db.state(), connection_state::ready);
    EXPECT_FALSE(db.is_reconnecting());
    EXPECT_NE(db.backend_pid(), first_pid);
    EXPECT_EQ(hook_runs, 2) << "the hook did not run on the new session";
    EXPECT_EQ(lost, std::vector<qb::io::down_reason>{qb::io::down_reason::peer});
    EXPECT_EQ(in_flight_failures, 1);
    EXPECT_EQ(in_flight_successes, 0);
    EXPECT_EQ(db.stats().reconnects, 1u);
    EXPECT_EQ(scalar(db, "SHOW application_name"), std::optional<std::string>{"qb_reconnect_run"});
    db.disconnect();
}

// An explicit disconnect() is terminal whatever the policy: down(user), no run, the hooks once each.
TEST_F(ReconnectContract, AnExplicitDisconnectIsTerminalWhateverThePolicy) {
    qb::pg::tcp::database            db;
    int                              connected = 0;
    std::vector<qb::io::down_reason> lost, downs;
    db.on_connected([&] { ++connected; });
    db.on_disconnected([&](qb::io::down_reason r) { lost.push_back(r); });
    db.on_down([&](qb::io::down_reason r) { downs.push_back(r); });
    db.enable_auto_reconnect(quick_policy());
    ASSERT_TRUE(qb::io::async::run_sync(db.connect(dsn_tcp_string())));
    db.disconnect();
    EXPECT_EQ(db.state(), connection_state::down);
    EXPECT_EQ(db.down_reason(), qb::io::down_reason::user);
    qb::pg::test::pump_for(std::chrono::milliseconds(300)); // a run, if one started, reconnects in ~20 ms
    EXPECT_EQ(connected, 1) << "an explicit disconnect() started a reconnection run";
    EXPECT_FALSE(db.is_reconnecting());
    EXPECT_EQ(lost, std::vector<qb::io::down_reason>{qb::io::down_reason::user});
    EXPECT_EQ(downs, std::vector<qb::io::down_reason>{qb::io::down_reason::user});
}

// A run that cannot reach the server spends its policy and ends down(retries_exhausted), calling
// on_retry before each wait.
TEST_F(ReconnectContract, ASpentPolicyEndsDownRetriesExhausted) {
    qb::pg::tcp::database db;
    std::vector<int>      attempts;
    auto policy = quick_policy(3).with_connect_timeout(std::chrono::milliseconds(500)).with_on_retry([&](int attempt, qb::duration) {
        attempts.push_back(attempt);
    });
    EXPECT_FALSE(qb::io::async::run_sync(db.connect_with_retry("tcp://test:test@127.0.0.1:1[test]", policy)));
    EXPECT_EQ(db.state(), connection_state::down);
    EXPECT_EQ(db.down_reason(), qb::io::down_reason::retries_exhausted);
    EXPECT_EQ(attempts, (std::vector<int>{1, 2}));
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
