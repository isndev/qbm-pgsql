/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file integration/api/coro-api.cpp
 * @brief Live integration tests for the coroutine-only PostgreSQL API surface.
 *
 * Every case here drives the coroutine overloads (`co_await db_->query(...)`,
 * `execute`, `prepare`, `begin/commit/rollback`, `with_transaction`, COPY in/out,
 * `query_stream`, out-of-band `cancel`) — no user callbacks. Blocking sync code uses
 * `qb::io::async::run_sync` on a `task` from an immediately-invoked coroutine lambda.
 *
 * Tier: integration (REQUIRES live postgres). Derives from the shared
 * `PgIntegrationTest` skip-not-fail fixture: when the daemon is unreachable the whole
 * binary is `GTEST_SKIP`-ped, never hard-failed.
 *
 * The three pure-logic free TESTs that used to live here (`PgsqlCancel`,
 * `ScramNonce`, `ScramEscape`) moved to `unit/auth/scram-and-cancel.cpp`.
 *
 * @ingroup Pgsql
 */
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/test_config.hpp"
#include "../pgsql.h"

using namespace qb::pg;
using namespace qb::pg::detail;

namespace {

/** Temp table name for this translation unit (session-local; integration tests are serialized). */
constexpr std::string_view kCoroApiTable = "qb_pgsql_coro_api_t";

} // namespace

/**
 * @brief Coroutine-API fixture: connect-or-skip, then (re)create a session TEMP table.
 *
 * Extends the shared `PgIntegrationTest` (which connects `db_` or skips the suite) by
 * provisioning the per-TU work table. All table setup uses the callback+await transport
 * so SetUp stays outside any `run_sync` coroutine.
 */
class PgsqlCoroApiTest : public qb::pg::test::PgIntegrationTest {
protected:
    void
    SetUp() override {
        qb::pg::test::PgIntegrationTest::SetUp(); // connect-or-skip
        if (IsSkipped())
            return;
        ASSERT_TRUE(
            db_->execute(std::string("DROP TABLE IF EXISTS ") + std::string(kCoroApiTable), qb::pg::discard_query, qb::pg::discard_error)
                .await());
        ASSERT_TRUE(db_->execute(std::string("CREATE TEMP TABLE ") + std::string(kCoroApiTable) + " (id SERIAL PRIMARY KEY, v TEXT NOT NULL)",
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
    bool ok    = false;
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
        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('a')");
        if (!ins)
            co_return;
        auto sel = co_await db_->query(std::string("SELECT v FROM ") + std::string(kCoroApiTable) + " WHERE v = 'a'");
        ok       = sel.ok() && sel.result().size() == 1 && sel.result()[0][0].as<std::string>() == "a";
    }());
    ASSERT_TRUE(ok);
}

// Typed row mapping: row.as<std::tuple<...>>(), resultset::one<Ts...>() and all<Ts...>(),
// plus structured-binding iteration.
TEST_F(PgsqlCoroApiTest, TypedRowMappingTupleOneAll) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('x'),('y'),('z')");
        if (!ins)
            co_return;
        auto sel = co_await db_->query(std::string("SELECT id, v FROM ") + std::string(kCoroApiTable) + " ORDER BY id");
        if (!sel.ok())
            co_return;
        const auto &res = sel.result();

        auto [id0, v0] = res[0].as<std::tuple<int, std::string>>();
        auto first     = res.one<int, std::string>();
        auto rows      = res.all<int, std::string>();

        std::string concat;
        for (auto [id, v] : rows) {
            (void) id;
            concat += v;
        }
        std::string concat2;
        for (auto [id, v] : res.rows<int, std::string>()) {
            (void) id;
            concat2 += v;
        }

        ok = res.size() == 3 && v0 == "x" && first.has_value() && std::get<1>(*first) == "x" && std::get<0>(*first) == id0 && rows.size() == 3
             && std::get<1>(rows[2]) == "z" && concat == "xyz" && concat2 == "xyz";
        co_return;
    }());
    ASSERT_TRUE(ok);
}

// Validates the UNNAMED prepared statement ("") round-trip and that it is reusable.
TEST_F(PgsqlCoroApiTest, UnnamedPrepareExecuteIsReusable) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto p = co_await db_->prepare("", "SELECT $1::int + $2::int AS s", type_oid_sequence{oid::int4, oid::int4});
        if (!p)
            co_return;
        auto r = co_await db_->execute("", QueryParams(2, 3));
        if (!(r.ok() && r.result().size() == 1 && r.result()[0][0].as<int>() == 5))
            co_return;
        auto p2 = co_await db_->prepare("", "SELECT $1::text AS t", type_oid_sequence{oid::text});
        if (!p2)
            co_return;
        auto r2 = co_await db_->execute("", QueryParams(std::string("hi")));
        ok      = r2.ok() && r2.result().size() == 1 && r2.result()[0][0].as<std::string>() == "hi";
        co_return;
    }());
    ASSERT_TRUE(ok);
}

// Inline one-shot parameterized query: db.query(sql, args...) with deduced OIDs.
TEST_F(PgsqlCoroApiTest, InlineParameterizedQuery) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto sum = co_await db_->query(std::string("SELECT $1::int + $2::int"), 2, 3);
        if (!(sum.ok() && sum.result().size() == 1 && sum.result()[0][0].as<int>() == 5))
            co_return;

        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('alpha'),('beta')");
        if (!ins)
            co_return;
        auto a  = co_await db_->query(std::string("SELECT id, v FROM ") + std::string(kCoroApiTable) + " WHERE v = $1", std::string("alpha"));
        auto b  = co_await db_->query(std::string("SELECT id, v FROM ") + std::string(kCoroApiTable) + " WHERE v = $1", std::string("beta"));
        auto ra = a.result().all<int, std::string>();
        auto rb = b.result().all<int, std::string>();
        ok      = a.ok() && b.ok() && ra.size() == 1 && std::get<1>(ra[0]) == "alpha" && rb.size() == 1 && std::get<1>(rb[0]) == "beta";
        co_return;
    }());
    ASSERT_TRUE(ok);
}

// Zero-copy field access: field::text() (string_view) and view() (span<const byte>).
TEST_F(PgsqlCoroApiTest, ZeroCopyFieldViewAndText) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('hello')");
        if (!ins)
            co_return;
        auto sel = co_await db_->query(std::string("SELECT v FROM ") + std::string(kCoroApiTable) + " WHERE v = 'hello'");
        if (!sel.ok() || sel.result().size() != 1)
            co_return;
        const auto      &res = sel.result();
        std::string_view tv  = res[0][0].text();
        auto             sp  = res[0][0].view();
        ok                   = tv == "hello" && sp.size() == 5 && res[0][0].as<std::string>() == std::string(tv);
        co_return;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroBeginCommit) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto b = co_await db_->begin();
        if (!b)
            co_return;
        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('txn_ok')");
        if (!ins)
            co_return;
        auto c = co_await db_->commit();
        if (!c)
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) + " WHERE v = 'txn_ok'");
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
        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('rolled')");
        if (!ins)
            co_return;
        auto rb = co_await db_->rollback();
        if (!rb)
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) + " WHERE v = 'rolled'");
        ok     = q.ok() && q.result().size() == 1 && q.result()[0][0].as<int>() == 0;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroPrepareAndExecutePrepared) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto pr = co_await db_->prepare("qb_coro_api_ins", std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ($1)",
                                        type_oid_sequence{oid::text});
        if (!pr)
            co_return;
        auto ex = co_await db_->execute("qb_coro_api_ins", params{std::string("prepared_row")});
        if (!ex)
            co_return;
        auto q = co_await db_->query(std::string("SELECT v FROM ") + std::string(kCoroApiTable) + " WHERE v = 'prepared_row'");
        ok     = q.ok() && q.result().size() == 1;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroInvalidSqlYieldsFailure) {
    error::db_error err{""};
    bool            failed = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply = co_await db_->query("THIS IS NOT VALID SQL");
        failed     = !reply.ok();
        if (failed)
            err = reply.error();
    }());
    ASSERT_TRUE(failed);
    EXPECT_EQ(err.sqlstate, sqlstate::syntax_error); // 42601
}

TEST_F(PgsqlCoroApiTest, WithTransaction_ReturnsValueAndCommits) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<int> {
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('with_tx_value')");
            if (!ins)
                throw transaction_abort{ins.error()};
            co_return 99;
        });
        if (!r.ok() || r.result() != 99)
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) + " WHERE v = 'with_tx_value'");
        ok     = q.ok() && q.result().size() == 1 && q.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_VoidBodyCommits) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<void> {
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('with_tx_void')");
            if (!ins)
                throw transaction_abort{ins.error()};
        });
        if (!r.ok())
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) + " WHERE v = 'with_tx_void'");
        ok     = q.ok() && q.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(ok);
}

/**
 * @brief P8: a nested with_transaction must FAIL FAST, not silently flatten.
 *
 * PostgreSQL has one transaction per session; a second BEGIN inside a live block warns 25001 and
 * flattens, so the inner scope's COMMIT/ROLLBACK would end the OUTER transaction and lose
 * isolation. The busy-guard rejects the nested begin (use a SAVEPOINT to nest); the outer
 * transaction still completes normally.
 */
TEST_F(PgsqlCoroApiTest, WithTransaction_NestedIsRejectedNotFlattened) {
    bool nested_rejected = false, outer_ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [&](Transaction &) -> qb::io::async::task<void> {
            // Inside the live transaction, in_transaction() is now true → the nested begin is refused.
            auto nested     = co_await with_transaction(*db_, [](Transaction &) -> qb::io::async::task<void> { co_return; });
            nested_rejected = !nested.ok();
            co_return;
        });
        outer_ok = r.ok();
    }());
    EXPECT_TRUE(nested_rejected) << "a nested with_transaction must be rejected, not flattened";
    EXPECT_TRUE(outer_ok) << "the outer transaction still completes";
}

TEST_F(PgsqlCoroApiTest, WithTransaction_TransactionAbortNoCommit) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<int> {
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('abort_me')");
            if (!ins)
                throw transaction_abort{ins.error()};
            auto bad = co_await tr.query("SELECT * FROM qb_nonexistent_table_12345");
            if (!bad)
                throw transaction_abort{bad.error()};
            co_return 1;
        });
        if (r.ok())
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) + " WHERE v = 'abort_me'");
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
                auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('ex_rollback')");
                if (!ins)
                    throw transaction_abort{ins.error()};
                throw std::runtime_error("coro_tx_test_abort");
            });
        } catch (std::runtime_error const &) {
            caught = true;
        }
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) + " WHERE v = 'ex_rollback'");
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
        auto r         = co_await with_transaction(*db_, mode, [](Transaction &tr) -> qb::io::async::task<int> {
            auto q = co_await tr.query("SELECT 1 AS x");
            if (!q)
                throw transaction_abort{q.error()};
            co_return static_cast<int>(q.result().size());
        });
        ok             = r.ok() && r.result() == 1;
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
        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('sp_nested')");
        if (!ins)
            co_return;
        auto rb = co_await db_->rollback_savepoint("coro_sp_api");
        if (!rb)
            co_return;
        auto c = co_await db_->commit();
        if (!c)
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) + " WHERE v = 'sp_nested'");
        ok     = q.ok() && q.result()[0][0].as<int>() == 0;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_PrepareAndExecuteInside) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<std::string> {
            auto pr = co_await tr.prepare("qb_coro_in_tx", std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ($1)",
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
        auto q = co_await db_->query(std::string("SELECT v FROM ") + std::string(kCoroApiTable) + " WHERE v = 'in_tx_prep'");
        ok     = q.ok() && q.result().size() == 1;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, TransactionQuery_AliasMatchesExecute) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto via_query   = co_await db_->query("SELECT 2 AS n");
        auto via_execute = co_await db_->execute("SELECT 2 AS n");
        ok               = via_query.ok() && via_execute.ok() && via_query.result().size() == 1 && via_execute.result().size() == 1
                           && via_query.result()[0][0].as<int>() == via_execute.result()[0][0].as<int>();
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
        auto ins = co_await db_->execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('rel_keep_v')");
        if (!ins)
            co_return;
        auto rel = co_await db_->release_savepoint("rel_keep");
        if (!rel)
            co_return;
        auto c = co_await db_->commit();
        if (!c)
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) + " WHERE v = 'rel_keep_v'");
        ok     = q.ok() && q.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroPrepareInvalidSqlFails) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto pr = co_await db_->prepare("qb_coro_bad_sql", "SELECT )syntax_error(", type_oid_sequence{});
        ok      = !pr.ok();
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, CoroExecuteFile) {
    std::filesystem::path const sql_path = std::filesystem::temp_directory_path() / "qb_pgsql_coro_execute_file.sql";
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
    std::filesystem::path const sql_path = std::filesystem::temp_directory_path() / "qb_pgsql_coro_prepare_file.sql";
    {
        std::ofstream f(sql_path);
        ASSERT_TRUE(f.is_open());
        f << "SELECT $1::text AS t";
        ASSERT_TRUE(f.good());
    }
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto pr = co_await db_->prepare_file("qb_coro_prep_file_stmt", sql_path, type_oid_sequence{oid::text});
        if (!pr.ok())
            co_return;
        auto ex = co_await db_->execute("qb_coro_prep_file_stmt", params{std::string("file_arg")});
        ok      = ex.ok() && ex.result().size() == 1 && ex.result()[0][0].as<std::string>() == "file_arg";
    }());
    std::error_code ec;
    std::filesystem::remove(sql_path, ec);
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_SequentialBothCommit) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r1 = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<void> {
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('seq_a')");
            if (!ins)
                throw transaction_abort{ins.error()};
        });
        auto r2 = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<void> {
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('seq_b')");
            if (!ins)
                throw transaction_abort{ins.error()};
        });
        if (!r1.ok() || !r2.ok())
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) + " WHERE v IN ('seq_a','seq_b')");
        ok     = q.ok() && q.result()[0][0].as<int>() == 2;
    }());
    ASSERT_TRUE(ok);
}

TEST_F(PgsqlCoroApiTest, WithTransaction_EmptyVoidBodyStillCommits) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &) -> qb::io::async::task<void> { co_return; });
        ok     = r.ok();
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
        db_->execute(std::string("CREATE TABLE IF NOT EXISTS ") + kRoGuard + " (v TEXT NOT NULL)", qb::pg::discard_query, qb::pg::discard_error)
            .await());
    ASSERT_TRUE(db_->execute(std::string("TRUNCATE TABLE ") + kRoGuard, qb::pg::discard_query, qb::pg::discard_error).await());

    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        transaction_mode mode;
        mode.read_only = true;
        auto r         = co_await with_transaction(*db_, mode, [](Transaction &tr) -> qb::io::async::task<int> {
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + kRoGuard + " (v) VALUES ('ro_violation')");
            if (!ins)
                throw transaction_abort{ins.error()};
            co_return 1;
        });
        ok             = !r.ok();
        if (!ok)
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + kRoGuard + " WHERE v = 'ro_violation'");
        ok     = q.ok() && q.result()[0][0].as<int>() == 0;
    }());
    ASSERT_TRUE(ok);

    (void) db_->execute(std::string("DROP TABLE IF EXISTS ") + kRoGuard, qb::pg::discard_query, qb::pg::discard_error).await();
}

TEST_F(PgsqlCoroApiTest, WithTransaction_MultiStatementReturnsAggregate) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<int> {
            for (char const *v : {"m1", "m2", "m3"}) {
                auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('" + v + "')");
                if (!ins)
                    throw transaction_abort{ins.error()};
            }
            auto q =
                co_await tr.query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) + " WHERE v LIKE 'm%' AND LENGTH(v) = 2");
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
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('in_sp')");
            if (!ins)
                throw transaction_abort{ins.error()};
            auto rel = co_await tr.release_savepoint("inner_sp");
            if (!rel)
                throw transaction_abort{rel.error()};
        });
        if (!r.ok())
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) + " WHERE v = 'in_sp'");
        ok     = q.ok() && q.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(ok);
}

// A savepoint name that passes pg_savepoint_name_ok (alphanumeric/underscore) but is BOTH
// digit-leading AND mixed-case: unquoted it is a syntax error (`SAVEPOINT 1Pt_Mix`) — and even a
// non-digit mixed-case name would silently case-fold, diverging from the quoted callback API. The
// co_await path now quotes the identifier exactly like SavePointQuery, so create + release round-trip
// under the same name. This is the regression guard for that cross-API quoting fix.
TEST_F(PgsqlCoroApiTest, Savepoint_DigitLeadingMixedCaseNameIsQuoted) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<void> {
            auto sp = co_await tr.savepoint("1Pt_Mix"); // pre-fix: `SAVEPOINT 1Pt_Mix` -> 42601 syntax error
            if (!sp)
                throw transaction_abort{sp.error()};
            auto ins = co_await tr.execute(std::string("INSERT INTO ") + std::string(kCoroApiTable) + " (v) VALUES ('mixsp')");
            if (!ins)
                throw transaction_abort{ins.error()};
            auto rel = co_await tr.release_savepoint("1Pt_Mix"); // must resolve the SAME savepoint
            if (!rel)
                throw transaction_abort{rel.error()};
        });
        if (!r.ok())
            co_return;
        auto q = co_await db_->query(std::string("SELECT COUNT(*) FROM ") + std::string(kCoroApiTable) + " WHERE v = 'mixsp'");
        ok     = q.ok() && q.result()[0][0].as<int>() == 1;
    }());
    ASSERT_TRUE(ok);
}

// SERIALIZABLE mode is actually applied: the body reads back `SHOW transaction_isolation`
// from inside the scope and must observe `serializable` (not merely `result()>=0`).
TEST_F(PgsqlCoroApiTest, WithTransaction_SerializableIsolationApplied) {
    bool        ok = false;
    std::string iso;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        transaction_mode mode;
        mode.isolation = isolation_level::serializable;
        auto r         = co_await with_transaction(*db_, mode, [](Transaction &tr) -> qb::io::async::task<std::string> {
            auto q = co_await tr.query("SHOW transaction_isolation");
            if (!q)
                throw transaction_abort{q.error()};
            co_return q.result()[0][0].as<std::string>();
        });
        ok             = r.ok();
        if (ok)
            iso = r.result();
    }());
    ASSERT_TRUE(ok);
    EXPECT_EQ(iso, "serializable");
}

/**
 * Nested `with_transaction` runs a second `BEGIN` inside an open transaction. Behavior is not
 * identical across PostgreSQL versions; we only require a well-defined outcome and a usable
 * connection afterward.
 */
TEST_F(PgsqlCoroApiTest, NestedWithTransaction_PortableOutcome) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto       r                    = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<int> {
            auto inner = co_await with_transaction(tr, [](Transaction &) -> qb::io::async::task<int> { co_return 42; });
            if (!inner.ok())
                throw transaction_abort{inner.error()};
            co_return inner.result();
        });
        const bool inner_begin_rejected = !r.ok();
        const bool both_committed       = r.ok() && r.result() == 42;
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

// Out-of-band query cancellation: db_->cancel() opens a SEPARATE connection and sends a
// CancelRequest built from the captured BackendKeyData, aborting the in-flight query. The
// awaiting caller observes SQLSTATE 57014 / sqlstate::query_canceled and the connection
// stays usable afterwards.
//
// HARDENED (no wall-clock race): a SECOND connection (`sentinel`) is the cancel trigger.
// Both the long pg_sleep and the trigger run as coroutine tasks on the SAME loop, driven by
// one `run_sync`. The trigger co_awaits a poll of pg_stat_activity until the sleep is
// server-confirmed RUNNING on the target backend, then issues db_->cancel() — so the cancel
// can never fire before the query is actually parked, nor after it would have completed. No
// nested run_sync (which would trip the ready-drain guard).
TEST_F(PgsqlCoroApiTest, CancelInFlightQuery) {
    auto sentinel = std::make_unique<qb::pg::tcp::database>();
    ASSERT_TRUE(qb::io::async::run_sync(sentinel->connect(qb::pg::test::dsn_tcp_string())));
    const int target_pid = db_->backend_pid();
    ASSERT_GT(target_pid, 0) << "need the backend PID of the connection that will run pg_sleep";

    bool        failed          = false;
    bool        is_cancel_state = false;
    bool        cancel_issued   = false;
    std::string code;

    auto trigger = [&]() -> qb::io::async::task<void> {
        const std::string probe = std::string("SELECT count(*)::int FROM pg_stat_activity WHERE pid = ") + std::to_string(target_pid)
                                  + " AND query LIKE 'SELECT pg_sleep%' AND state = 'active'";
        for (int attempt = 0; attempt < 250; ++attempt) { // up to ~5s @ 20ms; deadline-bounded
            auto a = co_await sentinel->query(probe);
            if (a.ok() && a.result().size() == 1 && a.result()[0][0].as<int>() > 0) {
                cancel_issued = db_->cancel();
                co_return;
            }
            auto pause = co_await sentinel->query("SELECT pg_sleep(0.02)"); // yield ~20ms without blocking the loop
            (void) pause;
        }
        co_return;
    };

    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        qb::io::async::coro_scheduler().spawn(trigger());
        auto r = co_await db_->query("SELECT pg_sleep(5)");
        failed = !r.ok();
        if (!r.ok()) {
            code            = r.error().code;
            is_cancel_state = (r.error().sqlstate == sqlstate::query_canceled);
        }
        co_return;
    }());
    sentinel->disconnect();

    EXPECT_TRUE(cancel_issued) << "db_->cancel() should have been issued once the sleep was server-confirmed running";
    EXPECT_TRUE(failed) << "pg_sleep(5) should have been canceled, not completed";
    EXPECT_EQ(code, "57014");
    EXPECT_TRUE(is_cancel_state);

    // A cancel aborts only the running query; the connection survives.
    bool ok_after = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r   = co_await db_->query("SELECT 1 AS one");
        ok_after = r.ok() && r.result().size() == 1 && r.result()[0][0].as<int>() == 1;
        co_return;
    }());
    EXPECT_TRUE(ok_after) << "connection must remain usable after a cancel";
}

// COPY ... TO STDOUT streams each row to the sink as it arrives. Verifies text + CSV.
TEST_F(PgsqlCoroApiTest, CopyOutStreamsToSink) {
    std::string text_out, csv_out;
    bool        ok_text = false, ok_csv = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->query("CREATE TEMP TABLE copyt (id int, v text)");
        (void) co_await db_->query("INSERT INTO copyt VALUES (1,'alpha'),(2,'beta'),(3,'gamma')");

        auto rt = co_await db_->copy_out("COPY copyt TO STDOUT", [&](std::string_view chunk) { text_out.append(chunk); });
        ok_text = rt.ok();

        auto rc = co_await db_->copy_out("COPY copyt TO STDOUT (FORMAT csv)", [&](std::string_view chunk) { csv_out.append(chunk); });
        ok_csv  = rc.ok();
        co_return;
    }());
    EXPECT_TRUE(ok_text);
    EXPECT_EQ(text_out, "1\talpha\n2\tbeta\n3\tgamma\n");
    EXPECT_TRUE(ok_csv);
    EXPECT_EQ(csv_out, "1,alpha\n2,beta\n3,gamma\n");
}

// COPY ... FROM STDIN bulk-loads client data; round-trips it back out.
TEST_F(PgsqlCoroApiTest, CopyInRoundTrip) {
    const std::string payload = "1\tx\n2\ty\n3\tz\n";
    std::string       out;
    bool              ok_in = false, ok_out = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->query("CREATE TEMP TABLE cprt (id int, v text)");
        auto ri = co_await db_->copy_in("COPY cprt FROM STDIN", payload);
        ok_in   = ri.ok();
        auto ro = co_await db_->copy_out("COPY cprt TO STDOUT", [&](std::string_view c) { out.append(c); });
        ok_out  = ro.ok();
        co_return;
    }());
    EXPECT_TRUE(ok_in);
    EXPECT_TRUE(ok_out);
    EXPECT_EQ(out, payload);
}

// COPY ... FROM STDIN driven by a streaming source (one row per source() call).
TEST_F(PgsqlCoroApiTest, CopyInStreamingSource) {
    int  loaded = 0;
    bool ok_in  = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->query("CREATE TEMP TABLE cins (id int)");
        int  next = 0;
        auto ri   = co_await db_->copy_in("COPY cins FROM STDIN", [&next]() -> std::optional<std::string> {
            if (next >= 1000)
                return std::nullopt;
            return std::to_string(next++) + "\n";
        });
        ok_in     = ri.ok();
        auto sel  = co_await db_->query("SELECT count(*)::int FROM cins");
        if (sel.ok() && sel.result().size() == 1)
            loaded = sel.result()[0][0].as<int>();
        co_return;
    }());
    EXPECT_TRUE(ok_in);
    EXPECT_EQ(loaded, 1000);
}

// A failing COPY must resolve the awaiter with an error (never hang) and leave the
// connection usable — both the server-side error (bad table) and a throwing source.
TEST_F(PgsqlCoroApiTest, CopyErrorsResolveAndConnectionSurvives) {
    bool out_failed = false, in_failed = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto ro    = co_await db_->copy_out("COPY qb_no_such_copy_table TO STDOUT", [](std::string_view) {});
        out_failed = !ro.ok();

        (void) co_await db_->query("CREATE TEMP TABLE cerr (id int)");
        auto ri =
            co_await db_->copy_in("COPY cerr FROM STDIN", []() -> std::optional<std::string> { throw std::runtime_error("source boom"); });
        in_failed = !ri.ok();

        auto ok  = co_await db_->query("SELECT 1 AS one");
        survived = ok.ok() && ok.result().size() == 1 && ok.result()[0][0].as<int>() == 1;
        co_return;
    }());
    EXPECT_TRUE(out_failed) << "COPY OUT on a missing table should resolve with an error";
    EXPECT_TRUE(in_failed) << "COPY IN with a throwing source should resolve with an error";
    EXPECT_TRUE(survived) << "the connection must stay usable after a failed COPY";
}

// query_stream fetches a large result in batches via a server-side cursor (constant
// memory). batch_size 137 does not divide 5000, exercising the short last batch.
TEST_F(PgsqlCoroApiTest, QueryStreamLargeResult) {
    std::uint64_t count = 0, sum = 0;
    bool          ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await db_->query_stream("SELECT g FROM generate_series(1, 5000) g", 137, [&](auto row) {
            ++count;
            sum += static_cast<std::uint64_t>(row[0].template as<int>());
        });
        ok     = r.ok();
        co_return;
    }());
    EXPECT_TRUE(ok);
    EXPECT_EQ(count, 5000u);
    EXPECT_EQ(sum, 5000ull * 5001 / 2);
}

// Inside an existing transaction, query_stream must NOT commit/rollback it.
TEST_F(PgsqlCoroApiTest, QueryStreamInsideTransaction) {
    std::uint64_t count = 0;
    bool          ok = false, committed = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->begin();
        auto r    = co_await db_->query_stream("SELECT g FROM generate_series(1, 100) g", 10, [&](auto) { ++count; });
        ok        = r.ok();
        auto c    = co_await db_->commit();
        committed = c.ok();
        co_return;
    }());
    EXPECT_TRUE(ok);
    EXPECT_EQ(count, 100u);
    EXPECT_TRUE(committed);
}

// A failing stream resolves with an error and leaves the connection usable.
TEST_F(PgsqlCoroApiTest, QueryStreamErrorResolvesAndSurvives) {
    bool failed = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r   = co_await db_->query_stream("SELECT * FROM qb_no_such_stream_table", 10, [](auto) {});
        failed   = !r.ok();
        auto ok  = co_await db_->query("SELECT 1");
        survived = ok.ok();
        co_return;
    }());
    EXPECT_TRUE(failed);
    EXPECT_TRUE(survived);
}

// An exception from on_row is rethrown (after closing the cursor + rolling back the
// self-opened transaction), and the connection stays usable.
TEST_F(PgsqlCoroApiTest, QueryStreamOnRowThrowRethrowsAndSurvives) {
    bool threw = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        try {
            (void) co_await db_->query_stream("SELECT g FROM generate_series(1, 100) g", 10,
                                              [](auto) { throw std::runtime_error("row boom"); });
        } catch (const std::exception &) {
            threw = true;
        }
        auto ok  = co_await db_->query("SELECT 1");
        survived = ok.ok();
        co_return;
    }());
    EXPECT_TRUE(threw);
    EXPECT_TRUE(survived);
}

// in_transaction() reflects the real ReadyForQuery status across idle / in-block /
// failed-block / rolled-back states.
TEST_F(PgsqlCoroApiTest, InTransactionTracksState) {
    bool after_select = true, after_begin = false, after_commit = true, in_failed = false, after_rollback = true;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->query("SELECT 1");
        after_select = db_->in_transaction();
        (void) co_await db_->begin();
        after_begin = db_->in_transaction();
        (void) co_await db_->commit();
        after_commit = db_->in_transaction();
        (void) co_await db_->begin();
        (void) co_await db_->query("SELECT * FROM qb_no_such_intxn_table");
        in_failed = db_->in_transaction();
        (void) co_await db_->rollback();
        after_rollback = db_->in_transaction();
        co_return;
    }());
    EXPECT_FALSE(after_select);
    EXPECT_TRUE(after_begin);
    EXPECT_FALSE(after_commit);
    EXPECT_TRUE(in_failed);
    EXPECT_FALSE(after_rollback);
}

// query_stream edge cases: empty result, exact-divisor batch, single row.
TEST_F(PgsqlCoroApiTest, QueryStreamEdgeCases) {
    std::uint64_t empty_n = 999, exact_n = 0, single_n = 0;
    bool          ok_empty = false, ok_exact = false, ok_single = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        empty_n   = 0;
        auto re   = co_await db_->query_stream("SELECT g FROM generate_series(1, 0) g", 10, [&](auto) { ++empty_n; });
        ok_empty  = re.ok();
        auto rx   = co_await db_->query_stream("SELECT g FROM generate_series(1, 100) g", 50, [&](auto) { ++exact_n; });
        ok_exact  = rx.ok();
        auto rs   = co_await db_->query_stream("SELECT 42", 10, [&](auto) { ++single_n; });
        ok_single = rs.ok();
        co_return;
    }());
    EXPECT_TRUE(ok_empty);
    EXPECT_EQ(empty_n, 0u);
    EXPECT_TRUE(ok_exact);
    EXPECT_EQ(exact_n, 100u);
    EXPECT_TRUE(ok_single);
    EXPECT_EQ(single_n, 1u);
}

// COPY in/out in BINARY format (distinct wire framing). Round-trips the binary stream.
TEST_F(PgsqlCoroApiTest, CopyBinaryRoundTrip) {
    std::string bin;
    int         count  = 0;
    bool        ok_out = false, ok_in = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->query("CREATE TEMP TABLE cbin (id int, v text)");
        (void) co_await db_->query("INSERT INTO cbin VALUES (1,'a'),(2,'b'),(3,'c')");
        auto ro = co_await db_->copy_out("COPY cbin TO STDOUT (FORMAT binary)", [&](std::string_view c) { bin.append(c); });
        ok_out  = ro.ok();
        (void) co_await db_->query("CREATE TEMP TABLE cbin2 (id int, v text)");
        auto ri  = co_await db_->copy_in("COPY cbin2 FROM STDIN (FORMAT binary)", bin);
        ok_in    = ri.ok();
        auto sel = co_await db_->query("SELECT count(*)::int FROM cbin2");
        if (sel.ok() && sel.result().size() == 1)
            count = sel.result()[0][0].as<int>();
        co_return;
    }());
    EXPECT_TRUE(ok_out);
    EXPECT_FALSE(bin.empty());
    EXPECT_TRUE(ok_in);
    EXPECT_EQ(count, 3);
}

// COPY FROM STDIN issued via a plain query() (no copy_in() source) must fail cleanly.
TEST_F(PgsqlCoroApiTest, CopyFromStdinWithoutSourceFails) {
    bool failed = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->query("CREATE TEMP TABLE cnosrc (id int)");
        auto r   = co_await db_->query("COPY cnosrc FROM STDIN");
        failed   = !r.ok();
        auto ok  = co_await db_->query("SELECT 1");
        survived = ok.ok();
        co_return;
    }());
    EXPECT_TRUE(failed) << "COPY FROM STDIN without a source should fail";
    EXPECT_TRUE(survived) << "connection must survive a sourceless COPY FROM STDIN";
}

// Regression: as<std::optional<int>> over the BINARY result path must decode a genuine
// int4 of -1 (0xFFFFFFFF) as -1, not std::nullopt; a real NULL -> nullopt.
TEST_F(PgsqlCoroApiTest, OptionalMinusOneAndNullViaBinary) {
    std::optional<int> neg, nul;
    bool               ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await db_->query("SELECT $1::int AS a, NULL::int AS b", -1);
        ok     = r.ok();
        if (r.ok() && r.result().size() == 1) {
            neg = r.result()[0][0].as<std::optional<int>>();
            nul = r.result()[0][1].as<std::optional<int>>();
        }
        co_return;
    }());
    EXPECT_TRUE(ok);
    ASSERT_TRUE(neg.has_value());
    EXPECT_EQ(*neg, -1);
    EXPECT_FALSE(nul.has_value());
}

// Regression: the timestamptz TEXT decoder must apply the printed UTC offset.
TEST_F(PgsqlCoroApiTest, TimestamptzTextOffsetApplied) {
    bool ok = false, equal = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->query("SET TIME ZONE 'Asia/Kolkata'");
        auto a = co_await db_->query("SELECT '2024-06-01 12:00:00+00'::timestamptz");
        (void) co_await db_->query("SET TIME ZONE 'UTC'");
        auto b = co_await db_->query("SELECT '2024-06-01 12:00:00+00'::timestamptz");
        ok     = a.ok() && b.ok() && a.result().size() == 1 && b.result().size() == 1;
        if (ok)
            equal = (a.result()[0][0].as<qb::wall_time>() == b.result()[0][0].as<qb::wall_time>());
        co_return;
    }());
    EXPECT_TRUE(ok);
    EXPECT_TRUE(equal) << "timestamptz text offset not applied (same instant decoded differently)";
}

// COPY IN where the client completes but the server REJECTS the data.
TEST_F(PgsqlCoroApiTest, CopyInServerRejectsData) {
    bool failed = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->query("CREATE TEMP TABLE crej (id int)");
        auto r   = co_await db_->copy_in("COPY crej FROM STDIN", std::string("not_an_int\n"));
        failed   = !r.ok();
        auto ok  = co_await db_->query("SELECT 1");
        survived = ok.ok();
        co_return;
    }());
    EXPECT_TRUE(failed) << "server should reject non-integer COPY data";
    EXPECT_TRUE(survived);
}

// used_channel_binding() must be FALSE on a plaintext connection (gs2 'n,,').
TEST_F(PgsqlCoroApiTest, ChannelBindingFalseOnPlaintext) {
    EXPECT_FALSE(db_->used_channel_binding());
}

// --------------------------------------------------------------------------------------
// Client-side savepoint-name validation (pg_savepoint_name_ok early-return branches)
// --------------------------------------------------------------------------------------

// An EMPTY savepoint name is rejected client-side (name.empty() branch) on all three
// savepoint verbs — savepoint / rollback_savepoint / release_savepoint — without ever
// touching the wire. The connection must stay usable afterwards.
TEST_F(PgsqlCoroApiTest, CoroSavepointEmptyNameRejectedClientSide) {
    bool sp_failed = false, rb_failed = false, rel_failed = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto b = co_await db_->begin();
        if (!b)
            co_return;
        auto sp    = co_await db_->savepoint("");
        sp_failed  = !sp.ok();
        auto rb    = co_await db_->rollback_savepoint("");
        rb_failed  = !rb.ok();
        auto rel   = co_await db_->release_savepoint("");
        rel_failed = !rel.ok();
        (void) co_await db_->rollback();
        auto ok  = co_await db_->query("SELECT 1");
        survived = ok.ok();
        co_return;
    }());
    EXPECT_TRUE(sp_failed) << "empty savepoint name must be rejected client-side";
    EXPECT_TRUE(rb_failed) << "empty rollback-to name must be rejected client-side";
    EXPECT_TRUE(rel_failed) << "empty release name must be rejected client-side";
    EXPECT_TRUE(survived);
}

// A savepoint name longer than 63 bytes is rejected client-side (name.size() > 63 branch).
TEST_F(PgsqlCoroApiTest, CoroSavepointTooLongNameRejectedClientSide) {
    bool failed = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto b = co_await db_->begin();
        if (!b)
            co_return;
        auto sp = co_await db_->savepoint(std::string(64, 'a'));
        failed  = !sp.ok();
        (void) co_await db_->rollback();
        co_return;
    }());
    EXPECT_TRUE(failed) << "savepoint name > 63 chars must be rejected client-side";
}

// rollback_savepoint / release_savepoint reject a malformed (non-alnum) name client-side
// (the per-character validation branch on those two verbs specifically).
TEST_F(PgsqlCoroApiTest, CoroRollbackAndReleaseSavepointInvalidNameRejected) {
    bool rb_failed = false, rel_failed = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto b = co_await db_->begin();
        if (!b)
            co_return;
        auto rb    = co_await db_->rollback_savepoint("bad name!");
        rb_failed  = !rb.ok();
        auto rel   = co_await db_->release_savepoint("also bad!");
        rel_failed = !rel.ok();
        (void) co_await db_->rollback();
        auto ok  = co_await db_->query("SELECT 1");
        survived = ok.ok();
        co_return;
    }());
    EXPECT_TRUE(rb_failed) << "malformed rollback-to name must be rejected client-side";
    EXPECT_TRUE(rel_failed) << "malformed release name must be rejected client-side";
    EXPECT_TRUE(survived);
}

// --------------------------------------------------------------------------------------
// execute_file / prepare_file error paths (missing file -> failed Reply, no throw)
// --------------------------------------------------------------------------------------

// A non-existent path makes coro execute_file resolve with a failed Reply (the
// std::filesystem::exists()==false branch) — not throw, not hang.
TEST_F(PgsqlCoroApiTest, CoroExecuteFileMissingFileFails) {
    std::filesystem::path const missing = std::filesystem::temp_directory_path() / "qb_pgsql_no_such_execute_file_zzz.sql";
    std::error_code             ec;
    std::filesystem::remove(missing, ec); // ensure absent
    bool failed = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r   = co_await db_->execute_file(missing);
        failed   = !r.ok();
        auto ok  = co_await db_->query("SELECT 1");
        survived = ok.ok();
        co_return;
    }());
    EXPECT_TRUE(failed) << "execute_file on a missing path must resolve with a failed Reply";
    EXPECT_TRUE(survived);
}

// A non-existent path makes coro prepare_file resolve with a failed Reply.
TEST_F(PgsqlCoroApiTest, CoroPrepareFileMissingFileFails) {
    std::filesystem::path const missing = std::filesystem::temp_directory_path() / "qb_pgsql_no_such_prepare_file_zzz.sql";
    std::error_code             ec;
    std::filesystem::remove(missing, ec);
    bool failed = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto pr  = co_await db_->prepare_file("qb_coro_missing_prep_stmt", missing, type_oid_sequence{});
        failed   = !pr.ok();
        auto ok  = co_await db_->query("SELECT 1");
        survived = ok.ok();
        co_return;
    }());
    EXPECT_TRUE(failed) << "prepare_file on a missing path must resolve with a failed Reply";
    EXPECT_TRUE(survived);
}

// --------------------------------------------------------------------------------------
// with_transaction: COMMIT-time failure -> ROLLBACK -> failed Reply
// --------------------------------------------------------------------------------------

// A DEFERRABLE INITIALLY DEFERRED unique constraint defers the duplicate-key check to
// COMMIT time, so the body succeeds but tr.commit() fails. with_transaction must then
// ROLLBACK and return a failed Reply (the value-path commit-failure branch). The
// connection must remain usable.
TEST_F(PgsqlCoroApiTest, WithTransaction_CommitFailureRollsBackValuePath) {
    constexpr char const *kDeferT = "qb_pgsql_coro_defer_commit";
    ASSERT_TRUE(db_->execute(std::string("DROP TABLE IF EXISTS ") + kDeferT, qb::pg::discard_query, qb::pg::discard_error).await());
    ASSERT_TRUE(db_->execute(std::string("CREATE TABLE ") + kDeferT + " (id int, UNIQUE(id) DEFERRABLE INITIALLY DEFERRED)",
                             qb::pg::discard_query, qb::pg::discard_error)
                    .await());
    bool failed = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r   = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<int> {
            auto a = co_await tr.execute(std::string("INSERT INTO ") + kDeferT + " VALUES (1)");
            if (!a)
                throw transaction_abort{a.error()};
            auto b = co_await tr.execute(std::string("INSERT INTO ") + kDeferT + " VALUES (1)");
            if (!b)
                throw transaction_abort{b.error()};
            co_return 7; // both INSERTs succeed; the dup-key check fires at COMMIT
        });
        failed   = !r.ok(); // COMMIT must fail, so the Reply is a failure
        auto ok  = co_await db_->query("SELECT 1");
        survived = ok.ok();
        co_return;
    }());
    (void) db_->execute(std::string("DROP TABLE IF EXISTS ") + kDeferT, qb::pg::discard_query, qb::pg::discard_error).await();
    EXPECT_TRUE(failed) << "with_transaction must surface the deferred-constraint COMMIT failure";
    EXPECT_TRUE(survived);
}

// Same deferred-constraint COMMIT failure but through a task<void> body (the void-path
// commit-failure branch).
TEST_F(PgsqlCoroApiTest, WithTransaction_CommitFailureRollsBackVoidPath) {
    constexpr char const *kDeferT = "qb_pgsql_coro_defer_commit_void";
    ASSERT_TRUE(db_->execute(std::string("DROP TABLE IF EXISTS ") + kDeferT, qb::pg::discard_query, qb::pg::discard_error).await());
    ASSERT_TRUE(db_->execute(std::string("CREATE TABLE ") + kDeferT + " (id int, UNIQUE(id) DEFERRABLE INITIALLY DEFERRED)",
                             qb::pg::discard_query, qb::pg::discard_error)
                    .await());
    bool failed = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r   = co_await with_transaction(*db_, [](Transaction &tr) -> qb::io::async::task<void> {
            auto a = co_await tr.execute(std::string("INSERT INTO ") + kDeferT + " VALUES (1)");
            if (!a)
                throw transaction_abort{a.error()};
            auto b = co_await tr.execute(std::string("INSERT INTO ") + kDeferT + " VALUES (1)");
            if (!b)
                throw transaction_abort{b.error()};
        });
        failed   = !r.ok();
        auto ok  = co_await db_->query("SELECT 1");
        survived = ok.ok();
        co_return;
    }());
    (void) db_->execute(std::string("DROP TABLE IF EXISTS ") + kDeferT, qb::pg::discard_query, qb::pg::discard_error).await();
    EXPECT_TRUE(failed) << "void-body with_transaction must surface the deferred COMMIT failure";
    EXPECT_TRUE(survived);
}

// --------------------------------------------------------------------------------------
// with_transaction with every explicit isolation level + read-only (begin(mode) coro path
// + to_string ISOLATION LEVEL / READ ONLY / DEFERRABLE emission).
// --------------------------------------------------------------------------------------

// REPEATABLE READ is actually applied: the body reads SHOW transaction_isolation back.
TEST_F(PgsqlCoroApiTest, WithTransaction_RepeatableReadIsolationApplied) {
    std::string seen;
    bool        ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        transaction_mode mode;
        mode.isolation = isolation_level::repeatable_read;
        auto r         = co_await with_transaction(*db_, mode, [](Transaction &tr) -> qb::io::async::task<std::string> {
            auto q = co_await tr.query("SHOW transaction_isolation");
            if (!q.ok() || q.result().size() != 1)
                throw transaction_abort{q.error()};
            co_return q.result()[0][0].as<std::string>();
        });
        ok             = r.ok();
        if (ok)
            seen = r.result();
        co_return;
    }());
    EXPECT_TRUE(ok);
    EXPECT_EQ(seen, "repeatable read");
}

// SERIALIZABLE READ ONLY DEFERRABLE: drives the full transaction_mode to_string emission
// (ISOLATION LEVEL SERIALIZABLE, READ ONLY, DEFERRABLE) AND the begin(mode) coro path; the
// body reads back both the isolation and the read-only GUC.
TEST_F(PgsqlCoroApiTest, WithTransaction_SerializableReadOnlyDeferrableApplied) {
    std::string iso, ro;
    bool        ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        transaction_mode mode{isolation_level::serializable, /*read_only=*/true, /*deferrable=*/true};
        auto             r = co_await with_transaction(*db_, mode, [](Transaction &tr) -> qb::io::async::task<std::string> {
            auto a = co_await tr.query("SHOW transaction_isolation");
            auto b = co_await tr.query("SHOW transaction_read_only");
            if (!a.ok() || !b.ok())
                throw transaction_abort{a.ok() ? b.error() : a.error()};
            co_return a.result()[0][0].as<std::string>() + "|" + b.result()[0][0].as<std::string>();
        });
        ok                 = r.ok();
        if (ok) {
            auto const &v   = r.result();
            auto        bar = v.find('|');
            iso             = v.substr(0, bar);
            ro              = v.substr(bar + 1);
        }
        co_return;
    }());
    EXPECT_TRUE(ok);
    EXPECT_EQ(iso, "serializable");
    EXPECT_EQ(ro, "on");
}

// Imperative coro begin(mode) with READ COMMITTED + DEFERRABLE only (no isolation change):
// exercises the DEFERRABLE-without-isolation branch of to_string (need_comma == false path).
TEST_F(PgsqlCoroApiTest, CoroBeginDeferrableOnly) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        transaction_mode mode;
        mode.deferrable = true; // isolation stays read_committed, read_only stays false
        auto b          = co_await db_->begin(mode);
        if (!b.ok())
            co_return;
        auto q = co_await db_->query("SELECT 1");
        auto c = co_await db_->commit();
        ok     = q.ok() && c.ok();
        co_return;
    }());
    EXPECT_TRUE(ok) << "BEGIN ... DEFERRABLE (no isolation change) must succeed";
}

// --------------------------------------------------------------------------------------
// connect(connection_options) overload (struct-form connect)
// --------------------------------------------------------------------------------------

// The connect(connection_options) overload (carries connect_timeout / ssl_verify) connects
// and the session is usable. A fresh database is used so its options are pristine.
TEST_F(PgsqlCoroApiTest, ConnectWithOptionsStruct) {
    auto opts            = connection_options::parse(qb::pg::test::dsn_tcp_string());
    opts.connect_timeout = std::chrono::seconds(5);
    auto db              = std::make_unique<qb::pg::tcp::database>();
    bool ok = false, queried = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        ok = static_cast<bool>(co_await db->connect(opts));
        if (!ok)
            co_return;
        auto q  = co_await db->query("SELECT 1");
        queried = q.ok() && q.result().size() == 1 && q.result()[0][0].as<int>() == 1;
        co_return;
    }());
    EXPECT_TRUE(ok) << "connect(connection_options) failed";
    EXPECT_TRUE(queried);
    db->disconnect();
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// ===========================================================================
// Two query_streams overlapping on ONE connection. Both halves of that were once broken:
//
//  * the cursor NAME used to be the fixed literal `qb_stream_cursor`, so the second DECLARE asked
//    for a name that already existed and PostgreSQL refused it ("cursor already exists");
//  * transaction OWNERSHIP used to be decided by `!in_transaction()`, which mirrors the last
//    ReadyForQuery. The second stream evaluated it while the first was still suspended on its own
//    BEGIN, read "idle", and opened a second transaction — PostgreSQL warns 25001 and keeps the ONE
//    session transaction, so whichever stream reached COMMIT first ended the block under the other,
//    whose next FETCH died with "cursor ... does not exist".
//
// `when_all` is the way to express the overlap: it suspends each branch at its `co_await` points,
// so the second DECLARE is issued while the first cursor is provably still open. (Nesting via
// `run_sync` inside `on_row` is NOT: the framework refuses re-entrant `run_sync`, so that path is
// unreachable by construction. Sequential streams prove nothing — CLOSE and COMMIT have both
// already run by the time the second DECLARE goes out.)
// ===========================================================================

TEST_F(PgsqlCoroApiTest, ConcurrentQueryStreamsDoNotCollideOnTheCursorName) {
    // The two lengths are deliberately lopsided: the short stream is finished after 2 FETCH round
    // trips while the long one still has ~98 of its 101 to go, so a stream that wrongly ends the
    // shared block kills its sibling every time. Balanced lengths do NOT test that — the earlier
    // 60-rows/batch-9 vs 35-rows/batch-6 pairing finished within one FETCH round of each other, so
    // the short stream's COMMIT happened to land after the long one's LAST FETCH and the test
    // passed on arithmetic rather than on behaviour.
    std::uint64_t long_rows = 0, short_rows = 0;
    bool          in_txn_after = true;

    const bool ok = qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto [r_long, r_short] = co_await qb::io::async::when_all(
            [&]() -> qb::io::async::task<bool> {
                auto r = co_await db_->query_stream("SELECT g FROM generate_series(1, 800) g", 8, [&](auto) { ++long_rows; });
                co_return r.ok();
            }(),
            [&]() -> qb::io::async::task<bool> {
                auto r = co_await db_->query_stream("SELECT g FROM generate_series(1, 5) g", 3, [&](auto) { ++short_rows; });
                co_return r.ok();
            }());
        // Both streams are done, so the block they shared must be closed — exactly once.
        in_txn_after = db_->in_transaction();
        co_return r_long && r_short;
    }());

    EXPECT_TRUE(ok) << "two overlapping query_stream calls on one connection failed: either both asked for the same "
                       "cursor name ('cursor already exists'), or one of them ended the transaction they share "
                       "('cursor \"qb_stream_cursor_N\" does not exist')";
    EXPECT_EQ(long_rows, 800u) << "the long stream lost its cursor before draining — the short one ended the shared transaction";
    EXPECT_EQ(short_rows, 5u);
    EXPECT_FALSE(in_txn_after) << "the transaction query_stream opened for its cursors was never closed";
}

// Same overlap, but inside a CALLER-opened transaction: query_stream only opens (and ends) a block
// when it finds none, so here neither stream may commit — the caller's COMMIT must still be the one
// that ends it. Guards the other side of the ownership decision.
TEST_F(PgsqlCoroApiTest, ConcurrentQueryStreamsInsideCallerTransactionLeaveItOpen) {
    std::uint64_t a_rows = 0, b_rows = 0;
    bool          still_in_txn = false, committed = false;

    const bool ok = qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto begun = co_await db_->begin();
        if (!begun.ok())
            co_return false;
        auto [ra, rb] = co_await qb::io::async::when_all(
            [&]() -> qb::io::async::task<bool> {
                auto r = co_await db_->query_stream("SELECT g FROM generate_series(1, 400) g", 8, [&](auto) { ++a_rows; });
                co_return r.ok();
            }(),
            [&]() -> qb::io::async::task<bool> {
                auto r = co_await db_->query_stream("SELECT g FROM generate_series(1, 5) g", 3, [&](auto) { ++b_rows; });
                co_return r.ok();
            }());
        still_in_txn = db_->in_transaction();
        auto c       = co_await db_->commit();
        committed    = c.ok();
        co_return ra && rb;
    }());

    EXPECT_TRUE(ok);
    EXPECT_EQ(a_rows, 400u);
    EXPECT_EQ(b_rows, 5u);
    EXPECT_TRUE(still_in_txn) << "a query_stream ended the caller's transaction instead of leaving it to the caller";
    EXPECT_TRUE(committed);
}

TEST_F(PgsqlCoroApiTest, SequentialQueryStreamsEachGetTheirOwnCursor) {
    std::uint64_t first = 0, second = 0;

    const bool ok = qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto a = co_await db_->query_stream("SELECT g FROM generate_series(1, 30) g", 8, [&](auto) { ++first; });
        if (!a.ok())
            co_return false;
        auto b = co_await db_->query_stream("SELECT g FROM generate_series(1, 12) g", 5, [&](auto) { ++second; });
        co_return b.ok();
    }());

    EXPECT_TRUE(ok);
    EXPECT_EQ(first, 30u);
    EXPECT_EQ(second, 12u) << "a second stream after the first closed must still work — the counter must not break reuse";
}
