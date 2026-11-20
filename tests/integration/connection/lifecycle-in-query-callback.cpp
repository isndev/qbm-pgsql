/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
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
 *         limitations under the License.
 */

/**
 * @file lifecycle-in-query-callback.cpp
 * @brief A query callback that ends the connection, begins another one and runs the loop before it returns.
 *
 * Integration tier: a live PostgreSQL (`QB_PG_DSN`).
 *
 * A query's callback runs inside the read loop's dispatch of the database's own link. `disconnect()` called there
 * cannot tear the link down under the callback: the teardown -- `on(disconnected)`, the failure of what was queued --
 * runs right after the callback returns. A `connect()` begun in the same callback opens its transport from the loop;
 * when the callback then runs the loop itself (a synchronous wait), that transport completes under it, and the link
 * must not be installed over the one whose teardown is still owed: `install_link_()` resets the buffers and the
 * io state the dispatch on the stack still reads. The same inside the teardown of a link the server ended -- an
 * error callback, the `on_disconnected` hook --, where the lifecycle such a callback begins, or ends, is the one the
 * teardown must leave alone.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/tcp/socket.h>

#include <qbm/pgsql/pgsql.h>

#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/pg_pump.hpp"

using namespace std::chrono_literals;
using namespace qb::pg::test;

namespace lifecycle_in_query_callback_test {

qb::io::async::task<void>
connect_again(qb::pg::tcp::database *db, std::string dsn, bool *ok, bool *done) {
    *ok   = static_cast<bool>(co_await db->connect(dsn));
    *done = true;
}

qb::io::async::task<void>
adopt_again(qb::pg::tcp::database *db, std::string dsn, qb::io::tcp::socket sock, bool *ok, bool *done) {
    *ok   = static_cast<bool>(co_await db->connect(dsn, std::move(sock)));
    *done = true;
}

class PgLifecycleInQueryCallback : public PgIntegrationTest {};

} // namespace lifecycle_in_query_callback_test

using namespace lifecycle_in_query_callback_test;

TEST_F(PgLifecycleInQueryCallback, AConnectMadeInAQueryCallbackThatThenRunsTheLoopWaitsForTheOldLinkToEnd) {
    int  callbacks = 0, errors = 0;
    bool connect_ok = false, connect_done = false, done_under_the_callback = false;
    db_->execute(
        "SELECT 1",
        [&](qb::pg::transaction &, qb::pg::results) {
            ++callbacks;
            db_->disconnect();
            qb::io::async::coro_scheduler().spawn(connect_again(db_.get(), dsn_tcp_string(), &connect_ok, &connect_done));
            (void) pump_until([&] { return connect_done; }, 500ms); // the callback waits, the loop turns under it
            done_under_the_callback = connect_done;
        },
        [&](qb::pg::error::db_error const &) { ++errors; });

    ASSERT_TRUE(pump_until([&] { return connect_done; }, 10s)) << "the connect() made in the callback never completed";
    EXPECT_EQ(callbacks, 1);
    EXPECT_FALSE(done_under_the_callback) << "the new link came up under the callback of the link it replaces";
    EXPECT_TRUE(connect_ok);
    EXPECT_EQ(db_->state(), qb::io::connection_state::ready);

    int answer = -1;
    db_->execute(
        "SELECT 41 + 1", [&](qb::pg::transaction &, qb::pg::results r) { answer = r[0][0].as<int>(); },
        [&](qb::pg::error::db_error const &) { ++errors; });
    ASSERT_TRUE(pump_until([&] { return answer != -1 || errors != 0; }, 10s)) << "the new link answers nothing";
    EXPECT_EQ(answer, 42) << "the new link answers";
    EXPECT_EQ(errors, 0);
}

TEST_F(PgLifecycleInQueryCallback, AnAdoptionMadeInAQueryCallbackThatThenRunsTheLoopWaitsForTheOldLinkToEnd) {
    // As above with a transport the callback opened itself and hands over (`connect(dsn, raw_io)`).
    const auto        opts  = qb::pg::connection_options::parse(dsn_tcp_string());
    const auto        colon = opts.uri.rfind(':');
    const std::string host  = opts.uri.substr(0, colon);
    const auto        port  = static_cast<std::uint16_t>(colon == std::string::npos ? 5432 : std::stoi(opts.uri.substr(colon + 1)));

    int  callbacks = 0, errors = 0;
    bool opened = false, adopt_ok = false, adopt_done = false, done_under_the_callback = false;
    db_->execute(
        "SELECT 1",
        [&](qb::pg::transaction &, qb::pg::results) {
            ++callbacks;
            db_->disconnect();
            qb::io::tcp::socket fresh;
            opened = fresh.connect_v4(host, port) == 0;
            qb::io::async::coro_scheduler().spawn(adopt_again(db_.get(), dsn_tcp_string(), std::move(fresh), &adopt_ok, &adopt_done));
            (void) pump_until([&] { return adopt_done; }, 500ms);
            done_under_the_callback = adopt_done;
        },
        [&](qb::pg::error::db_error const &) { ++errors; });

    ASSERT_TRUE(pump_until([&] { return adopt_done; }, 10s)) << "the adoption made in the callback never completed";
    ASSERT_TRUE(opened) << "no transport to " << opts.uri << ": the case would test nothing";
    EXPECT_EQ(callbacks, 1);
    EXPECT_FALSE(done_under_the_callback) << "the adopted link came up under the callback of the link it replaces";
    EXPECT_TRUE(adopt_ok);
    EXPECT_EQ(db_->state(), qb::io::connection_state::ready);

    int answer = -1;
    db_->execute(
        "SELECT 41 + 1", [&](qb::pg::transaction &, qb::pg::results r) { answer = r[0][0].as<int>(); },
        [&](qb::pg::error::db_error const &) { ++errors; });
    ASSERT_TRUE(pump_until([&] { return answer != -1 || errors != 0; }, 10s)) << "the adopted link answers nothing";
    EXPECT_EQ(answer, 42) << "the adopted link answers";
    EXPECT_EQ(errors, 0);
}

TEST_F(PgLifecycleInQueryCallback, AConnectMadeWhenTheLinkIsLostThatThenRunsTheLoopWaitsForTheTeardownToEnd) {
    // The server ends the session. The query queued behind fails from inside the link's teardown, and its error
    // callback connects again and runs the loop: the new transport completes while that teardown is still running --
    // the old link's watcher is stopped, and its transport ended, only after the callback returns.
    int  answered = 0, failures = 0, errors = 0;
    bool connect_ok = false, connect_done = false, done_under_the_teardown = false;
    db_->execute("SELECT pg_terminate_backend(pg_backend_pid())", qb::pg::discard_query, qb::pg::discard_error);
    db_->execute(
        "SELECT 1", [&](qb::pg::transaction &, qb::pg::results) { ++answered; },
        [&](qb::pg::error::db_error const &) {
            if (failures++ != 0)
                return;
            qb::io::async::coro_scheduler().spawn(connect_again(db_.get(), dsn_tcp_string(), &connect_ok, &connect_done));
            (void) pump_until([&] { return connect_done; }, 500ms);
            done_under_the_teardown = connect_done;
        });

    ASSERT_TRUE(pump_until([&] { return connect_done; }, 10s)) << "the connect() made in the error callback never completed";
    EXPECT_EQ(answered, 0);
    EXPECT_EQ(failures, 1) << "the query queued behind the terminated session fails once";
    EXPECT_FALSE(done_under_the_teardown) << "the new link came up inside the teardown of the link it replaces";
    EXPECT_TRUE(connect_ok);
    EXPECT_EQ(db_->state(), qb::io::connection_state::ready);

    int answer = -1;
    db_->execute(
        "SELECT 41 + 1", [&](qb::pg::transaction &, qb::pg::results r) { answer = r[0][0].as<int>(); },
        [&](qb::pg::error::db_error const &) { ++errors; });
    ASSERT_TRUE(pump_until([&] { return answer != -1 || errors != 0; }, 10s)) << "the new link answers nothing";
    EXPECT_EQ(answer, 42) << "the new link answers";
    EXPECT_EQ(errors, 0);
}

TEST_F(PgLifecycleInQueryCallback, ADisconnectThenAConnectFromAQueryCallbackEndsTheOldLinkThenBringsTheNewOneUp) {
    // No loop run under the callback: the connect starts at the scheduler's next drain, once the callback has returned
    // and the old link's teardown has run. Nothing is owed by then, and nothing may make the attempt wait.
    int  callbacks = 0, errors = 0, downs = 0;
    bool connect_ok = false, connect_done = false;
    db_->on_disconnected([&](qb::io::down_reason) { ++downs; });
    db_->execute(
        "SELECT 1",
        [&](qb::pg::transaction &, qb::pg::results) {
            ++callbacks;
            db_->disconnect();
            qb::io::async::coro_scheduler().spawn(connect_again(db_.get(), dsn_tcp_string(), &connect_ok, &connect_done));
        },
        [&](qb::pg::error::db_error const &) { ++errors; });

    ASSERT_TRUE(pump_until([&] { return connect_done; }, 10s)) << "the connect() made in the callback never completed";
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(downs, 1) << "the old link's end is told once";
    EXPECT_TRUE(connect_ok);
    EXPECT_EQ(db_->state(), qb::io::connection_state::ready);

    // And again on the new link: the end of each teardown leaves nothing owed.
    connect_ok = connect_done = false;
    db_->execute(
        "SELECT 2",
        [&](qb::pg::transaction &, qb::pg::results) {
            ++callbacks;
            db_->disconnect();
            qb::io::async::coro_scheduler().spawn(connect_again(db_.get(), dsn_tcp_string(), &connect_ok, &connect_done));
        },
        [&](qb::pg::error::db_error const &) { ++errors; });
    ASSERT_TRUE(pump_until([&] { return connect_done; }, 10s)) << "the second connect() never completed: a teardown left something owed";
    EXPECT_EQ(callbacks, 2);
    EXPECT_EQ(downs, 2);
    EXPECT_TRUE(connect_ok);

    int answer = -1;
    db_->execute(
        "SELECT 41 + 1", [&](qb::pg::transaction &, qb::pg::results r) { answer = r[0][0].as<int>(); },
        [&](qb::pg::error::db_error const &) { ++errors; });
    ASSERT_TRUE(pump_until([&] { return answer != -1 || errors != 0; }, 10s)) << "the new link answers nothing";
    EXPECT_EQ(answer, 42) << "the new link answers";
    EXPECT_EQ(errors, 0);
}

TEST_F(PgLifecycleInQueryCallback, AConnectMadeFromTheOnDisconnectedHookThatThenRunsTheLoopWaitsForTheTeardownToEnd) {
    // The hook runs inside the teardown, after the failure callbacks: a connect() made there, whose transport
    // completes while the hook runs the loop, is installed once the teardown is over.
    bool hooked = false, connect_ok = false, connect_done = false, done_under_the_hook = false;
    db_->on_disconnected([&](qb::io::down_reason) {
        if (hooked)
            return;
        hooked = true;
        qb::io::async::coro_scheduler().spawn(connect_again(db_.get(), dsn_tcp_string(), &connect_ok, &connect_done));
        (void) pump_until([&] { return connect_done; }, 500ms);
        done_under_the_hook = connect_done;
    });
    db_->execute("SELECT pg_terminate_backend(pg_backend_pid())", qb::pg::discard_query, qb::pg::discard_error);
    ASSERT_TRUE(pump_until([&] { return connect_done; }, 10s)) << "the connect() made in the hook never completed";
    EXPECT_FALSE(done_under_the_hook) << "the new link came up inside the teardown of the link it replaces";
    EXPECT_TRUE(connect_ok);
    EXPECT_EQ(db_->state(), qb::io::connection_state::ready);
}

TEST_F(PgLifecycleInQueryCallback, ADisconnectMadeFromAnErrorCallbackOfALostLinkStaysDownWithAPolicyArmed) {
    // An explicit disconnect() starts no reconnection run: the teardown of the lost link, which read the lifecycle's
    // epoch after the error callbacks, did not see it and went on to start one.
    int                              connected = 0;
    std::vector<qb::io::down_reason> downs;
    db_->on_connected([&] { ++connected; });
    db_->on_down([&](qb::io::down_reason r) { downs.push_back(r); });
    db_->enable_auto_reconnect(qb::pg::RetryPolicy{}.with_max_attempts(-1).with_initial_delay(20ms).with_max_delay(50ms).with_jitter(false));

    bool failed = false;
    db_->execute("SELECT pg_terminate_backend(pg_backend_pid())", qb::pg::discard_query, qb::pg::discard_error);
    db_->execute(
        "SELECT 1", [&](qb::pg::transaction &, qb::pg::results) {},
        [&](qb::pg::error::db_error const &) {
            if (failed)
                return;
            failed = true;
            db_->disconnect(); // the user ends the connection from the error callback
        });
    ASSERT_TRUE(pump_until([&] { return failed; }, 10s)) << "the queued query never failed";
    pump_for(800ms); // a run, if one started, reconnects in tens of milliseconds
    EXPECT_EQ(db_->state(), qb::io::connection_state::down) << "the explicit disconnect() was overridden";
    EXPECT_EQ(db_->down_reason(), qb::io::down_reason::user);
    EXPECT_EQ(connected, 0) << "an explicit disconnect() made in an error callback started a reconnection run";
    EXPECT_FALSE(db_->is_reconnecting());
    ASSERT_EQ(downs.size(), 1u);
    EXPECT_EQ(downs.front(), qb::io::down_reason::user);
}

TEST_F(PgLifecycleInQueryCallback, APrepareReconnectMadeInAQueryCallbackAfterItsDisconnectLeavesTheTeardownAlone) {
    // prepare_reconnect() closes the previous link's descriptor early and resets the buffers, the io state and the
    // per-backend state. Under the read dispatch of the link the callback has just ended, that emptied the buffer
    // the dispatch was reading, closed the descriptor under its watcher and zeroed the reason the teardown was owed
    // for: a build with assertions stopped in reset_io_state(), and a release build tore the link down on its next
    // read -- of the descriptor it had closed. It does nothing there: the teardown runs when the callback returns,
    // and releases the link itself.
    int         callbacks = 0, errors = 0, downs = 0;
    bool        open_before = false, open_after = false;
    std::size_t unread_before = 0u, unread_after = 0u;
    std::string version_before, version_after;
    db_->on_disconnected([&](qb::io::down_reason) { ++downs; });
    db_->execute(
        "SELECT 1",
        [&](qb::pg::transaction &, qb::pg::results) {
            ++callbacks;
            db_->disconnect();
            open_before    = db_->transport().is_open();
            unread_before  = db_->in().size();
            version_before = std::string(db_->parameter_status("server_version").value_or(""));
            db_->prepare_reconnect();
            open_after    = db_->transport().is_open();
            unread_after  = db_->in().size();
            version_after = std::string(db_->parameter_status("server_version").value_or(""));
        },
        [&](qb::pg::error::db_error const &) { ++errors; });

    ASSERT_TRUE(pump_until([&] { return callbacks == 1; }, 10s)) << "the query never completed";
    EXPECT_TRUE(open_before) << "the teardown of the link is owed, not made, while its dispatch is on the stack";
    EXPECT_TRUE(open_after) << "prepare_reconnect() closed the descriptor of the link whose dispatch was on the stack";
    EXPECT_NE(unread_before, 0u) << "the message being dispatched is still in the buffer: the case would test nothing";
    EXPECT_EQ(unread_after, unread_before) << "prepare_reconnect() emptied the buffer the dispatch on the stack was reading";
    EXPECT_FALSE(version_before.empty()) << "the server reported no version: the case would test nothing";
    EXPECT_EQ(version_after, version_before) << "prepare_reconnect() cleared the per-backend state under the teardown that owns it";
    // Not one more pass: the teardown ran in the pass that ran the callback, as soon as its dispatch returned.
    EXPECT_EQ(downs, 1) << "the teardown the dispatch owed did not run when the callback returned";
    EXPECT_EQ(db_->state(), qb::io::connection_state::down);
    EXPECT_EQ(db_->down_reason(), qb::io::down_reason::user);

    bool connect_ok = false, connect_done = false;
    qb::io::async::coro_scheduler().spawn(connect_again(db_.get(), dsn_tcp_string(), &connect_ok, &connect_done));
    ASSERT_TRUE(pump_until([&] { return connect_done; }, 10s)) << "the connect() made after it never completed";
    EXPECT_TRUE(connect_ok);
    EXPECT_EQ(downs, 1) << "the old link's end is told once";

    int answer = -1;
    db_->execute(
        "SELECT 41 + 1", [&](qb::pg::transaction &, qb::pg::results r) { answer = r[0][0].as<int>(); },
        [&](qb::pg::error::db_error const &) { ++errors; });
    ASSERT_TRUE(pump_until([&] { return answer != -1 || errors != 0; }, 10s)) << "the new link answers nothing";
    EXPECT_EQ(answer, 42) << "the new link answers";
    EXPECT_EQ(errors, 0);
}

TEST_F(PgLifecycleInQueryCallback, ADisconnectMadeByTheHookOfALostLinkStartsNoRunWhateverPolicyThatHookArms) {
    // disconnect() is terminal: it starts no reconnection run, whatever the policy. On a connection the loss had
    // already left down -- no policy armed when the link was lost -- it did nothing at all: a policy the same hook
    // armed then started a run, and the connection came back by itself.
    int                              connected = 0, hooks = 0;
    std::vector<qb::io::down_reason> downs;
    db_->on_connected([&] { ++connected; });
    db_->on_down([&](qb::io::down_reason r) { downs.push_back(r); });
    db_->on_disconnected([&](qb::io::down_reason) {
        ++hooks;
        db_->enable_auto_reconnect(
            qb::pg::RetryPolicy{}.with_max_attempts(-1).with_initial_delay(20ms).with_max_delay(50ms).with_jitter(false));
        db_->disconnect();
    });
    db_->execute("SELECT pg_terminate_backend(pg_backend_pid())", qb::pg::discard_query, qb::pg::discard_error);

    ASSERT_TRUE(pump_until([&] { return hooks == 1; }, 10s)) << "the link was never lost";
    pump_for(800ms); // a run, if one started, reconnects in tens of milliseconds
    EXPECT_EQ(db_->state(), qb::io::connection_state::down) << "the explicit disconnect() was overridden";
    EXPECT_EQ(connected, 0) << "a policy armed by the hook started a run after its disconnect()";
    EXPECT_FALSE(db_->is_reconnecting());
    ASSERT_EQ(downs.size(), 1u) << "down is told once";
    EXPECT_NE(downs.front(), qb::io::down_reason::user) << "down as the loss left it";

    // The next connect() is a new lifecycle: the mark does not outlive it, and the policy armed earlier recovers
    // the next loss.
    bool connect_ok = false, connect_done = false;
    qb::io::async::coro_scheduler().spawn(connect_again(db_.get(), dsn_tcp_string(), &connect_ok, &connect_done));
    ASSERT_TRUE(pump_until([&] { return connect_done; }, 10s));
    ASSERT_TRUE(connect_ok);
    hooks = 0;
    db_->on_disconnected([&](qb::io::down_reason) { ++hooks; });
    const int before = connected;
    db_->execute("SELECT pg_terminate_backend(pg_backend_pid())", qb::pg::discard_query, qb::pg::discard_error);
    ASSERT_TRUE(pump_until([&] { return hooks == 1; }, 10s)) << "the second link was never lost";
    ASSERT_TRUE(pump_until([&] { return connected == before + 1; }, 10s)) << "something was left of the disconnect(): no run";
}

TEST_F(PgLifecycleInQueryCallback, APolicyArmedByTheHookOfALostLinkStartsARun) {
    // The other half: a hook that arms a policy, and does not disconnect, has its run.
    int connected = 0, hooks = 0;
    db_->on_connected([&] { ++connected; });
    db_->on_disconnected([&](qb::io::down_reason) {
        ++hooks;
        db_->enable_auto_reconnect(
            qb::pg::RetryPolicy{}.with_max_attempts(-1).with_initial_delay(20ms).with_max_delay(50ms).with_jitter(false));
    });
    db_->execute("SELECT pg_terminate_backend(pg_backend_pid())", qb::pg::discard_query, qb::pg::discard_error);

    ASSERT_TRUE(pump_until([&] { return hooks == 1; }, 10s)) << "the link was never lost";
    ASSERT_TRUE(pump_until([&] { return connected == 1; }, 10s)) << "the policy the hook armed started no run";
    EXPECT_EQ(db_->state(), qb::io::connection_state::ready);
}

TEST_F(PgLifecycleInQueryCallback, ADisconnectMadeByAnErrorCallbackOfALostLinkStartsNoRunWhateverPolicyTheHookArms) {
    // The same from an error callback of the work the lost link carried, the hook arming the policy after it: the
    // loss is still deciding when the callback disconnects.
    int                              connected = 0, hooks = 0, failed = 0;
    std::vector<qb::io::down_reason> downs;
    db_->on_connected([&] { ++connected; });
    db_->on_down([&](qb::io::down_reason r) { downs.push_back(r); });
    db_->on_disconnected([&](qb::io::down_reason) {
        if (++hooks == 1)
            db_->enable_auto_reconnect(
                qb::pg::RetryPolicy{}.with_max_attempts(-1).with_initial_delay(20ms).with_max_delay(50ms).with_jitter(false));
    });
    db_->execute("SELECT pg_terminate_backend(pg_backend_pid())", qb::pg::discard_query, qb::pg::discard_error);
    db_->execute(
        "SELECT 1", [&](qb::pg::transaction &, qb::pg::results) {},
        [&](qb::pg::error::db_error const &) {
            if (++failed == 1)
                db_->disconnect();
        });

    ASSERT_TRUE(pump_until([&] { return hooks == 1; }, 10s)) << "the link was never lost";
    pump_for(800ms); // a run, if one started, reconnects in tens of milliseconds
    EXPECT_EQ(failed, 1);
    EXPECT_EQ(db_->state(), qb::io::connection_state::down);
    EXPECT_EQ(connected, 0) << "a policy the hook armed started a run after the error callback's disconnect()";
    ASSERT_EQ(downs.size(), 1u) << "down is told once";
    EXPECT_NE(downs.front(), qb::io::down_reason::user) << "down as the loss left it";
}

TEST_F(PgLifecycleInQueryCallback, ADisconnectMadeOnAConnectionALossLeftDownEndsTheSession) {
    // disconnect() ends the session: the listened channels are forgotten, and the next connect() is a fresh session
    // -- no LISTEN replayed, no reconnection counted. On a connection a loss had already left down it did nothing.
    ASSERT_TRUE(db_->listen("lot3_lifecycle_chan", qb::pg::discard_query, qb::pg::discard_error).await());
    ASSERT_EQ(db_->listened_channels().size(), 1u);
    int hooks = 0;
    db_->on_disconnected([&](qb::io::down_reason) {
        if (++hooks == 1)
            db_->disconnect();
    });
    const auto reconnects = db_->stats().reconnects;
    db_->execute("SELECT pg_terminate_backend(pg_backend_pid())", qb::pg::discard_query, qb::pg::discard_error);
    ASSERT_TRUE(pump_until([&] { return hooks == 1; }, 10s)) << "the link was never lost";
    EXPECT_TRUE(db_->listened_channels().empty()) << "an explicit disconnect() ends the session: the listened set is forgotten";

    bool connect_ok = false, connect_done = false;
    qb::io::async::coro_scheduler().spawn(connect_again(db_.get(), dsn_tcp_string(), &connect_ok, &connect_done));
    ASSERT_TRUE(pump_until([&] { return connect_done; }, 10s));
    ASSERT_TRUE(connect_ok);
    pump_for(100ms);
    EXPECT_TRUE(db_->listened_channels().empty()) << "a LISTEN of the session that was ended was replayed";
    EXPECT_EQ(db_->stats().reconnects, reconnects) << "the next connect() is a fresh session, not a reconnection";
    db_->on_disconnected({});
}
