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
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
        db_ = std::make_unique<qb::pg::tcp::database>();
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

    std::unique_ptr<qb::pg::tcp::database> db_;
};

/**
 * @brief Test successful connection establishment
 *
 * Verifies that a connection can be successfully established to
 * a PostgreSQL server with valid credentials.
 */
TEST_F(PostgreSQLConnectionTest, ConnectSuccess) {
    ASSERT_TRUE(qb::io::async::run_sync(db_->connect(qb::pg::test::dsn_tcp_string())));
}

/**
 * @brief Same as ConnectSuccess using `co_await connect()` on a spawned `task` (libev +
 * coro_scheduler).
 */
TEST_F(PostgreSQLConnectionTest, ConnectSuccess_Coroutine) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        ok = co_await db_->connect(qb::pg::test::dsn_tcp_string());
    }());
    ASSERT_TRUE(ok);
}

/**
 * @brief Test connection with invalid credentials
 *
 * Verifies that a connection attempt with invalid credentials
 * is properly rejected with appropriate error information.
 */
TEST_F(PostgreSQLConnectionTest, ConnectWithInvalidCredentials) {
    const auto invalid_db = std::make_unique<qb::pg::tcp::database>();
    const bool connected =
        qb::io::async::run_sync(invalid_db->connect(qb::pg::test::dsn_invalid_auth_string()));
    if (connected && std::getenv("QB_PG_INVALID_DSN") == nullptr) {
        GTEST_SKIP() << "Server accepted default wrong-password DSN (e.g. trust in "
                        "pg_hba). Set QB_PG_INVALID_DSN to a DSN that must fail auth.";
    }
    ASSERT_FALSE(connected);
}

/**
 * @brief Test reconnection after disconnection
 *
 * Verifies that a connection can be reestablished after
 * an explicit disconnection.
 */
TEST_F(PostgreSQLConnectionTest, ReconnectAfterDisconnect) {
    ASSERT_TRUE(qb::io::async::run_sync(db_->connect(qb::pg::test::dsn_tcp_string())));

    db_->disconnect();
    db_->prepare_reconnect();

    ASSERT_TRUE(qb::io::async::run_sync(db_->connect(qb::pg::test::dsn_tcp_string())));
}

/**
 * @brief Disconnect must fail EVERY outstanding query, not just the in-flight one.
 *
 * Regression: on(disconnected) only failed _current_query; queries queued behind
 * it had their error callback skipped, so a pipelined caller's coroutine awaiter
 * would suspend forever. fail_all_pending() now drains the whole queue.
 */
TEST_F(PostgreSQLConnectionTest, DisconnectFailsAllQueuedQueries) {
    ASSERT_TRUE(qb::io::async::run_sync(db_->connect(qb::pg::test::dsn_tcp_string())));

    int  errors = 0;
    auto on_ok  = [](transaction &, results) {};
    auto on_err = [&errors](error::db_error const &) { ++errors; };

    // First query parks the server; the next two queue behind it.
    db_->execute("SELECT pg_sleep(3)", on_ok, on_err);
    db_->execute("SELECT 1", on_ok, on_err);
    db_->execute("SELECT 2", on_ok, on_err);

    // Pump once so the first query is actually sent and in flight.
    qb::io::async::run(EVRUN_NOWAIT);

    // Drop the connection while all three are outstanding.
    db_->disconnect();

    // All three error callbacks must fire (was 1 before fail_all_pending).
    for (int i = 0; i < 200 && errors < 3; ++i)
        qb::io::async::run(EVRUN_NOWAIT);

    EXPECT_EQ(errors, 3);
}

/**
 * @brief Test connection timeout handling
 *
 * Verifies that a connection attempt to an unreachable server
 * returns false within the configured timeout (default 10 s).
 * Uses timed TCP connect (`qb::io::socket::connect_n`) so the attempt is not
 * stuck in a blocking `::connect()` for OS-default SYN timeouts.
 */
TEST_F(PostgreSQLConnectionTest, ConnectionTimeout) {
    constexpr std::string_view unreachable = "tcp://test:test@192.0.2.1:5432[test]";

    const auto timeout_db = std::make_unique<qb::pg::tcp::database>();

    const auto start  = std::chrono::steady_clock::now();
    const bool result = qb::io::async::run_sync(timeout_db->connect(std::string{unreachable}));
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
    constexpr int                                       num_connections = 5;
    std::vector<std::unique_ptr<qb::pg::tcp::database>> connections;
    connections.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i) {
        auto conn = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(qb::io::async::run_sync(conn->connect(qb::pg::test::dsn_tcp_string())));
        connections.push_back(std::move(conn));
    }

    // Verify all connections are working by executing a simple query
    for (const auto &conn : connections) {
        auto status = conn->execute("SELECT 1", discard_query, discard_error).await();
        ASSERT_TRUE(status);
    }
}

/**
 * @brief Same as ConnectionPool: multiple handshakes via `co_await connect()` then `co_await
 * query()`.
 */
TEST_F(PostgreSQLConnectionTest, ConnectionPool_Coroutine) {
    constexpr int num_connections = 5;
    bool          all_ok          = true;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        for (int i = 0; i < num_connections; ++i) {
            auto conn = std::make_unique<qb::pg::tcp::database>();
            if (!co_await conn->connect(qb::pg::test::dsn_tcp_string())) {
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
    ASSERT_TRUE(all_ok);
}

/**
 * @brief The handshake-deadline timer must not outlive the Database.
 *
 * The connection deadline used to be scheduled with qb::io::async::callback(), a
 * self-deleting heap Timeout that is decoupled from the Database lifetime. After a
 * successful connect the timer stays pending for the whole timeout window; if the
 * Database is destroyed within it (the normal case — the handshake finishes in ms,
 * the deadline is seconds) the timer later fires and dereferences a freed `this`
 * (use-after-free). It is now an owned ScopedTimeout member, cancelled on
 * destruction. This test connects with a short (1s) deadline, destroys the
 * Database, then pumps the loop past the deadline: under ASan the old code reports
 * a heap-use-after-free here; with the fix the timer was cancelled.
 */
TEST(PostgreSQLConnectDeadline, DeadlineTimerCancelledOnDestroy) {
    {
        auto db = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(qb::io::async::run_sync(db->connect(qb::pg::test::dsn_tcp_string())));
        db->disconnect();
        db->prepare_reconnect();
        // Reconnect with a 1s handshake deadline (reuses the parsed connection options).
        ASSERT_TRUE(qb::io::async::run_sync(db->connect(std::chrono::seconds(1))));
    } // Database destroyed here; the owned ScopedTimeout is cancelled with it.

    // Pump the event loop well past the 1s deadline. A fire-and-forget timer would
    // come due now and run against the freed Database.
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500))
        qb::io::async::run(EVRUN_NOWAIT);
    SUCCEED();
}

/**
 * @brief DSN parsing must preserve high-bit bytes in credentials.
 *
 * The parser skipped whitespace via std::isspace(*p) on a plain char; a high-bit
 * byte (e.g. 0xC3 in a non-ASCII password) is negative, and passing a negative
 * value to std::isspace is undefined behavior that, depending on the libc ctype
 * table, can misclassify the byte as whitespace and silently drop it from the
 * password. The fix casts to unsigned char first; this guards the byte-for-byte
 * round trip.
 */
TEST(PostgreSQLDsnParse, PreservesHighBitBytesInPassword) {
    const std::string password = std::string("p\xC3\xA9ss"); // "péss" in UTF-8
    const std::string dsn      = "tcp://user:" + password + "@localhost:5432[db]";

    auto opts = connection_options::parse(dsn);

    EXPECT_EQ(opts.user, "user");
    EXPECT_EQ(opts.password, password);
    EXPECT_EQ(opts.database, "db");
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}