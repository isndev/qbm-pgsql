/**
 * @file test-connection.cpp
 * @brief Unit tests for PostgreSQL connection management
 *
 * This file implements tests for the connection management functionality of the
 * PostgreSQL client module. It verifies the client's ability to establish,
 * maintain, and close database connections through various scenarios, including:
 *
 * - Basic connection establishment with valid credentials
 * - Connection handling with invalid credentials
 * - Connection string parsing and validation
 * - Reconnection after disconnection
 * - Connection timeout handling
 * - Connection pooling and concurrent connections
 *
 * The implementation validates both synchronous and asynchronous connection patterns,
 * ensuring that connections are properly managed across different network conditions.
 *
 * Key features tested:
 * - Connection string format handling
 * - Authentication method negotiation
 * - Connection parameter propagation
 * - Connection error reporting
 * - Connection lifecycle management
 *
 * @see qb::pg::tcp::database
 * @see qb::pg::detail::connection_options
 * @see qb::pg::detail::Database
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
 * limitations under the License.
 */

#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../pgsql.h"
#include "test_config.hpp"

using namespace qb::pg;

/** Skip SSL tests when default DSN cannot complete TLS (no QB_PG_SSL_DSN set). */
#define QB_PG_ASSERT_SSL_CONNECTED(db_expr)                                                               \
    do {                                                                                                  \
        const bool qb_pg_ok = qb::io::async::run_sync((db_expr).connect(qb::pg::test::dsn_ssl_string())); \
        if (!qb_pg_ok && std::getenv("QB_PG_SSL_DSN") == nullptr) {                                       \
            GTEST_SKIP() << "SSL connect failed with default DSN; set QB_PG_SSL_DSN or enable TLS "       \
                            "on the server.";                                                             \
        }                                                                                                 \
        ASSERT_TRUE(qb_pg_ok);                                                                            \
    } while (0)

/**
 * @brief Test fixture for PostgreSQL connection functionality
 *
 * Sets up a test environment for verifying connection establishment,
 * authentication, and connection management features.
 */
class PostgreSQLConnectionTest : public ::testing::Test {
protected:
    /**
     * @brief Set up the test environment
     *
     * Creates a new database connection instance for testing.
     */
    void
    SetUp() override {
        // Create a test database connection
        db_ = std::make_unique<qb::pg::tcp::ssl::database>();
    }

    /**
     * @brief Clean up after tests
     *
     * Disconnects and frees the database connection resources.
     */
    void
    TearDown() override {
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
    }

    std::unique_ptr<qb::pg::tcp::ssl::database> db_;
};

/**
 * @brief Test successful connection establishment
 *
 * Verifies that a connection can be successfully established to
 * a PostgreSQL server with valid credentials.
 */
TEST_F(PostgreSQLConnectionTest, ConnectSuccess) {
    QB_PG_ASSERT_SSL_CONNECTED(*db_);
}

/**
 * @brief Same as ConnectSuccess with `co_await connect()` (TLS client).
 */
TEST_F(PostgreSQLConnectionTest, ConnectSuccess_Coroutine) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        ok = co_await db_->connect(qb::pg::test::dsn_ssl_string());
    }());
    if (!ok && std::getenv("QB_PG_SSL_DSN") == nullptr) {
        GTEST_SKIP() << "SSL connect failed with default DSN; set QB_PG_SSL_DSN or enable TLS "
                        "on the server.";
    }
    ASSERT_TRUE(ok);
}

/**
 * @brief Test connection with invalid credentials
 *
 * Verifies that a connection attempt with invalid credentials
 * is properly rejected with appropriate error information.
 */
TEST_F(PostgreSQLConnectionTest, ConnectWithInvalidCredentials) {
    auto invalid_db = std::make_unique<qb::pg::tcp::ssl::database>();
    ASSERT_FALSE(qb::io::async::run_sync(invalid_db->connect(qb::pg::test::dsn_invalid_auth_string())));
}

/**
 * @brief Test reconnection after disconnection
 *
 * Verifies that a connection can be reestablished after
 * an explicit disconnection.
 */
TEST_F(PostgreSQLConnectionTest, ReconnectAfterDisconnect) {
    QB_PG_ASSERT_SSL_CONNECTED(*db_);

    db_->disconnect();
    db_->prepare_reconnect();

    QB_PG_ASSERT_SSL_CONNECTED(*db_);
}

/**
 * @brief Test connection timeout handling (TLS client; TCP phase is timed)
 *
 * Verifies that a connection attempt to an unreachable server
 * returns false within the configured timeout (default 10 s).
 */
TEST_F(PostgreSQLConnectionTest, ConnectionTimeout) {
    constexpr std::string_view unreachable = "tcp://test:test@192.0.2.1:5432[test]";

    const auto timeout_db = std::make_unique<qb::pg::tcp::ssl::database>();

    const auto start  = std::chrono::steady_clock::now();
    const bool result = qb::io::async::run_sync(timeout_db->connect(std::string(unreachable)));
    const auto end    = std::chrono::steady_clock::now();

    ASSERT_FALSE(result);

    const auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);
    ASSERT_LT(duration.count(), 13);
}

/**
 * @brief Test connection pooling functionality
 *
 * Verifies that multiple connections can be established concurrently
 * and maintained in a connection pool for efficient resource management.
 */
TEST_F(PostgreSQLConnectionTest, ConnectionPool) {
    constexpr int                                            num_connections = 5;
    std::vector<std::unique_ptr<qb::pg::tcp::ssl::database>> connections;
    connections.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i) {
        auto conn = std::make_unique<qb::pg::tcp::ssl::database>();
        QB_PG_ASSERT_SSL_CONNECTED(*conn);
        connections.push_back(std::move(conn));
    }

    // Verify all connections are working by executing a simple query
    for (const auto &conn : connections) {
        auto status = conn->execute("SELECT 1", discard_query, discard_error).await();
        ASSERT_TRUE(status);
    }
}

/**
 * @brief Pool scenario with coroutine connect + query (TLS).
 */
TEST_F(PostgreSQLConnectionTest, ConnectionPool_Coroutine) {
    constexpr int num_connections = 5;
    bool          all_ok          = true;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        for (int i = 0; i < num_connections; ++i) {
            auto conn = std::make_unique<qb::pg::tcp::ssl::database>();
            if (!co_await conn->connect(qb::pg::test::dsn_ssl_string())) {
                all_ok = false;
                co_return;
            }
            auto reply = co_await conn->query("SELECT 1");
            if (!reply.ok() || reply.result().size() != 1 || reply.result()[0][0].as<int>() != 1) {
                all_ok = false;
                co_return;
            }
        }
    }());
    if (!all_ok && std::getenv("QB_PG_SSL_DSN") == nullptr) {
        GTEST_SKIP() << "SSL connect failed with default DSN; set QB_PG_SSL_DSN or enable TLS "
                        "on the server.";
    }
    ASSERT_TRUE(all_ok);
}

// ssl_verify_mode actually gates the handshake. The default (none) encrypts without
// verifying — it connects. verify-full validates the chain against the system trust
// store + the hostname; the test server's self-signed certificate is NOT trusted, so
// verify-full MUST reject it before any data flows (proves verification is enforced,
// not cosmetic). Set the field via the connection_options overload of connect().
TEST_F(PostgreSQLConnectionTest, SslVerifyFullRejectsUntrustedCert) {
    // Precondition: TLS is actually available (default no-verify mode connects).
    {
        auto probe = std::make_unique<qb::pg::tcp::ssl::database>();
        if (!qb::io::async::run_sync(probe->connect(qb::pg::test::dsn_ssl_string()))) {
            GTEST_SKIP() << "SSL not available with default DSN; skipping verify-full test.";
            return;
        }
        probe->disconnect();
    }

    auto opts       = qb::pg::connection_options::parse(qb::pg::test::dsn_ssl_string());
    opts.ssl_verify = qb::pg::ssl_verify_mode::full;

    auto db = std::make_unique<qb::pg::tcp::ssl::database>();
    EXPECT_FALSE(qb::io::async::run_sync(db->connect(opts)))
        << "verify-full accepted an untrusted self-signed certificate (MITM hole)";
}

// Over TLS, SCRAM must negotiate SCRAM-SHA-256-PLUS with tls-server-end-point channel
// binding (PostgreSQL offers `-PLUS` on SSL connections). This binds the SCRAM proof to
// the server certificate — a wrong binding would make the server reject the proof, so a
// successful connect with used_channel_binding() == true proves the binding is correct.
// (Requires a scram-sha-256 server; a `trust` server skips SCRAM and this would not apply.)
TEST_F(PostgreSQLConnectionTest, ScramChannelBindingNegotiatedOverTls) {
    QB_PG_ASSERT_SSL_CONNECTED(*db_);
    EXPECT_TRUE(db_->used_channel_binding())
        << "SCRAM-SHA-256-PLUS (tls-server-end-point) channel binding was not negotiated over TLS";
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}