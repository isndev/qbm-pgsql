#include <gtest/gtest.h>
#include "../pgsql.h"
#include <qb/io/async.h>
#include <thread>
#include <chrono>

constexpr std::string_view PGSQL_CONNECTION_STR = "tcp://test:test@localhost:5432[test]";

using namespace qb::pg;
using namespace qb::pg::detail;

class PostgreSQLPreparedStatementsTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(db_->connect(PGSQL_CONNECTION_STR.data()));
        
        auto status = db_->execute("CREATE TEMP TABLE test_prepared (id SERIAL PRIMARY KEY, value TEXT)")
                .await();
        ASSERT_TRUE(status);
    }

    void TearDown() override {
        if (db_) {
            auto status = db_->execute("DROP TABLE IF EXISTS test_prepared")
                    .await();
            ASSERT_TRUE(status);
            db_->disconnect();
            db_.reset();
        }
    }

    std::unique_ptr<qb::pg::tcp::database> db_;
};

TEST_F(PostgreSQLPreparedStatementsTest, BasicPrepare) {
    auto status = db_->prepare("test_prepare", "INSERT INTO test_prepared (value) VALUES ($1)", type_oid_sequence{23})
        .execute("test_prepare", {23})
        .await();
    ASSERT_TRUE(status);
}

TEST_F(PostgreSQLPreparedStatementsTest, PrepareAndExecute) {
    auto status = db_->prepare("test_prepare", "INSERT INTO test_prepared (value) VALUES ($1)", type_oid_sequence{23})
        .execute("test_prepare", {std::string("test_value")})
        .await();
    ASSERT_TRUE(status);
}

TEST_F(PostgreSQLPreparedStatementsTest, MultipleParameters) {
    auto status = db_->prepare("test_prepare", "INSERT INTO test_prepared (value) VALUES ($1) RETURNING id", type_oid_sequence{23})
        .execute("test_prepare", {std::string("first")})
        .execute("test_prepare", {std::string("second")})
        .await();
    ASSERT_TRUE(status);
}

TEST_F(PostgreSQLPreparedStatementsTest, InvalidPrepare) {
    auto status = db_->prepare("test_prepare", "INSERT INTO nonexistent (value) VALUES ($1)", type_oid_sequence{23})
        .execute("test_prepare", {23})
        .await();
    ASSERT_FALSE(status);
}

TEST_F(PostgreSQLPreparedStatementsTest, ReusePreparedStatement) {
    auto status = db_->prepare("test_prepare", "INSERT INTO test_prepared (value) VALUES ($1)", type_oid_sequence{23})
        .execute("test_prepare", {23})
        .await();
    ASSERT_TRUE(status);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 