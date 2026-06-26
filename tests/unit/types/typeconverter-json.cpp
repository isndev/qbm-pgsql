/**
 * @file typeconverter-json.cpp
 * @brief Unit tests for the JSON / JSONB varlena converters and the std::optional
 *        scalar decode path.
 *
 * JSON wire = [int32 len][utf-8 text]. JSONB wire = [int32 varlena header][version
 * byte == 1][utf-8 text]; from_binary receives the VALUE bytes (the leading 4-byte
 * varlena header is present but ignored, the byte at offset 4 is the version). Split
 * out of the legacy monolith `test-data-types.cpp` (json / optional tests).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <optional>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "../pgsql.h"
#include "../../shared/pg_wire_ground_truth.hpp"

using namespace qb::pg;
using namespace qb::pg::detail;
using qb::pg::test::hex_to_bytes;

// ----------------------------------------------------------------------------
// get_oid()
// ----------------------------------------------------------------------------

TEST(TypeConverterJsonOid, KnownOids) {
    EXPECT_EQ(TypeConverter<qb::json>::get_oid(), static_cast<integer>(oid::json));
    EXPECT_EQ(TypeConverter<qb::jsonb>::get_oid(), static_cast<integer>(oid::jsonb));
}

// ----------------------------------------------------------------------------
// JSON (text varlena)
// ----------------------------------------------------------------------------

TEST(TypeConverterJsonTest, BinaryAndTextPaths) {
    // to_binary ([int32 len][json text]) -> from_binary round-trip.
    qb::json          obj = qb::json::parse(R"({"a":1})");
    std::vector<byte> buf;
    TypeConverter<qb::json>::to_binary(obj, buf);
    EXPECT_EQ(TypeConverter<qb::json>::from_binary(buf), obj);

    // Buffer <= 4 bytes -> "buffer too small" throw.
    EXPECT_THROW(TypeConverter<qb::json>::from_binary(hex_to_bytes("00000004")), std::runtime_error);

    // Key-value pair array payload [["k","v"]] -> converted to an object {"k":"v"}.
    {
        const std::string payload = R"([["k","v"]])";
        std::vector<byte> kv;
        kv.insert(kv.end(), 4, static_cast<byte>(0)); // 4-byte length prefix (skipped)
        kv.insert(kv.end(), payload.begin(), payload.end());
        auto parsed = TypeConverter<qb::json>::from_binary(kv);
        ASSERT_TRUE(parsed.is_object());
        EXPECT_EQ(parsed["k"], "v");
    }

    // from_text: valid parses, invalid throws.
    EXPECT_EQ(TypeConverter<qb::json>::from_text(R"({"x":true})"), qb::json::parse(R"({"x":true})"));
    EXPECT_THROW(TypeConverter<qb::json>::from_text("{not json"), std::runtime_error);
}

// Variety of JSON text shapes parse to the canonical dump.
TEST(TypeConverterJsonTest, TextFormatShapes) {
    const std::vector<std::string> cases = {
        R"({"id": 123, "name": "test"})",
        R"(["apple", "banana", "cherry"])",
        R"(42)",
        R"("simple string")",
        R"(true)",
        R"(null)",
        R"({"complex":{"nested":{"array":[1,2,3],"object":{"a":1,"b":2}},"types":[true,null,42,"string"]}})",
    };
    for (const auto &c : cases) {
        qb::jsonb expected(qb::json::parse(c));
        qb::jsonb result = TypeConverter<qb::jsonb>::from_text(c);
        EXPECT_EQ(result.dump(), expected.dump()) << "case: " << c;
    }
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_text(R"({"unclosed": "object")"), std::runtime_error);
}

// ----------------------------------------------------------------------------
// JSONB (versioned varlena)
// ----------------------------------------------------------------------------

TEST(TypeConverterJsonbTest, VarlenaBranchAndRoundTrip) {
    // to_binary ([int32 len][version 1][json]) -> from_binary round-trip.
    qb::jsonb         obj = qb::jsonb(qb::json::parse(R"({"a":1})"));
    std::vector<byte> buf;
    TypeConverter<qb::jsonb>::to_binary(obj, buf);
    EXPECT_EQ(TypeConverter<qb::jsonb>::from_binary(buf), obj);

    // 4-byte varlena header branch: bytes[4] == version 1, then key-value array
    // payload [["k","v"]] -> object {"k":"v"}.
    {
        const std::string payload = R"([["k","v"]])";
        std::vector<byte> wire;
        wire.insert(wire.end(), 4, static_cast<byte>(0)); // varlena header (ignored)
        wire.push_back(static_cast<byte>(1));             // jsonb version
        wire.insert(wire.end(), payload.begin(), payload.end());
        auto parsed = TypeConverter<qb::jsonb>::from_binary(wire);
        ASSERT_TRUE(parsed.is_object());
        EXPECT_EQ(parsed["k"], "v");
    }

    // Unversioned / unsupported leading bytes -> throw.
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_binary(hex_to_bytes("0203")), std::runtime_error);
}

// A realistic nested JSONB document decodes with all fields intact.
TEST(TypeConverterJsonbTest, NestedDocumentVersionedWire) {
    qb::jsonb doc = {
        {"id", 123},
        {"name", "test user"},
        {"active", true},
        {"scores", {98, 87, 95}},
        {"details", {{"address", "123 Test St"}, {"email", "test@example.com"}}},
    };
    const std::string json_str = doc.dump();

    // Build the JSONB wire VALUE: [int32 content-len][version 1][json text].
    std::vector<byte> wire;
    integer           content_size = static_cast<integer>(1 + json_str.size());
    integer           nbo          = htonl(content_size);
    wire.insert(wire.end(), reinterpret_cast<byte *>(&nbo), reinterpret_cast<byte *>(&nbo) + 4);
    wire.push_back(static_cast<byte>(1));
    wire.insert(wire.end(), json_str.begin(), json_str.end());

    qb::jsonb result = TypeConverter<qb::jsonb>::from_binary(wire);
    ASSERT_TRUE(result.is_object());
    EXPECT_EQ(result["id"].get<int>(), 123);
    EXPECT_EQ(result["name"].get<std::string>(), "test user");
    EXPECT_EQ(result["active"].get<bool>(), true);
    ASSERT_EQ(result["scores"].size(), 3u);
    EXPECT_EQ(result["scores"][0].get<int>(), 98);
    EXPECT_EQ(result["details"]["address"].get<std::string>(), "123 Test St");
    EXPECT_EQ(result["details"]["email"].get<std::string>(), "test@example.com");

    // ADD: truncated-after-version (header + version byte, no JSON text) -> throw,
    // never a silent empty object.
    std::vector<byte> truncated(wire.begin(), wire.begin() + 5);
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_binary(truncated), std::runtime_error);

    // Version byte 2 (unsupported) -> throw.
    std::vector<byte> badVersion = wire;
    badVersion[4] = static_cast<byte>(2);
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_binary(badVersion), std::runtime_error);
}

// ----------------------------------------------------------------------------
// std::optional scalar decode (the field VALUE always has bytes here; SQL NULL is
// decided upstream by field::as -> is_null())
// ----------------------------------------------------------------------------

// 0xFFFFFFFF is int4 -1 — a real value at this layer, NOT a SQL NULL.
TEST(TypeConverterOptionalTest, ValueDecodeIncludingMinusOne) {
    std::vector<byte> minus_one(4, static_cast<byte>(0xFF));
    auto              neg = TypeConverter<std::optional<integer>>::from_binary(minus_one);
    ASSERT_TRUE(neg.has_value());
    EXPECT_EQ(*neg, -1);

    auto v = TypeConverter<std::optional<integer>>::from_binary(hex_to_bytes("0000002a"));
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);

    // optional get_oid() delegates to the inner type's OID.
    EXPECT_EQ(TypeConverter<std::optional<integer>>::get_oid(), static_cast<integer>(oid::int4));
    EXPECT_EQ(TypeConverter<std::optional<bigint>>::get_oid(), static_cast<integer>(oid::int8));
}
