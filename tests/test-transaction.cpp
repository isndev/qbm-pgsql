#include <chrono>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <string_view>
#include <thread>
#include "../pgsql.h"

constexpr std::string_view PGSQL_CONNECTION_STR = "tcp://test:test@localhost:5432[test]";

using namespace qb::pg;
using namespace qb::pg::detail;

class PostgreSQLTransactionTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        db_ = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(db_->connect(PGSQL_CONNECTION_STR.data()));

        auto status =
            db_->execute("CREATE TEMP TABLE test_transactions (id SERIAL PRIMARY KEY, value TEXT)")
                .await();
        ASSERT_TRUE(status);
    }

    void
    TearDown() override {
        if (db_) {
            auto status = db_->execute("DROP TABLE IF EXISTS test_transactions").await();
            ASSERT_TRUE(status);
            db_->disconnect();
            db_.reset();
        }
    }

    std::unique_ptr<qb::pg::tcp::database> db_;
};

TEST_F(PostgreSQLTransactionTest, BasicTransaction) {
    bool success = false;
    auto status = db_->begin(
                         [&success](Transaction &t) {
                             t.execute(
                                 "SELECT * FROM test_transactions",
                                 [&success](Transaction &tr, results result) {
                                     ASSERT_EQ(result.size(), 0);
                                     success = true;
                                 },
                                 [](error::db_error error) { ASSERT_TRUE(false); });
                         },
                         [](error::db_error error) { ASSERT_TRUE(false); })
                      .await();
    ASSERT_TRUE(success);
}

TEST_F(PostgreSQLTransactionTest, TransactionRollback) {
    bool error_caught = false;
    auto status = db_->begin(
                         [](Transaction &t) {
                             t.execute(
                                 "INSERT INTO nonexistent (value) VALUES ('test')",
                                 [](Transaction &tr, results result) {
                                     ASSERT_TRUE(false); // Cette requête doit échouer
                                 },
                                 [](error::db_error error) {
                                     ASSERT_TRUE(true); // On s'attend à une erreur
                                 });
                         },
                         [&error_caught](error::db_error error) { error_caught = true; })
                      .await();
    ASSERT_TRUE(error_caught);
}

TEST_F(PostgreSQLTransactionTest, NestedTransactions) {
    bool success = false;
    auto status =
        db_->begin(
               [&success](Transaction &t) {
                   t.savepoint(
                       "nested_sp",
                       [&success](Transaction &tr) {
                           tr.execute(
                               "INSERT INTO test_transactions (value) VALUES ('test')",
                               [&success](Transaction &tr2, results result) { success = true; },
                               [](error::db_error error) { ASSERT_TRUE(false); });
                       },
                       [](error::db_error error) { ASSERT_TRUE(false); });
               },
               [](error::db_error error) { ASSERT_TRUE(false); })
            .await();
    ASSERT_TRUE(success);
}

TEST_F(PostgreSQLTransactionTest, TransactionIsolation) {
    bool success = false;
    auto status = db_->begin(
                         [&success](Transaction &t) {
                             t.execute(
                                 "SELECT * FROM test_transactions",
                                 [&success](Transaction &tr, results result) {
                                     ASSERT_EQ(result.size(), 0);
                                     success = true;
                                 },
                                 [](error::db_error error) { ASSERT_TRUE(false); });
                         },
                         [](error::db_error error) { ASSERT_TRUE(false); })
                      .await();
    ASSERT_TRUE(success);
}

TEST_F(PostgreSQLTransactionTest, TransactionTimeout) {
    bool success = false;
    auto status = db_->begin(
                         [&success](Transaction &t) {
                             t.execute(
                                 "SELECT pg_sleep(2)",
                                 [&success](Transaction &tr, results result) { success = true; },
                                 [](error::db_error error) { ASSERT_TRUE(false); });
                         },
                         [](error::db_error error) { ASSERT_TRUE(false); })
                      .await();
    ASSERT_TRUE(success);
}

TEST_F(PostgreSQLTransactionTest, MultipleStatements) {
    bool success = false;
    auto status = db_->begin(
                         [&success](Transaction &t) {
                             t.execute(
                                 "SELECT * FROM test_transactions",
                                 [&success](Transaction &tr, results result) {
                                     ASSERT_EQ(result.size(), 0);
                                     success = true;
                                 },
                                 [](error::db_error error) { ASSERT_TRUE(false); });
                         },
                         [](error::db_error error) { ASSERT_TRUE(false); })
                      .await();
    ASSERT_TRUE(success);
}

TEST_F(PostgreSQLTransactionTest, DISABLED_SavepointRollback) {
    bool before_savepoint = false;
    bool in_savepoint = false;
    bool after_savepoint = false;
    auto status =
        db_->begin(
               [&before_savepoint, &in_savepoint, &after_savepoint](Transaction &t) {
                   t.execute(
                       "INSERT INTO test_transactions (value) VALUES ('before_savepoint')",
                       [&before_savepoint, &in_savepoint, &after_savepoint](Transaction &tr,
                                                                            results result) {
                           before_savepoint = true;
                           tr.savepoint(
                               "sp1",
                               [&in_savepoint](Transaction &tr2) {
                                   tr2.execute(
                                       "INSERT INTO test_transactions (value) VALUES "
                                       "('in_savepoint')",
                                       [&in_savepoint](Transaction &tr3, results result) {
                                           in_savepoint = true;
                                           // Forcer une erreur dans le savepoint
                                           tr3.execute(
                                               "SELECT nonexistent",
                                               [](Transaction &tr4, results result) {
                                                   ASSERT_TRUE(false);
                                               },
                                               [](error::db_error error) {
                                                   // On s'attend à cette erreur
                                               });
                                       },
                                       [](error::db_error error) { ASSERT_TRUE(false); });
                               },
                               [](error::db_error error) {
                                   // Le savepoint sera rollback automatiquement
                               });
                           // La transaction principale continue
                           tr.execute(
                               "SELECT * FROM test_transactions WHERE value = 'before_savepoint'",
                               [&after_savepoint](Transaction &tr2, results result) {
                                   ASSERT_EQ(result.size(), 1);
                                   after_savepoint = true;
                               },
                               [](error::db_error error) { ASSERT_TRUE(false); });
                       },
                       [](error::db_error error) { ASSERT_TRUE(false); });
               },
               [](error::db_error error) { ASSERT_TRUE(false); })
            .await();
    ASSERT_TRUE(before_savepoint);
    ASSERT_TRUE(in_savepoint);
    ASSERT_TRUE(after_savepoint);

    // Vérifier que seul le savepoint a été rollback
    auto status2 =
        db_->begin(
               [](Transaction &t) {
                   t.execute(
                       "SELECT * FROM test_transactions WHERE value = 'in_savepoint'",
                       [](Transaction &tr, results result) { ASSERT_EQ(result.size(), 0); },
                       [](error::db_error error) { ASSERT_TRUE(false); });
                   t.execute(
                       "SELECT * FROM test_transactions WHERE value = 'before_savepoint'",
                       [](Transaction &tr, results result) { ASSERT_EQ(result.size(), 1); },
                       [](error::db_error error) { ASSERT_TRUE(false); });
               },
               [](error::db_error error) { ASSERT_TRUE(false); })
            .await();
}

TEST_F(PostgreSQLTransactionTest, MultipleNestedTransactions) {
    bool success1 = false;
    bool success2 = false;
    auto status =
        db_->begin(
               [&success1, &success2](Transaction &t) {
                   t.savepoint(
                       "sp1",
                       [&success1, &success2](Transaction &tr1) {
                           tr1.execute(
                               "INSERT INTO test_transactions (value) VALUES ('sp1')",
                               [&success1, &success2](Transaction &tr2, results result) {
                                   success1 = true;
                                   tr2.savepoint(
                                       "sp2",
                                       [&success2](Transaction &tr3) {
                                           tr3.execute(
                                               "INSERT INTO test_transactions (value) VALUES "
                                               "('sp2')",
                                               [&success2](Transaction &tr4, results result) {
                                                   success2 = true;
                                               },
                                               [](error::db_error error) { ASSERT_TRUE(false); });
                                       },
                                       [](error::db_error error) { ASSERT_TRUE(false); });
                               },
                               [](error::db_error error) { ASSERT_TRUE(false); });
                       },
                       [](error::db_error error) { ASSERT_TRUE(false); });
               },
               [](error::db_error error) { ASSERT_TRUE(false); })
            .await();
    ASSERT_TRUE(success1);
    ASSERT_TRUE(success2);
}

TEST_F(PostgreSQLTransactionTest, CommitMultipleChanges) {
    bool insert_success = false;
    bool select_success = false;
    auto status =
        db_->begin(
               [&insert_success, &select_success](Transaction &t) {
                   t.execute(
                       "INSERT INTO test_transactions (value) VALUES ('test1')",
                       [&insert_success, &select_success](Transaction &tr1, results result) {
                           insert_success = true;
                           tr1.execute(
                               "INSERT INTO test_transactions (value) VALUES ('test2')",
                               [&select_success](Transaction &tr2, results result) {
                                   tr2.execute(
                                       "SELECT * FROM test_transactions",
                                       [&select_success](Transaction &tr3, results result) {
                                           ASSERT_EQ(result.size(), 2);
                                           select_success = true;
                                       },
                                       [](error::db_error error) { ASSERT_TRUE(false); });
                               },
                               [](error::db_error error) { ASSERT_TRUE(false); });
                       },
                       [](error::db_error error) { ASSERT_TRUE(false); });
               },
               [](error::db_error error) { ASSERT_TRUE(false); })
            .await();
    ASSERT_TRUE(insert_success);
    ASSERT_TRUE(select_success);
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}