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
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <gtest/gtest.h>

#include "../../shared/pg_wire_ground_truth.hpp"
#include "../pgsql.h"

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
    std::vector<byte>  int2_neg(2, 0);
    const std::int16_t be = static_cast<std::int16_t>(htons(static_cast<std::uint16_t>(-42)));
    std::memcpy(int2_neg.data(), &be, sizeof(be));
    EXPECT_EQ(TypeConverter<integer>::from_binary(int2_neg), -42);
}

// bigint decode preserves the full 64-bit range (no int32 narrowing).
TEST(TypeConverterIntegerBinary, BigintDecodesFullRange) {
    std::vector<byte> max(8, 0);
    const bigint      be = static_cast<bigint>(qb::endian::to_big_endian(std::numeric_limits<bigint>::max()));
    std::memcpy(max.data(), &be, sizeof(be));
    EXPECT_EQ(TypeConverter<bigint>::from_binary(max), std::numeric_limits<bigint>::max());

    std::vector<byte> neg(8, 0);
    const bigint      be2 = static_cast<bigint>(qb::endian::to_big_endian(static_cast<bigint>(-9000000000LL)));
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

// ===========================================================================
// DataRow short-body guard — the 4-byte length field + 2-byte column count minimum.
// ===========================================================================

namespace {
/// Redirects `std::cerr` into a buffer for its lifetime, so a test can assert that a parse
/// wrote nothing to it. gtest reports its own failures on stdout, which stays untouched.
class cerr_capture {
    std::ostringstream _sink;
    std::streambuf    *_saved;

public:
    cerr_capture()
        : _saved(std::cerr.rdbuf(_sink.rdbuf())) {}
    ~cerr_capture() {
        std::cerr.rdbuf(_saved);
    }
    cerr_capture(cerr_capture const &)            = delete;
    cerr_capture &operator=(cerr_capture const &) = delete;

    /// Everything written to `std::cerr` since construction.
    [[nodiscard]] std::string
    str() const {
        return _sink.str();
    }
};
} // namespace

// A DataRow body that cannot even hold its own 4-byte length field plus the 2-byte column count
// is malformed and must be rejected — SILENTLY. Both bodies below are also caught downstream by
// the column-count read, so the stderr silence is what this test uniquely pins: the guard replaced
// an `assert` (compiled out of release builds) preceded by an unconditional `std::cerr` write, i.e.
// a synchronous unbuffered write from the I/O thread that a hostile or buggy server could drive at
// will, and which in release was the only trace a malformed row left.
TEST(ProtocolMessage, DataRowRejectsBodyShorterThanColumnCountSilently) {
    for (const std::size_t body : {std::size_t{4}, std::size_t{5}}) {
        message m(data_row_tag);
        if (body == 5)
            m.write(static_cast<char>(0x00)); // one stray byte: still one short of the column count
        (void) m.buffer();
        ASSERT_EQ(static_cast<std::size_t>(m.length()), body);
        ASSERT_EQ(m.size(), body); // fully-buffered message: declared length == bytes present
        m.reset_read();

        row_data    row;
        bool        accepted = true;
        std::string noise;
        {
            cerr_capture captured;
            accepted = m.read(row);
            noise    = captured.str();
        }
        EXPECT_FALSE(accepted) << "a " << body << "-byte DataRow body was accepted";
        EXPECT_TRUE(noise.empty()) << "parsing a " << body << "-byte DataRow wrote to stderr: " << noise;
        EXPECT_TRUE(row.empty());
    }
}

// The rejected parse must leave the caller's row untouched: `read()` swaps its temporary in only
// on success, so a malformed message cannot clobber the row already held.
TEST(ProtocolMessage, DataRowShortBodyLeavesDestinationRowUntouched) {
    message good(data_row_tag);
    good.write(static_cast<smallint>(1)); // 1 column
    good.write(static_cast<integer>(2));  // 2 bytes
    good.write('h');
    good.write('i');
    (void) good.buffer();
    good.reset_read();
    row_data row;
    ASSERT_TRUE(good.read(row));
    ASSERT_EQ(row.size(), 1u);

    message truncated(data_row_tag);
    truncated.write(static_cast<char>(0x00)); // 5-byte body, one short of the column count
    (void) truncated.buffer();
    truncated.reset_read();
    EXPECT_FALSE(truncated.read(row));
    EXPECT_EQ(row.size(), 1u) << "the rejected DataRow clobbered the row the caller already held";
    EXPECT_FALSE(row.is_null(0));
}

// Positive control for the guard's boundary. `len == 6` — the 4-byte length field plus an Int16
// column count of zero — is the SMALLEST LEGAL DataRow body and must be ACCEPTED: without this,
// the rejection test above cannot tell a correct bound from an off-by-one that eats valid rows.
TEST(ProtocolMessage, DataRowAcceptsMinimalZeroColumnBody) {
    message m(data_row_tag);
    m.write(static_cast<smallint>(0)); // zero columns
    (void) m.buffer();
    ASSERT_EQ(static_cast<std::size_t>(m.length()), sizeof(integer) + sizeof(smallint));
    m.reset_read();

    row_data row;
    EXPECT_TRUE(m.read(row));
    EXPECT_EQ(row.size(), 0u);
    EXPECT_TRUE(row.empty());
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

// A DataRow truncated mid-row (the per-field int32 length itself is cut short)
// must fail the field-length read, not over-read (distinct from the oversized and
// negative-count cases above, which fail earlier checks).
TEST(ProtocolMessage, DataRowRejectsTruncatedFieldLengthRead) {
    message m(data_row_tag);
    m.write(static_cast<smallint>(2)); // 2 columns
    m.write(static_cast<integer>(3));  // col0 length = 3
    m.write('a');
    m.write('b');
    m.write('c'); // col0 data
    m.write('x'); // only 1 of col1's 4 length bytes
    (void) m.buffer();
    m.reset_read();
    row_data row;
    EXPECT_FALSE(m.read(row));
}

// A field length below -1 is a protocol violation (only -1 == SQL NULL and >= 0
// are valid), rejected rather than treated as a huge size.
TEST(ProtocolMessage, DataRowRejectsInvalidNegativeFieldLength) {
    message m(data_row_tag);
    m.write(static_cast<smallint>(1)); // 1 column
    m.write(static_cast<integer>(-2)); // length < -1
    (void) m.buffer();
    m.reset_read();
    row_data row;
    EXPECT_FALSE(m.read(row));
}

// The move constructor transfers the framed payload (tag + body).
TEST(ProtocolMessage, MoveConstructorPreservesPayload) {
    message src(query_tag);
    src.write(std::string("SELECT 1"));
    (void) src.buffer();
    const auto len = src.length();

    message dst(std::move(src));
    EXPECT_EQ(dst.tag(), query_tag);
    EXPECT_EQ(dst.length(), len);
}

// A default-constructed (empty-payload) message reports the empty tag.
TEST(ProtocolMessage, DefaultMessageReportsEmptyTag) {
    message m;
    EXPECT_EQ(m.tag(), empty_tag);
}

// remaining() is empty once the read cursor has consumed the whole payload.
TEST(ProtocolMessage, RemainingIsEmptyAfterConsumingPayload) {
    message m(query_tag);
    m.write('a');
    m.reset_read();
    char c{};
    ASSERT_TRUE(m.read(c));
    EXPECT_TRUE(m.remaining().empty());
}

// A short read of an integer (fewer than 4 bytes) fails and leaves the target
// untouched (the integer analogue of the existing smallint short-read test).
TEST(ProtocolMessage, IntegerShortReadFailsAndLeavesTargetUntouched) {
    message m(query_tag);
    m.write(static_cast<char>(0x00)); // one byte; integer needs four
    m.reset_read();
    integer sentinel = -98765;
    EXPECT_FALSE(m.read(sentinel));
    EXPECT_EQ(sentinel, -98765);
}

// read(string, n) asking for more bytes than remain fails.
TEST(ProtocolMessage, ReadFixedStringInsufficientBytesFails) {
    message m(query_tag);
    m.write(std::string("ab"));
    m.reset_read();
    std::string s;
    EXPECT_FALSE(m.read(s, 5)); // only 2 bytes available
}

// row_data bounds: a default row is empty and any index is out of range.
TEST(ProtocolRowData, EmptyRowRejectsIndexAccess) {
    row_data a;
    EXPECT_TRUE(a.empty());
    EXPECT_EQ(a.size(), 0u);
    EXPECT_THROW(a.is_null(0), std::out_of_range);
}

// swap() exchanges row contents.
TEST(ProtocolRowData, SwapExchangesContents) {
    message m(data_row_tag);
    m.write(static_cast<smallint>(1));
    m.write(static_cast<integer>(2));
    m.write('h');
    m.write('i');
    (void) m.buffer();
    m.reset_read();
    row_data populated;
    ASSERT_TRUE(m.read(populated));
    ASSERT_EQ(populated.size(), 1u);

    row_data empty;
    populated.swap(empty);
    EXPECT_TRUE(populated.empty()); // contents moved out
    EXPECT_EQ(empty.size(), 1u);    // contents moved in
    EXPECT_FALSE(empty.is_null(0));
}

// notice_message::field() maps known single-byte codes to members and rejects
// an unknown code.
TEST(ProtocolNoticeMessage, FieldMapsKnownCodeAndRejectsUnknown) {
    notice_message n;
    n.field('S') = "ERROR"; // 'S' -> severity
    EXPECT_EQ(n.severity, "ERROR");
    EXPECT_THROW(n.field('Z'), std::runtime_error); // unknown code
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

    auto                      r = fail.buffer();
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

// ===========================================================================
// parse_header_attributes (pgsql.cpp) — the key=value[,;] attribute parser used
// to decode SASL/SCRAM auth header fields. Global-namespace free function
// declared in pgsql.h. Drives the name/value/quoted/ignore state machine and
// the control-char + max-length guards.
// ===========================================================================

namespace {
qb::icase_unordered_map<std::string>
parse_attrs(const std::string &s) {
    return ::parse_header_attributes(s.c_str(), s.size());
}
} // namespace

TEST(ParseHeaderAttributes, SimpleKeyValuePairs) {
    auto d = parse_attrs("r=abc,s=def,i=4096");
    EXPECT_EQ(d.at("r"), "abc");
    EXPECT_EQ(d.at("s"), "def");
    EXPECT_EQ(d.at("i"), "4096");
}

TEST(ParseHeaderAttributes, SemicolonAndCommaSeparators) {
    // ';' as a separator in NAME state with a pending (value-less) name, and as a
    // value terminator. Drives the name-terminator emplace (line 48) and the
    // value ';'/',' end branch.
    auto d = parse_attrs("k1=v1;k2=v2");
    EXPECT_EQ(d.at("k1"), "v1");
    EXPECT_EQ(d.at("k2"), "v2");
}

TEST(ParseHeaderAttributes, EmptyValueAndEmptyNameTokens) {
    // "a=" -> key with empty value; a stray ";;" yields empty names that are skipped.
    auto d = parse_attrs("a=;;b=2");
    EXPECT_EQ(d.at("a"), "");
    EXPECT_EQ(d.at("b"), "2");
}

TEST(ParseHeaderAttributes, LeadingWhitespaceInNameAndValueIgnored) {
    // Spaces in NAME state are skipped; leading unquoted whitespace in VALUE is
    // ignored until the first non-space (the `*ptr != ' ' || !value.empty()` guard).
    auto d = parse_attrs(" key =  value ");
    EXPECT_EQ(d.at("key"), "value ");
}

TEST(ParseHeaderAttributes, QuotedValuePreservesDelimitersThenIgnoresTrailing) {
    // A quoted value keeps the embedded ',' and ';'; after the closing quote the
    // parser enters IGNORE state and drops everything until the next ',' / ';'
    // (lines 90-111). Then a fresh attribute resumes.
    auto d = parse_attrs("k='a,b;c' junk , n=2");
    EXPECT_EQ(d.at("k"), "a,b;c");
    EXPECT_EQ(d.at("n"), "2");
}

TEST(ParseHeaderAttributes, DoubleQuotedValue) {
    auto d = parse_attrs("k=\"hello world\"");
    EXPECT_EQ(d.at("k"), "hello world");
}

TEST(ParseHeaderAttributes, QuoteCharMidUnquotedValueIsLiteral) {
    // A quote that appears after value bytes already accumulated is treated as a
    // literal value character (the `attribute_value.empty()` else-branch, line 79).
    auto d = parse_attrs("k=ab'cd");
    EXPECT_EQ(d.at("k"), "ab'cd");
}

TEST(ParseHeaderAttributes, ControlCharInNameThrows) {
    // A control byte in the attribute name trips the guard (line 55-56).
    std::string s = "ke\x01y=v";
    EXPECT_THROW(::parse_header_attributes(s.c_str(), s.size()), std::runtime_error);
}

TEST(ParseHeaderAttributes, ControlCharInUnquotedValueThrows) {
    // A control byte in an unquoted value trips the value guard (line 85-86).
    std::string s = "k=va\x02lue";
    EXPECT_THROW(::parse_header_attributes(s.c_str(), s.size()), std::runtime_error);
}

TEST(ParseHeaderAttributes, LastAttributeFlushedAtEndOfString) {
    // No trailing separator: the final attribute is emplaced after the loop.
    auto d = parse_attrs("only=tail");
    EXPECT_EQ(d.at("only"), "tail");
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
