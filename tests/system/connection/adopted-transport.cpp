/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file adopted-transport.cpp
 * @brief System: `connect(dsn, raw_io)` runs the handshake on the transport it is given (Huly QB-1096).
 *
 * The overload adopts an already-connected transport (a socket from a pool, one a supervisor handed
 * over) for the first connection of a lifecycle. Fake backends on loopback listeners answer the
 * handshake; the DSN names another address, so a client that dialled the DSN instead of adopting fails,
 * and the backend behind the given transport never reads a StartupMessage. The cases: adoption; a
 * reconnection run, which dials the DSN; a transport that is not open (down, peer); a client already
 * ready (false, the transport closed unused); and, with OpenSSL, a secure database adopting a transport
 * whose PostgreSQL TLS negotiation the caller ran. No daemon.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include <qbm/pgsql/pgsql.h>

#include "../../shared/pg_fake_backend.h"
#include "../../shared/pg_fake_tls.h"
#ifdef QB_HAS_SSL
#include "ssl_fixtures.h"
#endif

using namespace std::chrono_literals;
using namespace qb::pg::test::fake;

namespace adopted_transport_test {

/// What one fake backend saw on the one connection it accepted.
struct Seen {
    std::atomic<bool> accepted{false};
    std::atomic<bool> got_startup{false};
    std::atomic<bool> handshake_sent{false};
    std::atomic<bool> closed_by_client{false};
};

/// AuthenticationOk, BackendKeyData(pid, 77), ReadyForQuery('I').
std::vector<std::vector<std::uint8_t>>
handshake_replies(std::uint32_t pid) {
    std::vector<std::uint8_t> auth_ok;
    put_i32(auth_ok, 0);
    std::vector<std::uint8_t> key;
    put_i32(key, pid);
    put_i32(key, 77);
    return {backend_msg('R', auth_ok), backend_msg('K', key), backend_msg('Z', std::vector<std::uint8_t>{'I'})};
}

/// Hold an open link until the client closes it, the test stops, or (drop_after) the backend drops it.
void
hold(qb::io::tcp::socket &peer, Seen *seen, std::atomic<bool> *stop, std::atomic<bool> *drop_now) {
    while (!stop->load() && !(drop_now && drop_now->load())) {
        const auto r = read_one(peer, 20ms);
        if (r == peer_read_outcome::eof || r == peer_read_outcome::error) {
            seen->closed_by_client = true;
            break;
        }
    }
    peer.disconnect();
}

/// Accept one connection; answer the handshake unless `answer` is false (then only watch how it ends).
void
backend(qb::io::tcp::listener *listener, Seen *seen, std::atomic<bool> *stop, std::uint32_t pid, bool answer,
        std::atomic<bool> *drop_now = nullptr) {
    // Wait in short slices, so a backend that must see no connection ends once the test says stop; a
    // pending connection is looked for before `stop`, so one the client made is never missed.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (qb::io::socket::handle_read_ready(listener->native_handle(), 20ms) <= 0)
        if (stop->load() || std::chrono::steady_clock::now() >= deadline)
            return;
    qb::io::tcp::socket peer;
    if (listener->accept(peer) != qb::io::SocketStatus::Done)
        return;
    seen->accepted = true;
    if (!answer) {
        // A transport the client must close unused: it ends with EOF and no StartupMessage.
        std::uint8_t first = 0;
        while (!stop->load()) {
            if (qb::io::socket::handle_read_ready(peer.native_handle(), 20ms) <= 0)
                continue;
            const int n = peer.read(&first, 1);
            if (n > 0)
                seen->got_startup = true; // bytes arrived: the client wrote on a transport it should have closed
            else
                seen->closed_by_client = true;
            break;
        }
        peer.disconnect();
        return;
    }
    std::vector<std::uint8_t> startup;
    if (!read_startup(peer, startup))
        return;
    seen->got_startup = true;
    for (auto const &m : handshake_replies(pid))
        if (!send_all(peer, m))
            return;
    seen->handshake_sent = true;
    hold(peer, seen, stop, drop_now);
}

/// A listener on an ephemeral loopback port.
struct Loopback {
    qb::io::tcp::listener listener;
    std::uint16_t         port = 0;

    bool
    open() {
        if (listener.listen_v4(0, "127.0.0.1") != qb::io::SocketStatus::Done)
            return false;
        port = listener.local_endpoint().port();
        return port != 0;
    }
};

std::string
dsn_at(std::uint16_t port) {
    return "tcp://test:test@127.0.0.1:" + std::to_string(port) + "[test]";
}

template <typename Predicate>
bool
pump_until(Predicate predicate, std::chrono::milliseconds limit = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

} // namespace adopted_transport_test

// The first connection runs on the given transport; the DSN (port 1, nothing listens) is not dialled.
TEST(PgsqlAdoptedTransport, TheHandshakeRunsOnTheSuppliedTransport) {
    using namespace adopted_transport_test;
    qb::io::async::init();
    Loopback          backend_a;
    Seen              seen;
    std::atomic<bool> stop{false};
    ASSERT_TRUE(backend_a.open());
    std::thread server(backend, &backend_a.listener, &seen, &stop, 4242u, true, nullptr);

    qb::io::tcp::socket supplied;
    ASSERT_EQ(supplied.connect_v4("127.0.0.1", backend_a.port), 0) << "could not open the transport to hand over";
    qb::pg::tcp::database db;
    const bool            connected = static_cast<bool>(qb::io::async::run_sync(db.connect(dsn_at(1), std::move(supplied))));
    const int             pid       = db.backend_pid();
    db.disconnect();
    stop = true;
    server.join();
    backend_a.listener.disconnect();

    EXPECT_TRUE(seen.accepted.load()) << "the supplied transport never reached the backend";
    EXPECT_TRUE(seen.got_startup.load()) << "the StartupMessage was not sent on the supplied transport";
    EXPECT_TRUE(connected) << "connect(dsn, raw_io) opened its own socket instead of using the one it was given";
    EXPECT_EQ(pid, 4242) << "the session is not the one the supplied transport carries";
}

// Only the first connection adopts: when the adopted link drops, the reconnection run dials the DSN.
TEST(PgsqlAdoptedTransport, AReconnectionRunDialsTheDsn) {
    using namespace adopted_transport_test;
    qb::io::async::init();
    Loopback          adopted_end, dsn_end;
    Seen              seen_adopted, seen_dsn;
    std::atomic<bool> stop{false}, drop_adopted{false};
    ASSERT_TRUE(adopted_end.open());
    ASSERT_TRUE(dsn_end.open());
    std::thread server_a(backend, &adopted_end.listener, &seen_adopted, &stop, 1111u, true, &drop_adopted);
    std::thread server_b(backend, &dsn_end.listener, &seen_dsn, &stop, 2222u, true, nullptr);

    qb::io::tcp::socket supplied;
    ASSERT_EQ(supplied.connect_v4("127.0.0.1", adopted_end.port), 0);
    qb::pg::tcp::database db;
    int                   connected = 0;
    db.on_connected([&] { ++connected; });
    db.enable_auto_reconnect(qb::pg::RetryPolicy{}.with_initial_delay(20ms).with_jitter(false));
    ASSERT_TRUE(qb::io::async::run_sync(db.connect(dsn_at(dsn_end.port), std::move(supplied))));
    EXPECT_EQ(db.backend_pid(), 1111);
    drop_adopted           = true; // the adopted link's backend goes away
    const bool reconnected = pump_until([&] { return connected == 2; });
    const int  pid         = db.backend_pid();
    db.disconnect();
    stop = true;
    server_a.join();
    server_b.join();
    adopted_end.listener.disconnect();
    dsn_end.listener.disconnect();

    EXPECT_TRUE(reconnected) << "the run never reconnected";
    EXPECT_TRUE(seen_dsn.got_startup.load()) << "the reconnection did not dial the DSN";
    EXPECT_EQ(pid, 2222) << "the reconnected session is not the DSN's backend";
}

// A transport that is not open fails the attempt at once: down, for a peer reason, the DSN not dialled.
TEST(PgsqlAdoptedTransport, AClosedTransportGoesDownForAPeerReason) {
    using namespace adopted_transport_test;
    qb::io::async::init();
    Loopback          dsn_end;
    Seen              seen_dsn;
    std::atomic<bool> stop{false};
    ASSERT_TRUE(dsn_end.open());
    std::thread server(backend, &dsn_end.listener, &seen_dsn, &stop, 3333u, true, nullptr);

    qb::pg::tcp::database db;
    qb::io::tcp::socket   never_opened;
    const bool            connected = static_cast<bool>(qb::io::async::run_sync(db.connect(dsn_at(dsn_end.port), std::move(never_opened))));
    const auto            state     = db.state();
    const auto            reason    = db.down_reason();
    stop                            = true;
    server.join();
    dsn_end.listener.disconnect();

    EXPECT_FALSE(connected);
    EXPECT_EQ(state, qb::io::connection_state::down);
    EXPECT_EQ(reason, qb::io::down_reason::peer);
    EXPECT_FALSE(seen_dsn.accepted.load()) << "a failed adoption dialled the DSN instead";
}

// On a ready client the call adopts nothing: it resolves false and closes the moved-in transport unused.
TEST(PgsqlAdoptedTransport, AReadyClientClosesTheSuppliedTransportUnused) {
    using namespace adopted_transport_test;
    qb::io::async::init();
    Loopback          live_end, spare_end;
    Seen              seen_live, seen_spare;
    std::atomic<bool> stop{false};
    ASSERT_TRUE(live_end.open());
    ASSERT_TRUE(spare_end.open());
    std::thread server_live(backend, &live_end.listener, &seen_live, &stop, 5555u, true, nullptr);
    std::thread server_spare(backend, &spare_end.listener, &seen_spare, &stop, 6666u, false, nullptr);

    qb::pg::tcp::database db;
    ASSERT_TRUE(qb::io::async::run_sync(db.connect(dsn_at(live_end.port))));
    qb::io::tcp::socket spare;
    ASSERT_EQ(spare.connect_v4("127.0.0.1", spare_end.port), 0);
    const bool adopted = static_cast<bool>(qb::io::async::run_sync(db.connect(dsn_at(live_end.port), std::move(spare))));
    const bool closed  = pump_until([&] { return seen_spare.closed_by_client.load(); });
    const bool ready   = db.is_connected();
    const int  pid     = db.backend_pid();
    db.disconnect();
    stop = true;
    server_live.join();
    server_spare.join();
    live_end.listener.disconnect();
    spare_end.listener.disconnect();

    EXPECT_FALSE(adopted) << "a ready client reported the transport adopted";
    EXPECT_TRUE(closed) << "the transport was not closed";
    EXPECT_FALSE(seen_spare.got_startup.load()) << "the client wrote on a transport it did not adopt";
    EXPECT_TRUE(ready) << "the live connection was disturbed";
    EXPECT_EQ(pid, 5555);
}

#ifdef QB_HAS_SSL
// A secure database adopts a transport whose PostgreSQL TLS negotiation the caller ran -- the SSLRequest,
// the server's 'S', the TLS handshake -- and writes its StartupMessage on it at once, encrypted.
TEST(PgsqlAdoptedTransport, ASecureDatabaseAdoptsATlsNegotiatedTransport) {
    using namespace adopted_transport_test;
    qb::io::async::init();
    if (!qb::io::test::ssl_files_available())
        GTEST_SKIP() << "qb's test TLS certificate is not available";
    auto server_tls = qb::io::ssl::Context::server(qb::io::test::ssl_resource_path("cert.pem"), qb::io::test::ssl_resource_path("key.pem"));
    ASSERT_TRUE(server_tls.ok());
    Loopback          tls_end;
    std::atomic<bool> got_startup{false}, stop{false};
    ASSERT_TRUE(tls_end.open());
    std::thread server([&] {
        qb::io::tcp::ssl::socket session;
        if (!accept_postgres_tls(tls_end.listener, server_tls, session))
            return;
        std::uint8_t length[4];
        if (!tls_recv_exact(session, length, sizeof length) || get_i32(length) < 8u || get_i32(length) > 65536u)
            return;
        std::vector<std::uint8_t> startup(get_i32(length) - 4u);
        if (!tls_recv_exact(session, startup.data(), startup.size()))
            return;
        got_startup = true;
        for (auto const &m : handshake_replies(7777u))
            if (!tls_send_all(session, m))
                return;
        while (!stop.load())
            std::this_thread::sleep_for(5ms);
    });

    qb::io::tcp::socket plain;
    ASSERT_EQ(plain.connect_v4("127.0.0.1", tls_end.port), 0);
    auto client_tls = qb::io::ssl::Context::client();
    client_tls.verify(qb::io::ssl::VerifyMode::none);
    qb::io::tcp::ssl::socket negotiated;
    ASSERT_TRUE(negotiate_postgres_tls(plain, client_tls, negotiated)) << "the test could not run the TLS negotiation it hands over";

    qb::pg::tcp::ssl::database db;
    const bool                 connected = static_cast<bool>(qb::io::async::run_sync(db.connect(dsn_at(1), std::move(negotiated))));
    const int                  pid       = db.backend_pid();
    db.disconnect();
    stop = true;
    server.join();
    tls_end.listener.disconnect();

    EXPECT_TRUE(got_startup.load()) << "the StartupMessage did not arrive over the negotiated TLS session";
    EXPECT_TRUE(connected);
    EXPECT_EQ(pid, 7777);
}
#endif

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
