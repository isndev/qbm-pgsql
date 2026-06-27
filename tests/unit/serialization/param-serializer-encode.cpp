/**
 * @file param-serializer-encode.cpp
 * @brief Unit tests for the PostgreSQL parameter ENCODER (`ParamSerializer`).
 *
 * Pure-logic, daemon-free: no `qb::Main`, no event loop, no `RESOURCE_LOCK`. Verifies
 * that C++ values are encoded into the exact wire bytes the extended-query Bind path
 * sends: the per-parameter `int32` length prefix, big-endian scalars, the
 * `serialize_params` `int16` parameter-count header, the 1-D array header, and the
 * binary NUMERIC / DATE limb formats. Where a serializer could "write zeros and still
 * pass" (the old length-only asserts), the value bytes are decoded and compared against
 * either the round-tripped C++ value or — for NUMERIC / DATE — PostgreSQL `*_send()`
 * ground-truth captured in `shared/pg_wire_ground_truth.hpp`.
 *
 * Restructured from the legacy `test-param-serializer.cpp` (spec §2 RENAME, §3 D1, §7):
 * dead `qb/io/async*` + coroutine includes, the unused `createBinaryBuffer` helper, and
 * every unconditional `printBuffer` stdout dump were stripped; the misnamed
 * `NetworkAddress` / `Time` / `TimestampText` / `UUIDText` cases (which only exercised
 * `add_string`) were deleted; the bytea quartet was de-duplicated; and the value-blind
 * `BigInt` / `Float` / `Double` / NaN / Inf / MIN / MAX / array tests now decode the
 * actual bytes. The lone sharp value-semantics guard from `test-param-parsing.cpp`
 * (`QueryParamsCopyAndMovePreserveContents`, spec D1) is folded in here.
 *
 * @see qb::pg::detail::ParamSerializer
 * @see qb::pg::detail::QueryParams
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */

#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <qb/system/endian.h>

#include "../pgsql.h"
#include "../../shared/pg_wire_ground_truth.hpp"

using namespace qb::pg;
using namespace qb::pg::detail;
using qb::pg::test::gt::numeric::Case;
using qb::pg::test::hex_to_bytes;

namespace {

// ---------------------------------------------------------------------------
// Big-endian wire readers over a flat parameter buffer. The serialized scalar
// layout (per add_*) is: [int32 length][value bytes]. serialize_params prepends
// an [int16 param_count] header before the first parameter.
// ---------------------------------------------------------------------------

template <typename T>
T
read_be(const std::vector<byte> &buf, std::size_t off) {
    EXPECT_GE(buf.size(), off + sizeof(T));
    T v{};
    std::memcpy(&v, buf.data() + off, sizeof(T));
    return qb::endian::from_big_endian(v);
}

float
read_be_float(const std::vector<byte> &buf, std::size_t off) {
    std::uint32_t bits = read_be<std::uint32_t>(buf, off);
    float         f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

double
read_be_double(const std::vector<byte> &buf, std::size_t off) {
    std::uint64_t bits = read_be<std::uint64_t>(buf, off);
    double        d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

std::string
read_str(const std::vector<byte> &buf, std::size_t off, std::size_t len) {
    EXPECT_GE(buf.size(), off + len);
    return std::string(buf.begin() + off, buf.begin() + off + len);
}

} // namespace

class ParamSerializerTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        serializer = std::make_unique<ParamSerializer>();
    }
    std::unique_ptr<ParamSerializer> serializer;
};

// ===========================================================================
// Scalar encoders — OID + length + exact value bytes.
// ===========================================================================

TEST_F(ParamSerializerTest, SmallIntEncodesOidLengthAndBigEndianValue) {
    serializer->add_smallint(12345);
    const auto &buf = serializer->params_buffer();

    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 21); // int2
    EXPECT_EQ(read_be<integer>(buf, 0), sizeof(smallint));
    EXPECT_EQ(read_be<smallint>(buf, sizeof(integer)), 12345);
}

TEST_F(ParamSerializerTest, IntegerEncodesOidLengthAndBigEndianValue) {
    serializer->add_integer(987654321);
    const auto &buf = serializer->params_buffer();

    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 23); // int4
    EXPECT_EQ(read_be<integer>(buf, 0), sizeof(integer));
    EXPECT_EQ(read_be<integer>(buf, sizeof(integer)), 987654321);
}

// Was BigIntSerialization, which checked only the length ("conversion too complex").
TEST_F(ParamSerializerTest, BigIntEncodesExactValue) {
    constexpr bigint kMax = std::numeric_limits<bigint>::max(); // 9223372036854775807
    serializer->add_bigint(kMax);
    const auto &buf = serializer->params_buffer();

    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 20); // int8
    EXPECT_EQ(read_be<integer>(buf, 0), sizeof(bigint));
    EXPECT_EQ(read_be<bigint>(buf, sizeof(integer)), kMax);

    serializer->reset();
    constexpr bigint kMin = std::numeric_limits<bigint>::min();
    serializer->add_bigint(kMin);
    const auto &buf2 = serializer->params_buffer();
    EXPECT_EQ(read_be<bigint>(buf2, sizeof(integer)), kMin);
}

// Was FloatSerialization, which checked only the length.
TEST_F(ParamSerializerTest, FloatEncodesExactIeee754BigEndian) {
    serializer->add_float(3.14159f);
    const auto &buf = serializer->params_buffer();

    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 700); // float4
    EXPECT_EQ(read_be<integer>(buf, 0), sizeof(float));
    EXPECT_FLOAT_EQ(read_be_float(buf, sizeof(integer)), 3.14159f);
}

// Was DoubleSerialization, which checked only the length.
TEST_F(ParamSerializerTest, DoubleEncodesExactIeee754BigEndian) {
    serializer->add_double(2.718281828459045);
    const auto &buf = serializer->params_buffer();

    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 701); // float8
    EXPECT_EQ(read_be<integer>(buf, 0), sizeof(double));
    EXPECT_DOUBLE_EQ(read_be_double(buf, sizeof(integer)), 2.718281828459045);
}

TEST_F(ParamSerializerTest, StringEncodesTextOidLengthAndContent) {
    const std::string s = "Hello, PostgreSQL!";
    serializer->add_string(s);
    const auto &buf = serializer->params_buffer();

    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 25); // text
    EXPECT_EQ(read_be<integer>(buf, 0), static_cast<integer>(s.size()));
    EXPECT_EQ(read_str(buf, sizeof(integer), s.size()), s);
}

TEST_F(ParamSerializerTest, EmptyStringEncodesZeroLength) {
    serializer->add_string("");
    const auto &buf = serializer->params_buffer();
    EXPECT_EQ(serializer->param_types()[0], 25);
    EXPECT_EQ(read_be<integer>(buf, 0), 0);
}

TEST_F(ParamSerializerTest, BooleanEncodesSingleByte) {
    serializer->add_bool(true);
    const auto &t = serializer->params_buffer();
    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 16); // boolean
    EXPECT_EQ(read_be<integer>(t, 0), 1);
    EXPECT_EQ(static_cast<unsigned char>(t[sizeof(integer)]), 1u);

    serializer->reset();
    serializer->add_bool(false);
    const auto &f = serializer->params_buffer();
    EXPECT_EQ(read_be<integer>(f, 0), 1);
    EXPECT_EQ(static_cast<unsigned char>(f[sizeof(integer)]), 0u);
}

TEST_F(ParamSerializerTest, NullEncodesMinusOneLength) {
    serializer->add_null();
    const auto &buf = serializer->params_buffer();
    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 0); // null OID
    EXPECT_EQ(read_be<integer>(buf, 0), -1);
}

TEST_F(ParamSerializerTest, OptionalWithValueEncodesUnderlyingNoneEncodesNull) {
    std::optional<std::string> some = "Optional String";
    serializer->add_optional(some, &ParamSerializer::add_string);
    {
        const auto &buf = serializer->params_buffer();
        EXPECT_EQ(serializer->param_types()[0], 25);
        EXPECT_EQ(read_be<integer>(buf, 0), static_cast<integer>(some->size()));
        EXPECT_EQ(read_str(buf, sizeof(integer), some->size()), *some);
    }
    serializer->reset();
    std::optional<std::string> none;
    serializer->add_optional(none, &ParamSerializer::add_string);
    {
        const auto &buf = serializer->params_buffer();
        EXPECT_EQ(serializer->param_types()[0], 0);
        EXPECT_EQ(read_be<integer>(buf, 0), -1);
    }
}

// ===========================================================================
// Extreme + special floating-point values — assert the actual encoded bytes,
// not just OID/length (was SpecialFloatingPointValues / ExtremeValuesSerialization,
// which a serializer writing all-zeros would have passed, spec §7.6).
// ===========================================================================

TEST_F(ParamSerializerTest, FloatLimitsEncodeExactValues) {
    constexpr float kMin = std::numeric_limits<float>::min();
    constexpr float kMax = std::numeric_limits<float>::max();

    serializer->add_float(kMin);
    EXPECT_FLOAT_EQ(read_be_float(serializer->params_buffer(), sizeof(integer)), kMin);
    serializer->reset();
    serializer->add_float(kMax);
    EXPECT_FLOAT_EQ(read_be_float(serializer->params_buffer(), sizeof(integer)), kMax);
}

TEST_F(ParamSerializerTest, DoubleLimitsEncodeExactValues) {
    constexpr double kMin = std::numeric_limits<double>::min();
    constexpr double kMax = std::numeric_limits<double>::max();

    serializer->add_double(kMin);
    EXPECT_DOUBLE_EQ(read_be_double(serializer->params_buffer(), sizeof(integer)), kMin);
    serializer->reset();
    serializer->add_double(kMax);
    EXPECT_DOUBLE_EQ(read_be_double(serializer->params_buffer(), sizeof(integer)), kMax);
}

TEST_F(ParamSerializerTest, FloatNanInfEncodeBitPatterns) {
    serializer->add_float(std::numeric_limits<float>::quiet_NaN());
    EXPECT_TRUE(std::isnan(read_be_float(serializer->params_buffer(), sizeof(integer))));

    serializer->reset();
    serializer->add_float(std::numeric_limits<float>::infinity());
    EXPECT_EQ(read_be<std::uint32_t>(serializer->params_buffer(), sizeof(integer)), 0x7F800000u);

    serializer->reset();
    serializer->add_float(-std::numeric_limits<float>::infinity());
    EXPECT_EQ(read_be<std::uint32_t>(serializer->params_buffer(), sizeof(integer)), 0xFF800000u);
}

TEST_F(ParamSerializerTest, DoubleNanInfEncodeBitPatterns) {
    serializer->add_double(std::numeric_limits<double>::quiet_NaN());
    EXPECT_TRUE(std::isnan(read_be_double(serializer->params_buffer(), sizeof(integer))));

    serializer->reset();
    serializer->add_double(std::numeric_limits<double>::infinity());
    EXPECT_EQ(read_be<std::uint64_t>(serializer->params_buffer(), sizeof(integer)), 0x7FF0000000000000ull);

    serializer->reset();
    serializer->add_double(-std::numeric_limits<double>::infinity());
    EXPECT_EQ(read_be<std::uint64_t>(serializer->params_buffer(), sizeof(integer)), 0xFFF0000000000000ull);
}

// ===========================================================================
// bytea — one parametrized case replacing the legacy quartet
// (ByteArray / LargeBinaryData / UUIDBinaryFormat / TimestampBinaryFormat, spec §2 dedup).
// ===========================================================================

class ByteaEncodeTest : public ::testing::TestWithParam<std::size_t> {};

TEST_P(ByteaEncodeTest, EncodesByteaOidLengthAndExactBytes) {
    const std::size_t n = GetParam();
    std::vector<byte> data(n);
    for (std::size_t i = 0; i < n; ++i)
        data[i] = static_cast<byte>(i & 0xFF);

    ParamSerializer s;
    s.add_byte_array(data.data(), data.size());
    const auto &buf = s.params_buffer();

    ASSERT_EQ(s.param_count(), 1);
    EXPECT_EQ(s.param_types()[0], 17); // bytea
    EXPECT_EQ(read_be<integer>(buf, 0), static_cast<integer>(n));
    for (std::size_t i = 0; i < n; ++i)
        ASSERT_EQ(static_cast<unsigned char>(buf[sizeof(integer) + i]), static_cast<unsigned char>(i & 0xFF))
            << "byte mismatch at " << i;
}

INSTANTIATE_TEST_SUITE_P(Sizes, ByteaEncodeTest, ::testing::Values<std::size_t>(0, 1, 16, 256, 512));

// A C-string / string-literal param (decayed type `const char*`, which has no TypeConverter) must
// route through the std::string path and serialize IDENTICALLY to the equivalent std::string param.
// This is the live, tested replacement for the never-wired param_serializer_traits<const char*>;
// without the add_param const-char* branch, `params("text")` is a TypeConverter<char*> static_assert.
TEST_F(ParamSerializerTest, CStringLiteralSerializesIdenticallyToStdString) {
    ParamSerializer lit, str;
    lit.add_param("hello");              // const char* literal
    str.add_param(std::string("hello")); // std::string

    ASSERT_EQ(lit.param_count(), 1);
    EXPECT_EQ(lit.param_types(), str.param_types());     // identical OID
    EXPECT_EQ(lit.params_buffer(), str.params_buffer()); // byte-identical wire payload

    const auto &buf = lit.params_buffer();
    ASSERT_GE(buf.size(), sizeof(integer) + 5);
    EXPECT_EQ(read_be<integer>(buf, 0), 5); // int32 length prefix
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(buf.data() + sizeof(integer)), 5), "hello");
}

// ===========================================================================
// serialize_params header + 1-D array body.
// ===========================================================================

TEST_F(ParamSerializerTest, SerializeParamsPrependsBigEndianParamCount) {
    serializer->serialize_params(42, std::string("Test"), true);
    const auto &buf = serializer->params_buffer();
    ASSERT_GE(buf.size(), 2u);
    EXPECT_EQ(read_be<smallint>(buf, 0), 3);
}

// Was IntVectorSerialization, which only checked length>0. Decode the full 1-D
// header and the first/last element value (spec §7.6).
TEST_F(ParamSerializerTest, IntVectorEncodesArrayHeaderAndElements) {
    std::vector<int> v = {1, 2, 3, 4, 5};
    serializer->serialize_params(v);
    const auto &buf = serializer->params_buffer();

    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 1007); // int4[]

    // [int16 param_count][int32 array_len][array body...]
    std::size_t off = sizeof(smallint);
    const integer array_len = read_be<integer>(buf, off);
    ASSERT_GT(array_len, 0);
    off += sizeof(integer);

    // 1-D array header (20 bytes): ndim, has-null, elem OID, dim size, lower bound.
    EXPECT_EQ(read_be<integer>(buf, off + 0), 1);   // ndim
    EXPECT_EQ(read_be<integer>(buf, off + 4), 0);   // has-null
    EXPECT_EQ(read_be<integer>(buf, off + 8), 23);  // int4 elem OID
    EXPECT_EQ(read_be<integer>(buf, off + 12), 5);  // dim size
    EXPECT_EQ(read_be<integer>(buf, off + 16), 1);  // lower bound

    // First element: [int32 len=4][int32 value=1].
    std::size_t e0 = off + 20;
    EXPECT_EQ(read_be<integer>(buf, e0), 4);
    EXPECT_EQ(read_be<integer>(buf, e0 + 4), 1);
    // Last element value == 5 (each element is 8 bytes: len + int4).
    std::size_t e4 = off + 20 + 4 * 8;
    EXPECT_EQ(read_be<integer>(buf, e4), 4);
    EXPECT_EQ(read_be<integer>(buf, e4 + 4), 5);
}

// Was DoubleVectorSerialization (length>0 only). Decode header + first/last value.
TEST_F(ParamSerializerTest, DoubleVectorEncodesArrayHeaderAndElements) {
    std::vector<double> v = {1.5, 2.5, 3.5};
    serializer->serialize_params(v);
    const auto &buf = serializer->params_buffer();

    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 1022); // float8[]

    std::size_t off = sizeof(smallint) + sizeof(integer); // skip param-count + array-len
    EXPECT_EQ(read_be<integer>(buf, off + 0), 1);    // ndim
    EXPECT_EQ(read_be<integer>(buf, off + 8), 701);  // float8 elem OID
    EXPECT_EQ(read_be<integer>(buf, off + 12), 3);   // dim size

    std::size_t e0 = off + 20;
    EXPECT_EQ(read_be<integer>(buf, e0), 8);
    EXPECT_DOUBLE_EQ(read_be_double(buf, e0 + 4), 1.5);
    std::size_t e2 = off + 20 + 2 * 12; // each float8 element is 12 bytes (len + 8)
    EXPECT_DOUBLE_EQ(read_be_double(buf, e2 + 4), 3.5);
}

// Was BoolVectorSerialization (length>0 only). Decode header + element bytes.
TEST_F(ParamSerializerTest, BoolVectorEncodesArrayHeaderAndElements) {
    std::vector<bool> v = {true, false, true};
    serializer->serialize_params(v);
    const auto &buf = serializer->params_buffer();

    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 1000); // boolean[]

    std::size_t off = sizeof(smallint) + sizeof(integer);
    EXPECT_EQ(read_be<integer>(buf, off + 0), 1);   // ndim
    EXPECT_EQ(read_be<integer>(buf, off + 8), 16);  // boolean elem OID
    EXPECT_EQ(read_be<integer>(buf, off + 12), 3);  // dim size

    // Each bool element: [int32 len=1][1 byte]. Element stride = 5.
    std::size_t e0 = off + 20;
    EXPECT_EQ(read_be<integer>(buf, e0), 1);
    EXPECT_EQ(static_cast<unsigned char>(buf[e0 + 4]), 1u); // true
    std::size_t e1 = e0 + 5;
    EXPECT_EQ(static_cast<unsigned char>(buf[e1 + 4]), 0u); // false
    std::size_t e2 = e0 + 10;
    EXPECT_EQ(static_cast<unsigned char>(buf[e2 + 4]), 1u); // true
}

TEST_F(ParamSerializerTest, EmptyVectorEncodesEmptyArrayNotNull) {
    // An empty std::vector binds an EMPTY ARRAY ('{}'), not SQL NULL: a non-NULL value
    // (length >= 0) whose body is PostgreSQL's zero-dimension array header
    // [ndim=0][hasnull=0][elemtype]. (Binding NULL would change `col = ANY($1)` and
    // array_length semantics.)
    std::vector<int> empty;
    serializer->serialize_params(empty);
    const auto &buf = serializer->params_buffer();
    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 1007);                 // int4[]
    EXPECT_EQ(read_be<integer>(buf, sizeof(smallint)), 20);        // value length = 20 (NOT -1/NULL)
    const std::size_t body = sizeof(smallint) + sizeof(integer);
    EXPECT_EQ(read_be<integer>(buf, body + 0), 1);                 // ndim = 1
    EXPECT_EQ(read_be<integer>(buf, body + 4), 0);                 // has-nulls = 0
    EXPECT_EQ(read_be<integer>(buf, body + 8), 23);                // element OID = int4
    EXPECT_EQ(read_be<integer>(buf, body + 12), 0);               // dimension size = 0
    EXPECT_EQ(read_be<integer>(buf, body + 16), 1);               // lower bound = 1
}

TEST_F(ParamSerializerTest, DifferentNumericVectorTypesGetCorrectArrayOids) {
    serializer->reset();
    serializer->serialize_params(std::vector<smallint>{1, 2, 3});
    EXPECT_EQ(serializer->param_types()[0], 1005); // int2[]
    serializer->reset();
    serializer->serialize_params(std::vector<bigint>{1, 2, 3});
    EXPECT_EQ(serializer->param_types()[0], 1016); // int8[]
    serializer->reset();
    serializer->serialize_params(std::vector<float>{1.f, 2.f, 3.f});
    EXPECT_EQ(serializer->param_types()[0], 1021); // float4[]
}

TEST_F(ParamSerializerTest, MixedParametersWithVectorGetCorrectOidsAndCount) {
    serializer->serialize_params(std::string("Test string"), std::vector<int>{10, 20, 30}, 42);
    const auto &buf = serializer->params_buffer();
    ASSERT_EQ(serializer->param_count(), 3);
    EXPECT_EQ(serializer->param_types()[0], 25);   // text
    EXPECT_EQ(serializer->param_types()[1], 1007); // int4[]
    EXPECT_EQ(serializer->param_types()[2], 23);   // int4
    EXPECT_EQ(read_be<smallint>(buf, 0), 3);
}

// array_oid_for_element is the single source of truth for the Bind array OID. It must
// map every supported scalar to its concrete _T array OID (never anyarray 2277, which
// PostgreSQL rejects as a parameter type) and return invalid for the unmappable.
TEST(ArrayOidMapping, MapsEverySupportedElementToConcreteArrayOidNotAnyarray) {
    EXPECT_EQ(array_oid_for_element(oid::boolean), oid::boolean_array);
    EXPECT_EQ(array_oid_for_element(oid::bytea), oid::bytea_array);
    EXPECT_EQ(array_oid_for_element(oid::int2), oid::int2_array);
    EXPECT_EQ(array_oid_for_element(oid::int4), oid::int4_array);
    EXPECT_EQ(array_oid_for_element(oid::int8), oid::int8_array);
    EXPECT_EQ(array_oid_for_element(oid::oid_t), oid::oid_array);
    EXPECT_EQ(array_oid_for_element(oid::float4), oid::float4_array);
    EXPECT_EQ(array_oid_for_element(oid::float8), oid::float8_array);
    EXPECT_EQ(array_oid_for_element(oid::numeric), oid::numeric_array);
    EXPECT_EQ(array_oid_for_element(oid::text), oid::text_array);
    EXPECT_EQ(array_oid_for_element(oid::varchar), oid::varchar_array);
    EXPECT_EQ(array_oid_for_element(oid::bpchar), oid::bpchar_array);
    EXPECT_EQ(array_oid_for_element(oid::uuid), oid::uuid_array);
    EXPECT_EQ(array_oid_for_element(oid::json), oid::json_array);
    EXPECT_EQ(array_oid_for_element(oid::jsonb), oid::jsonb_array);
    EXPECT_EQ(array_oid_for_element(oid::date), oid::date_array);
    EXPECT_EQ(array_oid_for_element(oid::time), oid::time_array);
    EXPECT_EQ(array_oid_for_element(oid::timetz), oid::timetz_array);
    EXPECT_EQ(array_oid_for_element(oid::timestamp), oid::timestamp_array);
    EXPECT_EQ(array_oid_for_element(oid::timestamptz), oid::timestamptz_array);
    EXPECT_EQ(array_oid_for_element(oid::interval), oid::interval_array);
    // Unmapped element types -> invalid (the caller throws rather than emit anyarray).
    EXPECT_EQ(array_oid_for_element(oid::point), oid::invalid);
    EXPECT_EQ(array_oid_for_element(oid::any_array), oid::invalid);
    // ...and it is usable in a constant expression.
    static_assert(array_oid_for_element(oid::int4) == oid::int4_array);
}

// Regression: element types ABSENT from the old switch (numeric, uuid, json, ...) used
// to fall back to anyarray (2277); now they bind their concrete array OID end-to-end.
TEST_F(ParamSerializerTest, PreviouslyAnyarrayElementVectorsNowGetConcreteArrayOid) {
    serializer->reset();
    serializer->serialize_params(std::vector<numeric>{numeric("1.5"), numeric("2.5")});
    EXPECT_EQ(serializer->param_types()[0], static_cast<integer>(oid::numeric_array)); // 1231

    serializer->reset();
    serializer->serialize_params(std::vector<qb::json>{qb::json::parse(R"({"a":1})")});
    EXPECT_EQ(serializer->param_types()[0], static_cast<integer>(oid::json_array)); // 199
}

// std::vector<std::string> is the ONE intentional exception to "vector -> array param":
// it expands to N separate text params (batch VALUES ($1),($2),...), via add_string_vector,
// NOT a single text[] array. This pins that documented asymmetry so the array-OID unification
// does not silently absorb it.
TEST_F(ParamSerializerTest, StringVectorStaysMultiParamTextNotTextArray) {
    serializer->serialize_params(std::vector<std::string>{"a", "b", "c"});
    ASSERT_EQ(serializer->param_count(), 3);
    EXPECT_EQ(serializer->param_types()[0], static_cast<integer>(oid::text)); // 25, per-string
    EXPECT_EQ(serializer->param_types()[1], static_cast<integer>(oid::text)); // 25
    EXPECT_EQ(serializer->param_types()[2], static_cast<integer>(oid::text)); // 25
}

// ===========================================================================
// reset() round-trip.
// ===========================================================================

TEST_F(ParamSerializerTest, ResetClearsThenAllowsReuse) {
    serializer->add_integer(12345);
    ASSERT_EQ(serializer->param_count(), 1);
    ASSERT_FALSE(serializer->params_buffer().empty());

    serializer->reset();
    EXPECT_EQ(serializer->param_count(), 0);
    EXPECT_TRUE(serializer->params_buffer().empty());
    EXPECT_TRUE(serializer->param_types().empty());

    serializer->add_string("after reset");
    const auto &buf = serializer->params_buffer();
    EXPECT_EQ(serializer->param_types()[0], 25);
    EXPECT_EQ(read_str(buf, sizeof(integer), read_be<integer>(buf, 0)), "after reset");
}

// ===========================================================================
// NUMERIC / DATE — anchored to PostgreSQL *_send() ground truth, not self
// round-trips (spec §7.2). Encode the C++ value and compare the value bytes
// (after the 4-byte length prefix to_binary emits) against the golden literal.
// ===========================================================================

TEST_F(ParamSerializerTest, NumericToBinaryMatchesServerSendBytes) {
    EXPECT_EQ(TypeConverter<numeric>::get_oid(), 1700);

    for (const Case &c : qb::pg::test::gt::numeric::finite) {
        std::vector<byte> buf;
        TypeConverter<numeric>::to_binary(numeric(c.expect), buf);
        // to_binary frames as [int32 length][value bytes]; compare the value bytes.
        ASSERT_GE(buf.size(), sizeof(integer));
        const integer len = read_be<integer>(buf, 0);
        ASSERT_EQ(static_cast<std::size_t>(len), buf.size() - sizeof(integer)) << c.expect;
        std::vector<byte> value(buf.begin() + sizeof(integer), buf.end());
        EXPECT_EQ(value, hex_to_bytes(c.hex)) << "NUMERIC encode mismatch for " << c.expect;
    }
}

TEST_F(ParamSerializerTest, NumericToBinaryEncodesSpecialSignWords) {
    // Asserted against qb's ACTUAL encoder output, NOT the numeric_send golden, because
    // they legitimately differ in dscale for the infinities: PostgreSQL's numeric_send
    // emits dscale=0x0020 for ±Infinity (golden d0000020 / f0000020), whereas qb's
    // encode_pg_numeric emits a non-canonical dscale=0x0000 (d0000000 / f0000000). This
    // is wire-compatible — numeric_recv ignores dscale for non-finite values and accepts
    // the dscale=0 form (verified on PG 18.4 via a binary-COPY round trip). NaN matches
    // the golden exactly (both emit dscale=0). The framework encoder is intentionally
    // left unchanged; this test pins the wire-safe bytes qb produces.
    struct Expected {
        const char *value;
        const char *qb_hex; // value bytes qb's to_binary emits (after the length prefix)
    };
    static constexpr Expected expected[] = {
        {"NaN", "00000000c0000000"},       // matches numeric_send (dscale=0)
        {"Infinity", "00000000d0000000"},  // qb dscale=0; numeric_send canonical is d0000020
        {"-Infinity", "00000000f0000000"}, // qb dscale=0; numeric_send canonical is f0000020
    };
    for (const Expected &e : expected) {
        std::vector<byte> buf;
        TypeConverter<numeric>::to_binary(numeric(e.value), buf);
        ASSERT_GE(buf.size(), sizeof(integer)) << e.value;
        std::vector<byte> value(buf.begin() + sizeof(integer), buf.end());
        EXPECT_EQ(value, hex_to_bytes(e.qb_hex)) << "NUMERIC special encode mismatch for " << e.value;
    }
}

TEST_F(ParamSerializerTest, DateToBinaryMatchesServerSendBytes) {
    EXPECT_EQ(TypeConverter<qb::date>::get_oid(), 1082);

    qb::date          d = qb::date::parse("2024-03-15").value();
    std::vector<byte> buf;
    TypeConverter<qb::date>::to_binary(d, buf);

    // [int32 length=4][int32 days since 2000-01-01] == 0x00002288 (8840) ground truth.
    ASSERT_EQ(buf.size(), 8u);
    EXPECT_EQ(read_be<integer>(buf, 0), 4);
    std::vector<byte> value(buf.begin() + sizeof(integer), buf.end());
    EXPECT_EQ(value, hex_to_bytes(qb::pg::test::gt::temporal::date_2024_03_15));
}

TEST_F(ParamSerializerTest, DateToBinaryEncodesPre2000NegativeDays) {
    // 1990-01-01 is 3652 days before 2000-01-01 (leap days 1992 & 1996) => negative
    // day count on the wire.
    qb::date          d = qb::date::parse("1990-01-01").value();
    std::vector<byte> buf;
    TypeConverter<qb::date>::to_binary(d, buf);
    ASSERT_EQ(buf.size(), 8u);
    const integer days = read_be<integer>(buf, sizeof(integer));
    EXPECT_EQ(days, -3652);
    // And the value round-trips back through from_binary (value-bytes only).
    std::vector<byte> value(buf.begin() + sizeof(integer), buf.end());
    EXPECT_EQ(TypeConverter<qb::date>::from_binary(value).to_string(), "1990-01-01");
}

// ===========================================================================
// JSONB — version byte + content shape (was JSONSerialization, kept but trimmed
// of stdout). The binary jsonb param format is `[int32 len][version=1][text]`.
// ===========================================================================

TEST_F(ParamSerializerTest, JsonbEncodesVersionByteAndParsableContent) {
    qb::jsonb j = {
        {"id", 12345},
        {"name", "Test JSON"},
        {"active", true},
        {"nullable", nullptr},
    };
    serializer->add_param(j);
    const auto &buf = serializer->params_buffer();

    ASSERT_EQ(serializer->param_count(), 1);
    EXPECT_EQ(serializer->param_types()[0], 3802); // jsonb

    const integer len = read_be<integer>(buf, 0);
    ASSERT_EQ(buf.size(), static_cast<std::size_t>(len) + sizeof(integer));
    EXPECT_EQ(static_cast<unsigned char>(buf[sizeof(integer)]), 1u); // jsonb version 1

    const std::string content = read_str(buf, sizeof(integer) + 1, buf.size() - sizeof(integer) - 1);
    auto parsed = qb::json::parse(content); // the encoder emits an array of [key,value] pairs
    bool saw_id = false;
    for (const auto &pair : parsed) {
        ASSERT_TRUE(pair.is_array());
        ASSERT_EQ(pair.size(), 2u);
        if (pair[0].get<std::string>() == "id") {
            EXPECT_EQ(pair[1].get<int>(), 12345);
            saw_id = true;
        }
    }
    EXPECT_TRUE(saw_id);
}

// ===========================================================================
// QueryParams value semantics — folded from test-param-parsing.cpp (spec D1).
// The variadic forwarding ctor must NOT swallow a QueryParams copy/move.
// ===========================================================================

TEST(QueryParamsValueSemantics, CopyAndMovePreserveContents) {
    QueryParams original(123, std::string("abc"), true);
    ASSERT_EQ(original.param_count(), 3);

    QueryParams copy(original); // copy-construct from non-const lvalue: must duplicate
    EXPECT_EQ(copy.param_count(), original.param_count());
    EXPECT_EQ(copy.param_types(), original.param_types());
    EXPECT_EQ(copy.get(), original.get());
    EXPECT_EQ(original.param_count(), 3); // source untouched

    QueryParams assigned;
    assigned = original;
    EXPECT_EQ(assigned.get(), original.get());

    const auto  before = original.get();
    QueryParams moved(std::move(original));
    EXPECT_EQ(moved.get(), before);
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
