/**
 * @file datastructures.cpp
 * @brief Unit tests for the daemon-free pgsql data structures that the legacy
 *        "data-types" monolith mis-filed: resultset move/dtor semantics,
 *        row_data::null_map (vector<bool> bitmap), the ParamSerializer batch buffer,
 *        connection_options keepalive defaults, and the Reply<T>/Reply<void> monadic
 *        combinators. None of these are TypeConverter codecs.
 *
 * Pure logic, no daemon, no event loop. Split out of the legacy monolith
 * `test-data-types.cpp`.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <cstring>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "../pgsql.h"

using namespace qb::pg;
using namespace qb::pg::detail;

// ----------------------------------------------------------------------------
// resultset move semantics / dtor (regression for the P0-17 memory leak)
// ----------------------------------------------------------------------------

TEST(ResultsetDataStructure, MoveSemanticsAndDtor) {
    // Create+destroy many resultsets — ASan/valgrind catch a leak here.
    {
        std::vector<qb::pg::resultset> resultsets;
        resultsets.reserve(100);
        for (int i = 0; i < 100; ++i) {
            qb::pg::resultset rs;
            resultsets.push_back(std::move(rs));
        }
    }

    // Move-construct: the moved-to object is usable, the moved-from does not crash.
    {
        qb::pg::resultset original;
        qb::pg::resultset moved(std::move(original));
        EXPECT_NO_THROW({ (void) moved.empty(); });
    }

    // Move-assign: ownership transfers cleanly.
    {
        qb::pg::resultset rs1;
        qb::pg::resultset rs2;
        rs2 = std::move(rs1);
        EXPECT_NO_THROW({ (void) rs2.empty(); });
    }
}

// An empty resultset reports zero columns and is empty (honest name — the legacy
// "OutOfRangeThrows" never triggered the throw it claimed, so it is dropped in favor
// of the invariants that are actually observable without a live connection).
TEST(ResultsetDataStructure, EmptyInvariants) {
    qb::pg::resultset rs;
    EXPECT_EQ(rs.columns_size(), 0u);
    EXPECT_TRUE(rs.empty());
    EXPECT_EQ(rs.size(), 0u);
}

// ----------------------------------------------------------------------------
// row_data::null_map (vector<bool> bitmap — P0-4)
// ----------------------------------------------------------------------------

TEST(NullBitmapDataStructure, VectorBoolBitmap) {
    row_data row;
    row.null_map.resize(5, false); // 5 columns, all non-NULL initially
    row.null_map[1] = true;        // column 1 is NULL
    row.null_map[3] = true;        // column 3 is NULL

    EXPECT_FALSE(row.null_map[0]);
    EXPECT_TRUE(row.null_map[1]);
    EXPECT_FALSE(row.null_map[2]);
    EXPECT_TRUE(row.null_map[3]);
    EXPECT_FALSE(row.null_map[4]);
    EXPECT_EQ(row.null_map.size(), 5u);
}

// ----------------------------------------------------------------------------
// ParamSerializer batch buffer (P0-3) — assert the encoded bytes, not just a count
// ----------------------------------------------------------------------------

TEST(ParamSerializerBatch, StringVectorEncodesExactBytes) {
    ParamSerializer serializer;

    std::vector<std::string> values;
    for (int i = 0; i < 100; ++i)
        values.push_back("test_value_" + std::to_string(i));

    serializer.add_string_vector(values);

    // One param per string.
    EXPECT_EQ(serializer.param_count(), 100);

    // add_string_vector emits, per string, [int32 big-endian length][raw bytes] with
    // no leading count. Compute the exact total and verify it.
    std::size_t expected_bytes = 0;
    for (const auto &v : values)
        expected_bytes += sizeof(integer) + v.size();
    const auto &buffer = serializer.params_buffer();
    EXPECT_EQ(buffer.size(), expected_bytes);

    // reserve() (P0-3) means the capacity is at least the final size — no shrink.
    EXPECT_GE(buffer.capacity(), buffer.size());

    // Decode the first and last param straight out of the buffer to prove the layout.
    auto read_len = [&](std::size_t off) {
        integer be;
        std::memcpy(&be, buffer.data() + off, sizeof(integer));
        return static_cast<integer>(ntohl(be));
    };
    // First param.
    integer first_len = read_len(0);
    ASSERT_EQ(first_len, static_cast<integer>(values.front().size()));
    EXPECT_EQ(std::string(buffer.data() + sizeof(integer), buffer.data() + sizeof(integer) + first_len), values.front());

    // Last param (walk to its offset).
    std::size_t off = 0;
    for (std::size_t i = 0; i + 1 < values.size(); ++i)
        off += sizeof(integer) + values[i].size();
    integer last_len = read_len(off);
    ASSERT_EQ(last_len, static_cast<integer>(values.back().size()));
    EXPECT_EQ(std::string(buffer.data() + off + sizeof(integer), buffer.data() + off + sizeof(integer) + last_len), values.back());
}

// ----------------------------------------------------------------------------
// std::chrono::seconds INTERVAL conversion — assert the decoded VALUE, not size()
// ----------------------------------------------------------------------------

TEST(IntervalConversionDataStructure, ChronoDurationValueRoundTrip) {
    using namespace std::chrono;
    const auto duration = seconds(3600); // 1 hour

    std::vector<byte> buffer;
    TypeConverter<seconds>::to_binary(duration, buffer);
    // to_binary emits [int32 length == 16][int64 micros][int32 days][int32 months].
    ASSERT_EQ(buffer.size(), sizeof(integer) + 16u);
    integer len;
    std::memcpy(&len, buffer.data(), sizeof(integer));
    EXPECT_EQ(ntohl(len), 16);

    // OID is INTERVAL (1186).
    EXPECT_EQ(TypeConverter<seconds>::get_oid(), 1186);

    // The value round-trips through from_binary (strip the 4-byte length prefix).
    std::vector<byte> body(buffer.begin() + sizeof(integer), buffer.end());
    EXPECT_EQ(TypeConverter<seconds>::from_binary(body).count(), 3600);

    // Text spelling.
    EXPECT_EQ(TypeConverter<seconds>::to_text(duration), "3600 seconds");
}

// ----------------------------------------------------------------------------
// connection_options keepalive defaults (P1-1)
// ----------------------------------------------------------------------------

TEST(ConnectionOptionsDataStructure, KeepaliveDefaultsAndMutation) {
    qb::pg::connection_options opts;
    EXPECT_EQ(opts.keepalive_interval, 0); // disabled by default
    EXPECT_EQ(opts.keepalive_probes, 3);
    EXPECT_EQ(opts.keepalive_idle, 60);

    opts.keepalive_interval = 30;
    opts.keepalive_probes   = 5;
    opts.keepalive_idle     = 120;
    EXPECT_EQ(opts.keepalive_interval, 30);
    EXPECT_EQ(opts.keepalive_probes, 5);
    EXPECT_EQ(opts.keepalive_idle, 120);
}

// ----------------------------------------------------------------------------
// Reply<T> / Reply<void> std::expected-style monadic combinators
// ----------------------------------------------------------------------------

TEST(ReplyMonadicDataStructure, TransformAndThenOrElseValueOr) {
    using qb::pg::Reply;
    using qb::pg::error::db_error;

    const auto ok = Reply<int>::success(21);
    EXPECT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 21);
    EXPECT_EQ(ok.value_or(99), 21);
    EXPECT_EQ(ok.transform([](int x) { return x * 2; }).result(), 42);               // int -> int
    EXPECT_EQ(ok.transform([](int x) { return std::to_string(x); }).result(), "21"); // int -> string
    auto chained = ok.and_then([](int x) { return Reply<std::string>::success("v" + std::to_string(x)); });
    EXPECT_TRUE(chained.ok());
    EXPECT_EQ(chained.result(), "v21");

    const auto bad = Reply<int>::failure(db_error{"boom"});
    EXPECT_FALSE(bad.has_value());
    EXPECT_EQ(bad.value_or(7), 7);
    EXPECT_FALSE(bad.transform([](int x) { return x * 2; }).ok()); // f not called, error propagates
    EXPECT_FALSE(bad.and_then([](int) { return Reply<int>::success(1); }).ok());
    auto recovered = bad.or_else([](db_error const &) { return Reply<int>::success(123); });
    EXPECT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.result(), 123);

    // Reply<void>
    EXPECT_TRUE(Reply<void>::success().and_then([] { return Reply<int>::success(5); }).ok());
    EXPECT_FALSE(Reply<void>::failure(db_error{"x"}).and_then([] { return Reply<int>::success(5); }).ok());
    EXPECT_TRUE(Reply<void>::failure(db_error{"x"}).or_else([](db_error const &) { return Reply<void>::success(); }).ok());
}
