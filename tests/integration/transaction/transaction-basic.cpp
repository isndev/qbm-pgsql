/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file transaction-basic.cpp
 * @brief Live basic-transaction integration tests for the qbm-pgsql client.
 *
 * Covers begin/commit/rollback (callback and coroutine APIs), savepoint create + rollback
 * with explicit data-undo verification, nested savepoints, multi-statement commit, the
 * aborted-transaction (RfQ 'E') recovery path, and real two-connection READ COMMITTED
 * isolation (uncommitted writes are invisible to a second connection until COMMIT).
 *
 * Dedup (D4): the former `MultipleStatements` (byte-for-byte copy of `BasicTransaction`) is
 * deleted, the signal-less `TransactionTimeout` (`pg_sleep(2)` asserting nothing) is
 * deleted, and `TransactionIsolation` (a third empty-`SELECT *` smoke whose name lied) is
 * rewritten as a real 2-connection isolation test. In-lambda `ASSERT_TRUE(false)` sinks are
 * replaced with `ADD_FAILURE()` (a bare ASSERT inside a callback cannot unwind the test).
 *
 * Skip-not-fail: derives from the shared fixture; cases skip when no live PG is reachable.
 */

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/test_config.hpp"
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using namespace qb::pg::detail;

namespace {

class TransactionBasicTest : public qb::pg::test::PgIntegrationTest {
protected:
    void
    SetUp() override {
        PgIntegrationTest::SetUp();
        if (IsSkipped())
            return;
        ASSERT_TRUE(
            db_->execute("CREATE TEMP TABLE test_transactions (id SERIAL PRIMARY KEY, value TEXT)", discard_query, discard_error).await());
    }
};

} // namespace

/** @brief A simple begin→SELECT transaction commits and sees the (empty) seed table. */
TEST_F(TransactionBasicTest, BasicTransaction) {
    bool ok = false;
    auto st = db_->begin(
                     [&](Transaction &t) {
                         t.execute(
                             "SELECT * FROM test_transactions",
                             [&](Transaction &, results r) {
                                 ASSERT_EQ(r.size(), 0u);
                                 ok = true;
                             },
                             [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                     },
                     [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                  .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

/** @brief State written by a callback `begin` is visible to a follow-up `co_await query`. */
TEST_F(TransactionBasicTest, BasicTransactionCoroutineFollowUp) {
    bool txn_ok = false;
    auto st     = db_->begin(
                         [&](Transaction &t) {
                         t.execute(
                             "INSERT INTO test_transactions (value) VALUES ('row')", [&](Transaction &, results) { txn_ok = true; },
                             [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                         },
                         [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                      .await();
    ASSERT_TRUE(st);
    ASSERT_TRUE(txn_ok);

    bool coro_ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply = co_await db_->query("SELECT COUNT(*) FROM test_transactions");
        coro_ok    = reply.ok() && reply.result().size() == 1u && reply.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(coro_ok);
}

/** @brief Imperative coroutine transaction: begin/execute/commit then verify the row landed. */
TEST_F(TransactionBasicTest, ImperativeCoroBeginCommit) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        if (!co_await db_->begin())
            co_return;
        if (!co_await db_->execute("INSERT INTO test_transactions (value) VALUES ('coro_txn')"))
            co_return;
        if (!co_await db_->commit())
            co_return;
        auto v = co_await db_->query("SELECT COUNT(*) FROM test_transactions WHERE value = 'coro_txn'");
        ok     = v.ok() && v.result().size() == 1u && v.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(ok);
}

/** @brief Savepoint create → insert → rollback-to-savepoint → commit: the row is undone. */
TEST_F(TransactionBasicTest, SavepointCoroRollbackUndoesRow) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        if (!co_await db_->begin())
            co_return;
        if (!co_await db_->savepoint("sp_coro"))
            co_return;
        if (!co_await db_->execute("INSERT INTO test_transactions (value) VALUES ('sp_row')"))
            co_return;
        if (!co_await db_->rollback_savepoint("sp_coro"))
            co_return;
        if (!co_await db_->commit())
            co_return;
        auto v = co_await db_->query("SELECT COUNT(*) FROM test_transactions WHERE value = 'sp_row'");
        ok     = v.ok() && v.result().size() == 1u && v.result()[0][0].as<int>() == 0;
    }());
    ASSERT_TRUE(ok);
}

/** @brief A statement error rolls the transaction back; the outer error callback fires. */
TEST_F(TransactionBasicTest, TransactionRollbackOnError) {
    bool error_caught = false;
    auto st           = db_->begin(
                               [](Transaction &t) {
                         t.execute(
                             "INSERT INTO nonexistent (value) VALUES ('test')",
                             [](Transaction &, results) { ADD_FAILURE() << "insert into missing table must fail"; },
                             [](error::db_error const &e) { EXPECT_EQ(e.code, "42P01"); });
                               },
                               [&](error::db_error const &) { error_caught = true; })
                            .await();
    EXPECT_FALSE(st);
    EXPECT_TRUE(error_caught);
}

/** @brief Nested savepoint insert commits successfully. */
TEST_F(TransactionBasicTest, NestedSavepointInsert) {
    bool ok = false;
    auto st = db_->begin(
                     [&](Transaction &t) {
                         t.savepoint(
                             "nested_sp",
                             [&](Transaction &tr) {
                                 tr.execute(
                                     "INSERT INTO test_transactions (value) VALUES ('nested')", [&](Transaction &, results) { ok = true; },
                                     [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                             },
                             [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                     },
                     [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                  .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

/**
 * @brief Savepoint rollback undoes its write; an earlier autocommitted write survives.
 *
 * Phase 1 autocommits `before_savepoint` before BEGIN, then inside `sp1` inserts
 * `in_savepoint` and triggers an error (missing table) that rolls the savepoint back. Phase
 * 2 asserts `in_savepoint` is gone and `before_savepoint` remains. Every branch asserts a
 * boolean post-condition (no `std::cout`-only paths).
 */
TEST_F(TransactionBasicTest, SavepointRollbackPreservesEarlierCommittedWrite) {
    ASSERT_TRUE(db_->execute("DELETE FROM test_transactions", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("INSERT INTO test_transactions (value) VALUES ('before_savepoint')", discard_query, discard_error).await());

    bool in_savepoint     = false;
    bool error_caught     = false;
    int  savepoint_errors = 0;
    auto status           = db_->begin(
                                   [&](Transaction &t) {
                             t.savepoint(
                                 "sp1",
                                 [&](Transaction &tr2) {
                                     tr2.execute(
                                         "INSERT INTO test_transactions (value) VALUES ('in_savepoint')",
                                         [&](Transaction &tr3, results) {
                                             in_savepoint = true;
                                             tr3.execute(
                                                 "SELECT * FROM nonexistent_table",
                                                 [](Transaction &, results) { ADD_FAILURE() << "query on missing table must fail"; },
                                                 [&](error::db_error const &e) {
                                                     EXPECT_EQ(e.code, "42P01");
                                                     error_caught = true;
                                                 });
                                         },
                                         [](error::db_error const &e) { ADD_FAILURE() << "savepoint insert failed: " << e.what(); });
                                 },
                                 [&](error::db_error const &) { ++savepoint_errors; });
                                   },
                                   [](error::db_error const &e) { ADD_FAILURE() << "outer transaction failed: " << e.what(); })
                                .await();
    EXPECT_TRUE(status) << status.error().what();
    EXPECT_EQ(savepoint_errors, 1);

    bool verified = false;
    auto verify   = db_->begin(
                           [&](Transaction &t) {
                             t.execute(
                                 "SELECT * FROM test_transactions WHERE value = 'in_savepoint'",
                                 [](Transaction &, results r) { EXPECT_EQ(r.size(), 0u) << "savepoint write was not rolled back"; },
                                 [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                             t.execute(
                                 "SELECT * FROM test_transactions WHERE value = 'before_savepoint'",
                                 [&](Transaction &, results r) {
                                     EXPECT_EQ(r.size(), 1u) << "outer write was not preserved";
                                     verified = true;
                                 },
                                 [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                           },
                           [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                        .await();
    EXPECT_TRUE(verify);
    EXPECT_TRUE(in_savepoint) << "in-savepoint insert never ran";
    EXPECT_TRUE(error_caught) << "savepoint error never fired";
    EXPECT_TRUE(verified) << "verification block never completed";
}

/** A handled savepoint failure rolls back its body and lets the outer block commit. */
TEST_F(TransactionBasicTest, FailedSavepointKeepsWriteInsideOuterBegin) {
    int  savepoint_errors = 0;
    int  query_errors     = 0;
    int  outer_errors     = 0;
    auto st =
        db_->begin(
               [&](Transaction &outer) {
                   outer.execute("INSERT INTO test_transactions (value) VALUES ('outer_before')", discard_query, discard_error);
                   outer.savepoint(
                       "failed_child",
                       [&](Transaction &inner) {
                           inner.execute("INSERT INTO test_transactions (value) VALUES ('inner_write')", discard_query, discard_error);
                           inner.execute("SELECT * FROM missing_savepoint_table", discard_query, [&](error::db_error const &e) {
                               ++query_errors;
                               EXPECT_EQ(e.code, "42P01");
                           });
                       },
                       [&](error::db_error const &) { ++savepoint_errors; });
                   outer.savepoint(
                       "failed_child",
                       [&](Transaction &reused) {
                           reused.execute("INSERT INTO test_transactions (value) VALUES ('reused_name')", discard_query, discard_error);
                           reused.savepoint("nested_child", [&](Transaction &nested) {
                               nested.execute("INSERT INTO test_transactions (value) VALUES ('nested_write')", discard_query, discard_error);
                           });
                       },
                       [&](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                   outer.execute("INSERT INTO test_transactions (value) VALUES ('outer_after')", discard_query, discard_error);
               },
               [&](error::db_error const &) { ++outer_errors; })
            .await();

    EXPECT_TRUE(st) << st.error().what();
    EXPECT_EQ(query_errors, 1);
    EXPECT_EQ(savepoint_errors, 1);
    EXPECT_EQ(outer_errors, 0);
    auto verify = db_->execute("SELECT value FROM test_transactions ORDER BY id", discard_query, discard_error).await();
    ASSERT_TRUE(verify) << verify.error().what();
    ASSERT_EQ(verify.results().size(), 4u);
    EXPECT_EQ(verify.results()[0][0].as<std::string>(), "outer_before");
    EXPECT_EQ(verify.results()[1][0].as<std::string>(), "reused_name");
    EXPECT_EQ(verify.results()[2][0].as<std::string>(), "nested_write");
    EXPECT_EQ(verify.results()[3][0].as<std::string>(), "outer_after");
}

/** A callback exception is reported once, after local cleanup, without aborting BEGIN. */
TEST_F(TransactionBasicTest, ThrowingSavepointCallbackRecoversOuterBlock) {
    int  errors = 0;
    auto st     = db_->begin([&](Transaction &outer) {
                     outer.savepoint(
                         "callback_throw", [](Transaction &) { throw std::runtime_error("savepoint callback threw"); },
                         [&](error::db_error const &e) {
                             ++errors;
                             EXPECT_NE(std::string(e.what()).find("savepoint callback threw"), std::string::npos);
                         });
                     outer.execute("INSERT INTO test_transactions (value) VALUES ('after_callback_throw')", discard_query, discard_error);
                     })
                      .await();
    EXPECT_TRUE(st) << st.error().what();
    EXPECT_EQ(errors, 1);
    auto kept =
        db_->execute("SELECT count(*) FROM test_transactions WHERE value = 'after_callback_throw'", discard_query, discard_error).await();
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept.results()[0][0].as<int>(), 1);
}

/** Recovering a later savepoint must not erase an earlier root failure in one await. */
TEST_F(TransactionBasicTest, RecoveredSavepointPreservesEarlierRootError) {
    int  first_errors     = 0;
    int  savepoint_errors = 0;
    auto st               = db_->execute("SELECT 1 / 0", discard_query,
                                         [&](error::db_error const &e) {
                               ++first_errors;
                               EXPECT_EQ(e.code, "22012");
                                         })
                                .begin([&](Transaction &outer) {
                      outer.savepoint(
                          "after_prior_error",
                          [&](Transaction &inner) { inner.execute("SELECT * FROM missing_savepoint_table", discard_query, discard_error); },
                          [&](error::db_error const &) { ++savepoint_errors; });
                                })
                                .await();
    EXPECT_FALSE(st);
    EXPECT_EQ(first_errors, 1);
    EXPECT_EQ(savepoint_errors, 1);
    EXPECT_EQ(st.error().code, "22012") << st.error().what();
    EXPECT_TRUE(db_->execute("SELECT 1", discard_query, discard_error).await());
}

/** A throwing root Then reports a client failure to Error and await. */
TEST_F(TransactionBasicTest, RootThenExceptionIsVisible) {
    int  errors = 0;
    auto st     = db_->execute("INSERT INTO test_transactions (value) VALUES ('then_committed')", discard_query, discard_error)
                      .then([](Transaction &) { throw std::runtime_error("root then failed"); })
                      .error([&](error::db_error const &e) {
                      ++errors;
                      EXPECT_NE(std::string(e.what()).find("root then failed"), std::string::npos);
                      })
                      .await();
    EXPECT_FALSE(st);
    EXPECT_EQ(errors, 1);
    EXPECT_NE(std::string(st.error().what()).find("root then failed"), std::string::npos);
    auto committed =
        db_->execute(
               "SELECT count(*) FROM test_transactions WHERE value = 'then_committed'", [](Transaction &, results) {}, discard_error)
            .await();
    ASSERT_TRUE(committed);
    EXPECT_EQ(committed.results()[0][0].as<int>(), 1);
}

/** A throwing Then inside Begin still aborts that block and reports its cause. */
TEST_F(TransactionBasicTest, NestedThenExceptionAbortsOuterBlock) {
    int  errors = 0;
    auto st     = db_->begin([&](Transaction &outer) {
                     outer.execute("INSERT INTO test_transactions (value) VALUES ('nested_then')", discard_query, discard_error);
                     outer.then([](Transaction &) { throw std::runtime_error("nested then failed"); });
                     })
                      .error([&](error::db_error const &e) {
                      ++errors;
                      EXPECT_NE(std::string(e.what()).find("nested then failed"), std::string::npos);
                      })
                      .await();
    EXPECT_FALSE(st);
    EXPECT_EQ(errors, 1);
    EXPECT_NE(std::string(st.error().what()).find("nested then failed"), std::string::npos);
    auto gone = db_->execute(
                       "SELECT count(*) FROM test_transactions WHERE value = 'nested_then'", [](Transaction &, results) {}, discard_error)
                    .await();
    ASSERT_TRUE(gone);
    EXPECT_EQ(gone.results()[0][0].as<int>(), 0);
}

/** If ROLLBACK TO itself fails, the outer transaction cannot commit. */
TEST_F(TransactionBasicTest, FailedSavepointRollbackAbortsOuterBlock) {
    int  savepoint_errors = 0;
    int  outer_errors     = 0;
    auto st               = db_->begin(
                                   [&](Transaction &outer) {
                         outer.execute("INSERT INTO test_transactions (value) VALUES ('must_rollback')", discard_query, discard_error);
                         outer.savepoint(
                             "released_early",
                             [&](Transaction &inner) {
                                 inner.execute("RELEASE SAVEPOINT released_early", discard_query, discard_error);
                                 inner.execute("SELECT * FROM missing_savepoint_table", discard_query, discard_error);
                             },
                             [&](error::db_error const &e) {
                                 ++savepoint_errors;
                                 EXPECT_EQ(e.code, "3B001");
                             });
                                   },
                                   [&](error::db_error const &) { ++outer_errors; })
                                .await();
    EXPECT_FALSE(st);
    EXPECT_EQ(savepoint_errors, 1);
    EXPECT_EQ(outer_errors, 1);
    auto gone = db_->execute(
                       "SELECT count(*) FROM test_transactions WHERE value = 'must_rollback'", [](Transaction &, results) {}, discard_error)
                    .await();
    ASSERT_TRUE(gone);
    EXPECT_EQ(gone.results()[0][0].as<int>(), 0);
}

/** A SAVEPOINT rejected before creation is reported once and leaves no cleanup SQL. */
TEST_F(TransactionBasicTest, FailedSavepointCreationDoesNotAttemptCleanup) {
    int  success = 0;
    int  errors  = 0;
    auto st      = db_->savepoint(
                          "outside_transaction", [&](Transaction &) { ++success; },
                          [&](error::db_error const &e) {
                         ++errors;
                         EXPECT_EQ(e.code, "25P01");
                          })
                       .await();
    EXPECT_FALSE(st);
    EXPECT_EQ(success, 0);
    EXPECT_EQ(errors, 1);
    auto next = db_->execute("SELECT 1", discard_query, discard_error).await();
    EXPECT_TRUE(next) << next.error().what();
}

/** Both accepted callback signatures enqueue simple and prepared commands. */
TEST_F(TransactionBasicTest, ExecuteCallbackSignaturesEnqueueWork) {
    int  callbacks = 0;
    auto simple    = db_->execute(
                            "SELECT 1", [&](Transaction &) { ++callbacks; }, discard_error)
                         .execute(
                             "SELECT 2",
                             [&](Transaction &, results r) {
                              ASSERT_EQ(r.size(), 1u);
                              EXPECT_EQ(r[0][0].as<int>(), 2);
                              ++callbacks;
                             },
                             discard_error)
                         .await();
    ASSERT_TRUE(simple) << simple.error().what();
    EXPECT_EQ(callbacks, 2);

    ASSERT_TRUE(db_->prepare("callback_signature", "SELECT $1::int + 1", type_oid_sequence{oid::int4}, discard_prepare, discard_error).await());
    auto prepared = db_->execute(
                           "callback_signature", params{7}, [&](Transaction &) { ++callbacks; }, discard_error)
                        .execute(
                            "callback_signature", params{9},
                            [&](Transaction &, results r) {
                                ASSERT_EQ(r.size(), 1u);
                                EXPECT_EQ(r[0][0].as<int>(), 10);
                                ++callbacks;
                            },
                            discard_error)
                        .await();
    ASSERT_TRUE(prepared) << prepared.error().what();
    EXPECT_EQ(callbacks, 4);
}

/** @brief Two nested savepoints each commit their insert. */
TEST_F(TransactionBasicTest, MultipleNestedSavepoints) {
    bool sp1 = false;
    bool sp2 = false;
    auto st  = db_->begin(
                      [&](Transaction &t) {
                         t.savepoint(
                             "sp1",
                             [&](Transaction &tr1) {
                                 tr1.execute(
                                     "INSERT INTO test_transactions (value) VALUES ('sp1')",
                                     [&](Transaction &tr2, results) {
                                         sp1 = true;
                                         tr2.savepoint(
                                             "sp2",
                                             [&](Transaction &tr3) {
                                                 tr3.execute(
                                                     "INSERT INTO test_transactions (value) VALUES ('sp2')",
                                                     [&](Transaction &, results) { sp2 = true; },
                                                     [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                                             },
                                             [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                                     },
                                     [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                             },
                             [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                      },
                      [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                   .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(sp1);
    EXPECT_TRUE(sp2);
}

/** @brief Two inserts in one transaction both commit atomically (final SELECT sees both). */
TEST_F(TransactionBasicTest, CommitMultipleChanges) {
    bool insert_ok = false;
    bool select_ok = false;
    auto st        = db_->begin(
                            [&](Transaction &t) {
                         t.execute(
                             "INSERT INTO test_transactions (value) VALUES ('test1')",
                             [&](Transaction &tr1, results) {
                                 insert_ok = true;
                                 tr1.execute(
                                     "INSERT INTO test_transactions (value) VALUES ('test2')",
                                     [&](Transaction &tr2, results) {
                                         tr2.execute(
                                             "SELECT * FROM test_transactions",
                                             [&](Transaction &, results r) {
                                                 EXPECT_EQ(r.size(), 2u);
                                                 select_ok = true;
                                             },
                                             [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                                     },
                                     [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                             },
                             [](error::db_error const &e) { ADD_FAILURE() << e.what(); });
                            },
                            [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                         .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(insert_ok);
    EXPECT_TRUE(select_ok);
}

/**
 * @brief Aborted transaction (RfQ 'E') blocks further commands until ROLLBACK, then recovers.
 */
TEST_F(TransactionBasicTest, AbortedTransactionRequiresRollback) {
    ASSERT_TRUE(db_->execute("BEGIN", discard_query, discard_error).await());

    bool div_err = false;
    auto st      = db_->execute(
                          "SELECT 1 / 0", [](Transaction &, results) { ADD_FAILURE() << "division by zero should error"; },
                          [&](error::db_error const &e) {
                         EXPECT_EQ(e.code, "22012"); // division_by_zero
                         div_err = true;
                          })
                       .await();
    EXPECT_FALSE(st);
    EXPECT_TRUE(div_err);

    bool blocked = false;
    st           = db_->execute(
                          "SELECT 1", [](Transaction &, results) { ADD_FAILURE() << "commands must fail while aborted"; },
                          [&](error::db_error const &e) {
                    blocked = true;
                    EXPECT_EQ(e.code, "25P02"); // in_failed_sql_transaction
                          })
                       .await();
    EXPECT_FALSE(st);
    EXPECT_TRUE(blocked);

    ASSERT_TRUE(db_->execute("ROLLBACK", discard_query, discard_error).await());

    bool recovered = false;
    st             = db_->execute(
                            "SELECT 1 AS x",
                            [&](Transaction &, results r) {
                    ASSERT_EQ(r.size(), 1u);
                    EXPECT_EQ(r[0][0].as<int>(), 1);
                    recovered = true;
                            },
                            [](error::db_error const &e) { ADD_FAILURE() << e.what(); })
                         .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(recovered);
}

/**
 * @brief Real READ COMMITTED isolation across two connections.
 *
 * `db_` opens a transaction and inserts a row but does NOT commit. A second, independent
 * connection must NOT see that row (uncommitted writes are invisible). After `db_` commits,
 * the second connection sees it. This replaces the old single-connection `TransactionIsolation`
 * smoke test whose name lied (it only re-ran an empty `SELECT *`).
 */
TEST_F(TransactionBasicTest, ReadCommittedIsolationAcrossConnections) {
    // The temp table lives on db_'s session; use a shared, committed table instead so the
    // observer connection can see it. Create + truncate up front.
    ASSERT_TRUE(
        db_->execute("CREATE TABLE IF NOT EXISTS test_iso_shared (id SERIAL PRIMARY KEY, value TEXT)", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("DELETE FROM test_iso_shared", discard_query, discard_error).await());

    qb::pg::tcp::database observer;
    if (!qb::pg::test::pg_try_connect(observer))
        GTEST_SKIP() << qb::pg::test::kDaemonUnreachableSentinel << " (observer connection)";

    auto observer_count = [&]() -> int {
        int n = -1;
        (void) observer
            .execute(
                "SELECT COUNT(*) FROM test_iso_shared WHERE value = 'iso'",
                [&](Transaction &, results r) {
                    ASSERT_EQ(r.size(), 1u);
                    n = r[0][0].as<int>();
                },
                [](error::db_error const &e) { ADD_FAILURE() << "observer: " << e.what(); })
            .await();
        return n;
    };

    // Writer: open a transaction, insert, but do not commit yet.
    ASSERT_TRUE(db_->execute("BEGIN", discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("INSERT INTO test_iso_shared (value) VALUES ('iso')", discard_query, discard_error).await());

    // Observer must NOT see the uncommitted row.
    EXPECT_EQ(observer_count(), 0) << "uncommitted write leaked to a second connection";

    // Commit, then the observer sees it.
    ASSERT_TRUE(db_->execute("COMMIT", discard_query, discard_error).await());
    EXPECT_EQ(observer_count(), 1) << "committed write not visible to a second connection";

    // Cleanup the shared (non-temp) table.
    (void) db_->execute("DROP TABLE IF EXISTS test_iso_shared", discard_query, discard_error).await();
    observer.disconnect();
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
