/**
 * @file test-protocol-message.cpp
 * @brief Unit tests for PostgreSQL wire message helpers (no database)
 *
 * Covers message framing helpers used by the client: length encoding, read cursor,
 * discard_remaining, tag sets, packed multi-message sends (CopyFail+Sync), and the
 * fixed 8-byte SSLRequest pre-startup packet (PostgreSQL protocol documentation).
 */

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include "../pgsql.h"

using namespace qb::pg;
using namespace qb::pg::detail;

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

// Prepared-query int8 columns (e.g. COUNT(*)) are 8-byte big-endian; as<int32_t>() must
// not use only the first four octets (that yields 0 for small counts).
TEST(TypeConverterIntegerBinary, Int8WireDecodesAsInt32) {
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
    const auto decoded = static_cast<size_t>(m.length());
    EXPECT_EQ(decoded, m.size());
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
    EXPECT_EQ(col_cnt, -12345); // must be left exactly as it was
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

TEST(ProtocolMessage, PackAppendsSecondMessageWireBytes) {
    message fail(copy_fail_tag);
    fail.write(std::string("err"));
    message sync(sync_tag);
    fail.pack(sync);
    auto         r = fail.buffer();
    const size_t n = static_cast<size_t>(std::distance(r.first, r.second));
    EXPECT_GE(n, 10U);
    EXPECT_EQ(static_cast<unsigned char>(*r.first), static_cast<unsigned char>(copy_fail_tag));
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
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
