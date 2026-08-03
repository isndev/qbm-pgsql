/**
 * @file typeconverter-adversarial.cpp
 * @brief ADVERSARIAL edge-case / bug-hunting unit tests for the qbm-pgsql
 *        TypeConverter<T> serde layer (type_converter.h / type_converter.cpp).
 *
 * This file deliberately attacks the UNDER-tested codec paths that the existing
 * suites (typeconverter-scalar / -numeric / -temporal / -array / -json and
 * wire/typeconverter-codecs) do not cover: the text-format PARSERS (from_text),
 * malformed / truncated / oversized from_binary buffers, the NUMERIC limb
 * encoder's malformed-input behaviour, NUMERIC scale/weight boundary decodes,
 * float/double subnormal & non-finite text round-trips, timestamp text micro
 * scaling, UUID/bytea text edges, std::optional value-vs-NULL framing, and the
 * array decoder's nested / NULL / non-finite-element paths.
 *
 * ORACLE discipline (per the serde-audit warning that to_binary->from_binary
 * self-round-trips can hide a SHARED convention bug):
 *   - decode assertions are anchored to PostgreSQL *_send() / numeric_out ground
 *     truth or to externally-computed known values, NOT to bytes this test
 *     byte-swapped itself;
 *   - where a round-trip is used it is paired with at least one ground-truth or
 *     known-value anchor on the same path.
 *
 * Contract reminders (verified against the headers):
 *   - to_binary appends [int32 big-endian length][value bytes];
 *   - from_binary receives the VALUE bytes only (no length prefix);
 *   - byte == char; smallint=int16, integer=int32, bigint=int64;
 *   - the generic int-family / float / double from_text throw
 *     qb::pg::error::client_error on a fully-unparseable string, BUT std::stoi /
 *     std::stof stop at the first non-digit, so a "<number><garbage>" string
 *     parses the numeric prefix and SILENTLY DROPS the tail (documented below).
 *
 * Pure logic: no daemon, no qb::Main, no event loop, parallel-safe.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "../../shared/pg_wire_ground_truth.hpp"
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using namespace qb::pg::detail;
namespace gt = qb::pg::test::gt;
using qb::pg::test::hex_to_bytes;

// to_binary frames as [int32 big-endian length][value]; strip the prefix to get
// the wire field bytes that from_binary actually receives.
static std::vector<byte>
strip_prefix(std::vector<byte> b) {
    EXPECT_GE(b.size(), sizeof(integer));
    b.erase(b.begin(), b.begin() + sizeof(integer));
    return b;
}

// ============================================================================
// SECTION 1 — from_text<int-family>: range/overflow vs. SILENT trailing garbage.
//
// std::stoi / std::stoll parse a leading numeric prefix and stop at the first
// non-digit WITHOUT error. The converters therefore accept "12abc" as 12. This
// is a TOLERANCE GAP versus PostgreSQL ('12abc'::int4 -> 22P02 invalid_text), but
// it is non-corrupting (the prefix is exact). We PIN the current behaviour so a
// future hardening (reject-on-trailing-garbage) flips these intentionally.
// ============================================================================

TEST(TCAdversarialFromText, IntFamilySilentTrailingGarbage) {
    // Leading whitespace is skipped, trailing non-digits are dropped silently.
    EXPECT_EQ(TypeConverter<smallint>::from_text("12abc"), static_cast<smallint>(12));
    EXPECT_EQ(TypeConverter<smallint>::from_text("  42"), static_cast<smallint>(42));
    EXPECT_EQ(TypeConverter<integer>::from_text("100 200"), 100);
    EXPECT_EQ(TypeConverter<bigint>::from_text("9000000000xyz"), static_cast<bigint>(9000000000LL));

    // Fully-unparseable -> client_error (the catch path).
    EXPECT_THROW(TypeConverter<integer>::from_text("xyz"), error::client_error);
    EXPECT_THROW(TypeConverter<integer>::from_text(""), error::client_error);
    EXPECT_THROW(TypeConverter<bigint>::from_text(""), error::client_error);
    EXPECT_THROW(TypeConverter<smallint>::from_text(""), error::client_error);
}

TEST(TCAdversarialFromText, IntFamilyRangeBoundaries) {
    // smallint range-check happens AFTER std::stoi: in-int16-range ok, out-of-range throws.
    EXPECT_EQ(TypeConverter<smallint>::from_text("32767"), static_cast<smallint>(32767));
    EXPECT_EQ(TypeConverter<smallint>::from_text("-32768"), static_cast<smallint>(-32768));
    EXPECT_THROW(TypeConverter<smallint>::from_text("32768"), error::client_error);  // INT16_MAX+1
    EXPECT_THROW(TypeConverter<smallint>::from_text("-32769"), error::client_error); // INT16_MIN-1

    // integer uses std::stoi (32-bit); a > INT32 value overflows std::stoi -> out_of_range
    // -> wrapped as client_error (NOT a silent wrap).
    EXPECT_EQ(TypeConverter<integer>::from_text("2147483647"), 2147483647);
    EXPECT_EQ(TypeConverter<integer>::from_text("-2147483648"), std::numeric_limits<integer>::min());
    EXPECT_THROW(TypeConverter<integer>::from_text("2147483648"), error::client_error);  // INT32_MAX+1
    EXPECT_THROW(TypeConverter<integer>::from_text("-2147483649"), error::client_error); // INT32_MIN-1

    // bigint INT64 boundaries.
    EXPECT_EQ(TypeConverter<bigint>::from_text("9223372036854775807"), std::numeric_limits<bigint>::max());
    EXPECT_EQ(TypeConverter<bigint>::from_text("-9223372036854775808"), std::numeric_limits<bigint>::min());
    EXPECT_THROW(TypeConverter<bigint>::from_text("9223372036854775808"), error::client_error); // INT64_MAX+1
}

// ============================================================================
// SECTION 2 — from_text<float/double>: subnormals, non-finite spellings, and the
// SILENT trailing-garbage tolerance (std::stof/stod stop at the first bad char).
// ============================================================================

TEST(TCAdversarialFromText, FloatDoubleSpecialSpellings) {
    // The converter special-cases exactly "NaN" / "Infinity"/"inf" / "-Infinity"/"-inf".
    EXPECT_TRUE(std::isnan(TypeConverter<double>::from_text("NaN")));
    EXPECT_TRUE(std::isinf(TypeConverter<double>::from_text("inf")));
    EXPECT_GT(TypeConverter<double>::from_text("inf"), 0.0);
    EXPECT_TRUE(std::isinf(TypeConverter<double>::from_text("-inf")));
    EXPECT_LT(TypeConverter<double>::from_text("-inf"), 0.0);

    // Spellings NOT special-cased fall through to std::stod, which still recognises
    // "INF"/"NAN" case-insensitively -> non-finite (matches PG's case-insensitive accept).
    EXPECT_TRUE(std::isinf(TypeConverter<double>::from_text("INF")));
    EXPECT_TRUE(std::isnan(TypeConverter<double>::from_text("NAN")));

    // Trailing garbage after a finite value is silently dropped (tolerance gap).
    EXPECT_DOUBLE_EQ(TypeConverter<double>::from_text("2.5 extra"), 2.5);
    EXPECT_FLOAT_EQ(TypeConverter<float>::from_text("1.5xyz"), 1.5f);

    // Truly unparseable -> client_error.
    EXPECT_THROW(TypeConverter<double>::from_text("garbage"), error::client_error);
    EXPECT_THROW(TypeConverter<float>::from_text(""), error::client_error);
}

TEST(TCAdversarialFromText, FloatDoubleSubnormalAndOverflow) {
    // Smallest positive subnormal double (~4.9e-324) survives text round-trip exactly.
    const double dsub = std::numeric_limits<double>::denorm_min();
    EXPECT_DOUBLE_EQ(TypeConverter<double>::from_text(TypeConverter<double>::to_text(dsub)), dsub);

    // Smallest positive subnormal float survives float round-trip exactly.
    const float fsub = std::numeric_limits<float>::denorm_min();
    EXPECT_FLOAT_EQ(TypeConverter<float>::from_text(TypeConverter<float>::to_text(fsub)), fsub);

    // A decimal magnitude beyond double range: std::stod throws out_of_range -> client_error.
    EXPECT_THROW(TypeConverter<double>::from_text("1e400"), error::client_error);
    // float overflow likewise (1e40 > FLT_MAX).
    EXPECT_THROW(TypeConverter<float>::from_text("1e40"), error::client_error);

    // Negative zero text round-trips to a value that compares equal to 0.0 and keeps sign.
    const double nz = TypeConverter<double>::from_text("-0.0");
    EXPECT_EQ(nz, 0.0);
    EXPECT_TRUE(std::signbit(nz));
}

// ============================================================================
// SECTION 3 — from_text<bytea>: hex/escape edges.
//
// TWO distinct bytea converters exist and they DIVERGE on bad hex:
//   (a) the primary-template std::vector<byte> path (type_converter.h ~L551)
//       decodes "\\x.." via std::stoi(substr,16) with NO try/catch and drops a
//       trailing odd nibble silently;
//   (b) the std::vector<std::byte> specialization (type_converter.cpp from_text)
//       routes through qb::crypto::hex_to_string, which returns "" on odd-length
//       / non-hex input (graceful empty).
// ============================================================================

TEST(TCAdversarialFromText, ByteaPrimaryTemplateHexEdges) {
    using BV = std::vector<byte>;
    // Even-length hex decodes; trailing odd nibble is silently dropped (i+1<len guard).
    auto odd = TypeConverter<BV>::from_text("\\xabc"); // "abc" -> 1 byte 0xab, 'c' dropped
    ASSERT_EQ(odd.size(), 1u);
    EXPECT_EQ(static_cast<unsigned char>(odd[0]), 0xABu);

    // Empty hex payload "\\x" -> empty vector (valid empty bytea).
    EXPECT_TRUE(TypeConverter<BV>::from_text("\\x").empty());

    // Non-hex digits after "\\x" are now reported as error::client_error (the std::stoi(...,16)
    // failure is wrapped), consistent with every other from_text parser — previously this leaked
    // a raw std::invalid_argument.
    EXPECT_THROW(TypeConverter<BV>::from_text("\\xZZ"), error::client_error);

    // No "\\x" marker -> copied verbatim (raw escape-format fallback).
    auto raw = TypeConverter<BV>::from_text("hi");
    ASSERT_EQ(raw.size(), 2u);
    EXPECT_EQ(std::string(raw.begin(), raw.end()), "hi");
}

TEST(TCAdversarialFromText, ByteaStdByteHexGraceful) {
    using SB = std::vector<std::byte>;
    // The std::byte specialization rejects malformed hex by yielding an EMPTY vector
    // (qb::crypto::hex_to_string returns "" on odd-length / non-hex), never throwing.
    EXPECT_TRUE(TypeConverter<SB>::from_text("\\xZZ").empty());  // non-hex
    EXPECT_TRUE(TypeConverter<SB>::from_text("\\xabc").empty()); // odd length
    // Uppercase hex with and without the "\\x" marker both decode.
    SB expect{std::byte{0xDE}, std::byte{0xAD}};
    EXPECT_EQ(TypeConverter<SB>::from_text("\\xDEAD"), expect);
    EXPECT_EQ(TypeConverter<SB>::from_text("dead"), expect);
    // An embedded NUL byte round-trips through to_text/from_text.
    SB withnul{std::byte{0x00}, std::byte{0xFF}, std::byte{0x00}};
    EXPECT_EQ(TypeConverter<SB>::from_text(TypeConverter<SB>::to_text(withnul)), withnul);
}

// ============================================================================
// SECTION 4 — NUMERIC encoder/decoder adversarial input.
//
// encode_pg_numeric only collects [0-9] characters; everything else (signs aside)
// is ignored. The decode oracle is PostgreSQL numeric_out semantics (computed by
// hand below), NOT a self-round-trip.
// ============================================================================

// Malformed decimal text (any non-digit beyond the optional leading sign / single dot, or
// no digit at all) is now REJECTED with error::client_error. Previously the encoder silently
// stripped non-digits and emitted a zero/partial NUMERIC — sending wrong data to the server
// ("abc"->0, "12abc"->12, "1e5"->15). This pins the fail-loud contract.
TEST(TCAdversarialNumeric, EncoderRejectsMalformedDecimalText) {
    for (const char *bad : {"abc", "", "-", ".", "   ", "+", "1e5", "12abc", "1.2.3", "0x10", "1,5"}) {
        std::vector<byte> buf;
        EXPECT_THROW(TypeConverter<numeric>::to_binary(numeric(bad), buf), error::client_error)
            << "in='" << bad << "' must be rejected, not silently encoded as zero/partial";
    }
}

// Valid-but-degenerately-formatted decimals are still accepted and normalised (PG-correct).
TEST(TCAdversarialNumeric, EncoderNormalisesValidDegenerateText) {
    struct C {
        const char *in;
        const char *decoded; // decode_pg_numeric(encode_pg_numeric(in))
    };
    const C cases[] = {
        {"-0", "0"},    // negative zero normalises to "0"
        {"007", "7"},   // leading zeros stripped (PG-correct)
        {"5.", "5"},    // trailing dot, no fraction
        {"12.", "12"},  // ditto
        {"+.5", "0.5"}, // leading '+' and bare fraction
        {"-.5", "-0.5"},
        {"0.0", "0.0"},         // exact zero KEEPS dscale=1
        {"00.50", "0.50"},      // leading-zero int part + trailing-zero fraction (dscale=2)
        {"1.50000", "1.50000"}, // trailing-zero scale preserved
    };
    for (const auto &c : cases) {
        std::vector<byte> buf;
        TypeConverter<numeric>::to_binary(numeric(c.in), buf);
        EXPECT_EQ(TypeConverter<numeric>::from_binary(buf).str(), c.decoded) << "in='" << c.in << "'";
    }
}

// decode_pg_numeric weight/scale boundaries against numeric_out semantics, using
// HAND-BUILT wire headers (the limb math is verified independently, not round-tripped).
TEST(TCAdversarialNumeric, DecodeWeightAndScaleBoundaries) {
    // Build a numeric value-bytes buffer: [int16 ndigits][int16 weight][uint16 sign]
    // [uint16 dscale][int16 limbs...], all big-endian.
    auto mk = [](std::vector<std::uint16_t> words) {
        std::vector<byte> b;
        for (std::uint16_t w : words) {
            std::uint16_t be = qb::endian::to_big_endian(w);
            b.push_back(reinterpret_cast<const byte *>(&be)[0]);
            b.push_back(reinterpret_cast<const byte *>(&be)[1]);
        }
        return b;
    };

    // ndigits=1, weight=2, limb[0]=1 -> 1 * 10000^2 = 100000000 (trailing limbs absent => 0).
    EXPECT_EQ(decode_pg_numeric(mk({1, 2, 0x0000, 0, 1}).data(), 10), "100000000");
    // ndigits=2, weight=1, limbs {1,2} -> 1*10000 + 2 = 10002 (second limb zero-padded).
    EXPECT_EQ(decode_pg_numeric(mk({2, 1, 0x0000, 0, 1, 2}).data(), 12), "10002");
    // ndigits=1, weight=1, limb[0]=5 -> 5 * 10000 = 50000.
    EXPECT_EQ(decode_pg_numeric(mk({1, 1, 0x0000, 0, 5}).data(), 10), "50000");
    // Negative: ndigits=1, weight=-1, sign=0x4000, dscale=2, limb=1000 -> -0.10.
    EXPECT_EQ(decode_pg_numeric(mk({1, 0xFFFF, 0x4000, 2, 1000}).data(), 10), "-0.10");
    // Fraction needs more limbs than present: ndigits=1 weight=-1 dscale=8 limb=5000 ->
    // 0.50000000 (missing fraction limbs read as 0, truncated to dscale).
    EXPECT_EQ(decode_pg_numeric(mk({1, 0xFFFF, 0x0000, 8, 5000}).data(), 10), "0.50000000");
}

// decode_pg_numeric truncated-digit-stream guard: header says ndigits=3 but the limbs
// are missing -> "0" (the size < 8 + ndigits*2 guard), never an out-of-bounds read.
TEST(TCAdversarialNumeric, DecodeTruncatedDigitStreamGuard) {
    // header only (ndigits=3, weight=0, sign=0, dscale=0), no limbs.
    EXPECT_EQ(TypeConverter<numeric>::from_binary(hex_to_bytes("0003000000000000")).str(), "0");
    // header + 1 limb but claims 3 -> still guarded to "0".
    EXPECT_EQ(TypeConverter<numeric>::from_binary(hex_to_bytes("00030000000000000001")).str(), "0");
    // nullptr / zero size.
    EXPECT_EQ(decode_pg_numeric(nullptr, 0), "0");
}

// Round-trip the trailing-zero / scale corpus through REAL PG binary (to_binary writes
// the length prefix; from_binary detects & strips it). Anchored: each result is also a
// known canonical numeric_out string.
TEST(TCAdversarialNumeric, ScalePreservingRoundTrip) {
    for (const char *v : {"100.00", "0.10", "0.0001", "1.000000", "-0.50", "1000000", "0.0"}) {
        std::vector<byte> buf;
        TypeConverter<numeric>::to_binary(numeric(v), buf);
        EXPECT_EQ(TypeConverter<numeric>::from_binary(buf).str(), v) << "value=" << v;
    }
}

// ============================================================================
// SECTION 5 — TIMESTAMP (qb::wall_time) text parser edges (the LIVE from_text in
// type_converter.cpp, sscanf-based — the regex variant in the header is dead code,
// shadowed by the full specialization).
// ============================================================================

TEST(TCAdversarialTimestamp, FractionalScalingAndPadding) {
    // ".5" -> 500000 us; ".123" -> 123000 us; ".123456" -> 123456 us; full 6 digits exact.
    EXPECT_EQ(qb::unix_micros(TypeConverter<qb::wall_time>::from_text("2000-01-01 00:00:00.5")) % 1000000, 500000);
    EXPECT_EQ(qb::unix_micros(TypeConverter<qb::wall_time>::from_text("2000-01-01 00:00:00.123")) % 1000000, 123000);
    EXPECT_EQ(qb::unix_micros(TypeConverter<qb::wall_time>::from_text("2000-01-01 00:00:00.123456")) % 1000000, 123456);

    // Fraction delimited at the first non-digit so a trailing "+00" zone does NOT inflate
    // the digit count: ".789+00" must be 789000 us, not mis-scaled.
    auto tz = TypeConverter<qb::wall_time>::from_text("2024-01-01 12:00:00.789+00");
    EXPECT_EQ(qb::unix_micros(tz) % 1000000, 789000);
}

TEST(TCAdversarialTimestamp, TimezoneOffsetApplied) {
    // "12:00:00+02" is 10:00:00Z; the no-zone form is treated as UTC. Difference = 7200 s.
    auto withtz = TypeConverter<qb::wall_time>::from_text("2024-01-01 12:00:00+02");
    auto noz    = TypeConverter<qb::wall_time>::from_text("2024-01-01 12:00:00");
    EXPECT_EQ(qb::unix_micros(noz) - qb::unix_micros(withtz), 7200LL * 1000000LL);

    // "+05:30" half-hour zone east of UTC.
    auto half = TypeConverter<qb::wall_time>::from_text("2024-01-01 12:00:00+05:30");
    EXPECT_EQ(qb::unix_micros(noz) - qb::unix_micros(half), (5 * 3600 + 30 * 60) * 1000000LL);

    // West-of-UTC negative offset: "-08" is 8h AHEAD of UTC instant.
    auto west = TypeConverter<qb::wall_time>::from_text("2024-01-01 12:00:00-08");
    EXPECT_EQ(qb::unix_micros(west) - qb::unix_micros(noz), 8LL * 3600 * 1000000LL);
}

TEST(TCAdversarialTimestamp, PreEpochAndPre2000Boundaries) {
    // Pre-1970 (Unix epoch) instant: pure-integer UTC arithmetic, no platform divergence.
    // 1969-07-20 20:17:40 UTC is BEFORE the epoch -> negative unix micros.
    auto moon = TypeConverter<qb::wall_time>::from_text("1969-07-20 20:17:40");
    EXPECT_LT(qb::unix_micros(moon), 0);
    EXPECT_NE(TypeConverter<qb::wall_time>::to_text(moon).find("1969-07-20 20:17:40"), std::string::npos);

    // Pre-2000 but post-1970 (the PostgreSQL epoch is 2000-01-01, so this is negative
    // micros-since-2000 on the wire while still positive unix micros).
    auto y1995 = TypeConverter<qb::wall_time>::from_text("1995-06-15 10:30:00.250000");
    EXPECT_GT(qb::unix_micros(y1995), 0);
    EXPECT_EQ(qb::unix_micros(y1995) % 1000000, 250000);
    EXPECT_NE(TypeConverter<qb::wall_time>::to_text(y1995).find("1995-06-15 10:30:00.25"), std::string::npos);

    // A sub-second pre-1970 instant must BORROW a second (floor), not truncate toward zero.
    auto pre = TypeConverter<qb::wall_time>::from_text("1955-11-05 06:15:00.500000");
    EXPECT_NE(TypeConverter<qb::wall_time>::to_text(pre).find("1955-11-05 06:15:00.5"), std::string::npos);
}

TEST(TCAdversarialTimestamp, MalformedRejected) {
    EXPECT_THROW(TypeConverter<qb::wall_time>::from_text(""), std::runtime_error);
    EXPECT_THROW(TypeConverter<qb::wall_time>::from_text("not a timestamp"), std::runtime_error);
    EXPECT_THROW(TypeConverter<qb::wall_time>::from_text("2024-01-01"), std::runtime_error); // date only, no time
}

// from_binary timestamp boundary: a 9..11-byte field must be REJECTED (it would make the
// 4-byte-prefixed read run 1..3 bytes past the buffer), 8 and 12 accepted. Ground-truth
// 8-byte decode anchors the happy path.
TEST(TCAdversarialTimestamp, BinaryShortPrefixedFieldRejected) {
    for (std::size_t sz : {9u, 10u, 11u}) {
        std::vector<byte> buf(sz, byte{0x01});
        EXPECT_THROW(TypeConverter<qb::wall_time>::from_binary(buf), std::runtime_error) << "size " << sz;
    }
    EXPECT_NO_THROW(TypeConverter<qb::wall_time>::from_binary(std::vector<byte>(8, byte{0})));
    EXPECT_NO_THROW(TypeConverter<qb::wall_time>::from_binary(std::vector<byte>(12, byte{0})));
    // 8-byte ground truth still decodes to the known instant.
    EXPECT_EQ(qb::unix_micros(TypeConverter<qb::wall_time>::from_binary(hex_to_bytes(gt::temporal::ts_2024_03_15))),
              gt::temporal::ts_2024_03_15_unix_micros);
}

// ============================================================================
// SECTION 6 — DATE / TIME / TIMETZ from_binary truncated/oversized buffers; TIMETZ
// text offset variants. The civil converters DEGRADE to a default-constructed value
// on a short span (not a throw) — pin that, and prove no OOB on odd sizes.
// ============================================================================

TEST(TCAdversarialTemporal, ShortSpansDefaultNotThrow) {
    // 3 bytes is shorter than every fixed temporal field -> default value, no throw, no OOB.
    std::vector<byte> three(3, byte{0x7F});
    EXPECT_EQ(TypeConverter<qb::date>::from_binary(three), qb::date{});
    EXPECT_EQ(TypeConverter<qb::time_of_day>::from_binary(three), qb::time_of_day{});
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::from_binary(three), qb::time_of_day_tz{});
    EXPECT_EQ(TypeConverter<qb::calendar_interval>::from_binary(three), qb::calendar_interval{});

    // DATE accepts exactly 4 (value) or >= 8 (4-byte prefix + 4 value); 5..7 fall to default.
    for (std::size_t sz : {5u, 6u, 7u}) {
        std::vector<byte> b(sz, byte{0});
        EXPECT_EQ(TypeConverter<qb::date>::from_binary(b), qb::date{}) << "date size " << sz;
    }
    // TIME accepts 8 or >= 12; a 9..11-byte span is the default (no OOB read past offset 4).
    for (std::size_t sz : {9u, 10u, 11u}) {
        std::vector<byte> b(sz, byte{0});
        EXPECT_EQ(TypeConverter<qb::time_of_day>::from_binary(b), qb::time_of_day{}) << "time size " << sz;
    }
}

// DATE Unix-epoch / before-epoch decode via externally-known day counts (NOT a self
// round-trip): PG day 0 == 2000-01-01, day -10957 == 1970-01-01.
TEST(TCAdversarialTemporal, DateKnownDayCounts) {
    auto wire = [](std::int32_t pg_days) {
        std::int32_t be = qb::endian::to_big_endian(pg_days);
        return std::vector<byte>(reinterpret_cast<byte *>(&be), reinterpret_cast<byte *>(&be) + 4);
    };
    EXPECT_EQ(TypeConverter<qb::date>::from_binary(wire(0)).to_string(), "2000-01-01");
    EXPECT_EQ(TypeConverter<qb::date>::from_binary(wire(-10957)).to_string(), "1970-01-01");
    EXPECT_EQ(TypeConverter<qb::date>::from_binary(wire(-1)).to_string(), "1999-12-31");
    // ground truth 2024-03-15 == 8840 days since 2000.
    EXPECT_EQ(TypeConverter<qb::date>::from_binary(wire(8840)).to_string(), "2024-03-15");
}

TEST(TCAdversarialTemporal, TimetzTextOffsetForms) {
    // "+00", "Z" and "z" are all UTC; missing offset -> UTC (offset 0).
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::from_text("12:00:00+00").offset.count(), 0);
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::from_text("12:00:00Z").offset.count(), 0);
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::from_text("12:00:00z").offset.count(), 0);
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::from_text("12:00:00").offset.count(), 0);

    // "+02:00" / "-05" / "+05:30" / seconds-bearing "+02:00:00".
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::from_text("14:30:45.123456+02:00").offset.count(), 7200);
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::from_text("08:00:00-05").offset.count(), -18000);
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::from_text("06:00:00+05:30").offset.count(), (5 * 3600) + (30 * 60));
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::from_text("06:00:00+02:00:00").offset.count(), 7200);

    // The time component still parses with a fractional part present.
    auto z = TypeConverter<qb::time_of_day_tz>::from_text("14:30:45.123456+02:00");
    EXPECT_EQ(z.tod.since_midnight().count(), 52245123456LL);
}

// calendar_interval binary with the FULL field set (months + days + micros, including
// negatives) against the lossless decode — NOT folded to a single span. Ground-truth:
// '1 mon 2 days 03:04:05' send bytes from the golden header.
TEST(TCAdversarialTemporal, CalendarIntervalAllFieldsAndNegatives) {
    auto iv = TypeConverter<qb::calendar_interval>::from_binary(hex_to_bytes("00000002925553400000000200000001"));
    EXPECT_EQ(iv.months, 1);
    EXPECT_EQ(iv.days, 2);
    EXPECT_EQ(iv.micros.count(), 11045000000LL); // 03:04:05

    // Negative-days / negative-months wire (months=-1 => 0xFFFFFFFF, days=-2, micros=0).
    // Hand-built: int64 micros=0, int32 days=-2 (fffffffe), int32 months=-1 (ffffffff).
    auto neg = TypeConverter<qb::calendar_interval>::from_binary(hex_to_bytes("0000000000000000fffffffeffffffff"));
    EXPECT_EQ(neg.months, -1);
    EXPECT_EQ(neg.days, -2);
    EXPECT_EQ(neg.micros.count(), 0);

    // INTERVAL text decode is intentionally unsupported -> loud throw (never silent zero).
    EXPECT_THROW(TypeConverter<qb::calendar_interval>::from_text("1 mon 2 days 03:04:05"), std::runtime_error);
}

// ============================================================================
// SECTION 7 — UUID from_binary buffer-size edges (16 exact, < 20 throws, >= 20 prefixed)
// and from_text malformed.
// ============================================================================

TEST(TCAdversarialUuid, BinarySizeEdges) {
    const auto canon = qb::uuid::from_string("550e8400-e29b-41d4-a716-446655440000").value();

    // Exactly 16 raw bytes -> decode.
    EXPECT_EQ(TypeConverter<qb::uuid>::from_binary(hex_to_bytes("550e8400e29b41d4a716446655440000")), canon);

    // 17..19 bytes: neither 16 nor >= 20 -> "Buffer too small" throw (NOT an OOB read).
    for (std::size_t sz : {17u, 18u, 19u}) {
        std::vector<byte> b(sz, byte{0x11});
        EXPECT_THROW(TypeConverter<qb::uuid>::from_binary(b), std::runtime_error) << "size " << sz;
    }
    // 15 bytes -> throw.
    EXPECT_THROW(TypeConverter<qb::uuid>::from_binary(std::vector<byte>(15, byte{0x11})), std::runtime_error);

    // 20 bytes: 4-byte prefix + 16 raw -> reads from offset 4.
    EXPECT_EQ(TypeConverter<qb::uuid>::from_binary(hex_to_bytes("00000010550e8400e29b41d4a716446655440000")), canon);
    // 21 bytes (>= 20): still reads bytes [4..20), ignoring the 21st -> same UUID.
    EXPECT_EQ(TypeConverter<qb::uuid>::from_binary(hex_to_bytes("00000010550e8400e29b41d4a716446655440000ff")), canon);
}

TEST(TCAdversarialUuid, TextMalformedThrows) {
    // The live specialization throws std::runtime_error on any non-UUID text.
    EXPECT_THROW(TypeConverter<qb::uuid>::from_text(""), std::runtime_error);
    EXPECT_THROW(TypeConverter<qb::uuid>::from_text("550e8400-e29b-41d4-a716"), std::runtime_error);              // truncated
    EXPECT_THROW(TypeConverter<qb::uuid>::from_text("zzzzzzzz-e29b-41d4-a716-446655440000"), std::runtime_error); // non-hex
}

// ============================================================================
// SECTION 8 — std::optional value-vs-NULL framing (to_binary -1 sentinel; from_binary
// always decodes a value because SQL NULL is decided upstream).
// ============================================================================

TEST(TCAdversarialOptional, NullSentinelAndAllOnesValue) {
    // nullopt -> exactly the 4-byte 0xFFFFFFFF length sentinel, no payload.
    {
        std::vector<byte> buf;
        TypeConverter<std::optional<integer>>::to_binary(std::optional<integer>{}, buf);
        EXPECT_EQ(buf, hex_to_bytes("ffffffff"));
    }
    // present -> identical to the bare inner encoding (len 4 + value 0000002a).
    {
        std::vector<byte> buf;
        TypeConverter<std::optional<integer>>::to_binary(std::optional<integer>{42}, buf);
        EXPECT_EQ(strip_prefix(buf), hex_to_bytes("0000002a"));
    }
    // from_binary: a genuine int4 -1 value (0xFFFFFFFF) is a REAL -1, NOT a NULL marker
    // (NULL never reaches the converter — field::as decides it). This is the exact bug the
    // serde audit fixed: the old code sniffed for a -1 "NULL prefix" and dropped the value.
    auto neg = TypeConverter<std::optional<integer>>::from_binary(hex_to_bytes("ffffffff"));
    ASSERT_TRUE(neg.has_value());
    EXPECT_EQ(*neg, -1);

    // A genuine int8 all-ones value also decodes to -1, not nullopt.
    auto neg8 = TypeConverter<std::optional<bigint>>::from_binary(hex_to_bytes("ffffffffffffffff"));
    ASSERT_TRUE(neg8.has_value());
    EXPECT_EQ(*neg8, static_cast<bigint>(-1));

    // from_text: empty -> nullopt; non-empty delegates to inner from_text.
    EXPECT_FALSE(TypeConverter<std::optional<integer>>::from_text("").has_value());
    auto some = TypeConverter<std::optional<integer>>::from_text("7");
    ASSERT_TRUE(some.has_value());
    EXPECT_EQ(*some, 7);
}

// ============================================================================
// SECTION 9 — ARRAY decoder adversarial paths (NULL elements, non-finite element
// values, multi-dim flatten, bogus dims). Anchored to array_send ground truth and
// to known element values.
// ============================================================================

// text[] with a NULL element: the plain std::vector<std::string> path default-constructs
// the NULL slot (empty string), while the optional path yields nullopt. Both must agree on
// the surrounding values. Hand-built {"a", NULL, "c"}.
TEST(TCAdversarialArray, TextArrayWithNullElement) {
    // ndim=1, has-null=1, elem OID=text(25=0x19), dim size=3 lb=1, then a/NULL/c.
    std::string h;
    h += "00000001"; // ndim
    h += "00000001"; // has-null
    h += "00000019"; // text OID
    h += "00000003"; // size 3
    h += "00000001"; // lb 1
    h += "00000001"
         "61";       // "a"
    h += "ffffffff"; // NULL
    h += "00000001"
         "63"; // "c"
    auto plain = TypeConverter<std::vector<std::string>>::from_binary(hex_to_bytes(h));
    ASSERT_EQ(plain.size(), 3u);
    EXPECT_EQ(plain[0], "a");
    EXPECT_EQ(plain[1], ""); // NULL -> default-constructed empty string
    EXPECT_EQ(plain[2], "c");

    auto opt = decode_pg_array<std::optional<std::string>>(hex_to_bytes(h));
    ASSERT_EQ(opt.size(), 3u);
    ASSERT_TRUE(opt[0].has_value());
    EXPECT_EQ(*opt[0], "a");
    EXPECT_FALSE(opt[1].has_value()); // NULL -> nullopt (distinguishable from "")
    ASSERT_TRUE(opt[2].has_value());
    EXPECT_EQ(*opt[2], "c");
}

// float8[] carrying NaN / +Inf / -Inf element bit patterns must decode the non-finite
// values intact (not zero them). Hand-built {NaN, +Inf, -Inf}.
TEST(TCAdversarialArray, FloatArrayNonFiniteElements) {
    std::string h;
    h += "00000001"; // ndim
    h += "00000000"; // has-null
    h += "000002bd"; // float8 OID (701)
    h += "00000003"; // size 3
    h += "00000001"; // lb 1
    h += "00000008"
         "7ff8000000000000"; // NaN
    h += "00000008"
         "7ff0000000000000"; // +Inf
    h += "00000008"
         "fff0000000000000"; // -Inf
    auto v = TypeConverter<std::vector<double>>::from_binary(hex_to_bytes(h));
    ASSERT_EQ(v.size(), 3u);
    EXPECT_TRUE(std::isnan(v[0]));
    EXPECT_TRUE(std::isinf(v[1]) && v[1] > 0);
    EXPECT_TRUE(std::isinf(v[2]) && v[2] < 0);
}

// Nested (2-D) int4[2][3] flattens row-major to a 6-element vector. Anchored to the
// golden 2-D send bytes.
TEST(TCAdversarialArray, MultiDimFlattenGroundTruth) {
    auto v = TypeConverter<std::vector<integer>>::from_binary(hex_to_bytes(gt::array::int4_2d_2x3));
    EXPECT_EQ(v, (std::vector<integer>{1, 2, 3, 4, 5, 6}));
}

// Bogus/oversized dimension and an element length that overruns the buffer must degrade to
// empty/partial WITHOUT reading out of bounds (each targets a distinct guard branch).
TEST(TCAdversarialArray, MalformedDimsAndOverrunGuards) {
    using IV = std::vector<integer>;
    // dim size = INT32_MAX, count exceeds buffer -> empty (the `total > size` guard).
    EXPECT_TRUE(TypeConverter<IV>::from_binary(hex_to_bytes("0000000100000000000000177fffffff00000001")).empty());
    // dim size negative -> empty.
    EXPECT_TRUE(TypeConverter<IV>::from_binary(hex_to_bytes("000000010000000000000017ffffffff00000001")).empty());
    // claims 2 elements, only one present -> partial {10}, no OOB.
    EXPECT_EQ(TypeConverter<IV>::from_binary(hex_to_bytes("0000000100000000000000170000000200000001000000040000000a")), (IV{10}));
    // element length 0x10 (16) but only 4 value bytes follow -> stop before the overrun (empty).
    EXPECT_TRUE(TypeConverter<IV>::from_binary(hex_to_bytes("00000001000000000000001700000001000000010000001000000001")).empty());
    // a non-(-1) negative element length (here -2 = 0xfffffffe) -> break (empty).
    EXPECT_TRUE(TypeConverter<IV>::from_binary(hex_to_bytes("0000000100000000000000170000000100000001fffffffe")).empty());
}

// ============================================================================
// SECTION 10 — JSON / JSONB malformed binary value bytes.
// ============================================================================

TEST(TCAdversarialJson, JsonBinaryInvalidThrows) {
    // json from_binary parses the value bytes AS JSON text; invalid JSON -> throw.
    auto vb = [](std::string_view s) {
        return std::vector<byte>(s.data(), s.data() + s.size());
    };
    EXPECT_THROW(TypeConverter<qb::json>::from_binary(vb("{not json")), std::runtime_error);
    EXPECT_THROW(TypeConverter<qb::json>::from_binary(std::vector<byte>{}), std::runtime_error); // empty
    // A valid scalar payload decodes.
    EXPECT_EQ(TypeConverter<qb::json>::from_binary(vb("42")).get<int>(), 42);
}

TEST(TCAdversarialJson, JsonbVersionAndTruncationGuards) {
    // First byte must be version 1 (or a 4-byte varlena header whose byte[4] is 1).
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_binary(hex_to_bytes("0203")), std::runtime_error); // version 2
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_binary(hex_to_bytes("01")), std::runtime_error);   // < 2 bytes
    // version 1 but the JSON body is invalid -> parse throws.
    std::vector<byte> badbody;
    badbody.push_back(byte{1});
    for (char c : std::string("{bad"))
        badbody.push_back(static_cast<byte>(c));
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_binary(badbody), std::runtime_error);
    // version 1 + valid body decodes.
    std::vector<byte> ok;
    ok.push_back(byte{1});
    for (char c : std::string(R"({"k":1})"))
        ok.push_back(static_cast<byte>(c));
    EXPECT_EQ(TypeConverter<qb::jsonb>::from_binary(ok)["k"].get<int>(), 1);
}

// ============================================================================
// SECTION 11 — std::string from_binary verbatim (no phantom length-prefix strip),
// including leading NULs and high bytes. The regression the audit fixed.
// ============================================================================

TEST(TCAdversarialString, FromBinaryVerbatimIncludingLeadingNuls) {
    // Leading NULs preserved (NOT mistaken for a 4-byte length prefix and stripped).
    EXPECT_EQ(TypeConverter<std::string>::from_binary(hex_to_bytes("0000000568656c6c6f")), std::string("\0\0\0\x05hello", 9));
    // All-NUL value bytes -> a 4-char NUL string, not "".
    EXPECT_EQ(TypeConverter<std::string>::from_binary(hex_to_bytes("00000000")), std::string("\0\0\0\0", 4));
    // Embedded high bytes survive (no UTF-8 validation at this layer).
    EXPECT_EQ(TypeConverter<std::string>::from_binary(hex_to_bytes("ff00ff")), std::string("\xff\0\xff", 3));
    // Empty -> empty (not a throw).
    EXPECT_TRUE(TypeConverter<std::string>::from_binary(std::vector<byte>{}).empty());
}
