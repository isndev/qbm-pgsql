/**
 * @file typeconverter-scalar.cpp
 * @brief Unit tests for the scalar TypeConverter<T> codecs (bool / int family /
 *        float / double / std::string / bytea / UUID).
 *
 * Pure logic, no daemon, no event loop. Every assertion is anchored to PostgreSQL
 * `*_send()` ground truth (big-endian wire bytes) or to exact text spellings — never
 * to a buffer the test itself byte-swapped. Split out of the legacy monolith
 * `test-data-types.cpp` (scalar half).
 *
 * Contract reminders (verified against type_converter.h / .cpp):
 *  - to_binary appends [int32 big-endian length][value bytes].
 *  - from_binary receives the VALUE bytes only (no length prefix); to round-trip,
 *    slice off the leading 4 bytes.
 *  - byte == char; smallint=int16, integer=int32, bigint=int64.
 *  - generic from_text<int family/float/double> throws qb::pg::error::client_error
 *    on garbage; the generic from_binary integer-overflow path throws std::runtime_error.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "../pgsql.h"
#include "../../shared/pg_wire_ground_truth.hpp"

using namespace qb::pg;
using namespace qb::pg::detail;
using qb::pg::test::hex_to_bytes;

// ----------------------------------------------------------------------------
// to_text spellings
// ----------------------------------------------------------------------------

// bool ("t"/"f") and the exact integer text spellings.
TEST(TypeConverterScalarToText, BoolAndIntegerSpellings) {
    EXPECT_EQ(TypeConverter<bool>::to_text(true), "t");
    EXPECT_EQ(TypeConverter<bool>::to_text(false), "f");

    EXPECT_EQ(TypeConverter<smallint>::to_text(static_cast<smallint>(100)), "100");
    EXPECT_EQ(TypeConverter<smallint>::to_text(static_cast<smallint>(-32768)), "-32768");
    EXPECT_EQ(TypeConverter<integer>::to_text(2147483647), "2147483647");
    EXPECT_EQ(TypeConverter<integer>::to_text(-2147483648), "-2147483648");
    EXPECT_EQ(TypeConverter<bigint>::to_text(static_cast<bigint>(9223372036854775807LL)), "9223372036854775807");
    EXPECT_EQ(TypeConverter<bigint>::to_text(static_cast<bigint>(-1)), "-1");
}

// float/double special values (exact PostgreSQL spellings).
TEST(TypeConverterScalarToText, FloatDoubleSpecialValues) {
    EXPECT_EQ(TypeConverter<float>::to_text(std::numeric_limits<float>::quiet_NaN()), "NaN");
    EXPECT_EQ(TypeConverter<float>::to_text(std::numeric_limits<float>::infinity()), "Infinity");
    EXPECT_EQ(TypeConverter<float>::to_text(-std::numeric_limits<float>::infinity()), "-Infinity");

    EXPECT_EQ(TypeConverter<double>::to_text(std::numeric_limits<double>::quiet_NaN()), "NaN");
    EXPECT_EQ(TypeConverter<double>::to_text(std::numeric_limits<double>::infinity()), "Infinity");
    EXPECT_EQ(TypeConverter<double>::to_text(-std::numeric_limits<double>::infinity()), "-Infinity");

    // Normal value: std::to_string(float/double) (locale-default "%f", 6 decimals).
    EXPECT_EQ(TypeConverter<double>::to_text(1.5), std::to_string(1.5));
    EXPECT_EQ(TypeConverter<float>::to_text(2.25f), std::to_string(2.25f));
}

// bytea hex ("\\x...") and UUID canonical form.
TEST(TypeConverterScalarToText, ByteaHexAndUuidCanonical) {
    std::vector<byte> bytes{static_cast<byte>(0xDE), static_cast<byte>(0xAD), static_cast<byte>(0xBE), static_cast<byte>(0xEF)};
    EXPECT_EQ(TypeConverter<std::vector<byte>>::to_text(bytes), "\\xdeadbeef");
    EXPECT_EQ(TypeConverter<std::vector<byte>>::to_text(std::vector<byte>{}), "\\x");

    const std::string canon = "550e8400-e29b-41d4-a716-446655440000";
    qb::uuid          u     = qb::uuid::from_string(canon).value();
    EXPECT_EQ(TypeConverter<qb::uuid>::to_text(u), canon);
}

// ----------------------------------------------------------------------------
// from_text
// ----------------------------------------------------------------------------

// from_text<smallint>: valid, out-of-range throw, garbage throw.
TEST(TypeConverterScalarFromText, SmallintParsing) {
    EXPECT_EQ(TypeConverter<smallint>::from_text("100"), static_cast<smallint>(100));
    EXPECT_EQ(TypeConverter<smallint>::from_text("-32768"), static_cast<smallint>(-32768));
    EXPECT_EQ(TypeConverter<smallint>::from_text("32767"), static_cast<smallint>(32767));
    // std::stoi succeeds (fits int) but the value is out of int16 range -> client_error.
    EXPECT_THROW(TypeConverter<smallint>::from_text("40000"), error::client_error);
    EXPECT_THROW(TypeConverter<smallint>::from_text("-40000"), error::client_error);
    EXPECT_THROW(TypeConverter<smallint>::from_text("abc"), error::client_error);
}

// from_text: integer/bigint/float/double valid + garbage throw + float specials.
TEST(TypeConverterScalarFromText, IntegerBigintFloatDoubleParsing) {
    EXPECT_EQ(TypeConverter<integer>::from_text("12345"), 12345);
    EXPECT_EQ(TypeConverter<integer>::from_text("-2147483648"), -2147483648);
    EXPECT_THROW(TypeConverter<integer>::from_text("xyz"), error::client_error);

    EXPECT_EQ(TypeConverter<bigint>::from_text("9223372036854775807"), static_cast<bigint>(9223372036854775807LL));
    EXPECT_EQ(TypeConverter<bigint>::from_text("-100"), static_cast<bigint>(-100));
    EXPECT_THROW(TypeConverter<bigint>::from_text("not-a-number"), error::client_error);

    EXPECT_FLOAT_EQ(TypeConverter<float>::from_text("3.5"), 3.5f);
    EXPECT_THROW(TypeConverter<float>::from_text("garbage"), error::client_error);
    EXPECT_TRUE(std::isnan(TypeConverter<float>::from_text("NaN")));
    EXPECT_TRUE(std::isinf(TypeConverter<float>::from_text("Infinity")));
    EXPECT_GT(TypeConverter<float>::from_text("Infinity"), 0.0f);
    EXPECT_TRUE(std::isinf(TypeConverter<float>::from_text("-Infinity")));
    EXPECT_LT(TypeConverter<float>::from_text("-Infinity"), 0.0f);

    EXPECT_DOUBLE_EQ(TypeConverter<double>::from_text("2.71828"), 2.71828);
    EXPECT_THROW(TypeConverter<double>::from_text("garbage"), error::client_error);
    EXPECT_TRUE(std::isnan(TypeConverter<double>::from_text("NaN")));
    EXPECT_TRUE(std::isinf(TypeConverter<double>::from_text("Infinity")));
    EXPECT_GT(TypeConverter<double>::from_text("Infinity"), 0.0);
    EXPECT_TRUE(std::isinf(TypeConverter<double>::from_text("-Infinity")));
    EXPECT_LT(TypeConverter<double>::from_text("-Infinity"), 0.0);
}

// from_text<bool>: exact accepted truthy set (t/true/1/yes/y/on); everything else false.
TEST(TypeConverterScalarFromText, BoolTruthySet) {
    for (const char *t : {"t", "true", "1", "yes", "y", "on"})
        EXPECT_TRUE(TypeConverter<bool>::from_text(t)) << "expected true for '" << t << "'";
    // Only the exact 6 tokens above are truthy. "f", "0", "no", "", "off", "TRUE",
    // "True", "Y", "ON" all map to false (case-sensitive).
    for (const char *f : {"f", "0", "no", "", "off", "TRUE", "True", "Y", "ON"})
        EXPECT_FALSE(TypeConverter<bool>::from_text(f)) << "expected false for '" << f << "'";
}

// from_text<bytea>: "\\x..." hex decode and raw verbatim copy.
TEST(TypeConverterScalarFromText, ByteaHexAndRaw) {
    auto hex = TypeConverter<std::vector<byte>>::from_text("\\xDEADBEEF");
    ASSERT_EQ(hex.size(), 4u);
    EXPECT_EQ(static_cast<unsigned char>(hex[0]), 0xDE);
    EXPECT_EQ(static_cast<unsigned char>(hex[1]), 0xAD);
    EXPECT_EQ(static_cast<unsigned char>(hex[2]), 0xBE);
    EXPECT_EQ(static_cast<unsigned char>(hex[3]), 0xEF);

    // No "\x" marker -> copied byte-for-byte.
    auto raw = TypeConverter<std::vector<byte>>::from_text("hello");
    ASSERT_EQ(raw.size(), 5u);
    EXPECT_EQ(std::string(raw.begin(), raw.end()), "hello");
}

// UUID from_text: valid parses, invalid throws.
//
// The generic primary template's value_or(qb::uuid{}) fallback is dead code for
// qb::uuid because TypeConverter<qb::uuid> is a full specialization whose from_text()
// THROWS std::runtime_error on an invalid UUID. We test the reachable behavior.
TEST(TypeConverterScalarFromText, UuidInvalidThrows) {
    const std::string canon = "550e8400-e29b-41d4-a716-446655440000";
    EXPECT_EQ(TypeConverter<qb::uuid>::from_text(canon), qb::uuid::from_string(canon).value());
    EXPECT_THROW(TypeConverter<qb::uuid>::from_text("not-a-uuid"), std::runtime_error);
    EXPECT_THROW(TypeConverter<qb::uuid>::from_text(""), std::runtime_error);
}

// ----------------------------------------------------------------------------
// from_binary against PostgreSQL *_send() ground truth (no self-roundtrip)
// ----------------------------------------------------------------------------

// smallint/integer/bigint: known big-endian wire bytes decode to the known value.
// Replaces the legacy self-roundtrip read tests (createBinaryBuffer -> read_T -> ==v).
TEST(TypeConverterScalarFromBinary, IntegerFamilyGroundTruth) {
    // int2 12345 = 0x3039.
    EXPECT_EQ(TypeConverter<smallint>::from_binary(hex_to_bytes("3039")), static_cast<smallint>(12345));
    // int2 0xFFFF = -1 (two's complement).
    EXPECT_EQ(TypeConverter<smallint>::from_binary(hex_to_bytes("ffff")), static_cast<smallint>(-1));
    // int2 0x8000 = -32768.
    EXPECT_EQ(TypeConverter<smallint>::from_binary(hex_to_bytes("8000")), std::numeric_limits<smallint>::min());

    // int4 987654321 = 0x3ADE68B1.
    EXPECT_EQ(TypeConverter<integer>::from_binary(hex_to_bytes("3ade68b1")), 987654321);
    // int4 0x80000000 = INT32_MIN.
    EXPECT_EQ(TypeConverter<integer>::from_binary(hex_to_bytes("80000000")), std::numeric_limits<integer>::min());

    // int8 INT64_MAX = 0x7FFFFFFFFFFFFFFF.
    EXPECT_EQ(TypeConverter<bigint>::from_binary(hex_to_bytes("7fffffffffffffff")), std::numeric_limits<bigint>::max());
    // int8 0x8000000000000000 = INT64_MIN.
    EXPECT_EQ(TypeConverter<bigint>::from_binary(hex_to_bytes("8000000000000000")), std::numeric_limits<bigint>::min());
}

// float4 / float8 known IEEE-754 big-endian bit patterns + NaN/±Inf.
TEST(TypeConverterScalarFromBinary, FloatDoubleGroundTruthAndSpecials) {
    // float4 1.5 = 0x3FC00000.
    EXPECT_FLOAT_EQ(TypeConverter<float>::from_binary(hex_to_bytes("3fc00000")), 1.5f);
    // float4 -2.5 = 0xC0200000.
    EXPECT_FLOAT_EQ(TypeConverter<float>::from_binary(hex_to_bytes("c0200000")), -2.5f);
    // float8 1.5 = 0x3FF8000000000000.
    EXPECT_DOUBLE_EQ(TypeConverter<double>::from_binary(hex_to_bytes("3ff8000000000000")), 1.5);
    // float8 -2.5 = 0xC004000000000000.
    EXPECT_DOUBLE_EQ(TypeConverter<double>::from_binary(hex_to_bytes("c004000000000000")), -2.5);

    // Specials. float4 NaN=0x7FC00000, +Inf=0x7F800000, -Inf=0xFF800000.
    EXPECT_TRUE(std::isnan(TypeConverter<float>::from_binary(hex_to_bytes("7fc00000"))));
    EXPECT_TRUE(std::isinf(TypeConverter<float>::from_binary(hex_to_bytes("7f800000"))));
    EXPECT_GT(TypeConverter<float>::from_binary(hex_to_bytes("7f800000")), 0.0f);
    EXPECT_TRUE(std::isinf(TypeConverter<float>::from_binary(hex_to_bytes("ff800000"))));
    EXPECT_LT(TypeConverter<float>::from_binary(hex_to_bytes("ff800000")), 0.0f);
    // float8 +Inf=0x7FF0000000000000, -Inf=0xFFF0000000000000.
    EXPECT_TRUE(std::isinf(TypeConverter<double>::from_binary(hex_to_bytes("7ff0000000000000"))));
    EXPECT_GT(TypeConverter<double>::from_binary(hex_to_bytes("7ff0000000000000")), 0.0);
    EXPECT_TRUE(std::isinf(TypeConverter<double>::from_binary(hex_to_bytes("fff0000000000000"))));
    EXPECT_LT(TypeConverter<double>::from_binary(hex_to_bytes("fff0000000000000")), 0.0);
}

// Malformed/undersized buffers must throw, never read out of bounds.
TEST(TypeConverterScalarFromBinary, MalformedShortBufferThrows) {
    const std::vector<byte> one(1, byte{0});
    EXPECT_THROW(TypeConverter<smallint>::from_binary(one), std::runtime_error);
    EXPECT_THROW(TypeConverter<bigint>::from_binary(one), std::runtime_error);
    EXPECT_THROW(TypeConverter<float>::from_binary(one), std::runtime_error);
    EXPECT_THROW(TypeConverter<double>::from_binary(one), std::runtime_error);
    EXPECT_THROW(TypeConverter<bool>::from_binary(std::span<const byte>{}), std::runtime_error);
}

// Generic from_binary<integer>: size-dispatch (2/4/8 bytes) + overflow throw.
TEST(TypeConverterScalarFromBinary, IntegerSizeDispatch) {
    // 2-byte BE (int2 read, widened): 0x0064 == 100.
    EXPECT_EQ(TypeConverter<integer>::from_binary(hex_to_bytes("0064")), 100);
    // 4-byte BE: 0x0000002a == 42.
    EXPECT_EQ(TypeConverter<integer>::from_binary(hex_to_bytes("0000002a")), 42);
    // 8-byte BE in int32 range: 42.
    EXPECT_EQ(TypeConverter<integer>::from_binary(hex_to_bytes("000000000000002a")), 42);
    // 8-byte BE > INT32_MAX (2147483648) -> std::runtime_error.
    EXPECT_THROW(TypeConverter<integer>::from_binary(hex_to_bytes("0000000080000000")), std::runtime_error);
    // 8-byte BE < INT32_MIN (-2147483649) -> std::runtime_error.
    EXPECT_THROW(TypeConverter<integer>::from_binary(hex_to_bytes("ffffffff7fffffff")), std::runtime_error);
}

// ADD: from_binary<bigint> / from_binary<smallint> — NATIVE-WIDTH reads.
//
// Unlike <integer> (which size-dispatches 2/4/8 and overflow-checks the 8-byte case),
// <bigint> and <smallint> read their native width directly: a too-short buffer throws,
// and there is no widening/overflow dispatch. (The legacy file covered neither.)
TEST(TypeConverterScalarFromBinary, BigintNativeWidth) {
    EXPECT_EQ(TypeConverter<bigint>::from_binary(hex_to_bytes("000000000000002a")), static_cast<bigint>(42));
    EXPECT_EQ(TypeConverter<bigint>::from_binary(hex_to_bytes("7fffffffffffffff")), std::numeric_limits<bigint>::max());
    EXPECT_EQ(TypeConverter<bigint>::from_binary(hex_to_bytes("8000000000000000")), std::numeric_limits<bigint>::min());
    // No 2/4-byte dispatch: a sub-8-byte buffer throws (it is not widened).
    EXPECT_THROW(TypeConverter<bigint>::from_binary(hex_to_bytes("0064")), std::runtime_error);
    EXPECT_THROW(TypeConverter<bigint>::from_binary(hex_to_bytes("0000002a")), std::runtime_error);
}

TEST(TypeConverterScalarFromBinary, SmallintNativeWidth) {
    EXPECT_EQ(TypeConverter<smallint>::from_binary(hex_to_bytes("0064")), static_cast<smallint>(100));
    EXPECT_EQ(TypeConverter<smallint>::from_binary(hex_to_bytes("8000")), std::numeric_limits<smallint>::min());
    // No narrowing dispatch: a 4-byte buffer reads only the leading 2 bytes
    // (0x0000_002a -> 0x0000 == 0), it is NOT interpreted as int4 42.
    EXPECT_EQ(TypeConverter<smallint>::from_binary(hex_to_bytes("0000002a")), static_cast<smallint>(0));
    // A 1-byte buffer is too short -> throw.
    EXPECT_THROW(TypeConverter<smallint>::from_binary(hex_to_bytes("01")), std::runtime_error);
}

// Generic from_binary<bool> + UUID specialization edge sizes.
TEST(TypeConverterScalarFromBinary, BoolAndUuidEdgeSizes) {
    EXPECT_TRUE(TypeConverter<bool>::from_binary(hex_to_bytes("01")));
    EXPECT_FALSE(TypeConverter<bool>::from_binary(hex_to_bytes("00")));
    EXPECT_TRUE(TypeConverter<bool>::from_binary(hex_to_bytes("ff"))); // any non-zero byte is true

    // UUID 15-byte span: neither 16 nor >= 20 -> "Buffer too small for UUID" throw.
    std::vector<byte> fifteen(15, static_cast<byte>(0x11));
    EXPECT_THROW(TypeConverter<qb::uuid>::from_binary(fifteen), std::runtime_error);

    // UUID 16-byte value decodes to the canonical form.
    auto u = TypeConverter<qb::uuid>::from_binary(hex_to_bytes("550e8400e29b41d4a716446655440000"));
    EXPECT_EQ(u, qb::uuid::from_string("550e8400-e29b-41d4-a716-446655440000").value());

    // UUID 20-byte length-prefixed value: a 4-byte prefix precedes the 16 raw
    // bytes (the >= 20 path that strips the prefix and reads from offset 4).
    auto prefixed = TypeConverter<qb::uuid>::from_binary(hex_to_bytes("00000010550e8400e29b41d4a716446655440000"));
    EXPECT_EQ(prefixed, qb::uuid::from_string("550e8400-e29b-41d4-a716-446655440000").value());
}

// ----------------------------------------------------------------------------
// TypeConverter<std::string> (NUMERIC-as-text specialization) and get_oid()
// ----------------------------------------------------------------------------

TEST(TypeConverterStringTest, NumericAsTextSpecialization) {
    EXPECT_EQ(TypeConverter<std::string>::get_oid(), static_cast<integer>(oid::text));

    // to_binary writes [int32 len][bytes]; from_binary reads the prefixed form.
    std::vector<byte> buf;
    TypeConverter<std::string>::to_binary("12.5", buf);
    ASSERT_EQ(buf.size(), 4u + 4u);
    EXPECT_EQ(TypeConverter<std::string>::from_binary(buf), "12.5");

    // Buffer shorter than the 4-byte header -> "".
    EXPECT_EQ(TypeConverter<std::string>::from_binary(hex_to_bytes("0000")), "");
    // Header length <= 0 -> "" (here len == 0).
    EXPECT_EQ(TypeConverter<std::string>::from_binary(hex_to_bytes("00000000")), "");

    // read_string on a real length-prefixed binary string: [int32 5]["hello"].
    EXPECT_EQ(TypeConverter<std::string>::from_binary(hex_to_bytes("0000000568656c6c6f")), "hello");

    // to_text / from_text are identity for the NUMERIC-as-text converter.
    EXPECT_EQ(TypeConverter<std::string>::to_text("123.456"), "123.456");
    EXPECT_EQ(TypeConverter<std::string>::from_text("123.456"), "123.456");
}

// get_oid() for the scalar converters (precise wire OIDs).
TEST(TypeConverterScalarOid, KnownOids) {
    EXPECT_EQ(TypeConverter<bool>::get_oid(), static_cast<integer>(oid::boolean));
    EXPECT_EQ(TypeConverter<smallint>::get_oid(), static_cast<integer>(oid::int2));
    EXPECT_EQ(TypeConverter<integer>::get_oid(), static_cast<integer>(oid::int4));
    EXPECT_EQ(TypeConverter<bigint>::get_oid(), static_cast<integer>(oid::int8));
    EXPECT_EQ(TypeConverter<float>::get_oid(), static_cast<integer>(oid::float4));
    EXPECT_EQ(TypeConverter<double>::get_oid(), static_cast<integer>(oid::float8));
    EXPECT_EQ(TypeConverter<std::vector<byte>>::get_oid(), static_cast<integer>(oid::bytea));
    EXPECT_EQ(TypeConverter<qb::uuid>::get_oid(), static_cast<integer>(oid::uuid));
}
