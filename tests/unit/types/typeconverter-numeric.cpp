/**
 * @file typeconverter-numeric.cpp
 * @brief Unit tests for the PostgreSQL NUMERIC (base-10000) binary codec —
 *        TypeConverter<numeric>::from_binary / to_binary, decode_pg_numeric guards,
 *        and the non-finite (NaN / ±Infinity) sign words.
 *
 * Anchored to PostgreSQL `numeric_send()` ground truth (value bytes, no per-field
 * length prefix — the shape the protocol layer hands to from_binary). Wire layout:
 * int16 ndigits, int16 weight, uint16 sign, uint16 dscale, then base-10000 digits.
 * Sign words: 0x0000 positive, 0x4000 negative, 0xC000 NaN, 0xD000 +Inf, 0xF000 -Inf.
 *
 * Split out of the legacy monolith `test-data-types.cpp` (numeric tests).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "../../shared/pg_wire_ground_truth.hpp"
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using namespace qb::pg::detail;
using qb::pg::test::hex_to_bytes;

// Binary NUMERIC decode against PostgreSQL ground truth (the de-fraud corpus —
// these bytes are real numeric_send() output, NOT bytes the test emitted).
TEST(TypeConverterNumericBinary, DecodeAgainstPostgresGroundTruth) {
    struct C {
        const char *hex;
        const char *expect;
    };
    const C cases[] = {
        {"000200000000000404d2162e", "1234.5678"},
        {"0002000040000001000c1388", "-12.5"},
        {"00010001000000000064", "1000000"},
        {"0000000000000000", "0"},
        {"000600020000000a000109291a85007b11d722c4", "123456789.0123456789"},
        {"0001ffff000000011388", "0.5"},
        {"0001ffff0000000203e8", "0.10"},   // trailing-zero / dscale preserved
        {"00010000000000020064", "100.00"}, // dscale preserved
        {"000500000000000f03e7270f270f270f2706", "999.999999999999999"},
    };
    for (const auto &c : cases) {
        numeric got = TypeConverter<numeric>::from_binary(hex_to_bytes(c.hex));
        EXPECT_EQ(got.str(), c.expect) << "hex=" << c.hex;
    }
}

// Round-trip: to_binary (real PG binary, length-prefixed) -> from_binary.
TEST(TypeConverterNumericBinary, RoundTripCanonicalValues) {
    for (const char *v : {"0", "1", "-1", "12345.678", "-999.99", "123456789.0123456789", "0.0001", "1000000"}) {
        std::vector<byte> buf;
        TypeConverter<numeric>::to_binary(numeric(v), buf);
        EXPECT_EQ(TypeConverter<numeric>::from_binary(buf).str(), v) << "value=" << v;
    }
}

// ADD: NaN / +Infinity / -Infinity binary SIGN WORDS — decoder tolerance.
// Layout: [int16 ndigits=0000][int16 weight=0000][uint16 sign][uint16 dscale].
// The decoder ignores dscale for non-finite values, so the dscale=0 form below
// decodes identically to PG's canonical numeric_send (which emits dscale=0x0020 for
// ±Infinity, dscale=0 for NaN — see gt::numeric::specials). This pins exactly that
// dscale-agnostic decode behavior.
TEST(TypeConverterNumericBinary, NonFiniteSignWordsGroundTruth) {
    // sign 0xC000 == NaN.
    EXPECT_EQ(TypeConverter<numeric>::from_binary(hex_to_bytes("00000000c0000000")).str(), "NaN");
    // sign 0xD000 == +Infinity (PG 14+ numeric_send infinity), dscale=0 accepted.
    EXPECT_EQ(TypeConverter<numeric>::from_binary(hex_to_bytes("00000000d0000000")).str(), "Infinity");
    // sign 0xF000 == -Infinity, dscale=0 accepted.
    EXPECT_EQ(TypeConverter<numeric>::from_binary(hex_to_bytes("00000000f0000000")).str(), "-Infinity");

    // And the converter round-trips the non-finite spellings through to_binary.
    for (const char *v : {"NaN", "Infinity", "-Infinity"}) {
        std::vector<byte> buf;
        TypeConverter<numeric>::to_binary(numeric(v), buf);
        EXPECT_EQ(TypeConverter<numeric>::from_binary(buf).str(), v) << "value=" << v;
    }
}

// ADD: NUMERIC special boundary — large weight (big integer part) and a negative
// fractional value with a large weight magnitude, decoded from real send bytes.
TEST(TypeConverterNumericBinary, LargeWeightAndScale) {
    // 1000000 == ndigits=1, weight=1, sign=0, dscale=0, limb[0]=100 (== 100 * 10000^1).
    EXPECT_EQ(TypeConverter<numeric>::from_binary(hex_to_bytes("00010001000000000064")).str(), "1000000");
    // 0.10: ndigits=1, weight=-1, sign=0, dscale=2, limb[0]=1000 (0x03e8).
    EXPECT_EQ(TypeConverter<numeric>::from_binary(hex_to_bytes("0001ffff0000000203e8")).str(), "0.10");
}

// decode_pg_numeric guard paths: header shorter than 8 bytes and a truncated digit
// stream both fall back to "0" (never read out of bounds).
TEST(TypeConverterNumericBinary, ShortAndTruncatedBufferGuards) {
    // Null / zero-length.
    EXPECT_EQ(decode_pg_numeric(nullptr, 0), "0");
    // 6 bytes (< 8-byte header).
    std::vector<byte> short_buf = hex_to_bytes("000200000000");
    EXPECT_EQ(decode_pg_numeric(short_buf.data(), short_buf.size()), "0");
    // Header says ndigits=2 but only the header (no limbs) is present -> "0".
    std::vector<byte> truncated = hex_to_bytes("0002000000000004");
    EXPECT_EQ(decode_pg_numeric(truncated.data(), truncated.size()), "0");
    // Via the converter (value-first, < 12 so the legacy prefix branch is skipped).
    EXPECT_EQ(TypeConverter<numeric>::from_binary(short_buf).str(), "0");
}

// numeric is value-equality on its canonical text form (it is NOT an arithmetic type).
TEST(TypeConverterNumericValue, OidTextAndEquality) {
    EXPECT_EQ(TypeConverter<numeric>::get_oid(), 1700);

    numeric n1("123456789.0123456789");
    EXPECT_EQ(TypeConverter<numeric>::to_text(n1), "123456789.0123456789");
    EXPECT_EQ(TypeConverter<numeric>::from_text("999.999999999999999").str(), "999.999999999999999");

    numeric price("199.99");
    EXPECT_EQ(price, numeric("199.99"));
    EXPECT_FALSE(price == numeric("200.00"));
}

// Extreme-magnitude values preserved as exact text (financial precision).
TEST(TypeConverterNumericValue, ExtremeMagnitudes) {
    numeric huge("999999999999999999999999999.9999999999");
    EXPECT_EQ(huge.str(), "999999999999999999999999999.9999999999");

    numeric tiny("0.0000000000000000000000000000000000001");
    EXPECT_EQ(tiny.str(), "0.0000000000000000000000000000000000001");

    numeric negative("-9999999999.9999999999");
    EXPECT_EQ(negative.str(), "-9999999999.9999999999");

    EXPECT_EQ(numeric("0").str(), "0");
}
