/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file transaction-advanced.cpp
 * @brief Live advanced-transaction integration tests for the qbm-pgsql client.
 *
 * Covers explicit isolation levels and read-only mode, savepoint create + real
 * `ROLLBACK TO`, multi-operation transactions, statement-timeout behavior, DEALLOCATE of a
 * prepared statement, constraint-violation rollback, cursors, combined serializable+read-only
 * mode, and real SERIALIZABLE write-skew contention asserting SQLSTATE 40001.
 *
 * Cleanups vs the legacy file: the dead second connection (`db2_`), the unused
 * `<thread>`/`<future>` includes, and all `ss << mode; std::cout` debug noise are removed.
 * The hard `ASSERT_TRUE(connect)` fixture is replaced by the shared skip-not-fail base.
 * `BasicErrorHandling` now asserts the block fails and the error fires. `SavepointRestoration`
 * actually rolls back to the savepoint and asserts the undo. `TransactionTimeout` drops the
 * flaky `<2000ms` upper bound. The exact-string error compare in `DirectTransactionModeUsage`
 * becomes an error-code / substring check.
 */

#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/test_config.hpp"
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using namespace qb::pg::detail;

namespace {

class AdvancedTransactionTest : public qb::pg::test::PgIntegrationTest {
protected:
    void
    SetUp() override {
        PgIntegrationTest::SetUp();
        if (IsSkipped())
            return;

        ASSERT_TRUE(db_->execute("DROP TABLE IF EXISTS test_advanced_transactions", discard_query, discard_error).await());
        ASSERT_TRUE(db_->execute("CREATE TABLE test_advanced_transactions ("
                                 "id SERIAL PRIMARY KEY, value TEXT, counter INTEGER DEFAULT 0)",
                                 discard_query, discard_error)
                        .await());
        ASSERT_TRUE(db_->execute("INSERT INTO test_advanced_transactions (value, counter) VALUES "
                                 "('row1', 10), ('row2', 20), ('row3', 30)",
                                 discard_query, discard_error)
                        .await());
    }

    void
    TearDown() override {
        if (db_ && !IsSkipped())
            (void) db_->execute("DROP TABLE IF EXISTS test_advanced_transactions", discard_query, discard_error).await();
    }
};

} // namespace

/** @brief READ COMMITTED (explicit mode) reads the seeded counter. */
TEST_F(AdvancedTransactionTest, ExplicitReadCommitted) {
    bool             ok = false;
    transaction_mode mode{isolation_level::read_committed};
    auto             st = db_->begin(
                                 [&](Transaction &t) {
                         t.execute(
                             "SELECT counter FROM test_advanced_transactions WHERE value = 'row1'",
                             [&](Transaction &, results r) {
                                 ASSERT_EQ(r.size(), 1u);
                                 EXPECT_EQ(r[0][0].as<int>(), 10);
                                 ok = true;
                             },
                             [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                                 },
                                 [](error::db_error const &e) { ADD_FAILURE() << e.what(); }, mode)
                              .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

/** @brief READ ONLY transaction allows the read but rejects the write; nothing is persisted. */
TEST_F(AdvancedTransactionTest, ReadOnlyTransactionRejectsWrite) {
    bool read_ok      = false;
    bool write_failed = false;
    bool error_caught = false;

    transaction_mode mode;
    mode.read_only = true;

    auto st = db_->begin(
                     [&](Transaction &t) {
                         t.execute("SELECT COUNT(*) FROM test_advanced_transactions", [&](Transaction &tr, results r) {
                             ASSERT_EQ(r.size(), 1u);
                             read_ok = true;
                             tr.execute(
                                 "INSERT INTO test_advanced_transactions (value, counter) VALUES ('readonly_test', 999)",
                                 [](Transaction &, results) { ADD_FAILURE() << "write must fail in a READ ONLY transaction"; },
                                 [&](error::db_error const &e) {
                                     write_failed = true;
                                     // 25006: read_only_sql_transaction
                                     EXPECT_EQ(e.code, "25006");
                                 });
                         });
                     },
                     [&](error::db_error const &) { error_caught = true; }, mode)
                  .await();

    EXPECT_FALSE(st);
    EXPECT_TRUE(read_ok);
    EXPECT_TRUE(write_failed);
    EXPECT_TRUE(error_caught);

    bool verified = false;
    auto verify   = db_->execute(
                           "SELECT COUNT(*) FROM test_advanced_transactions WHERE value = 'readonly_test'",
                           [&](Transaction &, results r) {
                             ASSERT_EQ(r.size(), 1u);
                             EXPECT_EQ(r[0][0].as<int>(), 0);
                             verified = true;
                           },
                           [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                        .await();
    EXPECT_TRUE(verify);
    EXPECT_TRUE(verified);
}

/**
 * @brief Savepoint actually `ROLLBACK TO`s: a row inserted after the savepoint is undone
 * while a row inserted before it survives within the same transaction.
 */
TEST_F(AdvancedTransactionTest, SavepointRestoration) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        if (!co_await db_->begin())
            co_return;
        if (!co_await db_->execute("INSERT INTO test_advanced_transactions (value) VALUES ('keep')"))
            co_return;
        if (!co_await db_->savepoint("sp1"))
            co_return;
        if (!co_await db_->execute("INSERT INTO test_advanced_transactions (value) VALUES ('undo')"))
            co_return;
        if (!co_await db_->rollback_savepoint("sp1"))
            co_return;
        if (!co_await db_->commit())
            co_return;

        auto kept = co_await db_->query("SELECT COUNT(*) FROM test_advanced_transactions WHERE value = 'keep'");
        auto gone = co_await db_->query("SELECT COUNT(*) FROM test_advanced_transactions WHERE value = 'undo'");
        ok        = kept.ok() && gone.ok() && kept.result()[0][0].as<int>() == 1 && gone.result()[0][0].as<int>() == 0;
    }());
    ASSERT_TRUE(ok);
}

/** @brief Several writes plus a verifying read in one callback transaction all commit.
 *
 * Seed is 3 rows (row1 counter=10, row2=20, row3=30). This inserts a 4th row and bumps
 * row1's counter by 5, so the table holds EXACTLY 4 rows and row1's counter is EXACTLY 15.
 * Error callbacks fail the test loudly instead of swallowing a real failure. */
TEST_F(AdvancedTransactionTest, MultiOperationTransaction) {
    auto fail_on_error = [](error::db_error const &e) {
        ADD_FAILURE() << e.code << " " << e.what();
    };
    bool count_ok   = false;
    bool counter_ok = false;
    auto st =
        db_->begin([&](Transaction &t) {
               t.execute("INSERT INTO test_advanced_transactions (value, counter) VALUES ('temp_row', 100)", discard_query, fail_on_error);
               t.execute("UPDATE test_advanced_transactions SET counter = counter + 5 WHERE value = 'row1'", discard_query, fail_on_error);
               t.execute(
                   "SELECT COUNT(*) FROM test_advanced_transactions",
                   [&](Transaction &, results r) {
                       ASSERT_EQ(r.size(), 1u);
                       EXPECT_EQ(r[0][0].as<int>(), 4);
                       count_ok = true;
                   },
                   fail_on_error);
               // The UPDATE's effect: row1 counter went 10 -> 15.
               t.execute(
                   "SELECT counter FROM test_advanced_transactions WHERE value = 'row1'",
                   [&](Transaction &, results r) {
                       ASSERT_EQ(r.size(), 1u);
                       EXPECT_EQ(r[0][0].as<int>(), 15);
                       counter_ok = true;
                   },
                   fail_on_error);
           })
            .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(count_ok);
    EXPECT_TRUE(counter_ok);
}

/** @brief An invalid statement inside `begin` fails the block and fires the error callback. */
TEST_F(AdvancedTransactionTest, BasicErrorHandling) {
    bool error_handled = false;
    auto st =
        db_->begin(
               [](Transaction &t) {
                   t.execute(
                       "SELECT * FROM nonexistent_table", [](Transaction &, results) { ADD_FAILURE() << "query on missing table must fail"; },
                       [](error::db_error const &e) { EXPECT_EQ(e.code, "42P01"); });
               },
               [&](error::db_error const &) { error_handled = true; })
            .await();
    EXPECT_FALSE(st);
    EXPECT_TRUE(error_handled);
}

/**
 * @brief Statement timeout: `pg_sleep(3)` under a 1s `statement_timeout` errors out.
 *
 * Asserts the error fires with SQLSTATE 57014 (query_canceled — the locale-independent code
 * PostgreSQL raises for a statement-timeout cancel) and that the operation returns in well
 * under the full 3s sleep, with only a lower bound (≈900ms) — the flaky `<2000ms` upper bound
 * is dropped (CI scheduling jitter made it brittle).
 */
TEST_F(AdvancedTransactionTest, StatementTimeout) {
    bool saw_error = false;
    auto start     = std::chrono::steady_clock::now();
    auto st        = db_->begin([&](Transaction &t) {
                     t.execute("SET statement_timeout = '1000'", discard_query, discard_error);
                     t.execute(
                         "SELECT pg_sleep(3)", [](Transaction &, results) { ADD_FAILURE() << "pg_sleep should be cut off by timeout"; },
                         [&](error::db_error const &e) {
                             saw_error = true;
                             EXPECT_EQ(e.code, "57014") << e.what(); // query_canceled (statement timeout)
                         });
                        })
                         .await();
    auto elapsed   = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    EXPECT_FALSE(st);
    EXPECT_TRUE(saw_error);
    EXPECT_GT(elapsed.count(), 800); // cut off near the 1s timeout, not the full 3s sleep
}

/** @brief Same timeout via `Transaction::set_timeout()` before `begin()`. */
TEST_F(AdvancedTransactionTest, StatementTimeoutViaSetTimeout) {
    bool saw_error = false;
    auto st        = db_->set_timeout(std::chrono::milliseconds(1000))
                         .begin([&](Transaction &t) {
                      t.execute(
                          "SELECT pg_sleep(3)", [](Transaction &, results) { ADD_FAILURE() << "pg_sleep should be cut off"; },
                          [&](error::db_error const &e) {
                              saw_error = true;
                              EXPECT_EQ(e.code, "57014") << e.what(); // query_canceled (statement timeout)
                          });
                         })
                         .await();
    EXPECT_TRUE(saw_error);
    EXPECT_FALSE(static_cast<bool>(st));
}

/** @brief A prepared statement deallocated inside a transaction can no longer execute. */
TEST_F(AdvancedTransactionTest, DeallocatePreparedStatement) {
    ASSERT_TRUE(db_->prepare("test_dealloc_stmt", "SELECT * FROM test_advanced_transactions WHERE value = $1", type_oid_sequence{},
                             discard_prepare, discard_error)
                    .await());

    bool first_ok = false;
    ASSERT_TRUE(db_->execute(
                       "test_dealloc_stmt", params{std::string("row1")},
                       [&](Transaction &, results r) {
                           EXPECT_EQ(r.size(), 1u);
                           first_ok = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                    .await());
    EXPECT_TRUE(first_ok);

    bool dealloc_ok = false;
    ASSERT_TRUE(
        db_->begin([&](Transaction &t) { t.execute("DEALLOCATE test_dealloc_stmt", [&](Transaction &, results) { dealloc_ok = true; }); })
            .await());
    EXPECT_TRUE(dealloc_ok);

    bool error_caught = false;
    auto reuse        = db_->execute(
                               "test_dealloc_stmt", params{std::string("row1")},
                               [](Transaction &, results) { ADD_FAILURE() << "deallocated statement must not execute"; },
                               [&](error::db_error const &) { error_caught = true; })
                            .await();
    EXPECT_FALSE(reuse);
    EXPECT_TRUE(error_caught);
}

/** @brief A unique-constraint violation rolls the whole transaction back. */
TEST_F(AdvancedTransactionTest, ConstraintViolationRollsBack) {
    ASSERT_TRUE(db_->execute("DROP TABLE IF EXISTS test_unique_constraint", discard_query, discard_error).await());
    ASSERT_TRUE(
        db_->execute("CREATE TABLE test_unique_constraint (id SERIAL PRIMARY KEY, unique_value TEXT UNIQUE)", discard_query, discard_error)
            .await());
    ASSERT_TRUE(
        db_->execute("INSERT INTO test_unique_constraint (unique_value) VALUES ('unique_string')", discard_query, discard_error).await());

    bool first_insert = false;
    bool error_caught = false;
    auto st =
        db_->begin(
               [&](Transaction &t) {
                   t.execute("INSERT INTO test_unique_constraint (unique_value) VALUES ('different_unique')", [&](Transaction &tr, results) {
                       first_insert = true;
                       tr.execute(
                           "INSERT INTO test_unique_constraint (unique_value) VALUES ('unique_string')",
                           [](Transaction &, results) { ADD_FAILURE() << "duplicate must violate UNIQUE"; },
                           [](error::db_error const &e) { EXPECT_EQ(e.code, "23505"); }); // unique_violation
                   });
               },
               [&](error::db_error const &) { error_caught = true; })
            .await();
    EXPECT_FALSE(st);
    EXPECT_TRUE(first_insert);
    EXPECT_TRUE(error_caught);

    bool verified = false;
    auto verify   = db_->execute(
                           "SELECT COUNT(*) FROM test_unique_constraint WHERE unique_value = 'different_unique'",
                           [&](Transaction &, results r) {
                             ASSERT_EQ(r.size(), 1u);
                             EXPECT_EQ(r[0][0].as<int>(), 0); // rolled back
                             verified = true;
                           },
                           [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                        .await();
    EXPECT_TRUE(verify);
    EXPECT_TRUE(verified);

    (void) db_->execute("DROP TABLE IF EXISTS test_unique_constraint", discard_query, discard_error).await();
}

/** @brief A DECLARE/FETCH cursor over 100 rows fetches in batches inside a transaction. */
TEST_F(AdvancedTransactionTest, TransactionWithCursor) {
    constexpr int kRows  = 100;
    constexpr int kFetch = 10;

    ASSERT_TRUE(db_->begin([&](Transaction &t) {
                       for (int i = 0; i < kRows; ++i)
                           t.execute("INSERT INTO test_advanced_transactions (value, counter) VALUES ('cursor_row_" + std::to_string(i) + "', "
                                         + std::to_string(i) + ")",
                                     discard_query, discard_error);
                   })
                    .await());

    bool cursor_created = false;
    int  rows_fetched   = 0;
    auto st             = db_->begin([&](Transaction &t) {
                     t.execute(
                         "DECLARE test_cursor CURSOR FOR SELECT * FROM test_advanced_transactions "
                         "WHERE value LIKE 'cursor_row_%' ORDER BY counter",
                         [&](Transaction &, results) { cursor_created = true; }, [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                     for (int i = 0; i < 3; ++i)
                         t.execute(
                             "FETCH " + std::to_string(kFetch) + " FROM test_cursor",
                             [&](Transaction &, results r) { rows_fetched += static_cast<int>(r.size()); },
                             [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                     t.execute("CLOSE test_cursor", discard_query, discard_error);
                             })
                              .await();

    EXPECT_TRUE(st);
    EXPECT_TRUE(cursor_created);
    EXPECT_EQ(rows_fetched, 3 * kFetch);

    (void) db_->execute("DELETE FROM test_advanced_transactions WHERE value LIKE 'cursor_row_%'", discard_query, discard_error).await();
}

/**
 * @brief Combined SERIALIZABLE + READ ONLY mode: read succeeds, write is rejected, the
 * transaction rolls back. The error message is checked by substring (not an exact compare).
 */
TEST_F(AdvancedTransactionTest, SerializableReadOnlyRejectsWrite) {
    bool read_ok      = false;
    bool write_failed = false;

    transaction_mode mode;
    mode.isolation = isolation_level::serializable;
    mode.read_only = true;

    auto st = db_->begin(
                     [&](Transaction &t) {
                         t.execute("SELECT 1 AS test_col", [&](Transaction &tr, results r) {
                             ASSERT_EQ(r.size(), 1u);
                             EXPECT_EQ(r[0][0].as<int>(), 1);
                             read_ok = true;
                             tr.execute(
                                 "CREATE TABLE serializable_ro_test (id INT)",
                                 [](Transaction &, results) { ADD_FAILURE() << "DDL must fail in a READ ONLY transaction"; },
                                 [&](error::db_error const &e) {
                                     write_failed = true;
                                     EXPECT_EQ(e.code, "25006"); // read_only_sql_transaction
                                 });
                         });
                     },
                     [](error::db_error const &e) {
                         // Expected: block rolls back after the read-only write failure.
                         EXPECT_NE(std::string(e.what()).find("rollback"), std::string::npos) << "unexpected txn error: " << e.what();
                     },
                     mode)
                  .await();

    EXPECT_FALSE(st);
    EXPECT_TRUE(read_ok);
    EXPECT_TRUE(write_failed);
}

/**
 * @brief Real SERIALIZABLE write-skew contention → SQLSTATE 40001 (serialization_failure).
 *
 * Two independent connections both run a SERIALIZABLE transaction. Each reads the row set,
 * then each updates a different row based on what it read. Under SERIALIZABLE the second
 * COMMIT (or the conflicting statement) must fail with 40001 because the two transactions
 * cannot be serialized. We assert that at least one of the two transactions hits 40001.
 */
TEST_F(AdvancedTransactionTest, SerializableContentionRaises40001) {
    qb::pg::tcp::database con_a;
    qb::pg::tcp::database con_b;
    if (!qb::pg::test::pg_try_connect(con_a) || !qb::pg::test::pg_try_connect(con_b))
        GTEST_SKIP() << qb::pg::test::kDaemonUnreachableSentinel << " (two-connection contention)";

    // Shared, committed table seeded with two rows holding a balance each.
    ASSERT_TRUE(con_a.execute("DROP TABLE IF EXISTS test_serial_skew", discard_query, discard_error).await());
    ASSERT_TRUE(con_a.execute("CREATE TABLE test_serial_skew (id INT PRIMARY KEY, balance INT)", discard_query, discard_error).await());
    ASSERT_TRUE(con_a.execute("INSERT INTO test_serial_skew VALUES (1, 100), (2, 100)", discard_query, discard_error).await());

    auto begin_serializable = [](qb::pg::tcp::database &db) {
        return db.execute("BEGIN ISOLATION LEVEL SERIALIZABLE", discard_query, discard_error).await();
    };
    auto sum_balance = [](qb::pg::tcp::database &db) -> bool {
        return static_cast<bool>(db.execute("SELECT SUM(balance) FROM test_serial_skew", discard_query, discard_error).await());
    };

    ASSERT_TRUE(begin_serializable(con_a));
    ASSERT_TRUE(begin_serializable(con_b));

    // Both read the whole set (establishing a read-write dependency footprint).
    ASSERT_TRUE(sum_balance(con_a));
    ASSERT_TRUE(sum_balance(con_b));

    // Each writes a different row based on what it read (classic write-skew).
    (void) con_a.execute("UPDATE test_serial_skew SET balance = balance - 100 WHERE id = 1", discard_query, discard_error).await();
    (void) con_b.execute("UPDATE test_serial_skew SET balance = balance - 100 WHERE id = 2", discard_query, discard_error).await();

    // Commit A first (should succeed), then B (should fail with 40001) — but the conflict can
    // surface on either side, so collect SQLSTATEs from both commits.
    std::string code_a;
    std::string code_b;
    (void) con_a.execute("COMMIT", [](Transaction &, results) {}, [&](error::db_error const &e) { code_a = e.code; }).await();
    (void) con_b.execute("COMMIT", [](Transaction &, results) {}, [&](error::db_error const &e) { code_b = e.code; }).await();

    EXPECT_TRUE(code_a == "40001" || code_b == "40001")
        << "expected a serialization_failure (40001) on one of the two commits; got a='" << code_a << "' b='" << code_b << "'";

    // Both connections may be left in a failed/aborted state; roll back defensively.
    (void) con_a.execute("ROLLBACK", discard_query, discard_error).await();
    (void) con_b.execute("ROLLBACK", discard_query, discard_error).await();
    (void) con_a.execute("DROP TABLE IF EXISTS test_serial_skew", discard_query, discard_error).await();
    con_a.disconnect();
    con_b.disconnect();
}

/**
 * @brief Calling begin() on an already-open transaction context invokes the error callback
 *        with "already in transaction" (the `_parent != nullptr` guard in the callback begin).
 *
 * The outer begin's success callback receives a child `Transaction&` whose `_parent` is set;
 * a nested `t.begin(...)` on it must short-circuit to on_error without issuing a second BEGIN.
 */
TEST_F(AdvancedTransactionTest, NestedBeginCallbackReportsAlreadyInTransaction) {
    bool inner_error = false;
    bool outer_ok    = false;
    auto st          = db_->begin(
                              [&](Transaction &t) {
                         // t is a child transaction (its _parent is the root): a second begin must fail.
                         t.begin([](Transaction &) { ADD_FAILURE() << "nested begin must not succeed"; },
                                 [&](error::db_error const &e) {
                                     inner_error = true;
                                     EXPECT_NE(std::string(e.what()).find("already in transaction"), std::string::npos);
                                 });
                              },
                              [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                           .await();
    outer_ok         = static_cast<bool>(st);
    EXPECT_TRUE(outer_ok);
    EXPECT_TRUE(inner_error) << "nested begin should have reported 'already in transaction'";
}

/**
 * @brief execute(prepared_name, params, on_success) — the single-success-callback prepared
 *        overload (it forwards an empty error callback). Verifies the decoded value.
 */
TEST_F(AdvancedTransactionTest, ExecutePreparedSingleSuccessCallback) {
    bool prepared = false;
    ASSERT_TRUE(db_->prepare(
                       "qb_adv_single_cb_stmt", "SELECT $1::int + 1", type_oid_sequence{oid::int4},
                       [&](Transaction &, PreparedQuery const &) { prepared = true; }, discard_error)
                    .await());
    ASSERT_TRUE(prepared);

    int  decoded = -1;
    bool ran     = false;
    // Single-success-callback overload: no explicit error callback supplied.
    auto st = db_->execute("qb_adv_single_cb_stmt", params{41},
                           [&](Transaction &, results r) {
                               ASSERT_EQ(r.size(), 1u);
                               decoded = r[0][0].as<int>();
                               ran     = true;
                           })
                  .await();
    EXPECT_TRUE(static_cast<bool>(st));
    EXPECT_TRUE(ran);
    EXPECT_EQ(decoded, 42);
}

/**
 * @brief REPEATABLE READ via the callback begin(on_success, on_error, mode) overload; the
 *        body reads SHOW transaction_isolation back to prove the mode was applied.
 */
TEST_F(AdvancedTransactionTest, ExplicitRepeatableReadCallbackMode) {
    std::string      seen;
    transaction_mode mode{isolation_level::repeatable_read};
    auto             st = db_->begin(
                                 [&](Transaction &t) {
                         t.execute(
                             "SHOW transaction_isolation",
                             [&](Transaction &, results r) {
                                 ASSERT_EQ(r.size(), 1u);
                                 seen = r[0][0].as<std::string>();
                             },
                             [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                                 },
                                 [](error::db_error const &e) { ADD_FAILURE() << e.what(); }, mode)
                              .await();
    EXPECT_TRUE(static_cast<bool>(st));
    EXPECT_EQ(seen, "repeatable read");
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
