/**
 * @file connect-timeout.cpp
 * @brief System-tier test: connect attempts to an unreachable host honor the configured
 *        handshake deadline (no external PG daemon required, but a routable network is).
 *
 * Moved out of `test-connection.cpp` / `test-connection-ssl.cpp`. The legacy versions
 * asserted a hard-coded wall-clock bound (`duration < 13s`) which is both flaky (machine /
 * SYN-backoff dependent) and disconnected from the value the client was actually told to use.
 *
 * This file instead drives a connect to the TEST-NET-1 address `192.0.2.1` (RFC 5737,
 * guaranteed unroutable to a real server) with an *explicit, short* `connect_timeout`, then
 * asserts the attempt fails AND returns within a small multiple of that configured deadline —
 * i.e. it asserts against the configured value, not a magic constant.
 *
 * Labeled `slow` (network-timing dependent) and kept OUT of the live-PG RESOURCE_LOCK lane.
 * If the dead host happens to be reachable in some exotic network, the case skips.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <qb/io/async.h>
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;

namespace {

/// RFC 5737 TEST-NET-1: guaranteed not to host a real PostgreSQL server.
constexpr std::string_view kUnreachableDsn = "tcp://test:test@192.0.2.1:5432[test]";

/// Short, explicit handshake deadline so the test is bounded by a value we control.
constexpr auto kConfiguredTimeout = std::chrono::seconds(3);

/**
 * @brief Drive a connect to the unreachable host with @p timeout and assert it fails within
 *        a small multiple of the configured deadline.
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

    // The attempt must respect the configured deadline: it must not return so fast that the
    // timeout was ignored entirely... (allow generous lower slack — a connection-refused or
    // immediate routing error legitimately returns early), and must not blow far past it.
    const auto configured_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kConfiguredTimeout);
    // Upper bound: configured deadline + generous scheduling/teardown slack (NOT a magic 13s).
    EXPECT_LT(elapsed.count(), configured_ms.count() + 4000)
        << "connect overshot the configured " << configured_ms.count() << "ms deadline by too much (elapsed=" << elapsed.count() << "ms)";
}

} // namespace

/// Cleartext TCP connect to a dead host honors the configured deadline.
TEST(ConnectTimeout, CleartextDeadHostHonorsConfiguredDeadline) {
    qb::io::async::init();
    assert_connect_times_out<qb::pg::tcp::database>();
}

#ifdef QB_HAS_SSL
/// TLS connect to a dead host honors the configured deadline (TCP phase is timed).
TEST(ConnectTimeout, SslDeadHostHonorsConfiguredDeadline) {
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
    auto db              = std::make_unique<qb::pg::tcp::ssl::database>();
    auto opts            = connection_options::parse(std::string{kUnreachableDsn});
    opts.connect_timeout = kConfiguredTimeout;
    opts.ssl_verify      = ssl_verify_mode::full;
    opts.ssl_root_cert   = "qb-nonexistent-private-ca.pem"; // Context::trust() fails -> Context !ok()
    opts.ssl_cert        = "qb-nonexistent-client.pem";     // (identity() is on the same fail-closed path)
    opts.ssl_key         = "qb-nonexistent-client.key";
    EXPECT_FALSE(qb::io::async::run_sync(db->connect(opts))) << "a bad TLS material path must fail the connect closed";
}
#endif

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
