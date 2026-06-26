/**
 * @file protocol-message-codec.cpp
 * @brief Unit tests for the PostgreSQL wire-message framing + scalar codecs (no daemon).
 *
 * Pure-logic, daemon-free: no `qb::Main`, no event loop, no `RESOURCE_LOCK` (this file
 * anchors the `unit/protocol` dir). Covers the message framing helpers the client relies
 * on — length encoding, the read cursor, `discard_remaining`, the frontend/backend tag
 * sets, the packed multi-message send (`CopyFail` + `Sync`), the fixed 8-byte SSLRequest
 * pre-startup packet — plus the Bind format-code policy and the `TypeConverter` scalar
 * decoders that turn server wire bytes into C++ values.
 *
 * Restructured from `test-protocol-message.cpp` (spec §2 RENAME, §7): the honest
 * daemon-free header is kept, the legacy `qb::io::async::init()` in `main` is dropped
 * (no event loop is touched here), `PackAppendsSecondMessageWireBytes` now DECODES the
 * embedded Sync's length + tag rather than asserting `n >= 10`, and the int8 decode
 * matrix is extended with negative-value sign-extension, text-OID `from_text`
 * round-trips, and a binary-NUMERIC codec unit (ground-truth anchored).
 *
 * @see qb::pg::detail::message
 * @see qb::pg::detail::TypeConverter
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <gtest/gtest.h>

#include "../pgsql.h"
#include "../../shared/pg_wire_ground_truth.hpp"

using namespace qb::pg;
using namespace qb::pg::detail;
using qb::pg::test::hex_to_bytes;

// ===========================================================================
// Constants + SSLRequest pre-startup packet.
// ===========================================================================

TEST(ProtocolMessageConstants, MaxMessageBytesMatches256MiB) {
    EXPECT_EQ(PG_PROTOCOL_MAX_MESSAGE_BYTES, 256U * 1024U * 1024U);
}

// SSL negotiation: length 8 (Int32, network byte order) + request code 80877103 (Int32, BE).
// Same construction as `on_async_tcp_connected` in pgsql.h for `transport::stcp`.
TEST(ProtocolSslRequestWireFormat, MatchesPostgresqlDocumentation) {
    static constexpr std::array<std::uint8_t, 8> kExpected = {0x00, 0x00, 0x00, 0x08, 0x04, 0xD2, 0x16, 0x2F};
    static_assert(80877103u == 0x04D2162Fu);

    std::uint32_t               len  = htonl(8);
    std::uint32_t               code = htonl(0x04D2162F);
    std::array<std::uint8_t, 8> wire{};
    std::memcpy(wire.data(), &len, 4);
    std::memcpy(wire.data() + 4, &code, 4);
    EXPECT_EQ(wire, kExpected);
}

// ===========================================================================
// Bind/result format-code policy.
// ===========================================================================

TEST(ProtocolBindFormatPolicy, ScalarBinaryStringLikeText) {
    using qb::pg::oid;
    EXPECT_TRUE(qb::pg::type_oid_prefers_binary_result_format(oid::int4));
    EXPECT_TRUE(qb::pg::type_oid_prefers_binary_result_format(oid::int8));
    EXPECT_TRUE(qb::pg::type_oid_prefers_binary_result_format(oid::float8));
    EXPECT_TRUE(qb::pg::type_oid_prefers_binary_result_format(oid::boolean));
    EXPECT_TRUE(qb::pg::type_oid_prefers_binary_result_format(oid::jsonb));
    EXPECT_TRUE(qb::pg::type_oid_prefers_binary_result_format(oid::uuid));
    EXPECT_FALSE(qb::pg::type_oid_prefers_binary_result_format(oid::text));
    EXPECT_FALSE(qb::pg::type_oid_prefers_binary_result_format(oid::varchar));
    EXPECT_FALSE(qb::pg::type_oid_prefers_binary_result_format(oid::bpchar));
    EXPECT_FALSE(qb::pg::type_oid_prefers_binary_result_format(oid::unknown));
    EXPECT_FALSE(qb::pg::type_oid_prefers_binary_result_format(oid::json));
    EXPECT_FALSE(qb::pg::type_oid_prefers_binary_result_format(oid::tsvector));
    EXPECT_FALSE(qb::pg::type_oid_prefers_binary_result_format(oid::tsquery));
    EXPECT_FALSE(qb::pg::type_oid_prefers_binary_result_format(oid::name));
    EXPECT_FALSE(qb::pg::type_oid_prefers_binary_result_format(oid::regclass));
    EXPECT_FALSE(qb::pg::type_oid_prefers_binary_result_format(oid::regtype));
    EXPECT_FALSE(qb::pg::type_oid_prefers_binary_result_format(oid::cash));
}

TEST(SyncFieldFormatCodes, MatchesOidPolicy) {
    using qb::pg::oid;
    row_description_type desc;
    field_description    a{};
    a.type_oid    = oid::int4;
    a.format_code = protocol_data_format::Text;
    field_description b{};
    b.type_oid    = oid::text;
    b.format_code = protocol_data_format::Text;
    field_description c{};
    c.type_oid    = oid::jsonb;
    c.format_code = protocol_data_format::Text;
    desc.push_back(a);
    desc.push_back(b);
    desc.push_back(c);
    sync_field_format_codes_with_extended_query_bind(desc);
    EXPECT_EQ(desc[0].format_code, protocol_data_format::Binary);
    EXPECT_EQ(desc[1].format_code, protocol_data_format::Text);
    EXPECT_EQ(desc[2].format_code, protocol_data_format::Binary);
}

// ===========================================================================
// Integer binary decode — size-dispatch (int2/int4/int8) + sign extension.
// ===========================================================================

// Prepared-query int8 columns (e.g. COUNT(*)) are 8-byte big-endian; as<int32_t>() must
// not use only the first four octets (that yields 0 for small counts).
TEST(TypeConverterIntegerBinary, WidthDispatchDecodesInt2Int4Int8) {
    std::vector<byte> one(8, 0);
    one[7] = static_cast<byte>(1);
    EXPECT_EQ(TypeConverter<integer>::from_binary(one), 1);

    std::vector<byte> two_fifty_six(8, 0);
    two_fifty_six[6] = static_cast<byte>(1); // 0x0100 BE
    EXPECT_EQ(TypeConverter<integer>::from_binary(two_fifty_six), 256);

    std::vector<byte> int4(4, 0);
    int4[3] = static_cast<byte>(7);
    EXPECT_EQ(TypeConverter<integer>::from_binary(int4), 7);

    std::vector<byte> int2(2, 0);
    int2[1] = static_cast<byte>(42); // BE 42
    EXPECT_EQ(TypeConverter<integer>::from_binary(int2), 42);
}

// A negative int8 on the wire is all-0xFF in the high octets; the decoder must
// sign-extend, not read four zero octets (which would yield a bogus 0 or 0xFFFFFFFF).
TEST(TypeConverterIntegerBinary, NegativeInt8SignExtendsToInt32) {
    // -1 == 0xFFFFFFFFFFFFFFFF
    std::vector<byte> neg_one(8, static_cast<byte>(0xFF));
    EXPECT_EQ(TypeConverter<integer>::from_binary(neg_one), -1);

    // -256 == 0xFFFFFFFFFFFFFF00
    std::vector<byte> neg_256(8, static_cast<byte>(0xFF));
    neg_256[7] = static_cast<byte>(0x00);
    EXPECT_EQ(TypeConverter<integer>::from_binary(neg_256), -256);

    // int4 -1 (4-byte 0xFFFFFFFF) decodes to -1.
    std::vector<byte> int4_neg(4, static_cast<byte>(0xFF));
    EXPECT_EQ(TypeConverter<integer>::from_binary(int4_neg), -1);

    // int2 -42 (BE 0xFFD6) decodes to -42 through the smallint reader.
    std::vector<byte> int2_neg(2, 0);
    const std::int16_t be = static_cast<std::int16_t>(htons(static_cast<std::uint16_t>(-42)));
    std::memcpy(int2_neg.data(), &be, sizeof(be));
    EXPECT_EQ(TypeConverter<integer>::from_binary(int2_neg), -42);
}

// bigint decode preserves the full 64-bit range (no int32 narrowing).
TEST(TypeConverterIntegerBinary, BigintDecodesFullRange) {
    std::vector<byte> max(8, 0);
    const bigint be = static_cast<bigint>(qb::endian::to_big_endian(std::numeric_limits<bigint>::max()));
    std::memcpy(max.data(), &be, sizeof(be));
    EXPECT_EQ(TypeConverter<bigint>::from_binary(max), std::numeric_limits<bigint>::max());

    std::vector<byte> neg(8, 0);
    const bigint be2 = static_cast<bigint>(qb::endian::to_big_endian(static_cast<bigint>(-9000000000LL)));
    std::memcpy(neg.data(), &be2, sizeof(be2));
    EXPECT_EQ(TypeConverter<bigint>::from_binary(neg), -9000000000LL);
}

// ===========================================================================
// text-OID from_text round-trips (text-routed types are decoded via from_text).
// ===========================================================================

TEST(TypeConverterFromText, TextRoutedTypesParseExactValues) {
    EXPECT_EQ(TypeConverter<std::string>::from_text("hello"), "hello");
    EXPECT_EQ(TypeConverter<integer>::from_text("-2147483648"), std::numeric_limits<integer>::min());
    EXPECT_EQ(TypeConverter<integer>::from_text("2147483647"), std::numeric_limits<integer>::max());
    EXPECT_EQ(TypeConverter<smallint>::from_text("-32768"), std::numeric_limits<smallint>::min());
    EXPECT_EQ(TypeConverter<bigint>::from_text("9223372036854775807"), std::numeric_limits<bigint>::max());
    EXPECT_TRUE(TypeConverter<bool>::from_text("t"));
    EXPECT_FALSE(TypeConverter<bool>::from_text("f"));
}

// Out-of-range text must throw rather than silently wrap (smallint narrowing guard).
TEST(TypeConverterFromText, SmallintOutOfRangeThrows) {
    EXPECT_THROW((void) TypeConverter<smallint>::from_text("40000"), std::exception);
}

// ===========================================================================
// Binary NUMERIC codec — ground-truth anchored (decode + round-trip).
// ===========================================================================

TEST(TypeConverterNumericBinary, DecodesServerSendBytes) {
    for (const auto &c : qb::pg::test::gt::numeric::finite) {
        const auto    bytes = hex_to_bytes(c.hex);
        const numeric got   = TypeConverter<numeric>::from_binary(bytes);
        EXPECT_EQ(got.str(), c.expect) << "NUMERIC decode mismatch for " << c.hex;
    }
}

TEST(TypeConverterNumericBinary, DecodesSpecialSignWords) {
    for (const auto &c : qb::pg::test::gt::numeric::specials) {
        const auto    bytes = hex_to_bytes(c.hex);
        const numeric got   = TypeConverter<numeric>::from_binary(bytes);
        EXPECT_EQ(got.str(), c.expect) << "NUMERIC special decode mismatch for " << c.hex;
    }
}

// Encode -> decode through the framed (length-prefixed) to_binary path: the from_binary
// length-prefix sniff must strip it and recover the canonical text.
TEST(TypeConverterNumericBinary, FramedToBinaryRoundTrips) {
    for (const char *text : {"123456789.0123456789", "-12.5", "0.10", "100.00"}) {
        std::vector<byte> buf;
        TypeConverter<numeric>::to_binary(numeric(text), buf); // emits [int32 len][value]
        EXPECT_EQ(TypeConverter<numeric>::from_binary(buf).str(), text);
    }
}

// ===========================================================================
// message framing.
// ===========================================================================

TEST(ProtocolMessageTags, FrontendAndBackendSets) {
    EXPECT_TRUE(message::frontend_tags().count(query_tag));
    EXPECT_TRUE(message::frontend_tags().count(sync_tag));
    EXPECT_TRUE(message::frontend_tags().count(copy_fail_tag));
    EXPECT_FALSE(message::frontend_tags().count(ready_for_query_tag));

    EXPECT_TRUE(message::backend_tags().count(ready_for_query_tag));
    EXPECT_TRUE(message::backend_tags().count(notification_resp_tag));
    EXPECT_TRUE(message::backend_tags().count(copy_in_response_tag));
    EXPECT_FALSE(message::backend_tags().count(query_tag));
}

TEST(ProtocolMessage, BufferEncodesLengthMatchingSize) {
    message m(query_tag);
    m.write(std::string("SELECT 1"));
    (void) m.buffer();
    EXPECT_EQ(static_cast<size_t>(m.length()), m.size());
}

TEST(ProtocolMessage, DiscardRemainingSkipsRestOfPayload) {
    message m(query_tag);
    m.write(std::string("ab"));
    m.reset_read();
    char c{};
    ASSERT_TRUE(m.read(c));
    EXPECT_EQ(c, 'a');
    m.discard_remaining();
    EXPECT_FALSE(m.read(c));
}

TEST(ProtocolMessage, ReadStringThenDiscardRemaining) {
    message m(query_tag);
    m.write(std::string("hello"));
    m.reset_read();
    std::string prefix;
    ASSERT_TRUE(m.read(prefix, 2));
    EXPECT_EQ(prefix, "he");
    m.discard_remaining();
    std::string tail;
    EXPECT_FALSE(m.read(tail));
}

TEST(ProtocolMessage, SmallintRoundTrip) {
    message m(query_tag);
    m.write(static_cast<smallint>(-42));
    m.write(static_cast<smallint>(1000));
    m.reset_read();
    smallint a{};
    smallint b{};
    ASSERT_TRUE(m.read(a));
    ASSERT_TRUE(m.read(b));
    EXPECT_EQ(a, -42);
    EXPECT_EQ(b, 1000);
}

// A short read of a smallint must fail AND leave the destination untouched. The
// RowDescription handler relies on exactly this: it initializes the column count,
// checks the read result, and bails — otherwise a truncated RowDescription (whose
// length the server controls) would feed an indeterminate count into reserve()/the
// field loop.
TEST(ProtocolMessage, SmallintShortReadFailsAndLeavesTargetUntouched) {
    message m(query_tag);
    m.write(static_cast<char>(0x00)); // one byte only; smallint needs two
    m.reset_read();
    smallint col_cnt = -12345; // sentinel
    EXPECT_FALSE(m.read(col_cnt));
    EXPECT_EQ(col_cnt, -12345);
}

TEST(ProtocolMessage, IntegerRoundTrip) {
    message m(query_tag);
    m.write(static_cast<integer>(0x7FFFFFFFL));
    m.reset_read();
    integer v{};
    ASSERT_TRUE(m.read(v));
    EXPECT_EQ(v, 0x7FFFFFFFL);
}

// A DataRow whose declared field length exceeds the bytes remaining in the
// (fully-buffered) message body must be rejected, not over-read off the heap.
TEST(ProtocolMessage, DataRowRejectsOversizedFieldLength) {
    message m(data_row_tag);
    m.write(static_cast<smallint>(1));   // 1 column
    m.write(static_cast<integer>(1000)); // claims 1000 bytes...
    m.write('a');                        // ...but only 1 byte follows
    (void) m.buffer();
    m.reset_read();
    row_data row;
    EXPECT_FALSE(m.read(row)); // rejected, no OOB read / crash
}

// A DataRow with a negative column count must be rejected, not converted to a
// huge size_t in reserve()/resize() (which previously threw bad_alloc).
TEST(ProtocolMessage, DataRowRejectsNegativeColumnCount) {
    message m(data_row_tag);
    m.write(static_cast<smallint>(-1)); // negative column count
    (void) m.buffer();
    m.reset_read();
    row_data row;
    EXPECT_FALSE(m.read(row));
}

// A well-formed DataRow still round-trips: one NULL column and one value.
TEST(ProtocolMessage, DataRowValidRoundTrip) {
    message m(data_row_tag);
    m.write(static_cast<smallint>(2)); // 2 columns
    m.write(static_cast<integer>(-1)); // col 0 = NULL
    m.write(static_cast<integer>(3));  // col 1 = 3 bytes
    m.write('a');
    m.write('b');
    m.write('c');
    (void) m.buffer();
    m.reset_read();
    row_data row;
    ASSERT_TRUE(m.read(row));
    EXPECT_EQ(row.size(), 2u);
    EXPECT_TRUE(row.is_null(0));
    EXPECT_FALSE(row.is_null(1));
}

// pack() appends the SECOND message's full wire bytes (tag + length + body) after the
// first message's framed bytes. Was EXPECT_GE(n,10): now decode the embedded Sync
// header — verifying both messages are present, in order, with correct framing.
TEST(ProtocolMessage, PackAppendsSecondMessageWireBytes) {
    message fail(copy_fail_tag);
    fail.write(std::string("err")); // "err\0" -> 4 body bytes
    const auto first_length = static_cast<std::size_t>(([&] {
        (void) fail.buffer();
        return fail.length();
    })());

    message sync(sync_tag);
    fail.pack(sync);

    auto                     r = fail.buffer();
    std::vector<std::uint8_t> wire(r.first, r.second);

    // First message: tag at offset 0, then its int32 length (== body bytes, excl. tag).
    ASSERT_GE(wire.size(), 5u);
    EXPECT_EQ(wire[0], static_cast<std::uint8_t>(copy_fail_tag));
    std::uint32_t len0 = 0;
    std::memcpy(&len0, wire.data() + 1, sizeof(len0));
    EXPECT_EQ(ntohl(len0), static_cast<std::uint32_t>(first_length));

    // Embedded Sync begins right after the first message: 1 tag byte + first_length body.
    const std::size_t sync_off = 1 + first_length;
    ASSERT_GE(wire.size(), sync_off + 5);
    EXPECT_EQ(wire[sync_off], static_cast<std::uint8_t>(sync_tag));
    std::uint32_t sync_len = 0;
    std::memcpy(&sync_len, wire.data() + sync_off + 1, sizeof(sync_len));
    EXPECT_EQ(ntohl(sync_len), 4u); // Sync has no body: length field counts only itself (4).
    // The packed buffer ends exactly at the end of the Sync message.
    EXPECT_EQ(wire.size(), sync_off + 1 + 4);
}

TEST(ProtocolMessage, ResetReadOnHeaderOnlyMessage) {
    message m(query_tag);
    ASSERT_LE(m.buffer_size(), 5U);
    m.reset_read();
    char c{};
    EXPECT_FALSE(m.read(c));
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
