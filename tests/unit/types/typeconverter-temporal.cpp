/**
 * @file typeconverter-temporal.cpp
 * @brief Unit tests for the PostgreSQL temporal converters mapped onto the qb civil
 *        vocabulary: DATE (qb::date), TIME (qb::time_of_day), TIMETZ
 *        (qb::time_of_day_tz), INTERVAL (qb::calendar_interval and the lossy
 *        std::chrono::seconds convenience), and TIMESTAMP (qb::wall_time).
 *
 * Anchored to PostgreSQL `*_send()` ground truth (value bytes — the protocol layer
 * strips the per-field length prefix before from_binary sees them). Split out of the
 * legacy monolith `test-data-types.cpp` (temporal tests).
 *
 * Sign / epoch contracts (verified against type_converter.cpp):
 *  - DATE wire = int32 days since 2000-01-01; qb::date counts days since 1970.
 *  - TIME wire = int64 micros since midnight.
 *  - TIMETZ wire = int64 micros + int32 zone seconds WEST of UTC; qb::time_of_day_tz
 *    stores an east-positive offset (the decoder negates).
 *  - INTERVAL wire = int64 micros + int32 days + int32 months.
 *  - TIMESTAMP wire = int64 micros since 2000-01-01.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "../../shared/pg_wire_ground_truth.hpp"
#include "../pgsql.h"

using namespace qb::pg;
using namespace qb::pg::detail;
using qb::pg::test::hex_to_bytes;

// to_binary emits [4-byte length prefix][value]; the wire field is value-only.
static std::vector<byte>
strip_prefix(std::vector<byte> b) {
    b.erase(b.begin(), b.begin() + sizeof(integer));
    return b;
}

// ----------------------------------------------------------------------------
// get_oid() for the temporal converters
// ----------------------------------------------------------------------------

TEST(TypeConverterTemporalOid, KnownOids) {
    EXPECT_EQ(TypeConverter<qb::date>::get_oid(), 1082);
    EXPECT_EQ(TypeConverter<qb::time_of_day>::get_oid(), 1083);
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::get_oid(), 1266);
    EXPECT_EQ(TypeConverter<qb::calendar_interval>::get_oid(), 1186);
    EXPECT_EQ(TypeConverter<std::chrono::seconds>::get_oid(), 1186); // lossy INTERVAL convenience
}

// ----------------------------------------------------------------------------
// DATE / TIME / TIMETZ binary against PostgreSQL ground truth
// ----------------------------------------------------------------------------

// Real *_send() bytes decode to the actual civil value — NOT the 2000-01-01 epoch
// base (the unprefixed-field regression).
TEST(TypeConverterTemporalBinary, CivilTypesAgainstPostgresGroundTruth) {
    // DATE 2024-03-15 = 8840 days since 2000 = 19797 days since 1970.
    auto d = TypeConverter<qb::date>::from_binary(hex_to_bytes("00002288"));
    EXPECT_EQ(d.to_string(), "2024-03-15");
    EXPECT_EQ(d.days_since_epoch(), 19797);

    // TIME 14:30:45.123456 = 52245123456 micros since midnight (timetz_send w/o zone).
    auto t = TypeConverter<qb::time_of_day>::from_binary(hex_to_bytes("0000000c2a0d5180"));
    EXPECT_EQ(t.to_string(), "14:30:45.123456");

    // TIMETZ 14:30:45+02:00 — wire zone is west-positive (-7200), decoder negates to
    // east-positive (+7200).
    auto z = TypeConverter<qb::time_of_day_tz>::from_binary(hex_to_bytes("0000000c2a0b6f40ffffe3e0"));
    EXPECT_EQ(z.tod.since_midnight().count(), 52245000000LL);
    EXPECT_EQ(z.offset.count(), 7200);
    EXPECT_EQ(z.to_string(), "14:30:45+02:00");

    // TIMETZ 08:00:00-05:00.
    auto z2 = TypeConverter<qb::time_of_day_tz>::from_binary(hex_to_bytes("00000006b49d200000004650"));
    EXPECT_EQ(z2.offset.count(), -18000);
    EXPECT_EQ(z2.to_string(), "08:00:00-05:00");
}

// Regression: DATE/TIME/TIMETZ from_binary must decode the unprefixed wire field,
// not read 4 bytes past it. We reproduce the real wire buffer by serializing then
// dropping the length prefix; the legacy prefixed shape must decode identically.
TEST(TypeConverterTemporalBinary, UnprefixedFieldDecode) {
    {
        qb::date          d_in = qb::date::parse("2024-03-15").value();
        std::vector<byte> prefixed;
        TypeConverter<qb::date>::to_binary(d_in, prefixed);
        ASSERT_EQ(prefixed.size(), 8u); // 4 prefix + 4 value
        std::vector<byte> wire = strip_prefix(prefixed);
        ASSERT_EQ(wire.size(), 4u);
        qb::date d_wire = TypeConverter<qb::date>::from_binary(wire);
        EXPECT_EQ(d_wire, d_in);
        EXPECT_EQ(d_wire.to_string(), "2024-03-15"); // not the 2000-01-01 epoch base
        EXPECT_EQ(TypeConverter<qb::date>::from_binary(prefixed), d_in);
    }
    {
        qb::time_of_day   t_in = qb::time_of_day::from_hms(14, 30, 45, 123456);
        std::vector<byte> prefixed;
        TypeConverter<qb::time_of_day>::to_binary(t_in, prefixed);
        ASSERT_EQ(prefixed.size(), 12u); // 4 prefix + 8 value
        std::vector<byte> wire = strip_prefix(prefixed);
        ASSERT_EQ(wire.size(), 8u);
        qb::time_of_day t_wire = TypeConverter<qb::time_of_day>::from_binary(wire);
        EXPECT_EQ(t_wire, t_in);
        EXPECT_EQ(t_wire.to_string(), "14:30:45.123456"); // not 00:00:00
        EXPECT_EQ(TypeConverter<qb::time_of_day>::from_binary(prefixed), t_in);
    }
    {
        qb::time_of_day_tz z_in = qb::time_of_day_tz::from_hms_offset(14, 30, 45, 0, 7200);
        std::vector<byte>  prefixed;
        TypeConverter<qb::time_of_day_tz>::to_binary(z_in, prefixed);
        ASSERT_EQ(prefixed.size(), 16u); // 4 prefix + 12 value
        std::vector<byte> wire = strip_prefix(prefixed);
        ASSERT_EQ(wire.size(), 12u);
        qb::time_of_day_tz z_wire = TypeConverter<qb::time_of_day_tz>::from_binary(wire);
        EXPECT_EQ(z_wire, z_in);
        EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::from_binary(prefixed), z_in);
    }
}

// to_binary -> from_binary round-trips for the civil types (length-prefixed form).
TEST(TypeConverterTemporalBinary, CivilTypesRoundTrip) {
    auto rt = [](auto v) {
        using T = decltype(v);
        std::vector<byte> b;
        TypeConverter<T>::to_binary(v, b);
        return TypeConverter<T>::from_binary(b);
    };
    EXPECT_EQ(rt(qb::date::from_ymd(1999, 12, 31)), qb::date::from_ymd(1999, 12, 31));
    EXPECT_EQ(rt(qb::time_of_day::from_hms(23, 59, 59, 999999)), qb::time_of_day::from_hms(23, 59, 59, 999999));
    EXPECT_EQ(rt(qb::time_of_day_tz::from_hms_offset(8, 0, 0, 0, -18000)), qb::time_of_day_tz::from_hms_offset(8, 0, 0, 0, -18000));
    EXPECT_EQ(rt(qb::calendar_interval(13, 5, std::chrono::microseconds{123456})),
              qb::calendar_interval(13, 5, std::chrono::microseconds{123456}));
}

// Binary length prefixes are exactly the documented sizes.
TEST(TypeConverterTemporalBinary, WirePrefixLengths) {
    std::vector<byte> buf;
    TypeConverter<qb::time_of_day>::to_binary(qb::time_of_day::from_hms(14, 30, 45, 123456), buf);
    ASSERT_EQ(buf.size(), 12u);
    integer len;
    std::memcpy(&len, buf.data(), sizeof(integer));
    EXPECT_EQ(ntohl(len), 8); // TIME value is 8 bytes

    std::vector<byte> tzbuf;
    TypeConverter<qb::time_of_day_tz>::to_binary(qb::time_of_day_tz::from_hms_offset(18, 0, 0, 0, 7200), tzbuf);
    EXPECT_EQ(tzbuf.size(), 16u); // 4 length + 8 time + 4 zone
}

// ----------------------------------------------------------------------------
// calendar_interval / std::chrono::seconds binary
// ----------------------------------------------------------------------------

// calendar_interval keeps months/days/micros separate (lossless) — ground truth.
TEST(TypeConverterTemporalBinary, CalendarIntervalGroundTruth) {
    auto iv = TypeConverter<qb::calendar_interval>::from_binary(hex_to_bytes("00000002925553400000000200000001"));
    EXPECT_EQ(iv.months, 1);
    EXPECT_EQ(iv.days, 2);
    EXPECT_EQ(iv.micros.count(), 11045000000LL);        // 03:04:05
    EXPECT_EQ(iv.to_micros().count(), 2775845000000LL); // == PG EXTRACT(EPOCH) * 1e6
}

// Lossy std::chrono::seconds INTERVAL folds days+months into EXTRACT(EPOCH) seconds
// (24h day, 30-day residual month, 365.25-day year), including negatives.
TEST(TypeConverterTemporalBinary, SecondsIntervalGroundTruth) {
    using secs = std::chrono::seconds;
    struct C {
        const char *hex;
        long long   expect;
    };
    const C cases[] = {
        {"00000000000000000000000100000000", 86400},    // 1 day
        {"00000000000000000000000000000001", 2592000},  // 1 month (30 days)
        {"0000000218711a000000000000000000", 9000},     // 2h30m (pure time)
        {"00000002925553400000000200000001", 2775845},  // 1 mon 2 days 03:04:05
        {"0000000000000000ffffffff00000000", -86400},   // -1 day
        {"0000000000000000000000000000000c", 31557600}, // 12 months = 1 year (365.25d)
    };
    for (const auto &c : cases) {
        auto d = TypeConverter<secs>::from_binary(hex_to_bytes(c.hex));
        EXPECT_EQ(d.count(), c.expect) << "hex=" << c.hex;
    }
    // Pure-time duration round-trips (to_binary sets days=months=0).
    std::vector<byte> buf;
    TypeConverter<secs>::to_binary(secs{90061}, buf);
    EXPECT_EQ(TypeConverter<secs>::from_binary(strip_prefix(buf)).count(), 90061);
}

// ----------------------------------------------------------------------------
// TIMESTAMP (qb::wall_time) binary + text
// ----------------------------------------------------------------------------

// Decode the actual instant, not just the int64 read.
TEST(TypeConverterTemporalBinary, WallTimeGroundTruth) {
    // timestamptz '2024-03-15 14:30:45.123456+00' = 763828245123456 us since 2000
    // = 1710513045123456 us since the Unix epoch.
    auto t = TypeConverter<qb::wall_time>::from_binary(hex_to_bytes("0002b6b29f385180"));
    EXPECT_EQ(qb::unix_micros(t), 1710513045123456LL);
}

// Regression: from_binary must reject a 9..11 byte field (heap over-read guard); 8
// (exact) and 12 (legacy prefixed) remain valid.
TEST(TypeConverterTemporalBinary, WallTimeRejectsShortPrefixedBuffer) {
    for (size_t sz : {9u, 10u, 11u}) {
        std::vector<byte> buf(sz, byte{0x01});
        ASSERT_THROW(TypeConverter<qb::wall_time>::from_binary(buf), std::runtime_error)
            << "size " << sz << " must be rejected, not read out of bounds";
    }
    ASSERT_NO_THROW(TypeConverter<qb::wall_time>::from_binary(std::vector<byte>(8, byte{0})));
    ASSERT_NO_THROW(TypeConverter<qb::wall_time>::from_binary(std::vector<byte>(12, byte{0})));
}

// ----------------------------------------------------------------------------
// text-format decoders for the civil/temporal types
// ----------------------------------------------------------------------------

// A simple (text-protocol) query returns these columns as text, so from_text must
// decode — not return a silent default. TIMETZ parses; INTERVAL text is unsupported
// and must fail loudly.
TEST(TypeConverterTemporalText, FromTextBehaviors) {
    auto z = TypeConverter<qb::time_of_day_tz>::from_text("14:30:45.123456+02:00");
    EXPECT_EQ(z.tod.to_string(), "14:30:45.123456");
    EXPECT_EQ(z.offset.count(), 7200);
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::to_text(z), "14:30:45.123456+02:00");

    // Negative offset written without minutes on the wire ("-05").
    auto z2 = TypeConverter<qb::time_of_day_tz>::from_text("08:00:00-05");
    EXPECT_EQ(z2.offset.count(), -5 * 3600);
    EXPECT_EQ(z2.tod.to_string(), "08:00:00");

    // INTERVAL text decode is unsupported -> loud throw, never a silent zero interval.
    EXPECT_THROW(TypeConverter<qb::calendar_interval>::from_text("1 mon 2 days 03:04:05"), std::runtime_error);
}

// TIME / TIMETZ text round-trips through to_text.
TEST(TypeConverterTemporalText, TimeRoundTrips) {
    qb::time_of_day t1   = qb::time_of_day::from_hms(14, 30, 45, 123456);
    std::string     text = TypeConverter<qb::time_of_day>::to_text(t1);
    EXPECT_EQ(TypeConverter<qb::time_of_day>::from_text(text), t1);

    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::to_text(qb::time_of_day_tz::from_hms_offset(18, 0, 0, 0, 7200)), "18:00:00+02:00");
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::to_text(qb::time_of_day_tz::from_hms_offset(14, 30, 45, 0, (5 * 3600) + (30 * 60))),
              "14:30:45+05:30");
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::to_text(qb::time_of_day_tz::from_hms_offset(8, 0, 0, 0, -8 * 3600)), "08:00:00-08:00");
}

// DATE and INTERVAL inline to_text / from_text (the converter delegates to
// qb::date::to_string / qb::date::parse and qb::calendar_interval::to_string).
TEST(TypeConverterTemporalText, DateAndIntervalToFromText) {
    // DATE to_text == qb::date::to_string; from_text parses ISO, bad input -> default.
    qb::date d = qb::date::from_ymd(2024, 3, 15);
    EXPECT_EQ(TypeConverter<qb::date>::to_text(d), "2024-03-15");
    EXPECT_EQ(TypeConverter<qb::date>::from_text("2024-03-15"), d);
    // Unparseable text -> the value_or(qb::date{}) fallback (epoch default), not a throw.
    EXPECT_EQ(TypeConverter<qb::date>::from_text("not-a-date"), qb::date{});

    // calendar_interval to_text delegates to qb::calendar_interval::to_string.
    qb::calendar_interval iv(1, 2, std::chrono::microseconds{11045000000LL}); // 1 mon 2 day 03:04:05
    const std::string     txt = TypeConverter<qb::calendar_interval>::to_text(iv);
    EXPECT_EQ(txt, iv.to_string());
    EXPECT_FALSE(txt.empty());
}

// std::chrono::seconds from_text: leading integer parsed, garbage -> zero.
TEST(TypeConverterTemporalText, SecondsFromText) {
    using secs = std::chrono::seconds;
    EXPECT_EQ(TypeConverter<secs>::from_text("90 seconds"), secs{90});
    EXPECT_EQ(TypeConverter<secs>::from_text("-5 seconds"), secs{-5});
    EXPECT_EQ(TypeConverter<secs>::from_text("garbage"), secs::zero());
    EXPECT_EQ(TypeConverter<secs>::from_text(""), secs::zero());
    EXPECT_EQ(TypeConverter<secs>::to_text(secs{90}), "90 seconds");
}

// Timestamp from_text: micro padding (".5" -> 500000), tz offset, malformed -> throw.
TEST(TypeConverterTemporalText, TimestampMicrosAndTz) {
    auto t1 = TypeConverter<qb::wall_time>::from_text("2024-01-01 00:00:00.5");
    EXPECT_EQ(qb::unix_micros(t1) % 1000000, 500000);

    // "+02" trailing zone: 12:00:00+02 == 10:00:00Z, so noz - withtz == 7200s.
    auto withtz = TypeConverter<qb::wall_time>::from_text("2024-01-01 12:00:00+02");
    auto noz    = TypeConverter<qb::wall_time>::from_text("2024-01-01 12:00:00");
    EXPECT_EQ(qb::unix_micros(noz) - qb::unix_micros(withtz), 7200LL * 1000000LL);

    EXPECT_THROW(TypeConverter<qb::wall_time>::from_text("bad ts"), std::runtime_error);
    EXPECT_THROW(TypeConverter<qb::wall_time>::from_text(""), std::runtime_error);
}

// DATE far past, BCE, and the pre-1970 boundary round-trip (pure-integer UTC
// arithmetic — identical on all platforms, including Windows CRT divergence).
TEST(TypeConverterTemporalText, DateAndTimestampAcrossEpochBoundary) {
    for (const char *s : {"1969-12-31", "1900-01-01", "1858-11-17", "0001-01-01", "2000-01-01", "1970-01-01", "2099-12-31"}) {
        qb::date d = qb::date::parse(s).value();
        EXPECT_EQ(d.to_string(), s) << "DATE round-trip failed for " << s;
    }

    qb::wall_time ts;
    ASSERT_NO_THROW(ts = TypeConverter<qb::wall_time>::from_text("1969-07-20 20:17:40.000000"));
    EXPECT_NE(TypeConverter<qb::wall_time>::to_text(ts).find("1969-07-20 20:17:40"), std::string::npos);

    // A sub-second pre-1970 instant must borrow a second, not truncate toward zero.
    qb::wall_time ts2 = TypeConverter<qb::wall_time>::from_text("1955-11-05 06:15:00.500000");
    EXPECT_NE(TypeConverter<qb::wall_time>::to_text(ts2).find("1955-11-05 06:15:00.5"), std::string::npos);
}

// ----------------------------------------------------------------------------
// short/malformed span behavior (precise: civil types default, lossy interval throws)
// ----------------------------------------------------------------------------

TEST(TypeConverterTemporalBinary, MalformedShortSpanBehavior) {
    std::vector<byte> three(3, static_cast<byte>(0x00));

    EXPECT_EQ(TypeConverter<qb::date>::from_binary(three), qb::date{});
    EXPECT_EQ(TypeConverter<qb::time_of_day>::from_binary(three), qb::time_of_day{});
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::from_binary(three), qb::time_of_day_tz{});
    EXPECT_EQ(TypeConverter<qb::calendar_interval>::from_binary(three), qb::calendar_interval{});

    // Contrast: the lossy std::chrono::seconds interval converter throws on a short
    // span (neither 16 nor >= 20 bytes).
    EXPECT_THROW(TypeConverter<std::chrono::seconds>::from_binary(three), std::runtime_error);
    // wall_time (TIMESTAMP) likewise requires >= 8 bytes and rejects a short span.
    EXPECT_THROW(TypeConverter<qb::wall_time>::from_binary(three), std::runtime_error);
}
