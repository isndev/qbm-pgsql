/**
 * @file typeconverter-array.cpp
 * @brief Unit tests for the one-dimensional PostgreSQL ARRAY binary decoder
 *        (decode_pg_array / TypeConverter<std::vector<T>>) and the bytea
 *        std::vector<std::byte> converter.
 *
 * Anchored to PostgreSQL `array_send()` ground truth (value bytes, no length prefix).
 * Layout, all big-endian: int32 ndim, int32 has-null flags, int32 element OID, then
 * per dimension {int32 size, int32 lower-bound}, then per element {int32 length
 * (-1 = NULL), value}. The decoder is FAIL-LOUD (Huly QB-109): a NULL element throws
 * `value_is_null` unless the element type is `std::optional<T>`, a multi-dimensional array
 * throws `field_type_mismatch`, a malformed value throws `client_error`; and the text
 * format (the simple query protocol) is decoded and rendered, no longer stubbed to `{}`.
 * Split out of the legacy monolith `test-data-types.cpp` (array tests).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <cmath>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "../../shared/pg_wire_ground_truth.hpp"
#include <qbm/pgsql/pgsql.h>

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
    auto ints =
        TypeConverter<std::vector<integer>>::from_binary(hex_to_bytes("0000000100000000000000170000000400000001"
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
        TypeConverter<std::vector<bigint>>::from_binary(hex_to_bytes("00000001000000000000001400000003000000010000000800000000000000010000"
                                                                     "00080000000000000002000000080000000000000003")),
        (std::vector<bigint>{1, 2, 3}));

    EXPECT_EQ(
        TypeConverter<std::vector<smallint>>::from_binary(hex_to_bytes("0000000100000000000000150000000200000001000000020007000000020008")),
        (std::vector<smallint>{7, 8}));

    EXPECT_EQ(
        TypeConverter<std::vector<double>>::from_binary(hex_to_bytes("0000000100000000000002bd0000000200000001000000083ff80000000000000000"
                                                                     "00084004000000000000")),
        (std::vector<double>{1.5, 2.5}));

    EXPECT_EQ(TypeConverter<std::vector<float>>::from_binary(
                  hex_to_bytes("0000000100000000000002bc0000000200000001000000043fc0000000000004c0200000")),
              (std::vector<float>{1.5f, -2.5f}));

    EXPECT_EQ(
        TypeConverter<std::vector<bool>>::from_binary(hex_to_bytes("0000000100000000000000100000000300000001000000010100000001000000000101")),
        (std::vector<bool>{true, false, true}));
}

// decode_pg_array's bounds guards must degrade gracefully (empty / partial result) on a
// malformed or truncated binary array header — never read out of bounds. Each case targets
// one guard branch in decode_pg_array.
TEST(TypeConverterArrayBinary, MalformedBuffersThrowClientErrorWithoutOob) {
    using IV = std::vector<integer>;
    // header shorter than the 12-byte ndim+flags+elem_oid prefix.
    EXPECT_THROW(TypeConverter<IV>::from_binary(hex_to_bytes("00000001")), error::client_error);
    // ndim < 0.
    EXPECT_THROW(TypeConverter<IV>::from_binary(hex_to_bytes("ffffffff0000000000000017")), error::client_error);
    // ndim == 1 but no room for the 8-byte dim header (off+8 > size).
    EXPECT_THROW(TypeConverter<IV>::from_binary(hex_to_bytes("000000010000000000000017")), error::client_error);
    // a negative dimension size (dim_size = 0xffffffff, lower_bound = 1).
    EXPECT_THROW(TypeConverter<IV>::from_binary(hex_to_bytes("000000010000000000000017ffffffff00000001")), error::client_error);
    // a bogus huge dimension (dim_size = 0x7fffffff) whose count exceeds the buffer.
    EXPECT_THROW(TypeConverter<IV>::from_binary(hex_to_bytes("0000000100000000000000177fffffff00000001")), error::client_error);
    // header claims 3 elements but the buffer holds only one (10): until 3.2 this decoded to a
    // PARTIAL {10} -- a plausible value for a broken wire, the one answer a decoder must not give.
    EXPECT_THROW(TypeConverter<IV>::from_binary(hex_to_bytes("0000000100000000000000170000000300000001000000040000000a")), error::client_error);
    // a negative element length that is NOT the -1 NULL sentinel (here -2).
    EXPECT_THROW(TypeConverter<IV>::from_binary(hex_to_bytes("0000000100000000000000170000000100000001fffffffe")), error::client_error);
    // trailing bytes after the last element: not what array_send produces.
    EXPECT_THROW(TypeConverter<IV>::from_binary(hex_to_bytes("00000001000000000000001700000001000000010000000400000001ff")),
                 error::client_error);
}

// ----------------------------------------------------------------------------
// NULL elements: the plain-element path THROWS; the optional-element path nullopts
// ----------------------------------------------------------------------------

// Plain int4[] {1,NULL,3}: until 3.2 the NULL element decoded to a default-constructed 0
// -- `{1,0,3}`, a value the database never held. It is `value_is_null` now, the rule
// `field::as<T>()` applies to a NULL column, and the message names the way out.
TEST(TypeConverterArrayBinary, NullElementThrowsForPlainVector) {
    const auto bytes = hex_to_bytes("00000001000000010000001700000003000000010000000400000001ffffffff0000000400000003");
    EXPECT_THROW(TypeConverter<std::vector<integer>>::from_binary(bytes), error::value_is_null);
    try {
        (void) TypeConverter<std::vector<integer>>::from_binary(bytes);
    } catch (const error::value_is_null &e) {
        EXPECT_NE(std::string(e.what()).find("std::vector<std::optional<T>>"), std::string::npos) << e.what();
    }
    // The same bytes through the optional-element converter: {1, nullopt, 3}.
    auto opt = TypeConverter<std::vector<std::optional<integer>>>::from_binary(bytes);
    ASSERT_EQ(opt.size(), 3u);
    EXPECT_EQ(opt[0], 1);
    EXPECT_FALSE(opt[1].has_value());
    EXPECT_EQ(opt[2], 3);
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
    auto withMinusOne = decode_pg_array<std::optional<integer>>(hex_to_bytes("0000000100000000000000170000000200000001"
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
// elements row-major. Until 3.2 this FLATTENED to {1,2,3,4}, the shape lost in silence; a
// flat std::vector cannot hold it, so it is `field_type_mismatch` now.
TEST(TypeConverterArrayBinary, MultiDimensionalThrowsFieldTypeMismatch) {
    // Build the buffer field-by-field to avoid hand-counting a long hex literal.
    std::string h;
    h += "00000002"; // ndim = 2
    h += "00000000"; // has-null = 0
    h += "00000017"; // element OID = int4 (23)
    h += "00000002"; // dim0 size = 2
    h += "00000001"; // dim0 lower bound = 1
    h += "00000002"; // dim1 size = 2
    h += "00000001"; // dim1 lower bound = 1
    h += "00000004"
         "00000001"; // elem len 4, value 1
    h += "00000004"
         "00000002"; // 2
    h += "00000004"
         "00000003"; // 3
    h += "00000004"
         "00000004"; // 4
    EXPECT_THROW(TypeConverter<std::vector<integer>>::from_binary(hex_to_bytes(h)), error::field_type_mismatch);
    EXPECT_THROW(TypeConverter<std::vector<std::optional<integer>>>::from_binary(hex_to_bytes(h)), error::field_type_mismatch);
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

// std::vector<std::optional<T>> round-trips a NULL element: the -1 length on the wire and
// the has-null flag raised in the header (what array_recv checks), and nullopt back.
TEST(TypeConverterArrayBinary, OptionalElementRoundTripThroughEncode) {
    using OV = std::vector<std::optional<integer>>;
    const OV          in{7, std::nullopt, 100000};
    std::vector<byte> buf;
    TypeConverter<OV>::to_binary(in, buf);
    std::vector<byte> body(buf.begin() + 4, buf.end());
    // header: ndim=1, has-null=1, int4 OID, size=3, lb=1; then 7, NULL (-1), 100000.
    EXPECT_EQ(body, hex_to_bytes("00000001000000010000001700000003000000010000000400000007ffffffff00000004000186a0"));
    EXPECT_EQ(TypeConverter<OV>::from_binary(body), in);
    // No NULL element: the flag stays down, and the bytes equal the plain vector's.
    std::vector<byte> plain, opt;
    TypeConverter<std::vector<integer>>::to_binary(std::vector<integer>{1, 2}, plain);
    TypeConverter<OV>::to_binary(OV{1, 2}, opt);
    EXPECT_EQ(plain, opt);
}

// ----------------------------------------------------------------------------
// decode_pg_array guard paths (crafted buffers must never read out of bounds)
// ----------------------------------------------------------------------------

TEST(TypeConverterArrayBinary, DecodeGuardPaths) {
    // < 12 bytes (header is ndim + flags + element OID): malformed, loudly.
    EXPECT_THROW(decode_pg_array<integer>(hex_to_bytes("00000001")), error::client_error);
    EXPECT_THROW(decode_pg_array<integer>(std::span<const byte>{}), error::client_error);

    // ndim == 0 -> empty: the one empty answer, '{}' as PostgreSQL sends it.
    EXPECT_TRUE(decode_pg_array<integer>(hex_to_bytes("000000000000000000000017")).empty());

    // ndim < 0.
    EXPECT_THROW(decode_pg_array<integer>(hex_to_bytes("ffffffff0000000000000017")), error::client_error);

    // Truncated element: header claims a 1-D array of size 1 but the element's
    // length/value is missing.
    EXPECT_THROW(decode_pg_array<integer>(hex_to_bytes("0000000100000000000000170000000100000001")), error::client_error);

    // Element length present but value truncated (claims 4 bytes, only 2 follow).
    EXPECT_THROW(decode_pg_array<integer>(hex_to_bytes("0000000100000000000000170000000100000001000000040102")), error::client_error);
}

// ----------------------------------------------------------------------------
// The TEXT format (the simple query protocol): parsed and rendered (Huly QB-109). Until 3.2
// both were stubs -- `from_text` returned `{}` for every literal and `to_text` "", so a text
// column read as std::vector<int> was silently empty.
// ----------------------------------------------------------------------------

TEST(TypeConverterArrayText, ParsesEveryElementKind) {
    EXPECT_EQ(TypeConverter<std::vector<integer>>::from_text("{1,2,3}"), (std::vector<integer>{1, 2, 3}));
    EXPECT_EQ(TypeConverter<std::vector<integer>>::from_text("{-7, 0 ,  42}"), (std::vector<integer>{-7, 0, 42}));
    EXPECT_EQ(TypeConverter<std::vector<bigint>>::from_text("{9000000000}"), (std::vector<bigint>{9000000000LL}));
    EXPECT_EQ(TypeConverter<std::vector<smallint>>::from_text("{1,-1}"), (std::vector<smallint>{1, -1}));
    EXPECT_EQ(TypeConverter<std::vector<bool>>::from_text("{t,f,t}"), (std::vector<bool>{true, false, true}));
    EXPECT_EQ(TypeConverter<std::vector<double>>::from_text("{1.5,-2.25}"), (std::vector<double>{1.5, -2.25}));
    EXPECT_EQ(TypeConverter<std::vector<float>>::from_text("{0.5}"), (std::vector<float>{0.5f}));
    auto nonfinite = TypeConverter<std::vector<double>>::from_text("{NaN,Infinity,-Infinity}");
    ASSERT_EQ(nonfinite.size(), 3u);
    EXPECT_TRUE(std::isnan(nonfinite[0]));
    EXPECT_TRUE(std::isinf(nonfinite[1]) && nonfinite[1] > 0);
    EXPECT_TRUE(std::isinf(nonfinite[2]) && nonfinite[2] < 0);
}

TEST(TypeConverterArrayText, ParsesStringsQuotedAndBare) {
    // Bare words, quoted words with a space, an escaped quote and an escaped backslash, an
    // empty string (only ever quoted), and a word that spells NULL but is quoted -- a value.
    auto v = TypeConverter<std::vector<std::string>>::from_text(R"({a,"b c","d\"e","f\\g","","NULL"})");
    EXPECT_EQ(v, (std::vector<std::string>{"a", "b c", "d\"e", "f\\g", "", "NULL"}));
}

TEST(TypeConverterArrayText, EmptyAndDecoratedLiterals) {
    EXPECT_TRUE(TypeConverter<std::vector<integer>>::from_text("{}").empty());
    EXPECT_TRUE(TypeConverter<std::vector<std::string>>::from_text(" { } ").empty());
    // array_out prints a dimension decoration when the lower bound is not 1.
    EXPECT_EQ(TypeConverter<std::vector<integer>>::from_text("[0:2]={1,2,3}"), (std::vector<integer>{1, 2, 3}));
}

TEST(TypeConverterArrayText, NullElementThrowsUnlessOptional) {
    EXPECT_THROW(TypeConverter<std::vector<integer>>::from_text("{1,NULL,3}"), error::value_is_null);
    EXPECT_THROW(TypeConverter<std::vector<std::string>>::from_text("{a,null}"), error::value_is_null);
    auto opt = TypeConverter<std::vector<std::optional<integer>>>::from_text("{1,NULL,3}");
    ASSERT_EQ(opt.size(), 3u);
    EXPECT_EQ(opt[0], 1);
    EXPECT_FALSE(opt[1].has_value());
    EXPECT_EQ(opt[2], 3);
    auto opts = TypeConverter<std::vector<std::optional<std::string>>>::from_text(R"({a,null,"NULL"})");
    ASSERT_EQ(opts.size(), 3u);
    EXPECT_EQ(opts[0], "a");
    EXPECT_FALSE(opts[1].has_value());
    EXPECT_EQ(opts[2], "NULL"); // quoted: a value, not SQL NULL
}

TEST(TypeConverterArrayText, MultiDimensionalLiteralThrowsFieldTypeMismatch) {
    EXPECT_THROW(TypeConverter<std::vector<integer>>::from_text("{{1,2},{3,4}}"), error::field_type_mismatch);
    EXPECT_THROW(TypeConverter<std::vector<integer>>::from_text("[1:2][1:2]={{1,2},{3,4}}"), error::client_error);
}

TEST(TypeConverterArrayText, MalformedLiteralsThrowClientError) {
    using IV = std::vector<integer>;
    EXPECT_THROW(TypeConverter<IV>::from_text(""), error::client_error);
    EXPECT_THROW(TypeConverter<IV>::from_text("1,2,3"), error::client_error);
    EXPECT_THROW(TypeConverter<IV>::from_text("{1,2"), error::client_error);
    EXPECT_THROW(TypeConverter<IV>::from_text("{1,,2}"), error::client_error);
    EXPECT_THROW(TypeConverter<IV>::from_text("{1,2}x"), error::client_error);
    EXPECT_THROW(TypeConverter<IV>::from_text("{1 2}"), error::client_error);
    EXPECT_THROW(TypeConverter<std::vector<std::string>>::from_text(R"({"unterminated})"), error::client_error);
    EXPECT_THROW(TypeConverter<std::vector<std::string>>::from_text(R"({"dangling\)"), error::client_error);
    // An element the scalar parser refuses is the scalar parser's error, unchanged.
    EXPECT_THROW(TypeConverter<IV>::from_text("{1,x}"), error::client_error);
}

TEST(TypeConverterArrayText, RendersAndRoundTrips) {
    EXPECT_EQ(TypeConverter<std::vector<integer>>::to_text(std::vector<integer>{1, 2, 3}), "{1,2,3}");
    EXPECT_EQ(TypeConverter<std::vector<integer>>::to_text(std::vector<integer>{}), "{}");
    EXPECT_EQ(TypeConverter<std::vector<bool>>::to_text(std::vector<bool>{true, false}), "{t,f}");
    EXPECT_EQ(TypeConverter<std::vector<double>>::to_text(std::vector<double>{1.5}), "{1.5}");
    // Strings are always quoted, with `"` and `\` escaped, so every byte sequence round-trips.
    const std::vector<std::string> words{"a", "b c", "d\"e", "f\\g", "", "NULL"};
    const std::string              rendered = TypeConverter<std::vector<std::string>>::to_text(words);
    EXPECT_EQ(rendered, R"({"a","b c","d\"e","f\\g","","NULL"})");
    EXPECT_EQ(TypeConverter<std::vector<std::string>>::from_text(rendered), words);
    // A nullopt element renders as the bare word NULL, and reads back as nullopt.
    using OS = std::vector<std::optional<std::string>>;
    const OS mixed{"x", std::nullopt, "NULL"};
    EXPECT_EQ(TypeConverter<OS>::to_text(mixed), R"({"x",NULL,"NULL"})");
    EXPECT_EQ(TypeConverter<OS>::from_text(TypeConverter<OS>::to_text(mixed)), mixed);
    using OI = std::vector<std::optional<integer>>;
    const OI ints{1, std::nullopt, 3};
    EXPECT_EQ(TypeConverter<OI>::to_text(ints), "{1,NULL,3}");
    EXPECT_EQ(TypeConverter<OI>::from_text("{1,NULL,3}"), ints);
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
    //
    // Filled with one sized memcpy rather than an indexed store loop, and that is
    // deliberate — do not "simplify" it back. GCC 14 at -O3 inlines the vector
    // constructor into the loop, loses the extent it just allocated, and emits
    //     error: writing 8 bytes into a region of size 5 [-Werror=stringop-overflow=]
    //     error: writing 1 byte into a region of size 0  [-Werror=stringop-overflow=]
    // on the store below. It is a false positive — the vector is sized to in.size()
    // before the first write — but QB_TESTS_WERROR defaults to QB_CI, so it is fatal
    // on every runner and invisible on the maintainer's clang. Same family, and the
    // same remedy, as encode_pg_array()'s wr32 in
    // qbm/pgsql/src/qbm/pgsql/type_converter.h: state the extent GCC could not infer.
    std::vector<byte> wire(in.size());
    std::memcpy(wire.data(), in.data(), in.size());
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
