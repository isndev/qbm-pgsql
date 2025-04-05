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
using namespace qb::pg::detail;

class PostgreSQLOperationsTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        db_ = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(db_->connect(PGSQL_CONNECTION_STR.data()));
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

// Test simple query execution
TEST_F(PostgreSQLOperationsTest, SimpleQueryExecution) {
    bool success = false;
    auto status =
        db_->execute(
               "SELECT 1",
               [&success](Transaction &tr, results result) {
                   ASSERT_EQ(result.size(), 1);
                   ASSERT_EQ(result[0][0].as<int>(), 1);
                   success = true;
               },
               [](error::db_error error) { ASSERT_TRUE(false) << "Query failed: " << error.code; })
            .await();
    ASSERT_TRUE(success);
}

// Test prepared statement
TEST_F(PostgreSQLOperationsTest, PreparedStatement) {
    bool success = false;
    auto status = db_->prepare("test_prepare", "SELECT $1::int", type_oid_sequence{})
                      .execute(
                          "test_prepare", {42},
                          [&success](Transaction &tr, results result) {
                              ASSERT_EQ(result.size(), 1);
                              ASSERT_EQ(result[0][0].as<int>(), 42);
                              success = true;
                          },
                          [](error::db_error error) {
                              ASSERT_TRUE(false) << "Execute failed: " << error.code;
                          })
                      .await();
    ASSERT_TRUE(status);
    ASSERT_TRUE(success);
}

// Test error handling
TEST_F(PostgreSQLOperationsTest, ErrorHandling) {
    bool error_caught = false;
    auto status = db_->execute(
                         "SELECT * FROM nonexistent_table",
                         [](Transaction &tr, results result) {
                             ASSERT_TRUE(false) << "Query should have failed";
                         },
                         [&error_caught](error::db_error error) {
                             ASSERT_FALSE(error.code.empty());
                             error_caught = true;
                         })
                      .await();
    ASSERT_TRUE(error_caught);
}

// Test transaction
TEST_F(PostgreSQLOperationsTest, Transaction) {
    bool success = false;
    auto status = db_->begin(
                         [&success](Transaction &t) {
                             t.execute(
                                 "SELECT 1",
                                 [&success](Transaction &tr, results result) {
                                     ASSERT_EQ(result.size(), 1);
                                     ASSERT_EQ(result[0][0].as<int>(), 1);
                                     success = true;
                                 },
                                 [](error::db_error error) {
                                     ASSERT_TRUE(false) << "Query failed: " << error.code;
                                 });
                         },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Transaction failed: " << error.code;
                         })
                      .await();
    ASSERT_TRUE(success);
}

// Test savepoint
TEST_F(PostgreSQLOperationsTest, Savepoint) {
    bool success = false;
    auto status = db_->begin(
                         [&success](Transaction &t) {
                             t.savepoint(
                                 "test_savepoint",
                                 [&success](Transaction &tr) {
                                     tr.execute(
                                         "SELECT 1",
                                         [&success](Transaction &tr2, results result) {
                                             ASSERT_EQ(result.size(), 1);
                                             ASSERT_EQ(result[0][0].as<int>(), 1);
                                             success = true;
                                         },
                                         [](error::db_error error) {
                                             ASSERT_TRUE(false) << "Query failed: " << error.code;
                                         });
                                 },
                                 [](error::db_error error) {
                                     ASSERT_TRUE(false) << "Savepoint failed: " << error.code;
                                 });
                         },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Transaction failed: " << error.code;
                         })
                      .await();
    ASSERT_TRUE(success);
}

// Test chaining operations
TEST_F(PostgreSQLOperationsTest, ChainingOperations) {
    bool success = false;
    auto status = db_->begin(
                         [&success](Transaction &t) {
                             t.execute(
                                 "SELECT 1",
                                 [&success](Transaction &tr, results result) {
                                     ASSERT_EQ(result.size(), 1);
                                     ASSERT_EQ(result[0][0].as<int>(), 1);

                                     tr.execute(
                                         "SELECT 2",
                                         [&success](Transaction &tr2, results result) {
                                             ASSERT_EQ(result.size(), 1);
                                             ASSERT_EQ(result[0][0].as<int>(), 2);

                                             tr2.execute(
                                                 "SELECT 3",
                                                 [&success](Transaction &tr3, results result) {
                                                     ASSERT_EQ(result.size(), 1);
                                                     ASSERT_EQ(result[0][0].as<int>(), 3);
                                                     success = true;
                                                 },
                                                 [](error::db_error error) {
                                                     ASSERT_TRUE(false)
                                                         << "Query 3 failed: " << error.code;
                                                 });
                                         },
                                         [](error::db_error error) {
                                             ASSERT_TRUE(false) << "Query 2 failed: " << error.code;
                                         });
                                 },
                                 [](error::db_error error) {
                                     ASSERT_TRUE(false) << "Query 1 failed: " << error.code;
                                 });
                         },
                         [](error::db_error error) {
                             ASSERT_TRUE(false) << "Transaction failed: " << error.code;
                         })
                      .await();
    ASSERT_TRUE(success);
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}