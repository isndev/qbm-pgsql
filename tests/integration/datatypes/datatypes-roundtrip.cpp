/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file datatypes-roundtrip.cpp
 * @brief Integration: PostgreSQL data-type wire round-trips against a live backend.
 *
 * Each value is bound through the extended (prepared) protocol so result columns come
 * back in **binary** format — the path that exercises the module's `TypeConverter`
 * codecs (the simple-query path returns text and bypasses them). Every scalar carries a
 * server-side ground-truth check (`::text` cast / `= $1` round-trip) in addition to the
 * decoded C++ value, so a converter that silently agrees with itself cannot pass.
 *
 * Skips (never fails) when the daemon is unreachable, via the shared fixture.
 *
 * Migrated from test-data-types-integration.cpp. Key changes vs the monolith:
 *   - scalar skeleton parametrized via TYPED_TEST (kills ~600 LOC of copy-paste);
 *   - TIMESTAMP/TIMESTAMPTZ tolerance tightened 86400s -> 1s, pinned `SET TIME ZONE 'UTC'`
 *     + a fixed UTC instant (no `std::mktime`/`std::time(nullptr)` seed);
 *   - JSONB asserts the canonical decoded shape (no array-of-pairs self-normalize, no
 *     try/catch warning downgrade);
 *   - EdgeCases round-trips NUMERIC / DATE through real typed columns (not "HIGHPREC:" TEXT);
 *   - added: float NaN/±Inf, NUMERIC NaN/±Inf specials, DATE ±infinity sentinels,
 *     multi-dimensional + NULL-element arrays, empty-bytea-as-param protocol path.
 */

#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/json.h>
#include <qb/uuid.h>
#include "../pgsql.h"
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/test_config.hpp"

using namespace qb::pg;
using qb::pg::detail::numeric; // exact-decimal marker type (lives in detail)
using qb::pg::test::PgIntegrationTest;

namespace {

/**
 * @brief Fixture: a connected `db_` plus a scratch table per test, dropped on teardown.
 *
 * Session TZ pinned to UTC so timestamp/timestamptz round-trips are deterministic.
 */
class DataTypesRoundTrip : public PgIntegrationTest {
protected:
    void
    SetUp() override {
        PgIntegrationTest::SetUp(); // connect-or-skip
        if (IsSkipped())
            return;
        ASSERT_TRUE(db_->execute("SET TIME ZONE 'UTC'", discard_query, discard_error).await());
    }
};

} // namespace

// ===========================================================================
// Scalar round-trips — parametrized over (C++ type, OID, value, server ground truth)
// ===========================================================================
//
// Each TypeParam binds the value as a typed parameter (binary), reads it back through a
// prepared SELECT (binary result), and ALSO asks the server to render it as text via a
// `::text` cast so the assertion is anchored to PostgreSQL's own output, not to a buffer
// the converter emitted itself.

namespace {

// expected_result_format(): the per-column result format the client requests in Bind. The
// framework opts INTO binary only for OIDs it has a verified binary decoder for (see
// common.h `type_oid_prefers_binary_result_format`); everything else degrades to TEXT. So a
// SMALLINT/INTEGER/BIGINT/BOOLEAN column comes back Binary, but a TEXT column comes back Text.
struct ScalarSmallint {
    using cpp_t = smallint;
    static cpp_t        value() { return smallint{-12345}; }
    static const char  *col_type() { return "SMALLINT"; }
    static const char  *server_text() { return "-12345"; }
    static protocol_data_format expected_result_format() { return protocol_data_format::Binary; }
};
struct ScalarInteger {
    using cpp_t = integer;
    static cpp_t        value() { return integer{1234567890}; }
    static const char  *col_type() { return "INTEGER"; }
    static const char  *server_text() { return "1234567890"; }
    static protocol_data_format expected_result_format() { return protocol_data_format::Binary; }
};
struct ScalarBigint {
    using cpp_t = bigint;
    static cpp_t        value() { return std::numeric_limits<bigint>::max(); }
    static const char  *col_type() { return "BIGINT"; }
    static const char  *server_text() { return "9223372036854775807"; }
    static protocol_data_format expected_result_format() { return protocol_data_format::Binary; }
};
struct ScalarBigintMin {
    using cpp_t = bigint;
    static cpp_t        value() { return std::numeric_limits<bigint>::min(); }
    static const char  *col_type() { return "BIGINT"; }
    static const char  *server_text() { return "-9223372036854775808"; }
    static protocol_data_format expected_result_format() { return protocol_data_format::Binary; }
};
struct ScalarBoolean {
    using cpp_t = bool;
    static cpp_t        value() { return true; }
    static const char  *col_type() { return "BOOLEAN"; }
    static const char  *server_text() { return "true"; }
    static protocol_data_format expected_result_format() { return protocol_data_format::Binary; }
};
struct ScalarText {
    using cpp_t = std::string;
    static cpp_t value() { return std::string{"unicode: \xC3\xA9\xC3\xA8\xE2\x82\xAC"}; } // éè€
    static const char *col_type() { return "TEXT"; }
    static const char *server_text() { return "unicode: \xC3\xA9\xC3\xA8\xE2\x82\xAC"; }
    // TEXT has no binary decoder in this module -> the client requests TEXT result format.
    static protocol_data_format expected_result_format() { return protocol_data_format::Text; }
};

template <typename T>
class ScalarRoundTrip : public DataTypesRoundTrip {};

using ScalarTypes = ::testing::Types<ScalarSmallint, ScalarInteger, ScalarBigint,
                                     ScalarBigintMin, ScalarBoolean, ScalarText>;
TYPED_TEST_SUITE(ScalarRoundTrip, ScalarTypes);

TYPED_TEST(ScalarRoundTrip, BinaryParamTypedResult) {
    using P     = TypeParam;
    auto &db    = *this->db_;
    const auto v = P::value();

    const std::string create =
        std::string("CREATE TEMP TABLE st (v ") + P::col_type() + ")";
    ASSERT_TRUE(db.execute(create, discard_query, discard_error).await());

    // Insert via typed binary parameter.
    ASSERT_TRUE(db.prepare("st_ins", "INSERT INTO st (v) VALUES ($1)",
                           type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db.execute(
                      "st_ins", params(v),
                      [](transaction &, results) {},
                      [](error::db_error e) { FAIL() << "insert: " << e.what(); })
                   .await());

    // (a) Read back via prepared SELECT -> binary result column -> typed decode.
    ASSERT_TRUE(db.prepare("st_sel", "SELECT v FROM st", type_oid_sequence{},
                           discard_prepare, discard_error)
                    .await());
    bool decoded = false;
    ASSERT_TRUE(db.execute(
                      "st_sel", params(),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          ASSERT_FALSE(r[0][0].is_null());
                          EXPECT_EQ(r.field(0).format_code, P::expected_result_format());
                          EXPECT_EQ(r[0][0].template as<typename P::cpp_t>(), v);
                          decoded = true;
                      },
                      [](error::db_error e) { FAIL() << "select: " << e.what(); })
                   .await());
    EXPECT_TRUE(decoded);

    // (b) Server ground truth: render the stored value as text and compare to the
    // canonical PG output. Catches a binary decoder that happens to agree with a
    // matching binary encoder bug.
    bool gt = false;
    ASSERT_TRUE(db.execute(
                      "SELECT v::text FROM st",
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r[0][0].template as<std::string>(), P::server_text());
                          gt = true;
                      },
                      [](error::db_error e) { FAIL() << "gt: " << e.what(); })
                   .await());
    EXPECT_TRUE(gt);
}

} // namespace

// ===========================================================================
// Float / double — exact bit fidelity through the binary path, incl. specials
// ===========================================================================

TEST_F(DataTypesRoundTrip, Float4_ExactValueAndNaNInf) {
    ASSERT_TRUE(db_->prepare("f4", "SELECT $1::float4", type_oid_sequence{oid::float4},
                             discard_prepare, discard_error)
                    .await());

    auto echo_f4 = [&](float in, auto check) {
        ASSERT_TRUE(db_->execute(
                          "f4", params(in),
                          [&](transaction &, results r) {
                              ASSERT_EQ(r.size(), 1u);
                              EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                              check(r[0][0].as<float>());
                          },
                          [](error::db_error e) { FAIL() << e.what(); })
                       .await());
    };

    echo_f4(3.14159f, [](float v) { EXPECT_FLOAT_EQ(v, 3.14159f); });
    echo_f4(std::numeric_limits<float>::quiet_NaN(),
            [](float v) { EXPECT_TRUE(std::isnan(v)); });
    echo_f4(std::numeric_limits<float>::infinity(),
            [](float v) { EXPECT_TRUE(std::isinf(v) && v > 0); });
    echo_f4(-std::numeric_limits<float>::infinity(),
            [](float v) { EXPECT_TRUE(std::isinf(v) && v < 0); });
}

TEST_F(DataTypesRoundTrip, Float8_ExactValueAndNaNInf) {
    ASSERT_TRUE(db_->prepare("f8", "SELECT $1::float8", type_oid_sequence{oid::float8},
                             discard_prepare, discard_error)
                    .await());

    auto echo_f8 = [&](double in, auto check) {
        ASSERT_TRUE(db_->execute(
                          "f8", params(in),
                          [&](transaction &, results r) {
                              ASSERT_EQ(r.size(), 1u);
                              EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                              check(r[0][0].as<double>());
                          },
                          [](error::db_error e) { FAIL() << e.what(); })
                       .await());
    };

    echo_f8(2.718281828459045, [](double v) { EXPECT_DOUBLE_EQ(v, 2.718281828459045); });
    echo_f8(std::numeric_limits<double>::quiet_NaN(),
            [](double v) { EXPECT_TRUE(std::isnan(v)); });
    echo_f8(std::numeric_limits<double>::infinity(),
            [](double v) { EXPECT_TRUE(std::isinf(v) && v > 0); });
    echo_f8(-std::numeric_limits<double>::infinity(),
            [](double v) { EXPECT_TRUE(std::isinf(v) && v < 0); });
}

// ===========================================================================
// BYTEA — non-empty and empty (empty bytea exercises the 0-length value path)
// ===========================================================================

TEST_F(DataTypesRoundTrip, Bytea_RoundTripWithNul) {
    ASSERT_TRUE(db_->prepare("by", "SELECT $1::bytea", type_oid_sequence{oid::bytea},
                             discard_prepare, discard_error)
                    .await());
    const std::vector<byte> payload{static_cast<byte>(0xDE), static_cast<byte>(0xAD),
                                    static_cast<byte>(0x00), static_cast<byte>(0xBE),
                                    static_cast<byte>(0xEF)};
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "by", params(payload),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                          EXPECT_EQ(r[0][0].as<std::vector<byte>>(), payload);
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

TEST_F(DataTypesRoundTrip, EmptyByteaParam_RoundTripsAsZeroLength) {
    // Historical note: an empty bytea once tripped 08P01 (protocol_violation) on the
    // param path. This asserts the value now round-trips to a zero-length result.
    ASSERT_TRUE(db_->prepare("eby", "SELECT length($1::bytea) AS n, $1::bytea AS b",
                             type_oid_sequence{oid::bytea}, discard_prepare, discard_error)
                    .await());
    const std::vector<byte> empty;
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "eby", params(empty),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r[0][0].as<int>(), 0);
                          EXPECT_TRUE(r[0][1].as<std::vector<byte>>().empty());
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << "empty bytea param: " << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

// ===========================================================================
// UUID
// ===========================================================================

TEST_F(DataTypesRoundTrip, Uuid_RoundTrip) {
    const auto u = qb::uuid::from_string("12345678-1234-5678-1234-567812345678").value();
    ASSERT_TRUE(db_->prepare("uu", "SELECT $1::uuid", type_oid_sequence{oid::uuid},
                             discard_prepare, discard_error)
                    .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "uu", params(u),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                          EXPECT_EQ(r[0][0].as<qb::uuid>(), u);
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

// ===========================================================================
// NULL
// ===========================================================================

TEST_F(DataTypesRoundTrip, Null_DecodesAsNull) {
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "SELECT NULL::int AS n",
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_TRUE(r[0][0].is_null());
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

// ===========================================================================
// TIMESTAMP / TIMESTAMPTZ — pinned UTC, fixed instant, single-second tolerance
// ===========================================================================

namespace {
// 2023-01-15 12:34:56.789000 UTC as Unix microseconds (computed once, no mktime).
//   2023-01-15 12:34:56Z == 1673786096 (verified against datetime.timestamp()).
constexpr long long kFixedUtcSeconds = 1673786096LL;
constexpr long long kFixedUtcMicros  = kFixedUtcSeconds * 1000000LL + 789000LL;
} // namespace

TEST_F(DataTypesRoundTrip, Timestamp_FixedInstant_SecondTolerance) {
    const qb::wall_time ts =
        qb::wall_from_unix_seconds(kFixedUtcSeconds) + std::chrono::microseconds(789000);

    ASSERT_TRUE(db_->prepare("ts_ins", "SELECT $1::timestamp AS t",
                             type_oid_sequence{oid::timestamp}, discard_prepare, discard_error)
                    .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "ts_ins", params(ts),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                          const auto got = r[0][0].as<qb::wall_time>();
                          // Two-part check: the instant is within a 1s band of the expected
                          // value (whole-second agreement), and the sub-second component is
                          // exact (789000 us preserved) via the separate %1000000 assertion.
                          const long long got_us = static_cast<long long>(qb::unix_micros(got));
                          EXPECT_LE(std::llabs(got_us - kFixedUtcMicros), 1000000LL);
                          EXPECT_EQ(qb::unix_micros(got) % 1000000, 789000u);
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);

    // Server ground truth: with TZ=UTC the rendered text is exact and stable.
    bool gt = false;
    ASSERT_TRUE(db_->execute(
                      "SELECT (TIMESTAMP '2023-01-15 12:34:56.789')::text",
                      [&](transaction &, results r) {
                          EXPECT_EQ(r[0][0].as<std::string>(), "2023-01-15 12:34:56.789");
                          gt = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(gt);
}

TEST_F(DataTypesRoundTrip, TimestampTZ_FixedInstant_SecondTolerance) {
    const qb::wall_time ts = qb::wall_from_unix_seconds(kFixedUtcSeconds);

    ASSERT_TRUE(db_->prepare("tz_ins", "SELECT $1::timestamptz AS t",
                             type_oid_sequence{oid::timestamptz}, discard_prepare, discard_error)
                    .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "tz_ins", params(ts),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                          const auto got = r[0][0].as<qb::wall_time>();
                          const long long got_s = static_cast<long long>(qb::unix_seconds(got));
                          EXPECT_LE(std::llabs(got_s - kFixedUtcSeconds), 1LL);
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

// ===========================================================================
// NUMERIC — exact decimal + NaN/±Infinity specials through the binary codec
// ===========================================================================

TEST_F(DataTypesRoundTrip, Numeric_ExactDecimal) {
    ASSERT_TRUE(db_->prepare("num", "SELECT $1::numeric AS n",
                             type_oid_sequence{oid::numeric}, discard_prepare, discard_error)
                    .await());
    const numeric n("1234567.891011");
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "num", params(n),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                          EXPECT_EQ(r[0][0].as<numeric>().str(), "1234567.891011");
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

// REGRESSION (serde audit HIGH #1): as<double>() over the BINARY wire used to read
// sizeof(double) bytes of whatever the column was -> a NUMERIC's NBASE digit-array decoded as a
// raw IEEE-754 double = garbage. It now decodes per the column OID: NUMERIC/int -> converted to
// the float; a column with no numeric meaning (date here) fails LOUDLY instead of returning garbage.
TEST_F(DataTypesRoundTrip, ReadAsDoublePerColumnOidNotGarbage) {
    ASSERT_TRUE(db_->prepare("ad", "SELECT $1::numeric AS n, 42::int8 AS i, '2024-01-01'::date AS d",
                             type_oid_sequence{oid::numeric}, discard_prepare, discard_error)
                    .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "ad", params(numeric("1234.5678")),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                          EXPECT_NEAR(r[0][0].as<double>(), 1234.5678, 1e-9); // NUMERIC -> double (was garbage)
                          EXPECT_DOUBLE_EQ(r[0][1].as<double>(), 42.0);       // int8 -> double conversion
                          EXPECT_THROW((void) r[0][2].as<double>(),
                                       error::client_error); // date -> not numeric -> loud throw
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

// REGRESSION (serde audit HIGH #2): TypeConverter<std::string>::from_binary used to interpret the
// first 4 value bytes as a length prefix and strip them, so as<std::string>() on ANY binary value
// (a bytea here) returned "" or a wrong substring. It now reads the value bytes verbatim. Drives the
// param round-trip too (a bytea bound from a std::vector<char> with leading NULs).
TEST_F(DataTypesRoundTrip, ByteaWithLeadingNulsReadAsStringIsVerbatim) {
    ASSERT_TRUE(db_->prepare("bnul", "SELECT $1::bytea AS b", type_oid_sequence{oid::bytea},
                             discard_prepare, discard_error)
                    .await());
    const std::vector<char> raw{0, 0, 1, 2, 'h', 'i'}; // leading NULs the old prefix-strip would eat
    bool                    ok = false;
    ASSERT_TRUE(db_->execute(
                      "bnul", params(raw),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          ASSERT_FALSE(r[0][0].is_null());
                          EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                          EXPECT_EQ(r[0][0].as<std::string>(),
                                    std::string("\0\0\x01\x02hi", 6)); // 6 bytes verbatim
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

TEST_F(DataTypesRoundTrip, Numeric_HighPrecision_RealColumn) {
    // Replaces the EdgeCases "HIGHPREC:" TEXT hack: a genuine NUMERIC column round-trip.
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE hp (n NUMERIC)", discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("hp_ins", "INSERT INTO hp (n) VALUES ($1)",
                             type_oid_sequence{oid::numeric}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("hp_sel", "SELECT n FROM hp", type_oid_sequence{},
                             discard_prepare, discard_error)
                    .await());

    const std::string big = "999999999999999999999999999999.99999999999999999999";
    ASSERT_TRUE(db_->execute(
                      "hp_ins", params(numeric(big)),
                      [](transaction &, results) {},
                      [](error::db_error e) { FAIL() << "insert: " << e.what(); })
                   .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "hp_sel", params(),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r[0][0].as<numeric>().str(), big);
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << "select: " << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

TEST_F(DataTypesRoundTrip, Numeric_Specials_NaNAndInfinity) {
    // PG 14+ NUMERIC carries NaN / Infinity / -Infinity sign words. Read them back
    // through the binary codec and confirm canonical text. (Server-side cast to numeric
    // produces the special; the binary decoder must surface the right sign word.)
    ASSERT_TRUE(db_->prepare("nspec", "SELECT $1::numeric AS n", type_oid_sequence{oid::text},
                             discard_prepare, discard_error)
                    .await());
    struct Case { const char *in; const char *out; };
    for (const auto &c : {Case{"NaN", "NaN"}, Case{"Infinity", "Infinity"},
                          Case{"-Infinity", "-Infinity"}}) {
        bool ok = false;
        ASSERT_TRUE(db_->execute(
                          "nspec", params(std::string(c.in)),
                          [&](transaction &, results r) {
                              ASSERT_EQ(r.size(), 1u);
                              EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                              EXPECT_EQ(r[0][0].as<numeric>().str(), c.out)
                                  << "for NUMERIC special " << c.in;
                              ok = true;
                          },
                          [&](error::db_error e) {
                              // Older servers reject NUMERIC Infinity; skip rather than fail.
                              GTEST_SKIP() << "server rejects NUMERIC " << c.in << ": " << e.what();
                          })
                       .await());
        if (::testing::Test::IsSkipped())
            return;
        EXPECT_TRUE(ok);
    }
}

// ===========================================================================
// DATE — ordinary value + ±infinity sentinels
// ===========================================================================

TEST_F(DataTypesRoundTrip, Date_RoundTripThroughRealColumn) {
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE dt (d DATE)", discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("dt_ins", "INSERT INTO dt (d) VALUES ($1)",
                             type_oid_sequence{oid::date}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("dt_sel", "SELECT d FROM dt ORDER BY d", type_oid_sequence{},
                             discard_prepare, discard_error)
                    .await());

    for (const char *iso : {"1970-01-01", "2050-12-31"}) {
        ASSERT_TRUE(db_->execute(
                          "dt_ins", params(qb::date::parse(iso).value()),
                          [](transaction &, results) {},
                          [&](error::db_error e) { FAIL() << "insert " << iso << ": " << e.what(); })
                       .await());
    }
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "dt_sel", params(),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 2u);
                          EXPECT_EQ(r[0][0].as<qb::date>().to_string(), "1970-01-01");
                          EXPECT_EQ(r[1][0].as<qb::date>().to_string(), "2050-12-31");
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << "select: " << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

TEST_F(DataTypesRoundTrip, Date_InfinitySentinels_RenderAsText) {
    // DATE 'infinity' / '-infinity' are int32 INT_MAX / INT_MIN on the wire. We do not
    // bind them as qb::date (the civil type cannot represent a sentinel) — assert the
    // server's text rendering through a prepared column so the path is still binary.
    ASSERT_TRUE(db_->prepare("dinf", "SELECT $1::date::text AS d", type_oid_sequence{oid::text},
                             discard_prepare, discard_error)
                    .await());
    for (const char *s : {"infinity", "-infinity"}) {
        bool ok = false;
        ASSERT_TRUE(db_->execute(
                          "dinf", params(std::string(s)),
                          [&](transaction &, results r) {
                              ASSERT_EQ(r.size(), 1u);
                              EXPECT_EQ(r[0][0].as<std::string>(), s);
                              ok = true;
                          },
                          [](error::db_error e) { FAIL() << e.what(); })
                       .await());
        EXPECT_TRUE(ok);
    }
}

// ===========================================================================
// Arrays — 1-D ground-truth, multi-dimensional, NULL elements
// ===========================================================================

TEST_F(DataTypesRoundTrip, IntArray_OneDimensional) {
    ASSERT_TRUE(db_->prepare("ia", "SELECT $1::int[] AS a", type_oid_sequence{oid::int4_array},
                             discard_prepare, discard_error)
                    .await());
    const std::vector<int> a{10, 20, 30, 40};
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "ia", params(a),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                          EXPECT_EQ(r[0][0].as<std::vector<int>>(), a);
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

// REGRESSION (serde audit): an EMPTY std::vector binds an empty array ('{}'), NOT SQL
// NULL. The server must see a non-NULL value of cardinality 0. (The old add_vector wrote
// add_null() for an empty vector, so `cardinality($1)` came back NULL.)
TEST_F(DataTypesRoundTrip, EmptyIntArrayParam_BindsEmptyArrayNotNull) {
    ASSERT_TRUE(db_->prepare("eia",
                             "SELECT ($1::int[]) IS NOT NULL AS not_null, "
                             "cardinality($1::int[]) AS card",
                             type_oid_sequence{oid::int4_array}, discard_prepare, discard_error)
                    .await());
    const std::vector<int> empty;
    bool                   ok = false;
    ASSERT_TRUE(db_->execute(
                      "eia", params(empty),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_TRUE(r[0][0].as<bool>());           // IS NOT NULL
                          EXPECT_EQ(r[0][1].as<int>(), 0);           // empty array, cardinality 0
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

// REGRESSION (serde audit): a vector of an element type ABSENT from the old add_vector
// switch (numeric here) used to bind the anyarray pseudo-OID (2277), which PostgreSQL
// rejects. The fixed array-OID map binds numeric[] (1231); the server confirms the value
// bytes are wire-correct by comparing against its own ARRAY literal.
TEST_F(DataTypesRoundTrip, NumericArrayParam_PreviouslyAnyarray_BindsAndMatches) {
    ASSERT_TRUE(db_->prepare("nap", "SELECT $1::numeric[] = ARRAY[1.5,2.5,3.25]::numeric[] AS eq",
                             type_oid_sequence{oid::numeric_array}, discard_prepare, discard_error)
                    .await());
    const std::vector<numeric> a{numeric("1.5"), numeric("2.5"), numeric("3.25")};
    bool                       ok = false;
    ASSERT_TRUE(db_->execute(
                      "nap", params(a),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_TRUE(r[0][0].as<bool>()); // server-side array equality
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

TEST_F(DataTypesRoundTrip, IntArray_MultiDimensional_FlattensInOrder) {
    // ndim > 1: the binary array header carries 2 dims. The std::vector<int> decoder
    // flattens row-major; assert against the server's own flattened ordering via unnest.
    ASSERT_TRUE(db_->prepare("ia2", "SELECT ARRAY[[1,2,3],[4,5,6]]::int[] AS a",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "ia2", params(),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                          EXPECT_EQ(r[0][0].as<std::vector<int>>(),
                                    (std::vector<int>{1, 2, 3, 4, 5, 6}));
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

TEST_F(DataTypesRoundTrip, TextArray_WithNullElement_DecodesNullAsDefault) {
    // A NULL element on the wire is element length = -1. The std::vector<std::string>
    // decoder cannot represent SQL NULL, so per its documented contract (type_converter.h
    // decode_pg_array) a NULL element becomes a default-constructed (empty) string. This
    // pins that behavior AND proves the column truly held a NULL via a server-side check
    // (so the test is exercising the -1 length path, not just two empty strings).
    ASSERT_TRUE(db_->prepare(
                      "ta",
                      "SELECT a, (a[2] IS NULL) AS mid_is_null "
                      "FROM (SELECT ARRAY['a', NULL, 'c']::text[] AS a) s",
                      type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "ta", params(),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                          // Server confirms element 2 is genuinely NULL.
                          EXPECT_TRUE(r[0][1].as<bool>());
                          // Decoder flattens NULL -> default-constructed string.
                          const auto v = r[0][0].as<std::vector<std::string>>();
                          ASSERT_EQ(v.size(), 3u);
                          EXPECT_EQ(v[0], "a");
                          EXPECT_EQ(v[1], ""); // SQL NULL collapsed to default
                          EXPECT_EQ(v[2], "c");
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

// ===========================================================================
// Temporal / array battery preserved verbatim from the monolith (binary path)
// ===========================================================================

TEST_F(DataTypesRoundTrip, DateTimeNumericIntArray_BinaryRoundTrip) {
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE dtna_test "
                             "(d DATE, t TIME WITHOUT TIME ZONE, n NUMERIC(20,6), a INT[])",
                             discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("dtna_ins",
                             "INSERT INTO dtna_test (d, t, n, a) VALUES ($1, $2, $3, $4)",
                             {oid::date, oid::time, oid::numeric, oid::int4_array},
                             discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("dtna_sel", "SELECT d, t, n, a FROM dtna_test",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    const qb::date         d = qb::date::parse("2024-03-15").value();
    const qb::time_of_day  t = qb::time_of_day::from_hms(14, 30, 45, 123456);
    const numeric          n("1234567.891011");
    const std::vector<int> a{10, 20, 30, 40};

    ASSERT_TRUE(db_->execute(
                      "dtna_ins", params(d, t, n, a),
                      [](transaction &, results) {},
                      [](error::db_error e) { FAIL() << "insert: " << e.what(); })
                   .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "dtna_sel", params(),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          const auto &row = r[0];
                          EXPECT_EQ(row[0].as<qb::date>().to_string(), "2024-03-15");
                          EXPECT_EQ(row[1].as<qb::time_of_day>().to_string(), "14:30:45.123456");
                          EXPECT_EQ(row[2].as<numeric>().str(), "1234567.891011");
                          EXPECT_EQ(row[3].as<std::vector<int>>(), a);
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << "select: " << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

TEST_F(DataTypesRoundTrip, IntervalTimetzArrays_BinaryRoundTrip) {
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE ita_test AS SELECT "
                             "interval '1 month 2 days 03:04:05' AS iv, "
                             "timetz '14:30:45+02:00' AS tz, "
                             "ARRAY[10,20,30]::bigint[] AS ba, "
                             "ARRAY[1.5,2.5]::float8[] AS fa, "
                             "ARRAY[true,false,true]::bool[] AS bo",
                             discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("ita_sel", "SELECT iv, tz, ba, fa, bo FROM ita_test",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "ita_sel", params(),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          const auto &row = r[0];
                          EXPECT_EQ(row[0].as<std::chrono::seconds>().count(), 2775845); // EXTRACT(EPOCH)
                          EXPECT_EQ(row[1].as<qb::time_of_day_tz>().to_string(), "14:30:45+02:00");
                          EXPECT_EQ(row[2].as<std::vector<bigint>>(), (std::vector<bigint>{10, 20, 30}));
                          EXPECT_EQ(row[3].as<std::vector<double>>(), (std::vector<double>{1.5, 2.5}));
                          EXPECT_EQ(row[4].as<std::vector<bool>>(), (std::vector<bool>{true, false, true}));
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << "select: " << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

TEST_F(DataTypesRoundTrip, NetworkBitTypes_RouteAsText) {
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE nbt_test AS SELECT "
                             "inet '192.168.1.10' AS a, cidr '10.0.0.0/8' AS c, "
                             "macaddr '08:00:2b:01:02:03' AS m, bit '101' AS b, varbit '1101' AS v, "
                             "point '(1,2)' AS p, int4range(1,10) AS r, pg_lsn '0/16B3748' AS l",
                             discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("nbt_sel", "SELECT a, c, m, b, v, p, r, l FROM nbt_test",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "nbt_sel", params(),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          const auto &row = r[0];
                          EXPECT_EQ(row[0].as<std::string>(), "192.168.1.10");
                          EXPECT_EQ(row[1].as<std::string>(), "10.0.0.0/8");
                          EXPECT_EQ(row[2].as<std::string>(), "08:00:2b:01:02:03");
                          EXPECT_EQ(row[3].as<std::string>(), "101");
                          EXPECT_EQ(row[4].as<std::string>(), "1101");
                          EXPECT_EQ(row[5].as<std::string>(), "(1,2)");
                          EXPECT_EQ(row[6].as<std::string>(), "[1,10)");
                          EXPECT_EQ(row[7].as<std::string>(), "0/16B3748");
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << "select: " << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

TEST_F(DataTypesRoundTrip, QbCivilTypes_BinaryRoundTrip) {
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE qct_test (d DATE, t TIME, z TIMETZ, iv INTERVAL)",
                             discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("qct_ins",
                             "INSERT INTO qct_test (d, t, z, iv) VALUES ($1, $2, $3, $4)",
                             {oid::date, oid::time, oid::timetz, oid::interval},
                             discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("qct_sel", "SELECT d, t, z, iv FROM qct_test",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());

    const qb::date              d = qb::date::from_ymd(2024, 3, 15);
    const qb::time_of_day       t = qb::time_of_day::from_hms(14, 30, 45, 123456);
    const qb::time_of_day_tz    z = qb::time_of_day_tz::from_hms_offset(14, 30, 45, 0, 7200);
    const qb::calendar_interval iv{1, 2, std::chrono::microseconds{11045000000LL}};

    ASSERT_TRUE(db_->execute(
                      "qct_ins", params(d, t, z, iv),
                      [](transaction &, results) {},
                      [](error::db_error e) { FAIL() << "insert: " << e.what(); })
                   .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "qct_sel", params(),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          const auto &row = r[0];
                          EXPECT_EQ(row[0].as<qb::date>().to_string(), "2024-03-15");
                          EXPECT_EQ(row[1].as<qb::time_of_day>().to_string(), "14:30:45.123456");
                          EXPECT_EQ(row[2].as<qb::time_of_day_tz>().to_string(), "14:30:45+02:00");
                          const auto got = row[3].as<qb::calendar_interval>();
                          EXPECT_EQ(got.months, 1);
                          EXPECT_EQ(got.days, 2);
                          EXPECT_EQ(got.micros.count(), 11045000000LL);
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << "select: " << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

// ===========================================================================
// JSON / JSONB — assert canonical decoded shape, fail loudly (no self-normalize)
// ===========================================================================

TEST_F(DataTypesRoundTrip, Json_RoundTripPreservesStructure) {
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE j_test (j JSON)", discard_query, discard_error)
                    .await());
    // NB: j_test has only the `j` column — a `RETURNING id` here is 42703 (no such column).
    ASSERT_TRUE(db_->prepare("j_ins", "INSERT INTO j_test (j) VALUES ($1)",
                             type_oid_sequence{oid::json}, discard_prepare, discard_error)
                    .await());

    const qb::json in = {
        {"id", 12345},
        {"name", "JSON Test"},
        {"tags", {"database", "postgres", "json"}},
        {"details", {{"active", true}, {"version", 1.5}, {"metadata", nullptr}}}};

    ASSERT_TRUE(db_->execute(
                      "j_ins", params(in),
                      [](transaction &, results) {},
                      [](error::db_error e) { FAIL() << "insert: " << e.what(); })
                   .await());

    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "SELECT j FROM j_test",
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          ASSERT_FALSE(r[0][0].is_null());
                          const qb::json out = r[0][0].as<qb::json>();
                          EXPECT_EQ(out["id"].get<int>(), 12345);
                          EXPECT_EQ(out["name"].get<std::string>(), "JSON Test");
                          ASSERT_TRUE(out["tags"].is_array());
                          EXPECT_EQ(out["tags"].size(), 3u);
                          EXPECT_EQ(out["tags"][0].get<std::string>(), "database");
                          ASSERT_TRUE(out["details"].is_object());
                          EXPECT_TRUE(out["details"]["active"].get<bool>());
                          EXPECT_DOUBLE_EQ(out["details"]["version"].get<double>(), 1.5);
                          EXPECT_TRUE(out["details"]["metadata"].is_null());
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << "select: " << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

TEST_F(DataTypesRoundTrip, Jsonb_CanonicalShape) {
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE jb_test (jb JSONB)", discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("jb_ins", "INSERT INTO jb_test (jb) VALUES ($1)",
                             type_oid_sequence{oid::jsonb}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("jb_sel", "SELECT jb FROM jb_test", type_oid_sequence{},
                             discard_prepare, discard_error)
                    .await());

    const qb::jsonb in = {
        {"user", {{"id", 42}, {"name", "JSONB User"}, {"active", true}}},
        {"roles", {"editor", "reviewer"}},
        {"scores", {98.5, 87.0}}};

    ASSERT_TRUE(db_->execute(
                      "jb_ins", params(in),
                      [](transaction &, results) {},
                      [](error::db_error e) { FAIL() << "insert: " << e.what(); })
                   .await());

    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "jb_sel", params(),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary);
                          const qb::jsonb out = r[0][0].as<qb::jsonb>();
                          // Canonical shape — assert exact, no warning downgrade.
                          ASSERT_TRUE(out.is_object());
                          EXPECT_EQ(out["user"]["id"].get<int>(), 42);
                          EXPECT_EQ(out["user"]["name"].get<std::string>(), "JSONB User");
                          EXPECT_TRUE(out["user"]["active"].get<bool>());
                          ASSERT_TRUE(out["roles"].is_array());
                          EXPECT_EQ(out["roles"].size(), 2u);
                          EXPECT_EQ(out["roles"][0].get<std::string>(), "editor");
                          ASSERT_TRUE(out["scores"].is_array());
                          EXPECT_DOUBLE_EQ(out["scores"][0].get<double>(), 98.5);
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << "select: " << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

TEST_F(DataTypesRoundTrip, Jsonb_Containment_Operator) {
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE jbc (jb JSONB)", discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->execute(
                      R"(INSERT INTO jbc (jb) VALUES ('{"a":1,"b":2}'::jsonb))",
                      discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("jbc_sel", "SELECT count(*)::int FROM jbc WHERE jb @> $1",
                             type_oid_sequence{oid::jsonb}, discard_prepare, discard_error)
                    .await());
    // NB: `qb::jsonb{{"a", 1}}` is the nlohmann brace-init ambiguity — a SINGLE pair becomes the
    // array `[["a",1]]`, not the object `{"a":1}`, so `jb @> $1` would be false (count 0).
    // Build the object unambiguously via parse() so the containment predicate matches.
    const qb::jsonb probe = qb::jsonb(qb::json::parse(R"({"a":1})"));
    bool            ok    = false;
    ASSERT_TRUE(db_->execute(
                      "jbc_sel", params(probe),
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r[0][0].as<int>(), 1);
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

// ===========================================================================
// resultset -> JSON projection (text rendering of a mixed row set)
// ===========================================================================

TEST_F(DataTypesRoundTrip, ResultSet_JsonProjection) {
    ASSERT_TRUE(db_->execute(
                      "CREATE TEMP TABLE rj (id INT, s SMALLINT, t TEXT, b BOOLEAN, n TEXT)",
                      discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->execute(
                      "INSERT INTO rj VALUES (1,123,'Text value',true,NULL),"
                      "(2,456,'Another text',false,NULL)",
                      discard_query, discard_error)
                    .await());
    bool ok = false;
    ASSERT_TRUE(db_->execute(
                      "SELECT s AS smallint_val, t AS text_val, b AS boolean_val, "
                      "n AS null_val FROM rj ORDER BY id",
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 2u);
                          const qb::json j = r.json();
                          ASSERT_TRUE(j.is_array());
                          ASSERT_EQ(j.size(), 2u);
                          EXPECT_EQ(j[0]["smallint_val"], "123");
                          EXPECT_EQ(j[0]["text_val"], "Text value");
                          EXPECT_EQ(j[0]["boolean_val"], "t");
                          EXPECT_TRUE(j[0]["null_val"].is_null());
                          EXPECT_EQ(j[1]["smallint_val"], "456");
                          EXPECT_EQ(j[1]["boolean_val"], "f");
                          ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(ok);
}

// ===========================================================================
// Transport coverage — same expectation through callback and coroutine paths
// ===========================================================================

TEST_F(DataTypesRoundTrip, IntegerRoundTrip_CallbackAndCoroutine) {
    constexpr integer expected = 987654321;
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE rt (v INT)", discard_query, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("rt_ins", "INSERT INTO rt (v) VALUES ($1)",
                             type_oid_sequence{oid::int4}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->execute(
                      "rt_ins", params(expected),
                      [](transaction &, results) {},
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());

    // callback
    bool cb_ok = false;
    ASSERT_TRUE(db_->execute(
                      "SELECT v FROM rt",
                      [&](transaction &, results r) {
                          ASSERT_EQ(r.size(), 1u);
                          EXPECT_EQ(r[0][0].as<integer>(), expected);
                          cb_ok = true;
                      },
                      [](error::db_error e) { FAIL() << e.what(); })
                   .await());
    EXPECT_TRUE(cb_ok);

    // coroutine
    bool        coro_ok = false;
    std::string coro_err;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply = co_await db_->query("SELECT v FROM rt");
        if (!reply.ok()) {
            coro_err = reply.error().what();
            co_return;
        }
        if (reply.result().size() != 1u) {
            coro_err = "expected 1 row, got " + std::to_string(reply.result().size());
            co_return;
        }
        coro_ok = reply.result()[0][0].as<integer>() == expected;
    }());
    EXPECT_TRUE(coro_ok) << coro_err;
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
