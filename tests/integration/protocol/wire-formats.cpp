/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file wire-formats.cpp
 * @brief Integration: extended-query Bind format-code matrix (Text vs Binary per column),
 *        parameter format codes, and COPY / empty-query protocol corner paths.
 *
 * The unique value of this file (vs the type-codec round-trip suite) is asserting the
 * *format code* the backend assigns to each result column and each bound parameter —
 * the simple-query path always returns Text, while the extended (prepared) path returns
 * Binary for decodable OIDs and Text for the rest. These assertions pin the module's
 * result-format routing (common.h) and the param encoder's per-OID format selection.
 *
 * Skips (never fails) when the daemon is unreachable.
 *
 * Migrated from test-protocol-integration.cpp. LISTEN/NOTIFY cases moved to
 * integration/notify/listen-notify.cpp (D6). COPY round-trips kept here as the
 * protocol-path corner; CopyToStdout now asserts the streamed bytes, not just `ok`.
 */

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <qb/io/async.h>
#include <qb/json.h>
#include <qb/uuid.h>
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/test_config.hpp"
#include "../pgsql.h"

using namespace qb::pg;
using qb::pg::detail::numeric; // exact-decimal marker type (lives in detail)
using qb::pg::test::PgIntegrationTest;

namespace {

/// Fixture: connected `db_` + a scratch COPY table; session TZ pinned for stable text.
class WireFormats : public PgIntegrationTest {
protected:
    void
    SetUp() override {
        PgIntegrationTest::SetUp();
        if (IsSkipped())
            return;
        ASSERT_TRUE(db_->execute("SET TIME ZONE 'UTC'", discard_query, discard_error).await());
        ASSERT_TRUE(db_->execute("CREATE TEMP TABLE qb_copy_t (v TEXT NOT NULL)", discard_query, discard_error).await());
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Result-column format codes: simple query -> Text, prepared -> Binary/Text per OID
// ---------------------------------------------------------------------------

TEST_F(WireFormats, SimpleQuery_AllResultColumnsAreText) {
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                       "SELECT 7::int AS n, 'x'::text AS t",
                       [&](transaction &, results r) {
                           ASSERT_EQ(r.size(), 1u);
                           ASSERT_EQ(r.columns_size(), 2u);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Text);
                           EXPECT_EQ(r.field(1).format_code, protocol_data_format::Text);
                           EXPECT_EQ(r[0][0].as<int>(), 7);
                           EXPECT_EQ(r[0][1].as<std::string>(), "x");
                           ok = true;
                       },
                       [](error::db_error e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(ok);
}

TEST_F(WireFormats, Prepared_IntResultColumnIsBinary) {
    ASSERT_TRUE(db_->prepare("bin_one", "SELECT ($1::int * 2) AS n", type_oid_sequence{oid::int4}, discard_prepare, discard_error).await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                       "bin_one", params{11},
                       [&](transaction &, results r) {
                           ASSERT_EQ(r.size(), 1u);
                           ASSERT_EQ(r.columns_size(), 1u);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                           EXPECT_EQ(r[0][0].as<int>(), 22);
                           ok = true;
                       },
                       [](error::db_error e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(ok);
}

// The matrix: a single prepared SELECT exposing one column per representative OID, with
// an explicit expectation of Binary (decodable) vs Text (no binary decoder) per RESULT column.
//
// NB on PARAMETER OIDs: the binding sends each param in the format the client picked for the
// *declared* OID — binary for OIDs with a verified codec, text otherwise. So the param OID must
// match the C++ value being sent: int8/float8/uuid/bytea are passed as their native binary types,
// but numeric / timestamptz / inet are passed as *text strings* and MUST therefore be declared as
// `oid::text` (or `oid::unknown`) so PostgreSQL receives text and casts via the `::numeric` /
// `::timestamptz` / `::inet` in the SQL. Declaring them as their target binary OID while sending a
// text string produced "insufficient data left in message" / "incorrect binary data format" — the
// server tried to read the ASCII bytes as a binary numeric/timestamp/inet. The result column
// formats (what this test asserts) are unaffected by the parameter transmission format.
TEST_F(WireFormats, Prepared_ResultFormatMatrix) {
    ASSERT_TRUE(db_->prepare("fmt_matrix",
                             "SELECT $1::int8 AS i8, $2::text AS s, $3::float8 AS f8, "
                             "$4::numeric AS num, $5::timestamptz AS tz, $6::uuid AS u, "
                             "$7::bytea AS b, $8::inet AS net",
                             type_oid_sequence{oid::int8, oid::text, oid::float8, oid::text, oid::text, oid::uuid, oid::bytea, oid::text},
                             discard_prepare, discard_error)
                    .await());

    const auto  u = qb::uuid::from_string("6ba7b810-9dad-11d1-80b4-00c04fd430c8").value();
    const bytea bin{{static_cast<byte>(0xDE), static_cast<byte>(0xAD)}};

    bool ok = false;
    ASSERT_TRUE(db_->execute(
                       "fmt_matrix",
                       params{
                           static_cast<int64_t>(-9), std::string{"pq"}, 1.25, std::string{"3.50"}, std::string{"2023-01-15 12:34:56+00"}, u,
                           bin, std::string{"192.168.0.1"}
                       },
                       [&](transaction &, results r) {
                           ASSERT_EQ(r.size(), 1u);
                           ASSERT_EQ(r.columns_size(), 8u);
                           // Decodable OIDs come back Binary:
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary); // int8
                           EXPECT_EQ(r.field(2).format_code, protocol_data_format::Binary); // float8
                           EXPECT_EQ(r.field(3).format_code, protocol_data_format::Binary); // numeric
                           EXPECT_EQ(r.field(4).format_code, protocol_data_format::Binary); // timestamptz
                           EXPECT_EQ(r.field(5).format_code, protocol_data_format::Binary); // uuid
                           EXPECT_EQ(r.field(6).format_code, protocol_data_format::Binary); // bytea
                           // No binary decoder -> Text:
                           EXPECT_EQ(r.field(1).format_code, protocol_data_format::Text); // text
                           EXPECT_EQ(r.field(7).format_code, protocol_data_format::Text); // inet

                           // Values, to prove the asserted format actually decodes:
                           EXPECT_EQ(r[0][0].as<int64_t>(), -9);
                           EXPECT_EQ(r[0][1].as<std::string>(), "pq");
                           EXPECT_DOUBLE_EQ(r[0][2].as<double>(), 1.25);
                           EXPECT_EQ(r[0][3].as<numeric>().str(), "3.50");
                           EXPECT_EQ(r[0][5].as<qb::uuid>(), u);
                           EXPECT_EQ(r[0][6].as<bytea>(), bin);
                           EXPECT_EQ(r[0][7].as<std::string>(), "192.168.0.1");
                           ok = true;
                       },
                       [](error::db_error e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(ok);
}

// Parameter format codes: bound params are sent binary for decodable OIDs and text for
// the rest. Observe indirectly via pg_typeof on the bound value (it must arrive as the
// declared OID regardless of transmission format) plus a binary read-back.
TEST_F(WireFormats, Prepared_ParameterFormat_BinaryAndText) {
    ASSERT_TRUE(db_->prepare("param_fmt",
                             "SELECT pg_typeof($1::int8)::text AS ty8, $1::int8 AS v8, "
                             "pg_typeof($2::text)::text AS tytxt, $2::text AS vtxt",
                             type_oid_sequence{oid::int8, oid::text}, discard_prepare, discard_error)
                    .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                       "param_fmt", params{static_cast<int64_t>(123456789012345LL), std::string{"hi"}},
                       [&](transaction &, results r) {
                           ASSERT_EQ(r.size(), 1u);
                           EXPECT_EQ(r[0][0].as<std::string>(), "bigint");
                           EXPECT_EQ(r[0][1].as<int64_t>(), 123456789012345LL);
                           EXPECT_EQ(r[0][2].as<std::string>(), "text");
                           EXPECT_EQ(r[0][3].as<std::string>(), "hi");
                           ok = true;
                       },
                       [](error::db_error e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(ok);
}

TEST_F(WireFormats, Prepared_TextParameter_TextResult) {
    ASSERT_TRUE(db_->prepare("bin_textparam", "SELECT $1::text AS s", type_oid_sequence{oid::text}, discard_prepare, discard_error).await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                       "bin_textparam", params{std::string{"hello-binary"}},
                       [&](transaction &, results r) {
                           ASSERT_EQ(r.size(), 1u);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Text);
                           EXPECT_EQ(r[0][0].as<std::string>(), "hello-binary");
                           ok = true;
                       },
                       [](error::db_error e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(ok);
}

TEST_F(WireFormats, Prepared_NoParameters_StillBinaryResults) {
    ASSERT_TRUE(db_->prepare("bin_nop", "SELECT true AS b, 42::int AS n", type_oid_sequence{}, discard_prepare, discard_error).await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                       "bin_nop", params{},
                       [&](transaction &, results r) {
                           ASSERT_EQ(r.size(), 1u);
                           ASSERT_EQ(r.columns_size(), 2u);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                           EXPECT_EQ(r.field(1).format_code, protocol_data_format::Binary);
                           EXPECT_TRUE(r[0][0].as<bool>());
                           EXPECT_EQ(r[0][1].as<int>(), 42);
                           ok = true;
                       },
                       [](error::db_error e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(ok);
}

TEST_F(WireFormats, Prepared_JsonText_JsonbBinary) {
    ASSERT_TRUE(db_->prepare("json_col", "SELECT '[]'::json AS j", type_oid_sequence{}, discard_prepare, discard_error).await());
    ASSERT_TRUE(db_->prepare("jsonb_col", "SELECT '[true,1]'::jsonb AS jb", type_oid_sequence{}, discard_prepare, discard_error).await());
    bool json_ok = false;
    ASSERT_TRUE(db_->execute(
                       "json_col", params{},
                       [&](transaction &, results r) {
                           ASSERT_EQ(r.size(), 1u);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Text);
                           EXPECT_EQ(r[0][0].as<std::string>(), "[]");
                           json_ok = true;
                       },
                       [](error::db_error e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(json_ok);

    bool jsonb_ok = false;
    ASSERT_TRUE(db_->execute(
                       "jsonb_col", params{},
                       [&](transaction &, results r) {
                           ASSERT_EQ(r.size(), 1u);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                           const qb::jsonb jb = r[0][0].as<qb::jsonb>();
                           EXPECT_TRUE(jb.is_array());
                           EXPECT_EQ(jb.size(), 2u);
                           jsonb_ok = true;
                       },
                       [](error::db_error e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(jsonb_ok);
}

TEST_F(WireFormats, Prepared_InsertReturning_MixedBinaryAndText) {
    ASSERT_TRUE(db_->prepare("ins_ret",
                             "INSERT INTO qb_copy_t (v) VALUES ($1) RETURNING "
                             "42::int AS k, $1::text AS lbl",
                             type_oid_sequence{oid::text}, discard_prepare, discard_error)
                    .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                       "ins_ret", params{std::string{"ret-mix"}},
                       [&](transaction &, results r) {
                           ASSERT_EQ(r.size(), 1u);
                           ASSERT_EQ(r.columns_size(), 2u);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                           EXPECT_EQ(r.field(1).format_code, protocol_data_format::Text);
                           EXPECT_EQ(r[0][0].as<int>(), 42);
                           EXPECT_EQ(r[0][1].as<std::string>(), "ret-mix");
                           ok = true;
                       },
                       [](error::db_error e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(ok);
}

TEST_F(WireFormats, Insert_WithoutReturning_EmptyResultset) {
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                       "INSERT INTO qb_copy_t (v) VALUES ('no-returning-rows')",
                       [&](transaction &, results r) {
                           EXPECT_EQ(r.size(), 0u);
                           EXPECT_EQ(r.columns_size(), 0u);
                           ok = true;
                       },
                       [](error::db_error e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(ok);
}

// ---------------------------------------------------------------------------
// COPY paths + empty simple query
// ---------------------------------------------------------------------------

TEST_F(WireFormats, CopyInStream_LoadsRowsThenSelectableBack) {
    // COPY FROM STDIN via the streaming source API: feed three text rows, then read them
    // back. Exercises CopyInResponse + CopyData(frontend) + CopyDone.
    bool        loaded = false;
    std::string load_err;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        int  step  = 0;
        auto reply = co_await db_->copy_in("COPY qb_copy_t (v) FROM STDIN", [&]() -> std::optional<std::string> {
            switch (step++) {
                case 0:
                    return std::string{"row1\n"};
                case 1:
                    return std::string{"row2\nrow3\n"};
                default:
                    return std::nullopt;
            }
        });
        if (!reply.ok()) {
            load_err = reply.error().what();
            co_return;
        }
        loaded = true;
    }());
    ASSERT_TRUE(loaded) << load_err;

    bool ok = false;
    ASSERT_TRUE(db_->execute(
                       "SELECT v FROM qb_copy_t ORDER BY v",
                       [&](transaction &, results r) {
                           ASSERT_EQ(r.size(), 3u);
                           EXPECT_EQ(r[0][0].as<std::string>(), "row1");
                           EXPECT_EQ(r[1][0].as<std::string>(), "row2");
                           EXPECT_EQ(r[2][0].as<std::string>(), "row3");
                           ok = true;
                       },
                       [](error::db_error e) { FAIL() << e.what(); })
                    .await());
    EXPECT_TRUE(ok);
}

TEST_F(WireFormats, CopyFromStdin_NoSource_RejectedThenRecovers) {
    // A bare COPY ... FROM STDIN with no streaming source must fail (the server gets no
    // data / CopyFail) and the connection must accept the next simple query.
    bool saw_error = false;
    auto st =
        db_->execute(
               "COPY qb_copy_t FROM STDIN", [](transaction &, results) { FAIL() << "COPY FROM STDIN must not complete as a normal query"; },
               [&saw_error](error::db_error const &) { saw_error = true; })
            .await();
    EXPECT_FALSE(st);
    EXPECT_TRUE(saw_error);
    EXPECT_TRUE(db_->execute("SELECT 1", discard_query, discard_error).await());
}

TEST_F(WireFormats, CopyToStdout_TextFormat_StreamsExpectedBytes) {
    // copy_out delivers each CopyData chunk to the sink (rows are NOT buffered into a
    // resultset). Assert the streamed payload bytes, not just completion. Text format =>
    // one row per chunk, newline-terminated.
    std::string streamed;
    bool        copy_ok = false;
    std::string copy_err;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply = co_await db_->copy_out("COPY (SELECT unnest(ARRAY['a','b','c']::text[])) TO STDOUT WITH (FORMAT text)",
                                            [&](std::string_view chunk) { streamed.append(chunk); });
        copy_ok    = reply.ok();
        if (!reply.ok())
            copy_err = reply.error().what();
    }());
    EXPECT_TRUE(copy_ok) << copy_err;
    EXPECT_EQ(streamed, "a\nb\nc\n");
}

TEST_F(WireFormats, EmptySimpleQuery_Completes) {
    bool ok = false;
    auto st = db_->execute(
                     "", [&ok](transaction &, results) { ok = true; }, [](error::db_error const &e) { FAIL() << "empty query: " << e.what(); })
                  .await();
    EXPECT_TRUE(st);
    EXPECT_TRUE(ok);
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
