/**
 * @file connect-timeout.cpp
 * @brief System-tier test: distinguish a configured handshake deadline from native
 *        network failure without an external PostgreSQL daemon.
 *
 * Moved out of `test-connection.cpp` / `test-connection-ssl.cpp`. The legacy versions
 * asserted a hard-coded wall-clock bound (`duration < 13s`) which is both flaky (machine /
 * SYN-backoff dependent) and disconnected from the value the client was actually told to use.
 *
 * A silent local fake backend accepts TCP and the StartupMessage but never answers it.
 * It stays open until the client has failed, so only the configured handshake deadline
 * can finish that attempt. TEST-NET-1 remains a separate native-failure smoke.
 *
 * Labeled `slow` (network-timing dependent) and kept OUT of the live-PG RESOURCE_LOCK lane.
 * If the dead host happens to be reachable in some exotic network, the case skips.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <qb/io/async.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qbm/pgsql/pgsql.h>

#include "../../shared/pg_fake_backend.h"

using namespace qb::pg;
using namespace std::chrono_literals;

namespace {

/// RFC 5737 TEST-NET-1: guaranteed not to host a real PostgreSQL server.
constexpr std::string_view kUnreachableDsn = "tcp://test:test@192.0.2.1:5432[test]";

/// Short, explicit handshake deadline so the test is bounded by a value we control.
constexpr auto kConfiguredTimeout = std::chrono::seconds(3);

/**
 * @brief Keep the TEST-NET-1 native-failure smoke separate from the deadline witness.
 * @tparam Database  qb::pg::tcp::database (cleartext) or its ssl:: variant.
 */
template <typename Database>
void
assert_connect_times_out() {
    auto db = std::make_unique<Database>();

    auto opts            = connection_options::parse(std::string{kUnreachableDsn});
    opts.connect_timeout = kConfiguredTimeout;

    const auto start  = std::chrono::steady_clock::now();
    const bool result = qb::io::async::run_sync(db->connect(opts));
    const auto end    = std::chrono::steady_clock::now();

    if (result) {
        GTEST_SKIP() << "192.0.2.1:5432 unexpectedly accepted a connection; network is not "
                        "RFC-5737 clean. Skipping timeout assertion.";
        return;
    }
    ASSERT_FALSE(result) << "connect to an unreachable host must fail";

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // A hard native error may return immediately. This case only bounds failure time;
    // SilentLoopbackBackendExhaustsHandshakeDeadline proves the timer fired.
    const auto configured_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kConfiguredTimeout);
    // Upper bound: configured deadline + generous scheduling/teardown slack (NOT a magic 13s).
    EXPECT_LT(elapsed.count(), configured_ms.count() + 4000)
        << "connect overshot the configured " << configured_ms.count() << "ms deadline by too much (elapsed=" << elapsed.count() << "ms)";
}

} // namespace

/// A TEST-NET-1 attempt fails within its configured budget, including native errors.
TEST(ConnectTimeout, CleartextDeadHostFailsWithinBudget) {
    qb::io::async::init();
    assert_connect_times_out<qb::pg::tcp::database>();
}

#ifdef QB_HAS_SSL
/// TLS transport has the same bounded native-failure smoke.
TEST(ConnectTimeout, SslDeadHostFailsWithinBudget) {
    qb::io::async::init();
    assert_connect_times_out<qb::pg::tcp::ssl::database>();
}

/// The `ssl://` database builds a value-semantic `qb::io::ssl::Context` from the connection options; a bad
/// private-CA / client-certificate path makes that Context `!ok()`, so the connect fails CLOSED — it never
/// silently connects WITHOUT the requested TLS material. Covers the `ssl_root_cert` / `ssl_cert` / `ssl_key`
/// branches (the timeout cases above leave them empty) and the fail-closed integration. No live server: the
/// broken Context is rejected before any TCP/TLS, so this returns fast (well under the connect deadline).
TEST(ConnectTimeout, SslBadTlsMaterialFailsClosed) {
    qb::io::async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);
    auto db              = std::make_unique<qb::pg::tcp::ssl::database>();
    auto opts            = connection_options::parse("tcp://test:test@127.0.0.1:" + std::to_string(port) + "[test]");
    opts.connect_timeout = kConfiguredTimeout;
    opts.ssl_verify      = ssl_verify_mode::full;
    opts.ssl_root_cert   = "qb-nonexistent-private-ca.pem"; // Context::trust() fails -> Context !ok()
    opts.ssl_cert        = "qb-nonexistent-client.pem";     // (identity() is on the same fail-closed path)
    opts.ssl_key         = "qb-nonexistent-client.key";
    EXPECT_FALSE(qb::io::async::run_sync(db->connect(opts))) << "a bad TLS material path must fail the connect closed";
    EXPECT_LE(qb::io::socket::handle_read_ready(listener.native_handle(), 100ms), 0)
        << "bad TLS material opened a socket instead of failing locally";
    listener.disconnect();
}

#endif

TEST(ConnectTimeout, SilentLoopbackBackendExhaustsHandshakeDeadline) {
    qb::io::async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<bool> accepted{false};
    std::atomic<bool> startup_received{false};
    std::jthread      backend([&](std::stop_token stop) {
        if (qb::io::socket::handle_read_ready(listener.native_handle(), 5s) <= 0)
            return;
        qb::io::tcp::socket peer;
        if (listener.accept(peer) != qb::io::SocketStatus::Done)
            return;
        accepted = true;
        std::vector<std::uint8_t> startup;
        startup_received = qb::pg::test::fake::read_startup(peer, startup);
        while (!stop.stop_requested())
            std::this_thread::sleep_for(1ms);
        peer.disconnect();
    });

    qb::pg::tcp::database db;
    auto                  opts = connection_options::parse("tcp://test:test@127.0.0.1:" + std::to_string(port) + "[test]");
    opts.connect_timeout       = 250ms;
    const auto started         = std::chrono::steady_clock::now();
    const bool connected       = qb::io::async::run_sync(db.connect(opts));
    const auto elapsed         = std::chrono::steady_clock::now() - started;
    db.disconnect();
    backend.request_stop();
    backend.join();
    listener.disconnect();
    EXPECT_FALSE(connected) << "silent backend never sent AuthenticationOk or ReadyForQuery";
    EXPECT_TRUE(accepted.load()) << "the TCP phase did not complete, so this did not test the PostgreSQL handshake timer";
    EXPECT_TRUE(startup_received.load()) << "the fake backend did not receive a complete StartupMessage";
    EXPECT_GE(elapsed, 150ms) << "failure preceded the configured handshake deadline";
    EXPECT_LT(elapsed, 5s) << "configured handshake deadline did not bound the attempt";
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
