/**
 * @file qbm/pgsql/tests/system/connection/cancel-request-wire.cpp
 * @brief System test: the out-of-band CancelRequest on the wire, `cancel_async()` next to `cancel()`,
 *        without a daemon (Huly QB-113).
 *
 * A fake PostgreSQL backend on a background thread (the shared `pg_fake_backend.hpp` helpers: qb's
 * own cross-platform `qb::io::tcp` sockets, an ephemeral loopback port) speaks just enough of the
 * wire protocol to hand the client a session --
 *   Startup  ->  AuthenticationOk(0), BackendKeyData(K: pid, secret), ReadyForQuery('I')
 * -- then accepts the SECOND connection the cancel opens and reads what it carries. What is pinned:
 *   - `cancel_async()` delivers exactly the 16-byte CancelRequest (length 16, code 80877102, the
 *     pid and the secret the backend handed out) and reports true;
 *   - it does so WITHOUT blocking the loop: a callback deferred to the loop's next turn before the
 *     cancel runs before the cancel completes, which the blocking `cancel()` can never let happen;
 *   - `cancel()` sends the same bytes (the two variants share the packet) -- so a caller moving
 *     from the blocking to the non-blocking form changes nothing on the wire;
 *   - both report false, and open no socket, on a never-connected database.
 * The TLS half of QB-113 (a secure database negotiating SSLRequest -> TLS on the cancel
 * connection) needs a TLS-speaking peer and is proven against a live server in
 * `integration/connection/connection-ssl.cpp`.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include <qbm/pgsql/pgsql.h>

#include "../../shared/pg_fake_backend.hpp"

using namespace qb::pg;
using namespace qb::pg::test::fake;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kBackendPid    = 4242;
constexpr std::uint32_t kBackendSecret = 0x0BADF00Du;

struct CancelWire {
    std::atomic<bool>         session_up{false};  ///< the fake answered the startup (Ok + K + Z)
    std::atomic<bool>         second_conn{false}; ///< the cancel connection was accepted
    std::atomic<bool>         got_16{false};      ///< 16 bytes arrived on it
    std::atomic<bool>         then_eof{false};    ///< and nothing followed (the client closed)
    std::vector<std::uint8_t> bytes;              ///< what arrived on the cancel connection
};

// Accept the session, hand out the key, then accept the cancel connection and record its bytes.
void
run_cancel_backend(qb::io::tcp::listener &listener, CancelWire *res) {
    if (qb::io::socket::handle_read_ready(listener.native_handle(), std::chrono::seconds(10)) <= 0)
        return;
    qb::io::tcp::socket session;
    if (listener.accept(session) != qb::io::SocketStatus::Done)
        return;

    std::vector<std::uint8_t> startup;
    if (!read_startup(session, startup)) {
        session.disconnect();
        return;
    }
    {
        std::vector<std::uint8_t> ok;
        put_i32(ok, 0); // AuthenticationOk
        std::vector<std::uint8_t> key;
        put_i32(key, kBackendPid);
        put_i32(key, kBackendSecret);
        std::vector<std::uint8_t> ready{static_cast<std::uint8_t>('I')};
        if (!send_all(session, backend_msg('R', ok)) || !send_all(session, backend_msg('K', key))
            || !send_all(session, backend_msg('Z', ready))) {
            session.disconnect();
            return;
        }
    }
    res->session_up = true;

    // The cancel connection: a SECOND accept on the same listener.
    if (qb::io::socket::handle_read_ready(listener.native_handle(), std::chrono::seconds(10)) <= 0) {
        session.disconnect();
        return;
    }
    qb::io::tcp::socket cancel;
    if (listener.accept(cancel) != qb::io::SocketStatus::Done) {
        session.disconnect();
        return;
    }
    res->second_conn = true;
    std::uint8_t buf[16];
    if (recv_exact(cancel, buf, sizeof buf)) {
        res->bytes.assign(buf, buf + sizeof buf);
        res->got_16   = true;
        res->then_eof = peer_stays_silent(cancel, 500ms);
    }
    cancel.disconnect();
    session.disconnect();
}

/// The session against the fake backend: connected, and the BackendKeyData landed.
[[nodiscard]] bool
open_session(qb::pg::tcp::database &db, std::uint16_t port) {
    const std::string dsn = "tcp://test:test@127.0.0.1:" + std::to_string(port) + "[test]";
    if (!static_cast<bool>(qb::io::async::run_sync(db.connect(dsn))))
        return false;
    // AuthenticationOk resumes connect(); the K that follows is processed by the next passes.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (db.backend_pid() != static_cast<int>(kBackendPid) && std::chrono::steady_clock::now() < deadline)
        qb::io::async::run(EVRUN_NOWAIT);
    return db.backend_pid() == static_cast<int>(kBackendPid);
}

void
expect_cancel_request(const std::vector<std::uint8_t> &bytes) {
    ASSERT_EQ(bytes.size(), 16u);
    EXPECT_EQ(get_i32(bytes.data() + 0), 16u) << "CancelRequest length";
    EXPECT_EQ(get_i32(bytes.data() + 4), 80877102u) << "CancelRequest code (0x04D2162E)";
    EXPECT_EQ(get_i32(bytes.data() + 8), kBackendPid) << "the backend pid the session was handed";
    EXPECT_EQ(get_i32(bytes.data() + 12), kBackendSecret) << "the secret the session was handed";
}

} // namespace

TEST(PgsqlCancelWire, CancelAsyncSendsTheCancelRequestOnASecondConnectionWithoutBlockingTheLoop) {
    qb::io::async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    CancelWire  res;
    std::thread backend(run_cancel_backend, std::ref(listener), &res);

    bool delivered = false;
    int  ticks     = 0;
    {
        qb::pg::tcp::database db;
        ASSERT_TRUE(open_session(db, port)) << "the fake backend's AuthenticationOk + BackendKeyData never made a session";
        EXPECT_TRUE(res.session_up.load());

        // Liveness witness: a deferred callback runs on the loop's next turn. A cancel that blocked
        // the loop until its socket was connected and written would finish before that turn.
        bool ticked_before_cancel_completed = false;
        qb::io::async::defer([&] { ++ticks; });
        delivered = qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
            const bool ok                  = co_await db.cancel_async();
            ticked_before_cancel_completed = ticks > 0;
            co_return ok;
        }());
        EXPECT_TRUE(ticked_before_cancel_completed) << "cancel_async() completed before the loop ran anything else: it blocked";
        db.disconnect();
    }
    backend.join();
    listener.disconnect();

    EXPECT_TRUE(delivered) << "cancel_async() must report the request delivered";
    ASSERT_TRUE(res.second_conn.load()) << "no second connection reached the backend";
    ASSERT_TRUE(res.got_16.load()) << "the cancel connection did not carry 16 bytes";
    expect_cancel_request(res.bytes);
    EXPECT_TRUE(res.then_eof.load()) << "the cancel connection carried more than the CancelRequest, or stayed open";
}

TEST(PgsqlCancelWire, BlockingCancelSendsTheSameBytes) {
    qb::io::async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    CancelWire  res;
    std::thread backend(run_cancel_backend, std::ref(listener), &res);

    bool delivered = false;
    {
        qb::pg::tcp::database db;
        ASSERT_TRUE(open_session(db, port));
        delivered = db.cancel();
        db.disconnect();
    }
    backend.join();
    listener.disconnect();

    EXPECT_TRUE(delivered);
    ASSERT_TRUE(res.got_16.load());
    expect_cancel_request(res.bytes);
    EXPECT_TRUE(res.then_eof.load());
}

TEST(PgsqlCancelWire, NeverConnectedReportsFalseAndOpensNothing) {
    qb::io::async::init();
    // A listener nobody should reach: the never-connected database has no endpoint AND no key.
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);

    qb::pg::tcp::database db;
    EXPECT_FALSE(db.cancel());
    EXPECT_FALSE(qb::io::async::run_sync(db.cancel_async()));
    // Nothing knocked: the listener has no pending connection.
    EXPECT_LE(qb::io::socket::handle_read_ready(listener.native_handle(), 100ms), 0);
    listener.disconnect();
}
