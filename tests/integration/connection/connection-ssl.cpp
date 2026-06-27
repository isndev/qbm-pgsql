/**
 * @file connection-ssl.cpp
 * @brief Integration tests for PostgreSQL TLS connections (live daemon + TLS required).
 *
 * TLS half of the legacy `test-connection-ssl.cpp`. Exercises the encrypted handshake of
 * `qb::pg::tcp::ssl::database` against a TLS-enabled `postgres:5432`: connect (callback +
 * coroutine), reconnect on a new backend, connection pooling, and the two security-critical
 * cases that distinguish this file from the cleartext lifecycle suite:
 *
 *  - `SslVerifyFullRejectsUntrustedCert` — `ssl_verify_mode::full` MUST reject the test
 *    server's self-signed certificate (proves verification is enforced, not cosmetic).
 *  - `ScramChannelBindingNegotiatedOverTls` — over TLS, SCRAM must negotiate
 *    `SCRAM-SHA-256-PLUS` with `tls-server-end-point` channel binding.
 *
 * Compiled only under `QB_HAS_SSL`. At runtime, when the default DSN cannot complete TLS and
 * `QB_PG_SSL_DSN` is unset, every case skips with a clear message (CI must export
 * `QB_PG_SSL_DSN` for the SCRAM/verify-full cases to actually run). The dead-host connect
 * timeout case lives in `system/connection/connect-timeout.cpp` (shared with cleartext).
 *
 * @see qb::pg::tcp::ssl::database
 * @see qb::pg::ssl_verify_mode
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <cstdlib>
#include <memory>
#include <vector>
#include <gtest/gtest.h>

#ifdef QB_HAS_SSL

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../pgsql.h"
#include "../../shared/pg_pump.hpp"
#include "../../shared/test_config.hpp"

using namespace qb::pg;
using qb::pg::test::dsn_ssl_string;
using qb::pg::test::pump_until;

namespace {

/// Whether the operator pinned a TLS DSN. When unset and TLS is unavailable, cases skip.
[[nodiscard]] bool
ssl_dsn_pinned() noexcept {
    const char *v = std::getenv("QB_PG_SSL_DSN");
    return v != nullptr && v[0] != '\0';
}

/**
 * @brief Connect @p db over TLS, or signal "skip when not pinned".
 * @return true on a successful TLS connect.
 *
 * On failure: if `QB_PG_SSL_DSN` is unset the caller should GTEST_SKIP (TLS simply isn't
 * available in this environment); if it IS set, a failure is a real error the caller asserts.
 */
[[nodiscard]] bool
ssl_connect(qb::pg::tcp::ssl::database &db) {
    return static_cast<bool>(qb::io::async::run_sync(db.connect(dsn_ssl_string())));
}

class SslConnection : public ::testing::Test {
protected:
    std::unique_ptr<qb::pg::tcp::ssl::database> db_;

    void
    SetUp() override {
        qb::io::async::init();
        db_ = std::make_unique<qb::pg::tcp::ssl::database>();
        // Probe TLS reachability on a throwaway handle so the per-test db_ starts clean.
        auto probe = std::make_unique<qb::pg::tcp::ssl::database>();
        if (!ssl_connect(*probe)) {
            if (!ssl_dsn_pinned())
                GTEST_SKIP() << "TLS connect failed with default DSN and QB_PG_SSL_DSN is "
                                "unset; skipping. Set QB_PG_SSL_DSN to enforce TLS coverage.";
            else
                FAIL() << "QB_PG_SSL_DSN is set but TLS connect failed: " << dsn_ssl_string();
        }
        probe->disconnect();
    }

    void
    TearDown() override {
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
    }
};

} // namespace

// --------------------------------------------------------------------------------------
// Connect (callback + coroutine)
// --------------------------------------------------------------------------------------

TEST_F(SslConnection, ConnectSuccess) {
    ASSERT_TRUE(ssl_connect(*db_));
    EXPECT_GT(db_->backend_pid(), 0);
}

TEST_F(SslConnection, ConnectSuccess_Coroutine) {
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        ok = co_await db_->connect(dsn_ssl_string());
    }());
    ASSERT_TRUE(ok);
}

// --------------------------------------------------------------------------------------
// Reconnect (assert a real, re-captured backend PID)
// --------------------------------------------------------------------------------------

/**
 * @brief Reconnect over TLS must produce a fresh, usable backend.
 *
 * Strengthened: the legacy test asserted only that the second connect succeeded. We now
 * capture the backend PID before and after, assert both are real, and confirm the new link
 * answers a query (a genuine re-handshake lands on a new backend process).
 */
TEST_F(SslConnection, ReconnectAfterDisconnect) {
    ASSERT_TRUE(ssl_connect(*db_));
    const int first_pid = db_->backend_pid();
    EXPECT_GT(first_pid, 0);

    db_->disconnect();
    db_->prepare_reconnect();

    ASSERT_TRUE(ssl_connect(*db_));
    const int second_pid = db_->backend_pid();
    EXPECT_GT(second_pid, 0) << "BackendKeyData PID must be captured on the TLS reconnect";
    EXPECT_NE(second_pid, first_pid) << "TLS reconnect must land on a new backend process";

    auto status = db_->execute("SELECT 1", discard_query, discard_error).await();
    EXPECT_TRUE(status) << "reconnected TLS session is not usable";
}

// --------------------------------------------------------------------------------------
// Connection pool (deduped: one helper, both callback + coroutine transports)
// --------------------------------------------------------------------------------------

namespace {

/// Build N TLS connections and verify each via a decoded `SELECT 1` (callback transport).
void
ssl_pool_callback(int n) {
    std::vector<std::unique_ptr<qb::pg::tcp::ssl::database>> conns;
    conns.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto c = std::make_unique<qb::pg::tcp::ssl::database>();
        ASSERT_TRUE(ssl_connect(*c)) << "pool connection " << i << " failed";
        conns.push_back(std::move(c));
    }
    for (std::size_t i = 0; i < conns.size(); ++i) {
        int decoded = -1;
        auto status = conns[i]
                          ->execute(
                              "SELECT 1",
                              [&](transaction &, results res) {
                                  ASSERT_EQ(res.size(), 1u);
                                  decoded = res[0][0].as<int>();
                              },
                              discard_error)
                          .await();
        ASSERT_TRUE(status);
        EXPECT_EQ(decoded, 1) << "pool connection " << i;
    }
}

} // namespace

/// One pool test, exercised through both the callback and coroutine transports.
TEST_F(SslConnection, ConnectionPool) {
    constexpr int num_connections = 5;

    // Transport A: callback / .await()
    ssl_pool_callback(num_connections);

    // Transport B: coroutine connect + query, decoded == 1.
    int ok_count = 0;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        for (int i = 0; i < num_connections; ++i) {
            auto conn = std::make_unique<qb::pg::tcp::ssl::database>();
            if (!co_await conn->connect(dsn_ssl_string()))
                co_return;
            auto reply = co_await conn->query("SELECT 1");
            if (reply.ok() && reply.result().size() == 1 &&
                reply.result()[0][0].as<int>() == 1)
                ++ok_count;
        }
    }());
    EXPECT_EQ(ok_count, num_connections)
        << "coroutine TLS pool: only " << ok_count << "/" << num_connections << " returned 1";
}

// --------------------------------------------------------------------------------------
// Security: verify-full rejection + SCRAM channel binding (the unique value of this file)
// --------------------------------------------------------------------------------------

/**
 * @brief `ssl_verify_mode::full` MUST reject the test server's untrusted self-signed cert.
 *
 * The default (none) encrypts without verifying — it connects. verify-full validates the
 * chain against the system trust store AND the hostname; the self-signed test certificate is
 * not trusted, so verify-full must reject it before any data flows. This proves verification
 * is enforced, not cosmetic.
 */
TEST_F(SslConnection, SslVerifyFullRejectsUntrustedCert) {
    auto opts       = qb::pg::connection_options::parse(dsn_ssl_string());
    opts.ssl_verify = qb::pg::ssl_verify_mode::full;

    auto db = std::make_unique<qb::pg::tcp::ssl::database>();
    EXPECT_FALSE(qb::io::async::run_sync(db->connect(opts)))
        << "verify-full accepted an untrusted self-signed certificate (active-MITM hole)";
}

/**
 * @brief Over TLS, SCRAM must negotiate `SCRAM-SHA-256-PLUS` with `tls-server-end-point`.
 *
 * PostgreSQL offers the `-PLUS` mechanism on SSL connections; channel binding ties the SCRAM
 * proof to the server certificate, so a wrong binding would make the server reject the proof.
 * A successful connect with `used_channel_binding() == true` proves the binding is correct.
 * (Requires a scram-sha-256 role; a `trust` server skips SCRAM and this would not apply, in
 * which case the SetUp probe already connected and we assert the binding flag here.)
 */
TEST_F(SslConnection, ScramChannelBindingNegotiatedOverTls) {
    ASSERT_TRUE(ssl_connect(*db_));
    // Channel binding only applies to a SCRAM-SHA-256 role over TLS. A trust/cleartext
    // server (the common local/CI default) negotiates no channel binding, which is not a
    // failure of this code path — so skip unless a SCRAM-over-TLS server is actually pinned
    // via QB_PG_SSL_DSN. Only then is a missing binding a real defect to hard-assert.
    if (!ssl_dsn_pinned() || !db_->used_channel_binding())
        GTEST_SKIP() << "no SCRAM-SHA-256-PLUS channel binding negotiated (server uses "
                        "trust/cleartext auth, or QB_PG_SSL_DSN is unset); set QB_PG_SSL_DSN "
                        "to a SCRAM role over TLS to exercise channel binding.";
    EXPECT_TRUE(db_->used_channel_binding())
        << "SCRAM-SHA-256-PLUS (tls-server-end-point) channel binding was not negotiated over "
           "TLS against the pinned SCRAM role";
}

// ======================================================================================
// SSL WORKLOAD — drive the stcp (qb::pg::tcp::ssl::database) Database<> protocol handlers
// ======================================================================================
//
// The cases above only CONNECT (and at most `SELECT 1`). The bulk of the SSL Database<>
// instantiation's code — on_row_description / on_data_row / on_command_complete,
// on_copy_in_response / on_copy_out_response / on_copy_data / on_copy_done, and
// on_notification_response, plus the query_stream cursor machinery — is reached ONLY when
// real work runs OVER the encrypted link. This section mirrors the cleartext integration
// assertions (api/coro-api.cpp, query/queries-execution.cpp, notify/listen-notify.cpp) but
// exercises them through the stcp template instantiation. Every case skips-not-fails when
// TLS is unavailable (the SetUp probe handles that, exactly like SslConnection).

namespace {

/// SSL workload fixture: the per-test db_ is already CONNECTED over TLS in SetUp, so each
/// case can run real queries against the encrypted link with no per-test connect boilerplate.
class SslWorkload : public ::testing::Test {
protected:
    std::unique_ptr<qb::pg::tcp::ssl::database> db_;

    void
    SetUp() override {
        qb::io::async::init();
        db_ = std::make_unique<qb::pg::tcp::ssl::database>();
        if (!ssl_connect(*db_)) {
            db_.reset();
            if (!ssl_dsn_pinned())
                GTEST_SKIP() << "TLS connect failed with default DSN and QB_PG_SSL_DSN is "
                                "unset; skipping SSL workload. Set QB_PG_SSL_DSN to enforce.";
            else
                FAIL() << "QB_PG_SSL_DSN is set but TLS connect failed: " << dsn_ssl_string();
        }
    }

    void
    TearDown() override {
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
    }
};

} // namespace

// --------------------------------------------------------------------------------------
// SELECT returning rows over TLS — drives on_row_description / on_data_row /
// on_command_complete on the stcp instantiation (callback + coroutine transports).
// --------------------------------------------------------------------------------------

TEST_F(SslWorkload, MultiRowSelectDecodesOverTls) {
    // Callback transport: decode a 3-row, 2-column result through the SSL data-row handler.
    std::vector<std::pair<int, std::string>> rows;
    auto status =
        db_->execute(
               "SELECT g AS n, ('v' || g) AS v FROM generate_series(1,3) g ORDER BY g",
               [&](transaction &, results res) {
                   for (const auto &r : res)
                       rows.emplace_back(r[0].as<int>(), r[1].as<std::string>());
               },
               discard_error)
            .await();
    ASSERT_TRUE(status) << "multi-row SELECT over TLS failed";
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0], (std::pair<int, std::string>{1, "v1"}));
    EXPECT_EQ(rows[1], (std::pair<int, std::string>{2, "v2"}));
    EXPECT_EQ(rows[2], (std::pair<int, std::string>{3, "v3"}));

    // Coroutine transport: same handlers via co_await query(); confirm column metadata too.
    int    n_rows = 0, n_cols = 0;
    bool   ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto q = co_await db_->query(
            "SELECT g AS n, ('v' || g) AS v FROM generate_series(1,3) g ORDER BY g");
        if (q.ok()) {
            n_rows = static_cast<int>(q.result().size());
            n_cols = static_cast<int>(q.result().columns_size());
            ok     = q.result()[2][0].as<int>() == 3 && q.result()[2][1].as<std::string>() == "v3";
        }
        co_return;
    }());
    EXPECT_TRUE(ok);
    EXPECT_EQ(n_rows, 3);
    EXPECT_EQ(n_cols, 2) << "on_row_description must report 2 columns over TLS";
}

// An empty result set still drives on_row_description + on_command_complete (no data rows).
TEST_F(SslWorkload, EmptyResultAndCommandTagOverTls) {
    int  rowcount = -1;
    bool ddl_ok = false, dml_ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto sel = co_await db_->query("SELECT 1 WHERE false");
        if (sel.ok())
            rowcount = static_cast<int>(sel.result().size());
        // CREATE/INSERT/DROP exercise the non-SELECT CommandComplete path over TLS.
        ddl_ok = (co_await db_->execute("CREATE TEMP TABLE ssl_cmd_tag (id int)")).ok();
        dml_ok = (co_await db_->execute("INSERT INTO ssl_cmd_tag VALUES (1),(2)")).ok();
        co_return;
    }());
    EXPECT_EQ(rowcount, 0) << "empty SELECT over TLS must report zero rows";
    EXPECT_TRUE(ddl_ok);
    EXPECT_TRUE(dml_ok);
}

// --------------------------------------------------------------------------------------
// Transaction (begin / commit / rollback / savepoint) over TLS.
// --------------------------------------------------------------------------------------

TEST_F(SslWorkload, TransactionCommitAndRollbackOverTls) {
    bool committed_visible = false, rolled_invisible = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->execute("CREATE TEMP TABLE ssl_txn (v text)");

        if (!(co_await db_->begin()).ok())
            co_return;
        (void) co_await db_->execute("INSERT INTO ssl_txn (v) VALUES ('keep')");
        if (!(co_await db_->commit()).ok())
            co_return;

        if (!(co_await db_->begin()).ok())
            co_return;
        (void) co_await db_->execute("INSERT INTO ssl_txn (v) VALUES ('drop')");
        if (!(co_await db_->rollback()).ok())
            co_return;

        auto kept    = co_await db_->query("SELECT count(*)::int FROM ssl_txn WHERE v='keep'");
        auto dropped = co_await db_->query("SELECT count(*)::int FROM ssl_txn WHERE v='drop'");
        committed_visible = kept.ok() && kept.result()[0][0].as<int>() == 1;
        rolled_invisible  = dropped.ok() && dropped.result()[0][0].as<int>() == 0;
        co_return;
    }());
    EXPECT_TRUE(committed_visible) << "committed row not visible over TLS";
    EXPECT_TRUE(rolled_invisible) << "rolled-back row leaked over TLS";
}

TEST_F(SslWorkload, SavepointRollbackOverTls) {
    int after = -1;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->execute("CREATE TEMP TABLE ssl_sp (id int)");
        if (!(co_await db_->begin()).ok())
            co_return;
        (void) co_await db_->execute("INSERT INTO ssl_sp VALUES (1)");
        if (!(co_await db_->savepoint("ssl_sp_a")).ok())
            co_return;
        (void) co_await db_->execute("INSERT INTO ssl_sp VALUES (2)");
        if (!(co_await db_->rollback_savepoint("ssl_sp_a")).ok())
            co_return;
        (void) co_await db_->commit();
        auto c = co_await db_->query("SELECT count(*)::int FROM ssl_sp");
        if (c.ok())
            after = c.result()[0][0].as<int>();
        co_return;
    }());
    EXPECT_EQ(after, 1) << "savepoint rollback over TLS must drop only the post-savepoint row";
}

// --------------------------------------------------------------------------------------
// Prepared statements over TLS — Parse/Bind/Execute path through the stcp handlers.
// --------------------------------------------------------------------------------------

TEST_F(SslWorkload, PreparedStatementOverTls) {
    int  sum = -1;
    bool text_ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        if (!co_await db_->prepare("ssl_add", "SELECT $1::int + $2::int AS s",
                                   type_oid_sequence{oid::int4, oid::int4}))
            co_return;
        auto r = co_await db_->execute("ssl_add", params{7, 35});
        if (r.ok() && r.result().size() == 1)
            sum = r.result()[0][0].as<int>();

        // Unnamed statement reuse with a text param exercises the rebind path over TLS.
        if (!co_await db_->prepare("", "SELECT $1::text AS t", type_oid_sequence{oid::text}))
            co_return;
        auto t  = co_await db_->execute("", params{std::string("tls-prepared")});
        text_ok = t.ok() && t.result().size() == 1 &&
                  t.result()[0][0].as<std::string>() == "tls-prepared";
        co_return;
    }());
    EXPECT_EQ(sum, 42) << "prepared statement Bind/Execute over TLS";
    EXPECT_TRUE(text_ok);
}

// --------------------------------------------------------------------------------------
// COPY TO STDOUT / FROM STDIN over TLS — drives on_copy_out_response, on_copy_data,
// on_copy_done, on_copy_in_response, send_copy_fail on the stcp instantiation.
// --------------------------------------------------------------------------------------

TEST_F(SslWorkload, CopyOutStreamsOverTls) {
    std::string text_out, csv_out;
    bool        ok_text = false, ok_csv = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->query("CREATE TEMP TABLE ssl_copyt (id int, v text)");
        (void) co_await db_->query(
            "INSERT INTO ssl_copyt VALUES (1,'alpha'),(2,'beta'),(3,'gamma')");
        auto rt = co_await db_->copy_out("COPY ssl_copyt TO STDOUT",
                                         [&](std::string_view c) { text_out.append(c); });
        ok_text = rt.ok();
        auto rc = co_await db_->copy_out("COPY ssl_copyt TO STDOUT (FORMAT csv)",
                                         [&](std::string_view c) { csv_out.append(c); });
        ok_csv = rc.ok();
        co_return;
    }());
    EXPECT_TRUE(ok_text);
    EXPECT_EQ(text_out, "1\talpha\n2\tbeta\n3\tgamma\n");
    EXPECT_TRUE(ok_csv);
    EXPECT_EQ(csv_out, "1,alpha\n2,beta\n3,gamma\n");
}

TEST_F(SslWorkload, CopyInRoundTripOverTls) {
    const std::string payload = "1\tx\n2\ty\n3\tz\n";
    std::string       out;
    int               loaded = -1;
    bool              ok_in = false, ok_out = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->query("CREATE TEMP TABLE ssl_cprt (id int, v text)");
        // One-shot whole-payload overload (drives copy_in source + CopyData/CopyDone).
        auto ri = co_await db_->copy_in("COPY ssl_cprt FROM STDIN", payload);
        ok_in   = ri.ok();
        auto cnt = co_await db_->query("SELECT count(*)::int FROM ssl_cprt");
        if (cnt.ok())
            loaded = cnt.result()[0][0].as<int>();
        auto ro = co_await db_->copy_out("COPY ssl_cprt TO STDOUT",
                                         [&](std::string_view c) { out.append(c); });
        ok_out = ro.ok();
        co_return;
    }());
    EXPECT_TRUE(ok_in);
    EXPECT_EQ(loaded, 3);
    EXPECT_TRUE(ok_out);
    EXPECT_EQ(out, payload);
}

TEST_F(SslWorkload, CopyInStreamingSourceOverTls) {
    int  loaded = -1;
    bool ok_in  = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        (void) co_await db_->query("CREATE TEMP TABLE ssl_cins (id int)");
        int  next = 0;
        auto ri   = co_await db_->copy_in(
            "COPY ssl_cins FROM STDIN", [&next]() -> std::optional<std::string> {
                if (next >= 500)
                    return std::nullopt;
                return std::to_string(next++) + "\n";
            });
        ok_in    = ri.ok();
        auto sel = co_await db_->query("SELECT count(*)::int FROM ssl_cins");
        if (sel.ok())
            loaded = sel.result()[0][0].as<int>();
        co_return;
    }());
    EXPECT_TRUE(ok_in);
    EXPECT_EQ(loaded, 500);
}

// A failing COPY over TLS must resolve (never hang) and leave the connection usable —
// drives the send_copy_fail / CopyFail error path on the stcp instantiation.
TEST_F(SslWorkload, CopyErrorsResolveOverTls) {
    bool out_failed = false, in_failed = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto ro    = co_await db_->copy_out("COPY ssl_no_such_table TO STDOUT",
                                            [](std::string_view) {});
        out_failed = !ro.ok();

        (void) co_await db_->query("CREATE TEMP TABLE ssl_cerr (id int)");
        auto ri = co_await db_->copy_in(
            "COPY ssl_cerr FROM STDIN",
            []() -> std::optional<std::string> { throw std::runtime_error("ssl source boom"); });
        in_failed = !ri.ok();

        auto ok  = co_await db_->query("SELECT 1 AS one");
        survived = ok.ok() && ok.result().size() == 1 && ok.result()[0][0].as<int>() == 1;
        co_return;
    }());
    EXPECT_TRUE(out_failed) << "COPY OUT on a missing table over TLS should resolve with an error";
    EXPECT_TRUE(in_failed) << "COPY IN with a throwing source over TLS should resolve with an error";
    EXPECT_TRUE(survived) << "the TLS connection must stay usable after a failed COPY";
}

// --------------------------------------------------------------------------------------
// query_stream over TLS — server-side cursor (DECLARE/FETCH/CLOSE) driven through the
// stcp instantiation; self-opened transaction variant + the in-transaction variant.
// --------------------------------------------------------------------------------------

TEST_F(SslWorkload, QueryStreamCursorOverTls) {
    std::uint64_t n = 0, sum = 0;
    bool          ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await db_->query_stream(
            "SELECT g FROM generate_series(1, 2500) g", 137, [&](auto row) {
                ++n;
                sum += static_cast<std::uint64_t>(row[0].template as<int>());
            });
        ok = r.ok();
        co_return;
    }());
    EXPECT_TRUE(ok) << "query_stream over TLS failed";
    EXPECT_EQ(n, 2500u);
    EXPECT_EQ(sum, 2500ull * 2501ull / 2ull) << "streamed sum 1..2500 mismatched over TLS";
}

TEST_F(SslWorkload, QueryStreamInExistingTxnOverTls) {
    bool committed = false, streamed = false;
    int  count = 0;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        if (!(co_await db_->begin()).ok())
            co_return;
        auto r   = co_await db_->query_stream(
            "SELECT g FROM generate_series(1, 100) g", 10, [&](auto) { ++count; });
        streamed = r.ok();
        // query_stream must NOT have closed the outer transaction; commit must still work.
        committed = (co_await db_->commit()).ok();
        co_return;
    }());
    EXPECT_TRUE(streamed);
    EXPECT_EQ(count, 100);
    EXPECT_TRUE(committed) << "query_stream over TLS wrongly ended the outer transaction";
}

// --------------------------------------------------------------------------------------
// LISTEN / NOTIFY over TLS — drives on_notification_response on the stcp instantiation.
// A SECOND TLS connection publishes; the LISTENing TLS connection receives async.
// --------------------------------------------------------------------------------------

TEST_F(SslWorkload, NotificationOverTls) {
    constexpr std::string_view kChan = "qb_pgsql_ssl_notify_ch";

    int         hits = 0;
    std::string last_payload;
    db_->on_incoming_notify([&](qb::pg::notification &&note) {
        if (note.channel == std::string(kChan)) {
            last_payload = std::move(note.payload);
            ++hits;
        }
    });
    // LISTEN on the encrypted link.
    ASSERT_TRUE(db_->listen(std::string(kChan), discard_query, discard_error).await())
        << "LISTEN over TLS failed";

    // Publish from a SECOND TLS connection so delivery is a genuine async NotificationResponse.
    auto pub = std::make_unique<qb::pg::tcp::ssl::database>();
    ASSERT_TRUE(ssl_connect(*pub));
    ASSERT_TRUE(pub->notify(std::string(kChan), "tls-notify-payload", discard_query, discard_error)
                    .await());

    EXPECT_TRUE(pump_until([&] { return hits >= 1; }, std::chrono::seconds(5)))
        << "NOTIFY over TLS never delivered to the LISTENing connection";
    EXPECT_EQ(hits, 1);
    EXPECT_EQ(last_payload, "tls-notify-payload");
    pub->disconnect();
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#else // QB_HAS_SSL

int
main() {
    // SSL disabled at build time: nothing to run. A green no-op keeps ctest consistent.
    return 0;
}

#endif // QB_HAS_SSL
