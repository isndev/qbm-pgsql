#include <gtest/gtest.h>
#include "../pgsql.h"
#include <qb/io/async.h>
#include <thread>
#include <chrono>

constexpr std::string_view PGSQL_CONNECTION_STR = "tcp://test:test@localhost:5432[test]";

using namespace qb::pg;
using namespace qb::pg::detail;

class PostgreSQLDataTypesTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(db_->connect(PGSQL_CONNECTION_STR.data()));

        bool success = false;
        db_->begin(
                [&success](auto &tr) {
                    tr.execute(R"(
                        CREATE TEMP TABLE test_types (
                        id SERIAL PRIMARY KEY,
                        text_field TEXT,
                        int_field INTEGER,
                        float_field FLOAT,
                        bool_field BOOLEAN,
                        timestamp_field TIMESTAMP,
                        json_field JSONB
                        ))", [&success](auto &)
                        {
                        success = true;
                        });
                }).await();

        ASSERT_TRUE(success);
    }

    void TearDown() override {
        if (db_) {
            bool success = false;
            db_->begin(
                [&success](auto &tr) {
                    tr.execute("DROP TABLE IF EXISTS test_types", [&success](auto &) {
                        success = true;
                    });
                }
            ).await();
            ASSERT_TRUE(success);
            db_->disconnect();
            db_.reset();
        }
    }

    std::unique_ptr<qb::pg::tcp::database> db_;
};

TEST_F(PostgreSQLDataTypesTest, TextType) {
    auto status = db_->execute("INSERT INTO test_types (text_field) VALUES ('test text')")
            .execute("SELECT text_field FROM test_types")
            .await();
    ASSERT_EQ(status.results().size(), 1);
}

TEST_F(PostgreSQLDataTypesTest, IntegerType) {
    auto status = db_->execute("INSERT INTO test_types (int_field) VALUES (42)")
            .execute("SELECT int_field FROM test_types")
            .await();
    ASSERT_EQ(status.results().size(), 1);
}

TEST_F(PostgreSQLDataTypesTest, FloatType) {
    auto status = db_->execute("INSERT INTO test_types (float_field) VALUES (3.14159)")
            .execute("SELECT float_field FROM test_types")
            .await();
    ASSERT_EQ(status.results().size(), 1);
}

TEST_F(PostgreSQLDataTypesTest, BooleanType) {
    auto status = db_->execute("INSERT INTO test_types (bool_field) VALUES (true)")
            .execute("SELECT bool_field FROM test_types")
            .await();
    ASSERT_EQ(status.results().size(), 1);
}

TEST_F(PostgreSQLDataTypesTest, TimestampType) {
    auto status = db_->execute("INSERT INTO test_types (timestamp_field) VALUES (CURRENT_TIMESTAMP)")
            .execute("SELECT timestamp_field FROM test_types")
            .await();
    ASSERT_EQ(status.results().size(), 1);
}

TEST_F(PostgreSQLDataTypesTest, JsonType) {
    auto status = db_->execute("INSERT INTO test_types (json_field) VALUES ('{\"key\": \"value\"}'::jsonb)")
            .execute("SELECT json_field FROM test_types")
            .await();
    ASSERT_EQ(status.results().size(), 1);
}

TEST_F(PostgreSQLDataTypesTest, MultipleTypes) {
    auto status = db_->execute(R"(
        INSERT INTO test_types (
            text_field, int_field, float_field, bool_field, timestamp_field, json_field
        ) VALUES (
            'test', 42, 3.14159, true, CURRENT_TIMESTAMP, '{"key": "value"}'::jsonb
        )
    )")
            .execute("SELECT * FROM test_types")
            .await();
    ASSERT_EQ(status.results().size(), 1);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 