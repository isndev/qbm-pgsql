/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file client-static-exit.cpp
 * @brief System: a `Database` with STATIC STORAGE ends cleanly when the process exits (Huly QB-1246) -- never
 *        connected, in its handshake, ready and idle, a query in flight (its callbacks pending, or a coroutine
 *        parked on it), disconnected by the program, a reconnection run under way; with a reconnection policy armed
 *        and without.
 *
 * `exit()` destroys the main thread's thread-locals before the objects with static storage, and qb-io's
 * `listener::current` is one: a database a program keeps in static storage is destroyed AFTER its thread's event
 * loop, where nothing can be registered, armed or resumed any more -- a request for the loop there stops the process
 * on a line that names it, and a watcher started there writes into the freed loop. Each case asks, of the database in
 * one state: the process ends with the code its judge chose and its stderr is the judge's line and NOTHING else (no
 * named stop, no warning, no sanitizer report: under AddressSanitizer the destroyed listener is poisoned); the
 * destructor ran after the loop -- asserted, not assumed -- and returned; the peer of a connection reads a clean EOF,
 * never a reset; what was in flight ended as the module says it does (the judge's line carries how many times a
 * query's callbacks ran, whether after the loop, and which: 1 the result, 2 the error; whether the coroutine parked
 * on it was resumed, and when its frame was destroyed); LeakSanitizer, asked by the judge, has nothing to report.
 *
 * Every case is a CHILD process (a death test, re-executed) whose judge is an `atexit` handler registered before the
 * database is built. The peer is a relay the case steps by hand (qb's `static_exit_support.h`): by default a scripted
 * backend -- AuthenticationOk, BackendKeyData, ReadyForQuery, then CommandComplete to every simple query -- so the
 * binary needs no daemon; built with `QB_STATIC_EXIT_DAEMON` -- the `integration` tier's copy -- every octet goes
 * through it to the PostgreSQL that `QB_PG_DSN` names. A case that stops stepping the relay has a peer that is open
 * and silent.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

#include <qbm/pgsql/pgsql.h>

#ifdef QB_STATIC_EXIT_DAEMON
#include "../../shared/pg_integration_fixture.hpp"
#endif
#include "../../shared/test_config.hpp"
#include "static_exit_support.h"

using namespace std::chrono_literals;

namespace client_static_exit_test {

namespace async = qb::io::async;
using namespace qb::io::test::static_exit;

using database = qb::pg::tcp::database;

std::string
int32(std::uint32_t const v) {
    return {
        static_cast<char>((v >> 24) & 0xFF), static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 8) & 0xFF),
        static_cast<char>(v & 0xFF)
    };
}

std::string
backend_message(char const type, std::string const &payload) {
    return std::string(1, type) + int32(static_cast<std::uint32_t>(4 + payload.size())) + payload;
}

std::uint32_t
read_int32(std::string const &from, std::size_t const at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(from[at])) << 24)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(from[at + 1])) << 16)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(from[at + 2])) << 8)
           | static_cast<std::uint32_t>(static_cast<unsigned char>(from[at + 3]));
}

/// A scripted backend: the startup is answered AuthenticationOk, BackendKeyData, ReadyForQuery; a simple query,
/// CommandComplete and ReadyForQuery; anything else, nothing.
std::string
answer_frontend(std::string &unread) {
    std::string out;
    for (;;) {
        if (unread.size() < 5)
            return out;
        if (unread[0] == '\0') { // a StartupMessage has no type: its length comes first, and is short
            const std::uint32_t size = read_int32(unread, 0);
            if (unread.size() < size)
                return out;
            unread.erase(0, size);
            out += backend_message('R', int32(0)) + backend_message('K', int32(4242) + int32(2424)) + backend_message('Z', "I");
            continue;
        }
        const char          type = unread[0];
        const std::uint32_t size = read_int32(unread, 1);
        if (unread.size() < 1u + size)
            return out;
        unread.erase(0, 1u + size);
        if (type == 'Q')
            out += backend_message('C', std::string("SELECT 0") + '\0') + backend_message('Z', "I");
    }
}

std::string g_dsn; ///< what the database connects to: the relay

#ifdef QB_STATIC_EXIT_DAEMON
/// The PostgreSQL of the integration tier (`QB_PG_DSN`, tcp://user:password@host:port[database]), taken apart.
struct daemon_dsn {
    std::string   credentials; ///< up to and including the `@`
    std::string   host;        ///< an IPv4 host
    std::uint16_t port = 0;
    std::string   database; ///< from the `[`
    bool          ok = false;
};

daemon_dsn
daemon() {
    daemon_dsn        out;
    const std::string dsn   = qb::pg::test::dsn_tcp_string();
    const auto        at    = dsn.rfind('@');
    const auto        open  = at == std::string::npos ? std::string::npos : dsn.find('[', at);
    const auto        colon = at == std::string::npos ? std::string::npos : dsn.find(':', at);
    if (at == std::string::npos || open == std::string::npos || colon == std::string::npos || colon > open)
        return out;
    out.credentials = dsn.substr(0, at + 1);
    out.host        = dsn.substr(at + 1, colon - at - 1);
    if (out.host == "localhost")
        out.host = "127.0.0.1";
    out.port     = static_cast<std::uint16_t>(std::atoi(dsn.c_str() + colon + 1));
    out.database = dsn.substr(open);
    out.ok       = out.port != 0;
    return out;
}
#endif

relay &
pg_peer() {
    relay &peer = new_peer();
#ifdef QB_STATIC_EXIT_DAEMON
    const daemon_dsn to = daemon(); // the relay takes the place of the host and the port
    if (!to.ok)
        setup_failed("QB_PG_DSN is not tcp://user:password@host:port[database]");
    peer.through(to.host, to.port);
    g_dsn = to.credentials + "127.0.0.1:" + std::to_string(peer.port()) + to.database;
#else
    peer.answer(answer_frontend);
    g_dsn = "tcp://test:test@127.0.0.1:" + std::to_string(peer.port()) + "[test]";
#endif
    return peer;
}

#ifdef QB_STATIC_EXIT_DAEMON
/// The daemon serves the cases: a connection to it becomes ready, which is what the tier's own fixture asks
/// (`pg_try_connect`) -- a daemon that is down, and one whose authentication this build cannot answer, are the same
/// to a case. Asked once, by the process that runs the cases.
[[nodiscard]] bool
daemon_serves() {
    return asked_once("QB_STATIC_EXIT_PGSQL_SERVES", [] {
        database probe;
        return daemon().ok && qb::pg::test::pg_try_connect(probe);
    });
}
#endif

/// The fixture of the cases here: the integration tier's copy skips, it does not fail, without a daemon that serves
/// it.
template <typename Base>
class with_peer : public Base {
protected:
    void
    SetUp() override {
        Base::SetUp();
#ifdef QB_STATIC_EXIT_DAEMON
        if (!daemon_serves())
            GTEST_SKIP() << qb::pg::test::kDaemonUnreachableSentinel << " (postgres at " << qb::pg::test::dsn_tcp_string() << " not reachable)";
#endif
    }
};

bool g_connect_answered = false;
bool g_connect_ok       = false;

async::task<void>
connect_once(database &db) {
    g_connect_ok       = co_await db.connect(g_dsn);
    g_connect_answered = true;
}

void
make_ready(database &db, relay &peer) {
    async::coro_scheduler().spawn(connect_once(db));
    if (!pump_until(&peer, [] { return g_connect_answered; }) || !g_connect_ok)
        setup_failed("the database never became ready");
    if (db.state() != qb::io::connection_state::ready)
        setup_failed("the database is not ready");
    g.want_peer = 0;
}

/// A coroutine parked on connect(): its handshake is never answered.
async::task<void>
parked_on_connect(database &db) {
    frame_local local;
    g.parked      = true;
    const bool ok = co_await db.connect(g_dsn);
    ++g.resumed;
    g.outcome = ok ? 1 : 2;
}

async::task<void>
parked_on_a_query(database &db) {
    frame_local local;
    g.parked   = true;
    auto reply = co_await db.execute("SELECT 1");
    ++g.resumed;
    g.outcome = reply.ok() ? 1 : 2;
}

void
query_in_flight(database &db) {
    db.execute(
        "SELECT 1", [](qb::pg::transaction &, qb::pg::results) { called_with(1); }, [](qb::pg::error::db_error const &) { called_with(2); });
}

int g_retry_waits = 0; ///< waits the reconnection run announced (`on_retry`)

qb::pg::RetryPolicy
one_hour_between_attempts() {
    qb::pg::RetryPolicy policy;
    policy.with_initial_delay(1h).with_max_delay(1h).with_jitter(false).with_connect_timeout(2s);
    policy.on_retry = [](int, qb::duration) {
        ++g_retry_waits;
    };
    return policy;
}

enum class state {
    never_connected,
    handshake,           ///< a coroutine parked on connect(), the startup unanswered
    ready,               ///< ready, nothing asked
    query,               ///< a query written and a second queued behind it, their callbacks pending
    coroutine,           ///< a coroutine parked on a query
    own_callback,        ///< `exit()` called by the callback of a result
    disconnected,        ///< disconnect(), then `exit()`
    reconnecting,        ///< the server gone, the run waiting for its next attempt
    reconnect_handshake, ///< the server back, the run's startup unanswered
};

[[noreturn]] void
run_database(state const st, bool const policy_armed, site const where) {
    std::atexit(judge); // first: it runs after the destructor of `object`
    relay                   &peer = pg_peer();
    static at_exit<database> object;
    database                &db = object.client;
    if (policy_armed)
        db.enable_auto_reconnect(one_hour_between_attempts());
    switch (st) {
        case state::never_connected:
            break;
        case state::handshake:
            peer.go_silent(); // the port completes the TCP handshake; nobody answers the startup
            async::coro_scheduler().spawn(parked_on_connect(db));
            if (!pump_until(nullptr, [&] { return g.parked && db.state() == qb::io::connection_state::handshaking; }))
                setup_failed("the database never reached its handshake");
            pump_for(nullptr, 20ms);
            break;
        case state::ready:
            make_ready(db, peer);
            break;
        case state::query:
            make_ready(db, peer);
            peer.go_silent();
            query_in_flight(db);
            query_in_flight(db); // queued behind the first
            pump_for(nullptr, 20ms);
            break;
        case state::coroutine:
            make_ready(db, peer);
            peer.go_silent();
            async::coro_scheduler().spawn(parked_on_a_query(db));
            if (!pump_until(nullptr, [] { return g.parked; }))
                setup_failed("the coroutine never reached its query");
            pump_for(nullptr, 20ms);
            break;
        case state::own_callback:
            make_ready(db, peer);
            db.execute(
                "SELECT 1",
                [](qb::pg::transaction &, qb::pg::results) {
                    called_with(1);
                    std::exit(3); // inside the database's own dispatch: its callback, its protocol, its io handler
                },
                [](qb::pg::error::db_error const &) { called_with(2); });
            pump_for(&peer, 30s);
            setup_failed("the result never came");
        case state::disconnected:
            make_ready(db, peer);
            db.disconnect();
            break;
        case state::reconnecting:
            make_ready(db, peer);
            g.want_peer = -1;
            peer.go_away(); // the connection drops and the port refuses the next: attempt 1 fails, the run waits an hour
            if (!pump_until(nullptr, [&] { return g_retry_waits >= 1 && db.state() == qb::io::connection_state::reconnecting; }))
                setup_failed("the reconnection run never waited for its second attempt");
            pump_for(nullptr, 20ms);
            break;
        case state::reconnect_handshake:
            make_ready(db, peer);
            peer.hold_new_connections(); // the server comes back deaf: the next connection's startup stays unanswered
            peer.drop_client();
            g.want_peer = -1; // the connection the relay accepted is gone; the next one stays in its backlog
            if (!pump_until(&peer, [&] { return db.is_reconnecting() && db.state() == qb::io::connection_state::handshaking; }))
                setup_failed("the reconnection run never reached its handshake");
            pump_for(nullptr, 20ms);
            break;
    }
    leave(where);
}

class PgsqlDatabaseStaticExitDeathTest : public with_peer<in_a_child_from> {};

TEST_P(PgsqlDatabaseStaticExitDeathTest, NeverConnected) {
    EXPECT_EXIT(run_database(state::never_connected, false, GetParam()), ::testing::ExitedWithCode(0),
                stderr_is(verdict(-1, 0, 0, 0, 0, 0, 0)));
}

TEST_P(PgsqlDatabaseStaticExitDeathTest, NeverConnectedWithAPolicyArmed) {
    EXPECT_EXIT(run_database(state::never_connected, true, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(-1, 0, 0, 0, 0, 0, 0)));
}

TEST_P(PgsqlDatabaseStaticExitDeathTest, InItsHandshake) {
    EXPECT_EXIT(run_database(state::handshake, false, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(-1, 0, 0, 0, 0, 1, 0)));
}

TEST_P(PgsqlDatabaseStaticExitDeathTest, ReadyAndIdle) {
    EXPECT_EXIT(run_database(state::ready, false, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(0, 0, 0, 0, 0, 0, 0)));
}

TEST_P(PgsqlDatabaseStaticExitDeathTest, ReadyAndIdleWithAPolicyArmed) {
    EXPECT_EXIT(run_database(state::ready, true, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(0, 0, 0, 0, 0, 0, 0)));
}

// A query in flight and one queued behind it: neither callback runs.
TEST_P(PgsqlDatabaseStaticExitDeathTest, AQueryInFlight) {
    EXPECT_EXIT(run_database(state::query, false, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(0, 0, 0, 0, 0, 0, 0)));
}

TEST_P(PgsqlDatabaseStaticExitDeathTest, AQueryInFlightWithAPolicyArmed) {
    EXPECT_EXIT(run_database(state::query, true, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(0, 0, 0, 0, 0, 0, 0)));
}

TEST_P(PgsqlDatabaseStaticExitDeathTest, ACoroutineParkedOnAQuery) {
    EXPECT_EXIT(run_database(state::coroutine, false, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(0, 0, 0, 0, 0, 1, 0)));
}

TEST_P(PgsqlDatabaseStaticExitDeathTest, Disconnected) {
    EXPECT_EXIT(run_database(state::disconnected, false, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(0, 0, 0, 0, 0, 0, 0)));
}

TEST_P(PgsqlDatabaseStaticExitDeathTest, DisconnectedWithAPolicyArmed) {
    EXPECT_EXIT(run_database(state::disconnected, true, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(0, 0, 0, 0, 0, 0, 0)));
}

TEST_P(PgsqlDatabaseStaticExitDeathTest, InAReconnectionRun) {
    EXPECT_EXIT(run_database(state::reconnecting, true, GetParam()), ::testing::ExitedWithCode(0), stderr_is(verdict(-1, 0, 0, 0, 0, 0, 0)));
}

TEST_P(PgsqlDatabaseStaticExitDeathTest, InTheHandshakeOfAReconnectionRun) {
    EXPECT_EXIT(run_database(state::reconnect_handshake, true, GetParam()), ::testing::ExitedWithCode(0),
                stderr_is(verdict(-1, 0, 0, 0, 0, 0, 0)));
}

INSTANTIATE_TEST_SUITE_P(Exit, PgsqlDatabaseStaticExitDeathTest, ::testing::Values(site::main, site::callback), site_name);

class PgsqlDatabaseStaticExitOnceDeathTest : public with_peer<in_a_child> {};

// `exit()` from the callback of a result: the database's own dispatch is on the stack when its destructor runs.
TEST_F(PgsqlDatabaseStaticExitOnceDeathTest, ExitFromTheCallbackOfAResult) {
    EXPECT_EXIT(run_database(state::own_callback, false, site::main), ::testing::ExitedWithCode(0), stderr_is(verdict(0, 1, 0, 1, 0, 0, 0)));
}

// A coroutine that calls `exit()`: the thread's scheduler is destroyed from inside one of its coroutines, abandons
// what it owns and says so; the frame parked on the query is never destroyed.
TEST_F(PgsqlDatabaseStaticExitOnceDeathTest, ACoroutineParkedOnAQueryAndExitFromACoroutine) {
    EXPECT_EXIT(run_database(state::coroutine, false, site::coroutine), ::testing::ExitedWithCode(0),
                stderr_is(verdict(0, 0, 0, 0, 0, 0, 0), abandoned(2)));
}

} // namespace client_static_exit_test
