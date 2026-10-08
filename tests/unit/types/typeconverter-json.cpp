/**
 * @file typeconverter-json.cpp
 * @brief Unit tests for the JSON / JSONB varlena converters and the std::optional
 *        scalar decode path.
 *
 * JSON result wire = UTF-8 text. JSONB result wire = [version byte == 1][UTF-8
 * text]; the decoder also accepts a legacy 4-byte-prefixed shape. The protocol
 * strips each field's length prefix before from_binary. Split out of the legacy
 * monolith `test-data-types.cpp` (json / optional tests).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>
#include "../../shared/pg_wire_ground_truth.hpp"
#include <qbm/pgsql/pgsql.h>

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

// JSON / JSONB to_text delegate to nlohmann::dump() (compact, key-sorted for jsonb).
TEST(TypeConverterJsonToText, DumpRoundTrip) {
    qb::json j = qb::json::parse(R"({"a":1,"b":[true,null]})");
    EXPECT_EQ(TypeConverter<qb::json>::to_text(j), j.dump());

    qb::jsonb jb(qb::json::parse(R"({"z":2,"a":1})"));
    EXPECT_EQ(TypeConverter<qb::jsonb>::to_text(jb), jb.dump());
    // to_text must yield valid JSON that re-parses to the same value.
    EXPECT_EQ(qb::json::parse(TypeConverter<qb::json>::to_text(j)), j);
}

// ----------------------------------------------------------------------------
// JSON (text varlena)
// ----------------------------------------------------------------------------

TEST(TypeConverterJsonTest, BinaryAndTextPaths) {
    // from_binary receives the field VALUE bytes: the protocol already stripped the
    // per-field length prefix, and `json` (unlike `jsonb`) has no version byte, so the
    // bytes ARE the JSON text. This is NOT a to_binary round-trip — to_binary writes the
    // Bind [int32 len] framing, which from_binary must never see.
    auto value_of = [](std::string_view s) {
        return std::vector<byte>(s.data(), s.data() + s.size());
    };

    const std::string text = R"({"a":1})";
    EXPECT_EQ(TypeConverter<qb::json>::from_binary(value_of(text)), qb::json::parse(text));

    // Empty buffer -> throw.
    EXPECT_THROW(TypeConverter<qb::json>::from_binary(std::vector<byte>{}), std::runtime_error);

    // A JSON array of pairs is still an array: PostgreSQL does not tag it as
    // an object on the binary wire.
    {
        const std::string text   = R"([["k","v"]])";
        auto              parsed = TypeConverter<qb::json>::from_binary(value_of(text));
        ASSERT_TRUE(parsed.is_array());
        EXPECT_EQ(parsed, qb::json::parse(text));
    }

    // Numeric pair keys and nested pair arrays are likewise ordinary JSON arrays.
    {
        const std::string text   = R"([[1,"a"],[2,"b"]])";
        auto              parsed = TypeConverter<qb::json>::from_binary(value_of(text));
        ASSERT_TRUE(parsed.is_array());
        EXPECT_EQ(parsed, qb::json::parse(text));
        const std::string nested = R"([[[1,2]],[[3,4]]])";
        EXPECT_EQ(TypeConverter<qb::json>::from_binary(value_of(nested)), qb::json::parse(nested));
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

// A top-level JSON array without pair elements also keeps its array shape.
TEST(TypeConverterJsonTest, FromBinaryPlainArrayNotKeyValue) {
    const std::string text = "[1,2,3]";
    std::vector<byte> bytes(text.data(), text.data() + text.size());
    auto              parsed = TypeConverter<qb::json>::from_binary(bytes);
    ASSERT_TRUE(parsed.is_array());
    EXPECT_EQ(parsed, qb::json::parse(text));
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

    // 4-byte varlena header branch: bytes[4] == version 1, then an ordinary
    // JSON array of pairs, which must retain its array shape.
    {
        const std::string payload = R"([["k","v"]])";
        std::vector<byte> wire;
        wire.insert(wire.end(), 4, static_cast<byte>(0)); // varlena header (ignored)
        wire.push_back(static_cast<byte>(1));             // jsonb version
        wire.insert(wire.end(), payload.begin(), payload.end());
        auto parsed = TypeConverter<qb::jsonb>::from_binary(wire);
        ASSERT_TRUE(parsed.is_array());
        EXPECT_EQ(parsed, qb::jsonb(qb::json::parse(payload)));
    }

    // Numeric pair keys have no special object meaning either.
    {
        const std::string payload = R"([[10,"x"],[20,"y"]])";
        std::vector<byte> wire;
        wire.insert(wire.end(), 4, static_cast<byte>(0)); // varlena header (ignored)
        wire.push_back(static_cast<byte>(1));             // jsonb version
        wire.insert(wire.end(), payload.begin(), payload.end());
        auto parsed = TypeConverter<qb::jsonb>::from_binary(wire);
        ASSERT_TRUE(parsed.is_array());
        EXPECT_EQ(parsed, qb::jsonb(qb::json::parse(payload)));
    }

    // Unversioned / unsupported leading bytes -> throw.
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_binary(hex_to_bytes("0203")), std::runtime_error);
}

TEST(TypeConverterJsonbTest, VersionedValuePreservesArrayShape) {
    for (std::string text : {R"([[1,2]])", R"([["k","v"]])", R"([[[1,2]],[[3,4]]])", R"({"a":[[1,2]]})"}) {
        std::vector<byte> wire{static_cast<byte>(1)};
        wire.insert(wire.end(), text.begin(), text.end());
        const auto parsed = TypeConverter<qb::jsonb>::from_binary(wire);
        EXPECT_EQ(parsed, qb::jsonb(qb::json::parse(text))) << text;
    }
    EXPECT_THROW(TypeConverter<qb::jsonb>::from_binary(hex_to_bytes("025b5b312c325d5d")), std::runtime_error);
}

// A realistic nested JSONB document decodes with all fields intact.
TEST(TypeConverterJsonbTest, NestedDocumentVersionedWire) {
    // Parse an actual object: qb::jsonb brace initialization here produces an
    // array of pairs, which the old decoder wrongly folded into an object.
    qb::jsonb         doc(qb::json::parse(
        R"({"id":123,"name":"test user","active":true,"scores":[98,87,95],"details":{"address":"123 Test St","email":"test@example.com"}})"));
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
    badVersion[4]                = static_cast<byte>(2);
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
