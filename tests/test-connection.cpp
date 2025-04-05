#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <qb/io/async.h>
#include <string_view>
#include <thread>
#include <vector>
#include "../pgsql.h"

constexpr std::string_view PGSQL_CONNECTION_STR = "tcp://test:test@localhost:5432[test]";

using namespace qb::pg;

class PostgreSQLConnectionTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        // Create a test database connection
        db_ = std::make_unique<qb::pg::tcp::database>();
    }

    void
    TearDown() override {
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
    }

    std::unique_ptr<qb::pg::tcp::database> db_;
};

TEST_F(PostgreSQLConnectionTest, ConnectSuccess) {
    ASSERT_TRUE(db_->connect(PGSQL_CONNECTION_STR.data()));
}

TEST_F(PostgreSQLConnectionTest, ConnectWithInvalidCredentials) {
    const auto invalid_db = std::make_unique<qb::pg::tcp::database>();
    ASSERT_FALSE(invalid_db->connect("tcp://billy@localhost:5432[invalid]"));
}

TEST_F(PostgreSQLConnectionTest, ReconnectAfterDisconnect) {
    ASSERT_TRUE(db_->connect(PGSQL_CONNECTION_STR.data()));

    // Simulate disconnection
    db_->disconnect();

    // Attempt to reconnect
    ASSERT_TRUE(db_->connect(PGSQL_CONNECTION_STR.data()));
}

TEST_F(PostgreSQLConnectionTest, DISABLED_ConnectionTimeout) {
    const auto timeout_db = std::make_unique<qb::pg::tcp::database>();

    const auto start = std::chrono::steady_clock::now();
    const bool result = timeout_db->connect(PGSQL_CONNECTION_STR.data());
    const auto end = std::chrono::steady_clock::now();

    ASSERT_FALSE(result);

    // Check if connection attempt timed out within reasonable time (e.g., 5 seconds)
    const auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);
    ASSERT_LT(duration.count(), 5);
}

TEST_F(PostgreSQLConnectionTest, ConnectionPool) {
    constexpr int num_connections = 5;
    std::vector<std::unique_ptr<qb::pg::tcp::database>> connections;
    connections.reserve(num_connections);

    for (int i = 0; i < num_connections; ++i) {
        auto conn = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(conn->connect(PGSQL_CONNECTION_STR.data()));
        connections.push_back(std::move(conn));
    }

    // Verify all connections are working by executing a simple query
    for (const auto &conn : connections) {
        auto status = conn->execute("SELECT 1").await();
        ASSERT_TRUE(status);
    }
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}