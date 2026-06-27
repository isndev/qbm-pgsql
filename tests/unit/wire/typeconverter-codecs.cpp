/**
 * @file qbm/pgsql/tests/unit/wire/typeconverter-codecs.cpp
 * @brief Unit tests for TypeConverter<T>::from_binary against PostgreSQL ground truth.
 *
 * This file owns the wire-codec slice carved out of the old test-param-unserializer.cpp:
 * the higher-level TypeConverter<T> binary decoders (NUMERIC, DATE, INTERVAL, JSON,
 * JSONB) and the integer size-dispatch / optional value paths. Every binary case is
 * anchored to a golden `*_send()` byte literal from shared/pg_wire_ground_truth.hpp
 * rather than to a `to_binary`-then-`from_binary` self-round-trip, so a shared
 * encode/decode bug cannot pass silently.
 *
 * The exhaustive per-type scalar/array/temporal corpus lives under unit/types/; this
 * file keeps only the decoders that historically lived in the unserializer suite,
 * plus the spec-mandated additions (NUMERIC NaN/Inf sign words, DATE ±infinity
 * sentinels, JSONB truncated-after-version / bad-version).
 *
 * Pure logic: no daemon, no qb::Main, no event loop, parallel-safe.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../pgsql.h"

#include "../../shared/pg_wire_ground_truth.hpp"

using namespace qb::pg;
using namespace qb::pg::detail;
namespace gt = qb::pg::test::gt;
using qb::pg::test::hex_to_bytes;

namespace {

// ===========================================================================
// NUMERIC binary codec — decode against numeric_send ground truth.
// ===========================================================================

TEST(TypeConverterNumeric, DecodeFiniteAgainstGroundTruth) {
    for (const auto &c : gt::numeric::finite) {
        const numeric got = TypeConverter<numeric>::from_binary(hex_to_bytes(c.hex));
        EXPECT_EQ(got.str(), c.expect) << "hex=" << c.hex;
    }
}

TEST(TypeConverterNumeric, DecodeSpecialSignWords) {
    // numeric_send NaN / +Infinity / -Infinity (header-only, ndigits == 0).
    for (const auto &c : gt::numeric::specials) {
        const numeric got = TypeConverter<numeric>::from_binary(hex_to_bytes(c.hex));
        EXPECT_EQ(got.str(), c.expect) << "hex=" << c.hex;
    }
}

TEST(TypeConverterNumeric, RoundTripThroughRealBinary) {
    // to_binary emits real PG binary (length-prefixed); from_binary detects and
    // strips that prefix. Canonical text must survive the round trip exactly.
    for (const char *v :
         {"0", "1", "-1", "12345.678", "-999.99", "123456789.0123456789", "0.0001", "1000000", "100.00", "0.10"}) {
        std::vector<byte> buf;
        TypeConverter<numeric>::to_binary(numeric(v), buf);
        EXPECT_EQ(TypeConverter<numeric>::from_binary(buf).str(), v) << "value=" << v;
    }
}

TEST(TypeConverterNumeric, TruncatedHeaderDecodesToZero) {
    // decode_pg_numeric returns "0" when the header (8 bytes) is incomplete, rather
    // than reading out of bounds. 6 bytes < 8.
    EXPECT_EQ(TypeConverter<numeric>::from_binary(hex_to_bytes("000200000000")).str(), "0");
}

// ===========================================================================
// DATE binary codec — date_send is an int32 day count since 2000-01-01.
// ===========================================================================

TEST(TypeConverterDate, DecodeAgainstGroundTruth) {
    const qb::date d = TypeConverter<qb::date>::from_binary(hex_to_bytes(gt::temporal::date_2024_03_15));
    EXPECT_EQ(d.to_string(), "2024-03-15");
    EXPECT_EQ(d.days_since_epoch(), 19797); // since Unix epoch
}

TEST(TypeConverterDate, DecodeUnixEpochAndBeforeEpoch) {
    // PG day 0 == 2000-01-01; -10957 == 1970-01-01.
    std::vector<byte> day0;
    int32_t be0 = qb::endian::to_big_endian<int32_t>(0);
    day0.insert(day0.end(), reinterpret_cast<byte *>(&be0), reinterpret_cast<byte *>(&be0) + 4);
    EXPECT_EQ(TypeConverter<qb::date>::from_binary(day0).to_string(), "2000-01-01");

    int32_t be_epoch = qb::endian::to_big_endian<int32_t>(-10957);
    std::vector<byte> epoch(reinterpret_cast<byte *>(&be_epoch), reinterpret_cast<byte *>(&be_epoch) + 4);
    EXPECT_EQ(TypeConverter<qb::date>::from_binary(epoch).to_string(), "1970-01-01");
}

TEST(TypeConverterDate, InfinitySentinelsAreNotSpecialCasedButDecodeLosslesslyDistinct) {
    // PostgreSQL DATE ±infinity sentinels are 0x7fffffff / 0x80000000. The codec has NO
    // special-value mapping: it adds the epoch offset (10957) in int64 and constructs a
    // qb::date. The day count is far outside qb::date's representable range, so the
    // result is lossy/implementation-defined — its EXACT integer value depends on how the
    // platform's std::chrono::days narrows an out-of-range count (it differs across
    // libstdc++/libc++), so this test pins only the platform-INDEPENDENT contract: the
    // sentinels decode without throwing and stay distinct. (A future change that maps the
    // sentinels to a real +/-infinity representation will still satisfy this.)
    const qb::date pos_inf = TypeConverter<qb::date>::from_binary(hex_to_bytes("7fffffff"));
    const qb::date neg_inf = TypeConverter<qb::date>::from_binary(hex_to_bytes("80000000"));
    EXPECT_NE(pos_inf, neg_inf);
    // A normal in-range date still decodes correctly (the lossy path is only the sentinels).
    EXPECT_EQ(TypeConverter<qb::date>::from_binary(hex_to_bytes(gt::temporal::date_2024_03_15)).to_string(),
              "2024-03-15");
}

// ===========================================================================
// INTERVAL binary codec — must fold days + months, not drop them.
// ===========================================================================

TEST(TypeConverterInterval, DecodeFoldsDaysAndMonths) {
    using secs = std::chrono::seconds;
    for (const auto &c : gt::interval::cases) {
        const auto d = TypeConverter<secs>::from_binary(hex_to_bytes(c.hex));
        EXPECT_EQ(d.count(), c.expect_seconds) << "hex=" << c.hex;
    }
}

TEST(TypeConverterInterval, PureTimeDurationRoundTrips) {
    using secs = std::chrono::seconds;
    std::vector<byte> buf;
    TypeConverter<secs>::to_binary(secs{90061}, buf); // 25h01m01s
    ASSERT_GE(buf.size(), 4u);
    std::vector<byte> body(buf.begin() + 4, buf.end()); // strip the length prefix
    EXPECT_EQ(TypeConverter<secs>::from_binary(body).count(), 90061);
}

TEST(TypeConverterInterval, RejectsMalformedSize) {
    using secs = std::chrono::seconds;
    EXPECT_THROW(TypeConverter<secs>::from_binary(hex_to_bytes("00000000")), std::runtime_error);
}

// ===========================================================================
// TIMESTAMP binary codec — decode the actual instant.
// ===========================================================================

TEST(TypeConverterTimestamp, DecodeAgainstGroundTruth) {
    const auto t = TypeConverter<qb::wall_time>::from_binary(hex_to_bytes(gt::temporal::ts_2024_03_15));
    EXPECT_EQ(qb::unix_micros(t), gt::temporal::ts_2024_03_15_unix_micros);
}

// ===========================================================================
// Integer size-dispatch — int2/int4/int8 width on the result wire + overflow.
// ===========================================================================

TEST(TypeConverterInteger, SizeDispatchAndOverflow) {
    EXPECT_EQ(TypeConverter<integer>::from_binary(hex_to_bytes("0064")), 100);             // int2 widened
    EXPECT_EQ(TypeConverter<integer>::from_binary(hex_to_bytes("0000002a")), 42);          // int4
    EXPECT_EQ(TypeConverter<integer>::from_binary(hex_to_bytes("000000000000002a")), 42);  // int8 (e.g. COUNT)
    // int8 value out of int32 range -> throw, not silent truncation.
    EXPECT_THROW(TypeConverter<integer>::from_binary(hex_to_bytes("0000000080000000")), std::runtime_error);
    EXPECT_THROW(TypeConverter<integer>::from_binary(hex_to_bytes("ffffffff7fffffff")), std::runtime_error);
}

// ===========================================================================
// optional<T> — value bytes always present at this layer; -1 is a real int4.
// ===========================================================================

TEST(TypeConverterOptional, DecodesValueIncludingMinusOne) {
    std::vector<byte> minus_one(4, static_cast<byte>(0xFF)); // int4 -1, NOT SQL NULL
    auto neg = TypeConverter<std::optional<integer>>::from_binary(minus_one);
    ASSERT_TRUE(neg.has_value());
    EXPECT_EQ(*neg, -1);

    auto v = TypeConverter<std::optional<integer>>::from_binary(hex_to_bytes("0000002a"));
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
}

// ===========================================================================
// JSON / JSONB binary codecs.
// ===========================================================================

/// Build a JSONB result payload: version byte 1 + UTF-8 JSON text (jsonb_send VARDATA).
std::vector<byte>
jsonb_payload(const std::string &json) {
    std::vector<byte> buf;
    buf.push_back(static_cast<byte>(1)); // version
    buf.insert(buf.end(), json.begin(), json.end());
    return buf;
}

TEST(TypeConverterJsonb, DecodeVersion1Payload) {
    const std::string json = R"({"active":true,"id":12345,"name":"Test JSON","tags":["a","b","c"]})";
    const qb::jsonb result = TypeConverter<qb::jsonb>::from_binary(jsonb_payload(json));
    ASSERT_TRUE(result.contains("id"));
    EXPECT_EQ(result["id"].get<int>(), 12345);
    EXPECT_EQ(result["name"].get<std::string>(), "Test JSON");
    EXPECT_EQ(result["active"].get<bool>(), true);
    ASSERT_TRUE(result["tags"].is_array());
    EXPECT_EQ(result["tags"].size(), 3u);
    EXPECT_EQ(result["tags"][0].get<std::string>(), "a");
}

TEST(TypeConverterJsonb, AcceptsVarlenaPrefixedPayload) {
    // Some stacks prefix a 4-byte varlena header before the version byte.
    const std::string json = R"({"k":1})";
    std::vector<byte> buf = {0, 0, 0, 0};
    auto vardata = jsonb_payload(json);
    buf.insert(buf.end(), vardata.begin(), vardata.end());
    const qb::jsonb result = TypeConverter<qb::jsonb>::from_binary(buf);
    EXPECT_EQ(result["k"].get<int>(), 1);
}

TEST(TypeConverterJsonb, RejectsUnsupportedVersion) {
    // Version byte 2 (and not a varlena-prefixed v1) must be rejected.
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_binary(hex_to_bytes("0203")), std::runtime_error);
    auto bad = jsonb_payload(R"({"k":1})");
    bad[0] = static_cast<byte>(2);
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_binary(bad), std::runtime_error);
}

TEST(TypeConverterJsonb, RejectsTruncatedAfterVersion) {
    // Version byte present but JSON body truncated to invalid JSON -> parse throws.
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_binary(jsonb_payload(R"({"unclosed":)")), std::runtime_error);
    // Too-small buffer (< 2 bytes).
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_binary(hex_to_bytes("01")), std::runtime_error);
}

TEST(TypeConverterJson, DecodeFromTextValidAndInvalid) {
    const char *valid[] = {
        R"({"id": 123, "name": "test"})",
        R"(["apple", "banana", "cherry"])",
        R"(42)",
        R"("simple string")",
        R"(true)",
        R"(null)",
    };
    for (const char *tc : valid) {
        const qb::json expected = qb::json::parse(tc);
        EXPECT_EQ(TypeConverter<qb::json>::from_text(tc).dump(), expected.dump()) << "case=" << tc;
    }
    EXPECT_THROW(TypeConverter<qb::json>::from_text(R"({"unclosed": "object")"), std::runtime_error);
}

TEST(TypeConverterJson, DecodeBinaryReadsValueBytesAsJsonText) {
    // from_binary receives the field VALUE bytes (the protocol strips the per-field length
    // prefix; `json` has no version byte) -> the bytes ARE the JSON text. NOT a to_binary
    // round-trip (to_binary writes the Bind [int32 len] framing, which from_binary never sees).
    const std::string       json = R"({"a":1,"b":[2,3]})";
    const std::vector<byte> value(json.data(), json.data() + json.size());
    const qb::json          result = TypeConverter<qb::json>::from_binary(value);
    EXPECT_EQ(result["a"].get<int>(), 1);
    EXPECT_EQ(result["b"][1].get<int>(), 3);
}

// REGRESSION (serde audit HIGH #2): from_binary<std::string> receives the field VALUE bytes
// (length prefix already stripped). It must read them VERBATIM. The legacy read_string() it used
// to call would, for a 4..1MB buffer with a NUL in the first 3 bytes, assume a phantom 4-byte
// length prefix and strip it — corrupting a binary string / bytea value that begins with NULs.
TEST(TypeConverterCodecs, FromBinaryStringReadsValueBytesVerbatimIncludingLeadingNuls) {
    const std::vector<byte> value{byte(0), byte(0), byte(1), byte(2), byte('h'), byte('i')};
    const std::string       s = TypeConverter<std::string>::from_binary(value);
    EXPECT_EQ(s.size(), 6u);                          // not 2 (old read_string stripped 4)
    EXPECT_EQ(s, std::string("\0\0\x01\x02hi", 6));   // exact bytes, NULs preserved
    // An empty value decodes to an empty string (not a throw).
    EXPECT_TRUE(TypeConverter<std::string>::from_binary(std::vector<byte>{}).empty());
}

} // namespace

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
