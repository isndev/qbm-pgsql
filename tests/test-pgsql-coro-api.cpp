/**
 * @file test-pgsql-coro-api.cpp
 * @brief Focused integration tests for the coroutine-only PostgreSQL API surface
 *
 * Complements callback-heavy tests elsewhere: every case here uses `co_await` on
 * `pg_reply_awaiter` overloads (no user callbacks). Blocking sync code uses
 * `qb::io::async::run_sync` on a `task` from an immediately-invoked coroutine lambda.
 *
 * Callback-style sync remains: `execute(..., cb, err)` or discards, then `Transaction::await()`.
 *
 * `with_transaction` / `transaction_abort` — see `pgsql.h` API note and
 * `src/coro_with_transaction.hpp`.
 */
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include <stdexcept>
#include <string>
#include "../pgsql.h"
#include "test_config.hpp"

using namespace qb::pg;
using namespace qb::pg::detail;

namespace {

/** Temp table name for this translation unit (session-local; integration tests are serialized). */
constexpr std::string_view kCoroApiTable = "qb_pgsql_coro_api_t";

} // namespace

class PgsqlCoroApiTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        db_ = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(qb::io::async::run_sync(db_->connect(qb::pg::test::dsn_tcp_string())));

        ASSERT_TRUE(db_->execute(std::string("DROP TABLE IF EXISTS ") + std::string(kCoroApiTable),
                                 qb::pg::discard_query, qb::pg::discard_error)
                        .await());
        ASSERT_TRUE(db_->execute(std::string("CREATE TEMP TABLE ") + std::string(kCoroApiTable) +
                                     " (id SERIAL PRIMARY KEY, v TEXT NOT NULL)",
                                 qb::pg::discard_query, qb::pg::discard_error)
                        .await());
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

TEST_F(PgsqlCoroApiTest, CoroBegin_SetTimeout_AbortsLongStatement) {
    const bool ok = qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        db_->set_timeout(std::chrono::milliseconds(800));
        auto begun = co_await db_->begin();
        if (!begun.ok())
            co_return false;
        auto slow = co_await db_->execute("SELECT pg_sleep(3)");
        if (slow.ok())
            co_return false;
        (void) co_await db_->rollback();
        co_return true;
    }());
    EXPECT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroConnectThenQuery) {
    bool ok = false;
    auto fresh = std::make_unique<qb::pg::tcp::database>();
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        if (!co_await fresh->connect(qb::pg::test::dsn_tcp_string()))
            co_return;
        auto reply = co_await fresh->query("SELECT 1 AS one");
        ok         = reply.ok() && reply.result().size() == 1 && reply.result()[0][0].as<int>() == 1;
    }());
    fresh->disconnect();
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroQueryAndExecute) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                         " (v) VALUES ('a')");
        if (!ins)
            co_return;
        auto sel = co_await db_->query(std::string("SELECT v FROM ") + std::string(kCoroApiTable) +
                                       " WHERE v = 'a'");
        ok = sel.ok() && sel.result().size() == 1 && sel.result()[0][0].as<std::string>() == "a";
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroBeginCommit) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto b = co_await db_->begin();
        if (!b)
            co_return;
        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                         " (v) VALUES ('txn_ok')");
        if (!ins)
            co_return;
        auto c = co_await db_->commit();
        if (!c)
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") +
                                     std::string(kCoroApiTable) + " WHERE v = 'txn_ok'");
        ok     = q.ok() && q.result().size() == 1 && q.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroBeginRollback) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto b = co_await db_->begin();
        if (!b)
            co_return;
        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                         " (v) VALUES ('rolled')");
        if (!ins)
            co_return;
        auto rb = co_await db_->rollback();
        if (!rb)
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") +
                                     std::string(kCoroApiTable) + " WHERE v = 'rolled'");
        ok     = q.ok() && q.result().size() == 1 && q.result()[0][0].as<int>() == 0;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroPrepareAndExecutePrepared) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto pr = co_await db_->prepare("qb_coro_api_ins",
                                        std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                            " (v) VALUES ($1)",
                                        type_oid_sequence{oid::text});
        if (!pr)
            co_return;
        auto ex = co_await db_->execute("qb_coro_api_ins", params{std::string("prepared_row")});
        if (!ex)
            co_return;
        auto q = co_await db_->query(std::string("SELECT v FROM ") + std::string(kCoroApiTable) +
                                     " WHERE v = 'prepared_row'");
        ok     = q.ok() && q.result().size() == 1;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroInvalidSqlYieldsFailure) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply = co_await db_->query("THIS IS NOT VALID SQL");
        ok         = !reply.ok();
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_ReturnsValueAndCommits) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<int> {
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                           " (v) VALUES ('with_tx_value')");
            if (!ins)
                throw transaction_abort{ins.error()};
            co_return 99;
        });
        if (!r.ok() || r.result() != 99)
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") +
                                     std::string(kCoroApiTable) + " WHERE v = 'with_tx_value'");
        ok     = q.ok() && q.result().size() == 1 && q.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_VoidBodyCommits) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<void> {
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                           " (v) VALUES ('with_tx_void')");
            if (!ins)
                throw transaction_abort{ins.error()};
        });
        if (!r.ok())
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") +
                                     std::string(kCoroApiTable) + " WHERE v = 'with_tx_void'");
        ok     = q.ok() && q.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_TransactionAbortNoCommit) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<int> {
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                           " (v) VALUES ('abort_me')");
            if (!ins)
                throw transaction_abort{ins.error()};
            auto bad = co_await tr.query("SELECT * FROM qb_nonexistent_table_12345");
            if (!bad)
                throw transaction_abort{bad.error()};
            co_return 1;
        });
        if (r.ok())
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") +
                                     std::string(kCoroApiTable) + " WHERE v = 'abort_me'");
        ok     = q.ok() && q.result()[0][0].as<int>() == 0;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_ExceptionRethrowAndRollback) {
    bool caught = false;
    bool rolled = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        try {
            co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<void> {
                auto         ins =
                    co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                        " (v) VALUES ('ex_rollback')");
                if (!ins)
                    throw transaction_abort{ins.error()};
                throw std::runtime_error("coro_tx_test_abort");
            });
        } catch (std::runtime_error const &) {
            caught = true;
        }
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") +
                                     std::string(kCoroApiTable) + " WHERE v = 'ex_rollback'");
        rolled = q.ok() && q.result()[0][0].as<int>() == 0;
    }());
    ASSERT_TRUE(caught);
    ASSERT_TRUE(rolled);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_ReadOnlyModeSelect) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        transaction_mode mode;
        mode.read_only = true;
        auto         r =
            co_await with_transaction(*db_, mode, [](Transaction &tr) -> qb::io::async::task<int> {
                auto q = co_await tr.query("SELECT 1 AS x");
                if (!q)
                    throw transaction_abort{q.error()};
                co_return static_cast<int>(q.result().size());
            });
        ok = r.ok() && r.result() == 1;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroSavepointRollbackNested) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto b = co_await db_->begin();
        if (!b)
            co_return;
        auto sp = co_await db_->savepoint("coro_sp_api");
        if (!sp)
            co_return;
        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                         " (v) VALUES ('sp_nested')");
        if (!ins)
            co_return;
        auto rb = co_await db_->rollback_savepoint("coro_sp_api");
        if (!rb)
            co_return;
        auto c = co_await db_->commit();
        if (!c)
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") +
                                     std::string(kCoroApiTable) + " WHERE v = 'sp_nested'");
        ok     = q.ok() && q.result()[0][0].as<int>() == 0;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_PrepareAndExecuteInside) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto         r =
            co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<std::string> {
                auto pr = co_await tr.prepare("qb_coro_in_tx",
                                              std::string("INSERT INTO ") +
                                                  std::string(kCoroApiTable) + " (v) VALUES ($1)",
                                              type_oid_sequence{oid::text});
                if (!pr)
                    throw transaction_abort{pr.error()};
                auto ex = co_await tr.execute("qb_coro_in_tx", params{std::string("in_tx_prep")});
                if (!ex)
                    throw transaction_abort{ex.error()};
                co_return std::string{"ok"};
            });
        if (!r.ok() || r.result() != "ok")
            co_return;
        auto q = co_await db_->query(std::string("SELECT v FROM ") + std::string(kCoroApiTable) +
                                     " WHERE v = 'in_tx_prep'");
        ok     = q.ok() && q.result().size() == 1;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, TransactionQuery_AliasMatchesExecute) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto via_query   = co_await db_->query("SELECT 2 AS n");
        auto via_execute = co_await db_->execute("SELECT 2 AS n");
        ok               = via_query.ok() && via_execute.ok() && via_query.result().size() == 1 &&
             via_execute.result().size() == 1 &&
             via_query.result()[0][0].as<int>() == via_execute.result()[0][0].as<int>();
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroSavepointInvalidNameRejectedClientSide) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto b = co_await db_->begin();
        if (!b)
            co_return;
        auto sp = co_await db_->savepoint("bad!");
        if (sp.ok())
            co_return;
        auto rb = co_await db_->rollback();
        ok      = rb.ok();
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroReleaseSavepointKeepsRow) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto b = co_await db_->begin();
        if (!b)
            co_return;
        auto sp = co_await db_->savepoint("rel_keep");
        if (!sp)
            co_return;
        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                         " (v) VALUES ('rel_keep_v')");
        if (!ins)
            co_return;
        auto rel = co_await db_->release_savepoint("rel_keep");
        if (!rel)
            co_return;
        auto c = co_await db_->commit();
        if (!c)
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") +
                                     std::string(kCoroApiTable) + " WHERE v = 'rel_keep_v'");
        ok     = q.ok() && q.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroPrepareInvalidSqlFails) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto         pr =
            co_await db_->prepare("qb_coro_bad_sql", "SELECT )syntax_error(", type_oid_sequence{});
        ok = !pr.ok();
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroExecuteFile) {
    std::filesystem::path const sql_path =
        std::filesystem::temp_directory_path() / "qb_pgsql_coro_execute_file.sql";
    {
        std::ofstream f(sql_path);
        ASSERT_TRUE(f.is_open());
        f << "SELECT 42 AS n";
        ASSERT_TRUE(f.good());
    }
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await db_->execute_file(sql_path);
        ok     = r.ok() && r.result().size() == 1 && r.result()[0][0].as<int>() == 42;
    }());
    std::error_code ec;
    std::filesystem::remove(sql_path, ec);
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroPrepareFile) {
    std::filesystem::path const sql_path =
        std::filesystem::temp_directory_path() / "qb_pgsql_coro_prepare_file.sql";
    {
        std::ofstream f(sql_path);
        ASSERT_TRUE(f.is_open());
        f << "SELECT $1::text AS t";
        ASSERT_TRUE(f.good());
    }
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto pr = co_await db_->prepare_file("qb_coro_prep_file_stmt", sql_path,
                                             type_oid_sequence{oid::text});
        if (!pr.ok())
            co_return;
        auto ex = co_await db_->execute("qb_coro_prep_file_stmt", params{std::string("file_arg")});
        ok = ex.ok() && ex.result().size() == 1 && ex.result()[0][0].as<std::string>() == "file_arg";
    }());
    std::error_code ec;
    std::filesystem::remove(sql_path, ec);
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_SequentialBothCommit) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r1 = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<void> {
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                           " (v) VALUES ('seq_a')");
            if (!ins)
                throw transaction_abort{ins.error()};
        });
        auto r2 = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<void> {
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                           " (v) VALUES ('seq_b')");
            if (!ins)
                throw transaction_abort{ins.error()};
        });
        if (!r1.ok() || !r2.ok())
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") +
                                     std::string(kCoroApiTable) + " WHERE v IN ('seq_a','seq_b')");
        ok     = q.ok() && q.result()[0][0].as<int>() == 2;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_EmptyVoidBodyStillCommits) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(
            *db_, [](Transaction &) -> qb::io::async::task<void> { co_return; });
        ok = r.ok();
    }());
    ASSERT_TRUE(ok);
}

/**
 * READ ONLY still allows writes to TEMP tables in PostgreSQL; use a non-temporary table to assert
 * the server rejects the INSERT and `transaction_abort` / failed `Reply` behavior.
 */
TEST_F(PgsqlCoroApiTest, WithTransaction_ReadOnlyRejectsInsertViaAbort) {
    constexpr char const *kRoGuard = "qb_pgsql_coro_ro_guard";
    ASSERT_TRUE(
        db_->execute(std::string("CREATE TABLE IF NOT EXISTS ") + kRoGuard + " (v TEXT NOT NULL)",
                     qb::pg::discard_query, qb::pg::discard_error)
            .await());
    ASSERT_TRUE(db_->execute(std::string("TRUNCATE TABLE ") + kRoGuard, qb::pg::discard_query,
                             qb::pg::discard_error)
                    .await());

    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        transaction_mode mode;
        mode.read_only = true;
        auto r         = co_await with_transaction(
            *db_, mode, [](Transaction &tr) -> qb::io::async::task<int> {
                auto ins = co_await tr.execute(std::string("INSERT INTO ") + kRoGuard +
                                                       " (v) VALUES ('ro_violation')");
                if (!ins)
                    throw transaction_abort{ins.error()};
                co_return 1;
            });
        ok = !r.ok();
        if (!ok)
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + kRoGuard +
                                     " WHERE v = 'ro_violation'");
        ok     = q.ok() && q.result()[0][0].as<int>() == 0;
    }());
    ASSERT_TRUE(ok);

    (void) db_
        ->execute(std::string("DROP TABLE IF EXISTS ") + kRoGuard, qb::pg::discard_query,
                  qb::pg::discard_error)
        .await();
}

TEST_F(PgsqlCoroApiTest, WithTransaction_MultiStatementReturnsAggregate) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<int> {
            for (char const *v : {"m1", "m2", "m3"}) {
                auto         ins =
                    co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                        " (v) VALUES ('" + v + "')");
                if (!ins)
                    throw transaction_abort{ins.error()};
            }
            auto         q =
                co_await tr.query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) +
                                  " WHERE v LIKE 'm%' AND LENGTH(v) = 2");
            if (!q)
                throw transaction_abort{q.error()};
            co_return q.result()[0][0].as<int>();
        });
        ok     = r.ok() && r.result() == 3;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_SavepointReleaseInsideScope) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<void> {
            auto sp = co_await tr.savepoint("inner_sp");
            if (!sp)
                throw transaction_abort{sp.error()};
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) +
                                           " (v) VALUES ('in_sp')");
            if (!ins)
                throw transaction_abort{ins.error()};
            auto rel = co_await tr.release_savepoint("inner_sp");
            if (!rel)
                throw transaction_abort{rel.error()};
        });
        if (!r.ok())
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") +
                                     std::string(kCoroApiTable) + " WHERE v = 'in_sp'");
        ok     = q.ok() && q.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_SerializableIsolationSmoke) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        transaction_mode mode;
        mode.isolation = isolation_level::serializable;
        auto         r =
            co_await with_transaction(*db_, mode, [](Transaction &tr) -> qb::io::async::task<int> {
                auto q = co_await tr.query(std::string("SELECT COUNT(*) FROM ") +
                                           std::string(kCoroApiTable));
                if (!q)
                    throw transaction_abort{q.error()};
                co_return q.result()[0][0].as<int>();
            });
        ok = r.ok() && r.result() >= 0;
    }());
    ASSERT_TRUE(ok);
}

/**
 * Nested `with_transaction` runs a second `BEGIN` inside an open transaction. Behavior is not
 * identical across PostgreSQL versions and client stacks: some reject the inner `BEGIN` (then you
 * must `throw transaction_abort{inner.error()}` and the outer `Reply` fails); others complete both
 * scopes and return the inner value. We only require a well-defined outcome and a usable connection
 * afterward — production code should still avoid nesting and use savepoints.
 */
TEST_F(PgsqlCoroApiTest, NestedWithTransaction_PortableOutcome) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<int> {
            auto inner = co_await with_transaction(
                tr, [](Transaction &) -> qb::io::async::task<int> { co_return 42; });
            if (!inner.ok())
                throw transaction_abort{inner.error()};
            co_return inner.result();
        });
        const bool        inner_begin_rejected = !r.ok();
        const bool        both_committed       = r.ok() && r.result() == 42;
        if (!inner_begin_rejected && !both_committed)
            co_return;
        auto ping = co_await db_->query("SELECT 1");
        ok        = ping.ok();
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroExecuteAfterErrorRequiresRollback_Manual) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto b = co_await db_->begin();
        if (!b)
            co_return;
        auto bad = co_await db_->query("SELECT * FROM qb_no_such_table_coro_xyz");
        if (bad.ok())
            co_return;
        auto rb = co_await db_->rollback();
        if (!rb)
            co_return;
        auto good = co_await db_->query("SELECT 1");
        ok        = good.ok();
    }());
    ASSERT_TRUE(ok);
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
