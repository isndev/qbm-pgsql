/**
 * @file pg_wire_ground_truth.hpp
 * @brief Golden PostgreSQL wire-format byte vectors (server *_send() output).
 *
 * Every literal here is the EXACT value-bytes payload that PostgreSQL emits from
 * its `*_send()` functions (numeric_send / array_send / date_send / timetz_send /
 * interval_send / timestamp_send / ...), captured with the per-field length prefix
 * already stripped — i.e. the precise shape the protocol layer hands to
 * `TypeConverter<T>::from_binary`. Anchoring the codec unit tests to these external
 * literals (instead of self-emitted `to_binary` buffers) is what makes the decode
 * tests ground-truth rather than self-round-trip: a shared bug in the encoder can
 * no longer hide a matching bug in the decoder.
 *
 * `hex_to_bytes` is the single de-duplicated hex decoder previously copy-pasted in
 * test-data-types.cpp / test-param-unserializer.cpp / test-param-serializer.cpp /
 * test-param-parsing.cpp.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "../pgsql.h"

namespace qb::pg::test {

/// PostgreSQL wire byte alias (== qb::pg::byte == char).
using wire_byte = qb::pg::byte;

/**
 * @brief Decode a contiguous hex string ("04d2") into a wire byte buffer.
 *
 * Whitespace-free, lowercase or uppercase. An odd trailing nibble is ignored.
 */
[[nodiscard]] inline std::vector<wire_byte>
hex_to_bytes(std::string_view h) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        return (c | 0x20) - 'a' + 10;
    };
    std::vector<wire_byte> b;
    b.reserve(h.size() / 2);
    for (std::size_t i = 0; i + 1 < h.size(); i += 2)
        b.push_back(static_cast<wire_byte>((nib(h[i]) << 4) | nib(h[i + 1])));
    return b;
}

// ===========================================================================
// Golden NUMERIC (numeric_send) — value bytes, no length prefix.
// Layout: int16 ndigits, int16 weight, uint16 sign, uint16 dscale, base-10000 limbs.
// sign: 0x0000 +, 0x4000 -, 0xC000 NaN, 0xD000 +Inf, 0xF000 -Inf.
// ===========================================================================
namespace gt::numeric {

struct Case {
    const char *hex;    ///< numeric_send value bytes
    const char *expect; ///< canonical numeric_out text
};

/// Finite NUMERIC values captured from a live server.
inline constexpr Case finite[] = {
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

/// Special sign-word NUMERIC values (header only, ndigits == 0).
inline constexpr Case specials[] = {
    {"00000000c0000000", "NaN"},
    {"00000000d0000000", "Infinity"},
    {"00000000f0000000", "-Infinity"},
};

} // namespace gt::numeric

// ===========================================================================
// Golden ARRAY (array_send) — value bytes, no length prefix.
// Layout: int32 ndim, int32 has-null, int32 elem OID, per-dim{int32 size,int32 lb},
// then per element { int32 len (-1 = NULL), value bytes }.
// ===========================================================================
namespace gt::array {

// int4[] {10,20,30,40}
inline constexpr const char *int4_10_20_30_40 =
    "0000000100000000000000170000000400000001"
    "000000040000000a0000000400000014000000040000001e0000000400000028";
// text[] {apple,banana}
inline constexpr const char *text_apple_banana =
    "0000000100000000000000190000000200000001000000056170706c650000000662616e616e61";
// int4[] {1,NULL,3} — NULL element decodes to default-constructed 0
inline constexpr const char *int4_1_null_3 =
    "00000001000000010000001700000003000000010000000400000001ffffffff0000000400000003";
// empty int4[]
inline constexpr const char *int4_empty = "000000000000000000000017";
// int8[] {1,2,3}
inline constexpr const char *int8_1_2_3 =
    "00000001000000000000001400000003000000010000000800000000000000010000"
    "00080000000000000002000000080000000000000003";
// int2[] {7,8}
inline constexpr const char *int2_7_8 = "0000000100000000000000150000000200000001000000020007000000020008";
// float8[] {1.5,2.5}
inline constexpr const char *float8_1_5_2_5 =
    "0000000100000000000002bd0000000200000001000000083ff80000000000000000"
    "00084004000000000000";
// float4[] {1.5,-2.5}
inline constexpr const char *float4_1_5_n2_5 = "0000000100000000000002bc0000000200000001000000043fc0000000000004c0200000";
// bool[] {true,false,true}
inline constexpr const char *bool_t_f_t = "0000000100000000000000100000000300000001000000010100000001000000000101";
// 2-D int4[][] {{1,2,3},{4,5,6}} — ndim=2, dims 2x3, row-major. decode_pg_array
// flattens to {1,2,3,4,5,6}.
inline constexpr const char *int4_2d_2x3 =
    "0000000200000000000000170000000200000001000000030000000100000004"
    "0000000100000004000000020000000400000003000000040000000400000004000000050000000400000006";

} // namespace gt::array

// ===========================================================================
// Golden INTERVAL (interval_send) — value bytes, no length prefix.
// Layout: int64 micros, int32 days, int32 months. expect = EXTRACT(EPOCH) seconds
// (24h day, 30-day residual month, 365.25-day whole year).
// ===========================================================================
namespace gt::interval {

struct Case {
    const char *hex;
    long long   expect_seconds;
};

inline constexpr Case cases[] = {
    {"00000000000000000000000100000000", 86400},    // 1 day
    {"00000000000000000000000000000001", 2592000},  // 1 month (30 days)
    {"0000000218711a000000000000000000", 9000},     // 2h30m (pure time)
    {"00000002925553400000000200000001", 2775845},  // 1 mon 2 days 03:04:05
    {"0000000000000000ffffffff00000000", -86400},   // -1 day
    {"0000000000000000000000000000000c", 31557600}, // 12 months = 1 year (365.25d)
};

} // namespace gt::interval

// ===========================================================================
// Golden TIMESTAMP / DATE / TIME / TIMETZ — value bytes, no length prefix.
// ===========================================================================
namespace gt::temporal {

// timestamptz '2024-03-15 14:30:45.123456+00' => 1710513045123456 us since Unix epoch.
inline constexpr const char *ts_2024_03_15 = "0002b6b29f385180";
inline constexpr long long   ts_2024_03_15_unix_micros = 1710513045123456LL;
// The RAW big-endian int8 those bytes carry is micros since the PostgreSQL epoch
// (2000-01-01 UTC), i.e. unix_micros - 946684800000000. read_bigint() returns this raw
// value (the unix conversion happens one layer up, in the timestamptz TypeConverter).
inline constexpr long long   ts_2024_03_15_pg_micros = 763828245123456LL;

// date '2024-03-15' => 8840 days since 2000-01-01 (19797 since Unix epoch).
inline constexpr const char *date_2024_03_15 = "00002288";

// time '14:30:45.123456' => 52245123456 us since midnight.
inline constexpr const char *time_14_30_45 = "0000000c2a0d5180";

// timetz '14:30:45+02:00' (wire zone is WEST-positive: -7200 => 0xffffe3e0).
inline constexpr const char *timetz_14_30_45_p02 = "0000000c2a0b6f40ffffe3e0";
// timetz '08:00:00-05:00' (wire zone +18000 => 0x00004650).
inline constexpr const char *timetz_08_00_00_n05 = "00000006b49d200000004650";

} // namespace gt::temporal

} // namespace qb::pg::test
