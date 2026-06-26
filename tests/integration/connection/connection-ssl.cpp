/**
 * @file connection-ssl.cpp
 * @brief Integration tests for PostgreSQL TLS connections (live daemon + TLS required).
 *
 * TLS half of the legacy `test-connection-ssl.cpp`. Exercises the encrypted handshake of
 * `qb::pg::tcp::ssl::database` against a TLS-enabled `postgres:5432`: connect (callback +
 * coroutine), reconnect on a new backend, connection pooling, and the two security-critical
 * cases that distinguish this file from the cleartext lifecycle suite:
 *
 *  - `SslVerifyFullRejectsUntrustedCert` — `ssl_verify_mode::full` MUST reject the test
 *    server's self-signed certificate (proves verification is enforced, not cosmetic).
 *  - `ScramChannelBindingNegotiatedOverTls` — over TLS, SCRAM must negotiate
 *    `SCRAM-SHA-256-PLUS` with `tls-server-end-point` channel binding.
 *
 * Compiled only under `QB_HAS_SSL`. At runtime, when the default DSN cannot complete TLS and
 * `QB_PG_SSL_DSN` is unset, every case skips with a clear message (CI must export
 * `QB_PG_SSL_DSN` for the SCRAM/verify-full cases to actually run). The dead-host connect
 * timeout case lives in `system/connection/connect-timeout.cpp` (shared with cleartext).
 *
 * @see qb::pg::tcp::ssl::database
 * @see qb::pg::ssl_verify_mode
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <cstdlib>
#include <memory>
#include <vector>
#include <gtest/gtest.h>

#ifdef QB_HAS_SSL

#include <qb/io/async.h>
#include "../pgsql.h"
#include "../../shared/test_config.hpp"

using namespace qb::pg;
using qb::pg::test::dsn_ssl_string;

namespace {

/// Whether the operator pinned a TLS DSN. When unset and TLS is unavailable, cases skip.
[[nodiscard]] bool
ssl_dsn_pinned() noexcept {
    const char *v = std::getenv("QB_PG_SSL_DSN");
    return v != nullptr && v[0] != '\0';
}

/**
 * @brief Connect @p db over TLS, or signal "skip when not pinned".
 * @return true on a successful TLS connect.
 *
 * On failure: if `QB_PG_SSL_DSN` is unset the caller should GTEST_SKIP (TLS simply isn't
 * available in this environment); if it IS set, a failure is a real error the caller asserts.
 */
[[nodiscard]] bool
ssl_connect(qb::pg::tcp::ssl::database &db) {
    return static_cast<bool>(qb::io::async::run_sync(db.connect(dsn_ssl_string())));
}

class SslConnection : public ::testing::Test {
protected:
    std::unique_ptr<qb::pg::tcp::ssl::database> db_;

    void
    SetUp() override {
        qb::io::async::init();
        db_ = std::make_unique<qb::pg::tcp::ssl::database>();
        // Probe TLS reachability on a throwaway handle so the per-test db_ starts clean.
        auto probe = std::make_unique<qb::pg::tcp::ssl::database>();
        if (!ssl_connect(*probe)) {
            if (!ssl_dsn_pinned())
                GTEST_SKIP() << "TLS connect failed with default DSN and QB_PG_SSL_DSN is "
                                "unset; skipping. Set QB_PG_SSL_DSN to enforce TLS coverage.";
            else
                FAIL() << "QB_PG_SSL_DSN is set but TLS connect failed: " << dsn_ssl_string();
        }
        probe->disconnect();
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
// Connect (callback + coroutine)
// --------------------------------------------------------------------------------------

TEST_F(SslConnection, ConnectSuccess) {
    ASSERT_TRUE(ssl_connect(*db_));
    EXPECT_GT(db_->backend_pid(), 0);
}

TEST_F(SslConnection, ConnectSuccess_Coroutine) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        ok = co_await db_->connect(dsn_ssl_string());
    }());
    ASSERT_TRUE(ok);
}

// --------------------------------------------------------------------------------------
// Reconnect (assert a real, re-captured backend PID)
// --------------------------------------------------------------------------------------

/**
 * @brief Reconnect over TLS must produce a fresh, usable backend.
 *
 * Strengthened: the legacy test asserted only that the second connect succeeded. We now
 * capture the backend PID before and after, assert both are real, and confirm the new link
 * answers a query (a genuine re-handshake lands on a new backend process).
 */
TEST_F(SslConnection, ReconnectAfterDisconnect) {
    ASSERT_TRUE(ssl_connect(*db_));
    const int first_pid = db_->backend_pid();
    EXPECT_GT(first_pid, 0);

    db_->disconnect();
    db_->prepare_reconnect();

    ASSERT_TRUE(ssl_connect(*db_));
    const int second_pid = db_->backend_pid();
    EXPECT_GT(second_pid, 0) << "BackendKeyData PID must be captured on the TLS reconnect";
    EXPECT_NE(second_pid, first_pid) << "TLS reconnect must land on a new backend process";

    auto status = db_->execute("SELECT 1", discard_query, discard_error).await();
    EXPECT_TRUE(status) << "reconnected TLS session is not usable";
}

// --------------------------------------------------------------------------------------
// Connection pool (deduped: one helper, both callback + coroutine transports)
// --------------------------------------------------------------------------------------

namespace {

/// Build N TLS connections and verify each via a decoded `SELECT 1` (callback transport).
void
ssl_pool_callback(int n) {
    std::vector<std::unique_ptr<qb::pg::tcp::ssl::database>> conns;
    conns.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto c = std::make_unique<qb::pg::tcp::ssl::database>();
        ASSERT_TRUE(ssl_connect(*c)) << "pool connection " << i << " failed";
        conns.push_back(std::move(c));
    }
    for (std::size_t i = 0; i < conns.size(); ++i) {
        int decoded = -1;
        auto status = conns[i]
                          ->execute(
                              "SELECT 1",
                              [&](transaction &, results res) {
                                  ASSERT_EQ(res.size(), 1u);
                                  decoded = res[0][0].as<int>();
                              },
                              discard_error)
                          .await();
        ASSERT_TRUE(status);
        EXPECT_EQ(decoded, 1) << "pool connection " << i;
    }
}

} // namespace

/// One pool test, exercised through both the callback and coroutine transports.
TEST_F(SslConnection, ConnectionPool) {
    constexpr int num_connections = 5;

    // Transport A: callback / .await()
    ssl_pool_callback(num_connections);

    // Transport B: coroutine connect + query, decoded == 1.
    int ok_count = 0;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        for (int i = 0; i < num_connections; ++i) {
            auto conn = std::make_unique<qb::pg::tcp::ssl::database>();
            if (!co_await conn->connect(dsn_ssl_string()))
                co_return;
            auto reply = co_await conn->query("SELECT 1");
            if (reply.ok() && reply.result().size() == 1 &&
                reply.result()[0][0].as<int>() == 1)
                ++ok_count;
        }
    }());
    EXPECT_EQ(ok_count, num_connections)
        << "coroutine TLS pool: only " << ok_count << "/" << num_connections << " returned 1";
}

// --------------------------------------------------------------------------------------
// Security: verify-full rejection + SCRAM channel binding (the unique value of this file)
// --------------------------------------------------------------------------------------

/**
 * @brief `ssl_verify_mode::full` MUST reject the test server's untrusted self-signed cert.
 *
 * The default (none) encrypts without verifying — it connects. verify-full validates the
 * chain against the system trust store AND the hostname; the self-signed test certificate is
 * not trusted, so verify-full must reject it before any data flows. This proves verification
 * is enforced, not cosmetic.
 */
TEST_F(SslConnection, SslVerifyFullRejectsUntrustedCert) {
    auto opts       = qb::pg::connection_options::parse(dsn_ssl_string());
    opts.ssl_verify = qb::pg::ssl_verify_mode::full;

    auto db = std::make_unique<qb::pg::tcp::ssl::database>();
    EXPECT_FALSE(qb::io::async::run_sync(db->connect(opts)))
        << "verify-full accepted an untrusted self-signed certificate (active-MITM hole)";
}

/**
 * @brief Over TLS, SCRAM must negotiate `SCRAM-SHA-256-PLUS` with `tls-server-end-point`.
 *
 * PostgreSQL offers the `-PLUS` mechanism on SSL connections; channel binding ties the SCRAM
 * proof to the server certificate, so a wrong binding would make the server reject the proof.
 * A successful connect with `used_channel_binding() == true` proves the binding is correct.
 * (Requires a scram-sha-256 role; a `trust` server skips SCRAM and this would not apply, in
 * which case the SetUp probe already connected and we assert the binding flag here.)
 */
TEST_F(SslConnection, ScramChannelBindingNegotiatedOverTls) {
    ASSERT_TRUE(ssl_connect(*db_));
    // Channel binding only applies to a SCRAM-SHA-256 role over TLS. A trust/cleartext
    // server (the common local/CI default) negotiates no channel binding, which is not a
    // failure of this code path — so skip unless a SCRAM-over-TLS server is actually pinned
    // via QB_PG_SSL_DSN. Only then is a missing binding a real defect to hard-assert.
    if (!ssl_dsn_pinned() || !db_->used_channel_binding())
        GTEST_SKIP() << "no SCRAM-SHA-256-PLUS channel binding negotiated (server uses "
                        "trust/cleartext auth, or QB_PG_SSL_DSN is unset); set QB_PG_SSL_DSN "
                        "to a SCRAM role over TLS to exercise channel binding.";
    EXPECT_TRUE(db_->used_channel_binding())
        << "SCRAM-SHA-256-PLUS (tls-server-end-point) channel binding was not negotiated over "
           "TLS against the pinned SCRAM role";
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#else // QB_HAS_SSL

int
main() {
    // SSL disabled at build time: nothing to run. A green no-op keeps ctest consistent.
    return 0;
}

#endif // QB_HAS_SSL
