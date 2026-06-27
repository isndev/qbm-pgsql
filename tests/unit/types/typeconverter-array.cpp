/**
 * @file typeconverter-array.cpp
 * @brief Unit tests for the one-dimensional PostgreSQL ARRAY binary decoder
 *        (decode_pg_array / TypeConverter<std::vector<T>>) and the bytea
 *        std::vector<std::byte> converter.
 *
 * Anchored to PostgreSQL `array_send()` ground truth (value bytes, no length prefix).
 * Layout, all big-endian: int32 ndim, int32 has-null flags, int32 element OID, then
 * per dimension {int32 size, int32 lower-bound}, then per element {int32 length
 * (-1 = NULL), value}. decode_pg_array flattens multi-dimensional arrays into one
 * flat vector. Split out of the legacy monolith `test-data-types.cpp` (array tests).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <cstdint>
#include <cstring>
#include <optional>
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
// get_oid() — the registered array OIDs and bytea
// ----------------------------------------------------------------------------

TEST(TypeConverterArrayOid, KnownArrayOids) {
    EXPECT_EQ(TypeConverter<std::vector<bool>>::get_oid(), 1000);
    EXPECT_EQ(TypeConverter<std::vector<smallint>>::get_oid(), 1005);
    EXPECT_EQ(TypeConverter<std::vector<integer>>::get_oid(), 1007);
    EXPECT_EQ(TypeConverter<std::vector<bigint>>::get_oid(), 1016);
    EXPECT_EQ(TypeConverter<std::vector<float>>::get_oid(), 1021);
    EXPECT_EQ(TypeConverter<std::vector<double>>::get_oid(), 1022);
    EXPECT_EQ(TypeConverter<std::vector<std::string>>::get_oid(), 1009);
    // bytea (std::vector<std::byte>) is NOT an array — it maps to bytea (17).
    EXPECT_EQ(TypeConverter<std::vector<std::byte>>::get_oid(), static_cast<integer>(oid::bytea));
}

// ----------------------------------------------------------------------------
// int4[] / text[] against PostgreSQL ground truth
// ----------------------------------------------------------------------------

TEST(TypeConverterArrayBinary, IntAndTextAgainstPostgresGroundTruth) {
    // int4[] {10,20,30,40}
    auto ints = TypeConverter<std::vector<integer>>::from_binary(
        hex_to_bytes("0000000100000000000000170000000400000001"
                     "000000040000000a0000000400000014000000040000001e0000000400000028"));
    EXPECT_EQ(ints, (std::vector<integer>{10, 20, 30, 40}));

    // text[] {apple,banana}
    auto txt = TypeConverter<std::vector<std::string>>::from_binary(
        hex_to_bytes("0000000100000000000000190000000200000001000000056170706c650000000662616e616e61"));
    EXPECT_EQ(txt, (std::vector<std::string>{"apple", "banana"}));

    // empty int4[]
    auto empty = TypeConverter<std::vector<integer>>::from_binary(hex_to_bytes("000000000000000000000017"));
    EXPECT_TRUE(empty.empty());
}

// Scalar element types (bigint/smallint/double/float/bool) against ground truth.
TEST(TypeConverterArrayBinary, ScalarElementTypesAgainstPostgresGroundTruth) {
    EXPECT_EQ(
        TypeConverter<std::vector<bigint>>::from_binary(
            hex_to_bytes("00000001000000000000001400000003000000010000000800000000000000010000"
                         "00080000000000000002000000080000000000000003")),
        (std::vector<bigint>{1, 2, 3}));

    EXPECT_EQ(
        TypeConverter<std::vector<smallint>>::from_binary(
            hex_to_bytes("0000000100000000000000150000000200000001000000020007000000020008")),
        (std::vector<smallint>{7, 8}));

    EXPECT_EQ(
        TypeConverter<std::vector<double>>::from_binary(
            hex_to_bytes("0000000100000000000002bd0000000200000001000000083ff80000000000000000"
                         "00084004000000000000")),
        (std::vector<double>{1.5, 2.5}));

    EXPECT_EQ(
        TypeConverter<std::vector<float>>::from_binary(
            hex_to_bytes("0000000100000000000002bc0000000200000001000000043fc0000000000004c0200000")),
        (std::vector<float>{1.5f, -2.5f}));

    EXPECT_EQ(
        TypeConverter<std::vector<bool>>::from_binary(
            hex_to_bytes("0000000100000000000000100000000300000001000000010100000001000000000101")),
        (std::vector<bool>{true, false, true}));
}

// decode_pg_array's bounds guards must degrade gracefully (empty / partial result) on a
// malformed or truncated binary array header — never read out of bounds. Each case targets
// one guard branch in decode_pg_array.
TEST(TypeConverterArrayBinary, MalformedBuffersDecodeGracefullyWithoutOob) {
    using IV = std::vector<integer>;
    // header shorter than the 12-byte ndim+flags+elem_oid prefix -> empty.
    EXPECT_TRUE(TypeConverter<IV>::from_binary(hex_to_bytes("00000001")).empty());
    // ndim < 0 -> empty (the zero-dimensional / negative guard).
    EXPECT_TRUE(TypeConverter<IV>::from_binary(hex_to_bytes("ffffffff0000000000000017")).empty());
    // ndim == 1 but no room for the 8-byte dim header (off+8 > size) -> empty.
    EXPECT_TRUE(TypeConverter<IV>::from_binary(hex_to_bytes("000000010000000000000017")).empty());
    // a negative dimension size (dim_size = 0xffffffff, lower_bound = 1) -> empty.
    EXPECT_TRUE(TypeConverter<IV>::from_binary(hex_to_bytes("000000010000000000000017ffffffff00000001")).empty());
    // a bogus huge dimension (dim_size = 0x7fffffff) whose count exceeds the buffer -> empty.
    EXPECT_TRUE(TypeConverter<IV>::from_binary(hex_to_bytes("0000000100000000000000177fffffff00000001")).empty());
    // header claims 3 elements but the buffer holds only one (10) -> partial {10}, no OOB.
    EXPECT_EQ(TypeConverter<IV>::from_binary(
                  hex_to_bytes("0000000100000000000000170000000300000001000000040000000a")),
              (IV{10}));
    // a negative element length that is NOT the -1 NULL sentinel (here -2) -> break (empty).
    // header: ndim=1, flags, elem_oid, dim_size=1, lower_bound, then elem_len=0xfffffffe.
    EXPECT_TRUE(
        TypeConverter<IV>::from_binary(hex_to_bytes("0000000100000000000000170000000100000001fffffffe")).empty());
}

// ----------------------------------------------------------------------------
// NULL elements: the plain-element path defaults; the optional-element path nullopts
// ----------------------------------------------------------------------------

// Plain int4[] {1,NULL,3}: a NULL element decodes to the default-constructed 0
// (the std::vector<integer> path cannot represent SQL NULL).
TEST(TypeConverterArrayBinary, NullElementDefaultsForPlainVector) {
    auto withnull = TypeConverter<std::vector<integer>>::from_binary(
        hex_to_bytes("00000001000000010000001700000003000000010000000400000001ffffffff0000000400000003"));
    EXPECT_EQ(withnull, (std::vector<integer>{1, 0, 3}));
}

// ADD: optional-element array NULL decode — the value-level NULL path that the plain
// vector cannot express. A -1 element length yields std::nullopt; a real value (incl.
// a genuine int4 -1 == 0xFFFFFFFF value, which is NOT a NULL marker) decodes.
TEST(TypeConverterArrayBinary, OptionalElementNullDecode) {
    // int4[] {1,NULL,3} via decode_pg_array<std::optional<integer>>.
    auto opt = decode_pg_array<std::optional<integer>>(
        hex_to_bytes("00000001000000010000001700000003000000010000000400000001ffffffff0000000400000003"));
    ASSERT_EQ(opt.size(), 3u);
    ASSERT_TRUE(opt[0].has_value());
    EXPECT_EQ(*opt[0], 1);
    EXPECT_FALSE(opt[1].has_value()); // SQL NULL -> nullopt (NOT 0)
    ASSERT_TRUE(opt[2].has_value());
    EXPECT_EQ(*opt[2], 3);

    // A genuine int4 -1 value (elem length 4, bytes 0xFFFFFFFF) is a real -1, NOT a
    // NULL marker (NULL is encoded by elem length == -1, decoded before the value).
    auto withMinusOne = decode_pg_array<std::optional<integer>>(
        hex_to_bytes("0000000100000000000000170000000200000001"
                     "00000004ffffffff0000000400000007"));
    ASSERT_EQ(withMinusOne.size(), 2u);
    ASSERT_TRUE(withMinusOne[0].has_value());
    EXPECT_EQ(*withMinusOne[0], -1);
    ASSERT_TRUE(withMinusOne[1].has_value());
    EXPECT_EQ(*withMinusOne[1], 7);
}

// ----------------------------------------------------------------------------
// ADD: multi-dimensional (ndim > 1) arrays flatten into one vector
// ----------------------------------------------------------------------------

// 2-D int4[2][2] = {{1,2},{3,4}}: ndim=2, two dimensions each {size=2,lb=1}, then 4
// elements row-major. decode_pg_array flattens to {1,2,3,4}.
TEST(TypeConverterArrayBinary, MultiDimensionalFlattens) {
    // Build the buffer field-by-field to avoid hand-counting a long hex literal.
    std::string h;
    h += "00000002"; // ndim = 2
    h += "00000000"; // has-null = 0
    h += "00000017"; // element OID = int4 (23)
    h += "00000002"; // dim0 size = 2
    h += "00000001"; // dim0 lower bound = 1
    h += "00000002"; // dim1 size = 2
    h += "00000001"; // dim1 lower bound = 1
    h += "00000004" "00000001"; // elem len 4, value 1
    h += "00000004" "00000002"; // 2
    h += "00000004" "00000003"; // 3
    h += "00000004" "00000004"; // 4
    auto twoD = TypeConverter<std::vector<integer>>::from_binary(hex_to_bytes(h));
    EXPECT_EQ(twoD, (std::vector<integer>{1, 2, 3, 4}));
}

// ----------------------------------------------------------------------------
// round-trip through encode (mirrors ParamSerializer::add_vector) + decode
// ----------------------------------------------------------------------------

TEST(TypeConverterArrayBinary, RoundTripThroughEncode) {
    std::vector<byte> buf;
    TypeConverter<std::vector<integer>>::to_binary(std::vector<integer>{7, -3, 100000}, buf);
    // strip the 4-byte length prefix that to_binary writes
    std::vector<byte> body(buf.begin() + 4, buf.end());
    EXPECT_EQ(TypeConverter<std::vector<integer>>::from_binary(body), (std::vector<integer>{7, -3, 100000}));
}

// ----------------------------------------------------------------------------
// decode_pg_array guard paths (crafted buffers must never read out of bounds)
// ----------------------------------------------------------------------------

TEST(TypeConverterArrayBinary, DecodeGuardPaths) {
    // < 12 bytes (header is ndim + flags + element OID) -> empty.
    EXPECT_TRUE(decode_pg_array<integer>(hex_to_bytes("00000001")).empty());
    EXPECT_TRUE(decode_pg_array<integer>(std::span<const byte>{}).empty());

    // ndim == 0 -> empty (header present, zero-dimensional array).
    EXPECT_TRUE(decode_pg_array<integer>(hex_to_bytes("000000000000000000000017")).empty());

    // ndim < 0 -> empty.
    EXPECT_TRUE(decode_pg_array<integer>(hex_to_bytes("ffffffff0000000000000017")).empty());

    // Truncated element: header claims a 1-D array of size 1 but the element's
    // length/value is missing -> decode stops, returns empty.
    EXPECT_TRUE(decode_pg_array<integer>(hex_to_bytes("0000000100000000000000170000000100000001")).empty());

    // Element length present but value truncated (claims 4 bytes, only 2 follow) -> empty.
    EXPECT_TRUE(decode_pg_array<integer>(hex_to_bytes("0000000100000000000000170000000100000001000000040102")).empty());
}

// ----------------------------------------------------------------------------
// Array converter to_text / from_text are intentionally inert (arrays travel in
// binary). They must be reachable and return the documented empty result rather
// than mis-encoding — covers the QB_PG_DEFINE_ARRAY_CONVERTER text stubs.
// ----------------------------------------------------------------------------

TEST(TypeConverterArrayText, TextStubsAreInert) {
    EXPECT_EQ(TypeConverter<std::vector<integer>>::to_text(std::vector<integer>{1, 2, 3}), std::string{});
    EXPECT_TRUE(TypeConverter<std::vector<integer>>::from_text("{1,2,3}").empty());
    EXPECT_EQ(TypeConverter<std::vector<std::string>>::to_text(std::vector<std::string>{"a", "b"}), std::string{});
    EXPECT_TRUE(TypeConverter<std::vector<std::string>>::from_text("{a,b}").empty());
    EXPECT_EQ(TypeConverter<std::vector<double>>::to_text(std::vector<double>{1.5}), std::string{});
}

// to_binary for every registered array element type frames as [int32 body-len][body]
// and round-trips through from_binary (exercises encode_pg_array<Elem> + each
// QB_PG_DEFINE_ARRAY_CONVERTER::to_binary, not just the int4 specialization).
TEST(TypeConverterArrayBinary, ToBinaryRoundTripAllElementTypes) {
    auto rt = [](auto vec) {
        using V = decltype(vec);
        std::vector<byte> buf;
        TypeConverter<V>::to_binary(vec, buf);
        ASSERT_GE(buf.size(), 4u);
        // declared body length matches the bytes after the prefix
        integer len_be;
        std::memcpy(&len_be, buf.data(), sizeof(integer));
        EXPECT_EQ(static_cast<size_t>(ntohl(static_cast<uint32_t>(len_be))), buf.size() - 4u);
        std::vector<byte> body(buf.begin() + 4, buf.end());
        EXPECT_EQ(TypeConverter<V>::from_binary(body), vec);
    };
    rt(std::vector<bool>{true, false, true});
    rt(std::vector<smallint>{7, -8, 9});
    rt(std::vector<bigint>{1, 2, 3});
    rt(std::vector<float>{1.5f, -2.5f});
    rt(std::vector<double>{1.5, 2.5});
    rt(std::vector<std::string>{"apple", "banana"});
}

// ----------------------------------------------------------------------------
// bytea via std::vector<std::byte>
// ----------------------------------------------------------------------------

// Previously this hit the generic unsupported-type fallback and returned empty for
// any non-empty bytea. Now it round-trips raw bytes (incl. an embedded NUL).
TEST(TypeConverterArrayBinary, ByteaStdByteRoundTrip) {
    const std::vector<std::byte> in{std::byte{0xDE}, std::byte{0xAD}, std::byte{0x00}, std::byte{0xBE}, std::byte{0xEF}};

    // Result value carries no length prefix.
    std::vector<byte> wire(in.size());
    for (size_t i = 0; i < in.size(); ++i)
        wire[i] = static_cast<byte>(in[i]);
    EXPECT_EQ(TypeConverter<std::vector<std::byte>>::from_binary(wire), in);

    // to_binary writes [int32 length][raw]; strip the prefix and decode back.
    std::vector<byte> buf;
    TypeConverter<std::vector<std::byte>>::to_binary(in, buf);
    ASSERT_GE(buf.size(), 4u);
    std::vector<byte> body(buf.begin() + 4, buf.end());
    EXPECT_EQ(TypeConverter<std::vector<std::byte>>::from_binary(body), in);
}

// bytea text: "\xDEADBEEF" <-> raw bytes, both directions.
TEST(TypeConverterArrayBinary, ByteaStdByteText) {
    std::vector<std::byte> raw{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    const std::string      hex = TypeConverter<std::vector<std::byte>>::to_text(raw);
    EXPECT_EQ(hex, "\\xdeadbeef");
    EXPECT_EQ(TypeConverter<std::vector<std::byte>>::from_text(hex), raw);
    EXPECT_EQ(TypeConverter<std::vector<std::byte>>::from_text("deadbeef"), raw); // no "\x" prefix
}
