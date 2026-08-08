/**
 * @file qbm/pgsql/tests/unit/wire/unserializer-primitives.cpp
 * @brief Unit tests for the low-level byte-order readers of ParamUnserializer.
 *
 * These cover the network-byte-order (big-endian) scalar readers
 * (read_smallint / read_integer / read_bigint / read_float / read_double) and the
 * string reader (read_string with its text/binary auto-detect heuristic). Each
 * scalar is asserted against a buffer whose big-endian bytes are written by hand,
 * so a wrong endianness conversion in the reader is caught (this is decode-vs-bytes,
 * not a serializer self-round-trip).
 *
 * The auto-detect heuristic in ParamUnserializer::read_string (param_unserializer.cpp:184)
 * has a documented limitation for large BYTEA-shaped buffers whose first bytes look
 * like a binary length prefix; HeuristicMisfiresOnLengthPrefixShapedBytea pins that
 * exact behavior (replacing the prior `result[0] == 4` bug-baked-in assertion).
 *
 * Pure logic: no daemon, no qb::Main, no event loop, parallel-safe.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <qb/system/endian.h>

#include <qbm/pgsql/pgsql.h>

#include "../../shared/pg_wire_ground_truth.hpp"

using namespace qb::pg;
using namespace qb::pg::detail;
using qb::pg::test::hex_to_bytes;

namespace {

/// Append @p value to @p buf in PostgreSQL network byte order (big-endian).
template <typename T>
std::vector<byte>
big_endian_bytes(T value) {
    std::vector<byte> buf(sizeof(T));
    if constexpr (std::is_integral_v<T>) {
        auto be = qb::endian::to_big_endian(value);
        std::memcpy(buf.data(), &be, sizeof(T));
    } else {
        // Float/double: reinterpret as the same-width unsigned, byte-swap that.
        using U = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
        U raw;
        std::memcpy(&raw, &value, sizeof(T));
        U be = qb::endian::to_big_endian(raw);
        std::memcpy(buf.data(), &be, sizeof(T));
    }
    return buf;
}

/// Frame @p value as PostgreSQL binary string: 4-byte big-endian length + data.
std::vector<byte>
pg_binary_string(const std::string &value) {
    auto buf = big_endian_bytes(static_cast<integer>(value.size()));
    buf.insert(buf.end(), value.begin(), value.end());
    return buf;
}

class UnserializerPrimitives : public ::testing::Test {
protected:
    ParamUnserializer u;
};

// ---------------------------------------------------------------------------
// Scalar big-endian readers: decode hand-written network-order bytes.
// ---------------------------------------------------------------------------

TEST_F(UnserializerPrimitives, ReadSmallintBigEndian) {
    EXPECT_EQ(u.read_smallint(big_endian_bytes<smallint>(12345)), 12345);
    EXPECT_EQ(u.read_smallint(big_endian_bytes<smallint>(-1)), -1);
    // 0x3039 big-endian == 12345; verify the byte order explicitly.
    EXPECT_EQ(u.read_smallint(hex_to_bytes("3039")), 12345);
}

TEST_F(UnserializerPrimitives, ReadIntegerBigEndian) {
    EXPECT_EQ(u.read_integer(big_endian_bytes<integer>(987654321)), 987654321);
    EXPECT_EQ(u.read_integer(hex_to_bytes("3ade68b1")), 987654321);
    EXPECT_EQ(u.read_integer(hex_to_bytes("ffffffff")), -1); // sign-extension
    EXPECT_EQ(u.read_integer(hex_to_bytes("80000000")), std::numeric_limits<integer>::min());
}

TEST_F(UnserializerPrimitives, ReadBigintBigEndian) {
    EXPECT_EQ(u.read_bigint(big_endian_bytes<bigint>(9223372036854775807LL)), 9223372036854775807LL);
    EXPECT_EQ(u.read_bigint(hex_to_bytes("ffffffffffffffff")), -1LL);
    EXPECT_EQ(u.read_bigint(hex_to_bytes("8000000000000000")), std::numeric_limits<bigint>::min());
}

TEST_F(UnserializerPrimitives, ReadFloatBigEndian) {
    EXPECT_NEAR(u.read_float(big_endian_bytes<float>(3.14159f)), 3.14159f, 1e-5f);
    // 0x3fc00000 big-endian IEEE-754 == 1.5
    EXPECT_FLOAT_EQ(u.read_float(hex_to_bytes("3fc00000")), 1.5f);
}

TEST_F(UnserializerPrimitives, ReadDoubleBigEndian) {
    const double e = 2.7182818284590452353602874713527;
    EXPECT_NEAR(u.read_double(big_endian_bytes<double>(e)), e, 1e-13);
    // 0x3ff8000000000000 big-endian IEEE-754 == 1.5
    EXPECT_DOUBLE_EQ(u.read_double(hex_to_bytes("3ff8000000000000")), 1.5);
}

TEST_F(UnserializerPrimitives, ReadFloatSpecialValues) {
    EXPECT_TRUE(std::isnan(u.read_float(big_endian_bytes<float>(std::numeric_limits<float>::quiet_NaN()))));
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_TRUE(std::isinf(u.read_float(big_endian_bytes<float>(inf))) && u.read_float(big_endian_bytes<float>(inf)) > 0);
    EXPECT_TRUE(std::isinf(u.read_float(big_endian_bytes<float>(-inf))) && u.read_float(big_endian_bytes<float>(-inf)) < 0);
}

TEST_F(UnserializerPrimitives, ScalarBoundaryValuesRoundTrip) {
    EXPECT_EQ(u.read_smallint(big_endian_bytes(std::numeric_limits<smallint>::min())), std::numeric_limits<smallint>::min());
    EXPECT_EQ(u.read_smallint(big_endian_bytes(std::numeric_limits<smallint>::max())), std::numeric_limits<smallint>::max());
    EXPECT_EQ(u.read_integer(big_endian_bytes(std::numeric_limits<integer>::min())), std::numeric_limits<integer>::min());
    EXPECT_EQ(u.read_integer(big_endian_bytes(std::numeric_limits<integer>::max())), std::numeric_limits<integer>::max());
    EXPECT_EQ(u.read_bigint(big_endian_bytes(std::numeric_limits<bigint>::min())), std::numeric_limits<bigint>::min());
    EXPECT_EQ(u.read_bigint(big_endian_bytes(std::numeric_limits<bigint>::max())), std::numeric_limits<bigint>::max());
}

// ---------------------------------------------------------------------------
// Buffer-size validation: scalar readers throw on short buffers.
// ---------------------------------------------------------------------------

TEST_F(UnserializerPrimitives, ScalarReadersThrowOnUndersizedBuffer) {
    struct R {
        const char                                *name;
        std::function<void(std::span<const byte>)> call;
        std::size_t                                need;
    };
    const R rows[] = {
        {"smallint", [&](auto b) { u.read_smallint(b); }, sizeof(smallint)}, {"integer", [&](auto b) { u.read_integer(b); }, sizeof(integer)},
        {"bigint", [&](auto b) { u.read_bigint(b); }, sizeof(bigint)},       {"float", [&](auto b) { u.read_float(b); }, sizeof(float)},
        {"double", [&](auto b) { u.read_double(b); }, sizeof(double)},
    };
    for (const auto &r : rows) {
        std::vector<byte> empty;
        EXPECT_THROW(r.call(empty), std::runtime_error) << r.name << " empty";
        std::vector<byte> one_short(r.need - 1);
        EXPECT_THROW(r.call(one_short), std::runtime_error) << r.name << " one-short";
    }
}

TEST_F(UnserializerPrimitives, IntegerThrowsAtEveryTruncationPosition) {
    const auto full = big_endian_bytes<integer>(12345678);
    for (std::size_t cut = 0; cut < sizeof(integer); ++cut) {
        std::vector<byte> truncated(full.begin(), full.begin() + cut);
        EXPECT_THROW(u.read_integer(truncated), std::runtime_error) << "cut=" << cut;
    }
}

// ---------------------------------------------------------------------------
// String reader: text passthrough, binary length-prefix, edge cases.
// ---------------------------------------------------------------------------

TEST_F(UnserializerPrimitives, ReadStringTextFormatPassthrough) {
    const std::string s = "Hello, PostgreSQL!";
    EXPECT_EQ(u.read_string({reinterpret_cast<const byte *>(s.data()), s.size()}), s);
}

TEST_F(UnserializerPrimitives, ReadStringEmptyBufferIsEmpty) {
    std::vector<byte> empty;
    EXPECT_EQ(u.read_string(empty), "");
}

TEST_F(UnserializerPrimitives, ReadBinaryStringStripsLengthPrefix) {
    const std::string s      = "Binary PG Format Test";
    const auto        framed = pg_binary_string(s);
    // read_binary_string consumes the 4-byte prefix itself.
    EXPECT_EQ(u.read_binary_string(framed), s);
}

// read_binary_string needs the full 4-byte length prefix; anything shorter cannot even
// read the length and throws (param_unserializer.cpp:220).
TEST_F(UnserializerPrimitives, ReadBinaryStringThrowsOnBufferShorterThanLengthPrefix) {
    for (std::size_t n = 0; n < 4; ++n) {
        std::vector<byte> buf(n);
        EXPECT_THROW(u.read_binary_string(buf), std::runtime_error) << "size=" << n;
    }
}

// A 0xFFFFFFFF big-endian length prefix decodes to -1 (negative), which read_binary_string
// treats as SQL NULL and returns "" — any trailing payload bytes are ignored
// (param_unserializer.cpp:228).
TEST_F(UnserializerPrimitives, ReadBinaryStringNegativeLengthIsSqlNull) {
    // '\xFF' rather than byte(0xFF): `byte` is `char` (pg_types.h:126), which is SIGNED on
    // MSVC, so the functional cast of 255 is a truncating cast and warns (C4310) x8. The
    // character literal states the same wire byte with no cast at all, on every compiler.
    std::vector<byte> null_prefix{'\xFF', '\xFF', '\xFF', '\xFF'};
    EXPECT_EQ(u.read_binary_string(null_prefix), "");

    // Same NULL sentinel followed by data: still empty (the negative length short-circuits).
    std::vector<byte> null_with_tail{'\xFF', '\xFF', '\xFF', '\xFF', 'x', 'y'};
    EXPECT_EQ(u.read_binary_string(null_with_tail), "");
}

TEST_F(UnserializerPrimitives, ReadStringPreservesUnicodeAndEmbeddedNuls) {
    const std::string unicode = "Unicode: \xC3\xA4\xC3\xB6\xC3\xBC \xE4\xBD\xA0\xE5\xA5\xBD";
    EXPECT_EQ(u.read_string({reinterpret_cast<const byte *>(unicode.data()), unicode.size()}), unicode);

    std::string       with_nul("a\0b\0c", 5);
    std::vector<byte> buf(with_nul.begin(), with_nul.end());
    const std::string out = u.read_string(buf);
    EXPECT_EQ(out.size(), 5u);
    EXPECT_EQ(out, with_nul);
}

TEST_F(UnserializerPrimitives, ReadStringRecoversTruncatedPrefix) {
    const std::string original = "This string should be partially truncated";
    std::vector<byte> full(original.begin(), original.end());
    for (std::size_t cut = 5; cut < original.size(); cut += 5) {
        std::vector<byte> truncated(full.begin(), full.begin() + cut);
        EXPECT_EQ(u.read_string(truncated), original.substr(0, cut));
    }
}

// ---------------------------------------------------------------------------
// Documented heuristic limitation (was: ASSERT_EQ(result[0], 4) bug-baked-in).
//
// read_string auto-detects binary format when buffer.size() in [4, 1 MiB] and one
// of the first three bytes is 0 (param_unserializer.cpp:184). A 1 MiB BYTEA filled
// with i % 256 begins 00 01 02 03 ... : byte[0] == 0 triggers the binary path,
// the first 4 bytes are read as a big-endian length (0x00010203 = 66051), and the
// returned string starts at offset 4 (value 0x04). The prior test asserted
// `result[0] == 4` WITHOUT explaining it was a misfire. We pin the misfire here so
// any future fix to the heuristic flips this test loudly.
// ---------------------------------------------------------------------------

TEST_F(UnserializerPrimitives, HeuristicMisfiresOnLengthPrefixShapedBytea) {
    const std::size_t size = 1024 * 1024; // exactly the heuristic's upper bound
    std::vector<byte> buf(size);
    for (std::size_t i = 0; i < size; ++i)
        buf[i] = static_cast<byte>(i % 256);

    const std::string result = u.read_string(buf);

    // Heuristic-driven: first 4 bytes consumed as length, payload begins at byte 4.
    const integer parsed_len = u.read_integer({buf.data(), 4});
    EXPECT_EQ(parsed_len, 0x00010203);
    ASSERT_EQ(result.size(), static_cast<std::size_t>(parsed_len));
    EXPECT_EQ(static_cast<unsigned char>(result[0]), 0x04u) << "If the read_string heuristic was fixed to return raw BYTEA, update this test.";
}

TEST_F(UnserializerPrimitives, TextShapedByteaIsReturnedRaw) {
    // A BYTEA whose first three bytes are all non-zero stays on the text path and
    // is returned verbatim (the heuristic does NOT misfire here).
    std::vector<byte> buf;
    for (int i = 1; i <= 600; ++i)
        buf.push_back(static_cast<byte>((i % 255) + 1)); // never 0
    const std::string result = u.read_string(buf);
    ASSERT_EQ(result.size(), buf.size());
    EXPECT_EQ(std::memcmp(result.data(), buf.data(), buf.size()), 0);
}

// ---------------------------------------------------------------------------
// read_bool: binary length-prefix form, every text spelling, raw single byte.
// (param_unserializer.cpp:257-285 — previously entirely uncovered.)
// ---------------------------------------------------------------------------

TEST_F(UnserializerPrimitives, ReadBoolEmptyBufferThrows) {
    std::vector<byte> empty;
    EXPECT_THROW(u.read_bool(empty), std::runtime_error);
}

TEST_F(UnserializerPrimitives, ReadBoolBinaryLengthPrefixedForm) {
    // Formal binary bool: [int32 length == 1][value byte]. length==1 path.
    auto framed_true = big_endian_bytes<integer>(1);
    framed_true.push_back(static_cast<byte>(1));
    auto framed_false = big_endian_bytes<integer>(1);
    framed_false.push_back(static_cast<byte>(0));
    EXPECT_TRUE(u.read_bool(framed_true));
    EXPECT_FALSE(u.read_bool(framed_false));
}

TEST_F(UnserializerPrimitives, ReadBoolBinaryLengthNotOneFallsThroughToRaw) {
    // read_bool takes the binary branch ONLY when the 4-byte BE length prefix == 1 (value=buffer[4]);
    // otherwise it falls through to text, then to the raw last-resort `buffer[0] != 0`. So the FIRST
    // byte decides the raw outcome. length prefix 0x02000000 (!=1), buffer[0]==0x02 -> raw -> true.
    std::vector<byte> raw_true{byte(0x02), byte(0x00), byte(0x00), byte(0x00), byte(0x00)};
    EXPECT_TRUE(u.read_bool(raw_true));
    // length prefix 99 (!=1), buffer[0]==0x00 -> raw -> false.
    std::vector<byte> raw_false{byte(0x00), byte(0x00), byte(0x00), byte(0x63), byte(0x02)};
    EXPECT_FALSE(u.read_bool(raw_false));
}

TEST_F(UnserializerPrimitives, ReadBoolTextSpellings) {
    auto B = [](const char *s) {
        return std::vector<byte>(reinterpret_cast<const byte *>(s), reinterpret_cast<const byte *>(s) + std::strlen(s));
    };
    // True spellings.
    EXPECT_TRUE(u.read_bool(B("true")));
    EXPECT_TRUE(u.read_bool(B("t")));
    EXPECT_TRUE(u.read_bool(B("1")));
    EXPECT_TRUE(u.read_bool(B("y")));
    EXPECT_TRUE(u.read_bool(B("yes")));
    EXPECT_TRUE(u.read_bool(B("on")));
    // False spellings (text-letter first byte but not a true spelling).
    EXPECT_FALSE(u.read_bool(B("false")));
    EXPECT_FALSE(u.read_bool(B("f")));
    EXPECT_FALSE(u.read_bool(B("0")));
    EXPECT_FALSE(u.read_bool(B("n")));
    // Uppercase first byte enters the text branch too.
    EXPECT_FALSE(u.read_bool(B("F")));
    EXPECT_FALSE(u.read_bool(B("T"))); // "T" alone is not a listed true spelling
}

TEST_F(UnserializerPrimitives, ReadBoolRawSingleByteLastResort) {
    // A short buffer (<5) whose first byte is not a recognized text letter:
    // the raw-byte last resort returns buffer[0] != 0.
    std::vector<byte> nz{static_cast<byte>(0x42)};
    std::vector<byte> z{static_cast<byte>(0x00)};
    EXPECT_TRUE(u.read_bool(nz));
    EXPECT_FALSE(u.read_bool(z));
}

// ---------------------------------------------------------------------------
// read_string binary-detection branch (param_unserializer.cpp:184 TRUE path):
// a small buffer whose first bytes look like a length prefix is read as binary.
// ---------------------------------------------------------------------------

TEST_F(UnserializerPrimitives, ReadStringBinaryDetectionSucceeds) {
    // [int32 length == 3]["abc"] : byte[0..2] include a zero, size in [4,1MiB],
    // so the heuristic takes the binary path and read_binary_string returns "abc".
    auto framed = pg_binary_string("abc");
    EXPECT_EQ(u.read_string(framed), "abc");
}

TEST_F(UnserializerPrimitives, ReadStringBinaryDetectionFallsBackToTextOnInvalidLength) {
    // Leading zero triggers the binary path, but the declared length (0x00FFFFFF)
    // exceeds the buffer, so read_binary_string throws and read_string recovers by
    // returning the raw bytes as text.
    std::vector<byte> buf{
        static_cast<byte>(0x00), static_cast<byte>(0xFF), static_cast<byte>(0xFF), static_cast<byte>(0xFF), static_cast<byte>('x')
    };
    const std::string out = u.read_string(buf);
    ASSERT_EQ(out.size(), buf.size());
    EXPECT_EQ(std::memcmp(out.data(), buf.data(), buf.size()), 0);
}

} // namespace

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
