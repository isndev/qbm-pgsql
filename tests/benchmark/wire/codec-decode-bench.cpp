/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/wire/codec-decode-bench.cpp
 * @brief google-benchmark harness for the qbm-pgsql binary result decode hot path.
 *
 * Every result column the driver returns in BINARY format is decoded through these
 * functions — once per field, per row — so they are squarely on the read hot path:
 *
 *   - TypeConverter<integer>::from_binary  (the int2/int4/int8 size-dispatch + overflow gate)
 *   - TypeConverter<bigint>::from_binary   / ParamUnserializer::read_bigint
 *   - TypeConverter<double>::from_binary   / ParamUnserializer::read_double
 *   - TypeConverter<numeric>::from_binary  (decode_pg_numeric — variable-length BE digit groups)
 *   - decode_pg_array<integer> via TypeConverter<std::vector<integer>>::from_binary
 *     (1000-element int4[]: the array header walk + per-element length-prefixed decode)
 *   - TypeConverter<qb::wall_time>::from_binary  (timestamptz 8-byte microsecond epoch)
 *   - ParamUnserializer::read_string       (the 1 MB auto-detect heuristic, pressure-tested)
 *
 * Daemon-free: seeded from the exact PostgreSQL `*_send()` golden hex byte-vectors that
 * the unit suite pins (`shared/pg_wire_ground_truth.hpp` / unit/types). Setup is hoisted
 * out of the timed loop; each benchmark has ONE out-of-loop correctness gate
 * (state.SkipWithError) so a broken codec can never report a bogus number. No EXPECT_LT
 * duration assertions — this is a profiler, not a correctness gate.
 *
 * @ingroup Pgsql
 */
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../../shared/pg_wire_ground_truth.hpp"
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using namespace qb::pg::detail;
using qb::pg::test::hex_to_bytes;

namespace {

/// Build the BINARY wire body for a 1-D int4[] of {0,1,...,n-1}: header (ndim, hasnull,
/// elem oid, dim, lbound) followed by n length-prefixed 4-byte big-endian elements. This
/// is exactly the layout `decode_pg_array<integer>` walks. The shared corpus only carries
/// small arrays; a 1000-element array is generated here to stress the per-element walk.
std::vector<byte>
build_int4_array(int n) {
    std::vector<byte> b;
    auto              put_i32 = [&b](std::uint32_t v) {
        b.push_back(static_cast<byte>((v >> 24) & 0xFF));
        b.push_back(static_cast<byte>((v >> 16) & 0xFF));
        b.push_back(static_cast<byte>((v >> 8) & 0xFF));
        b.push_back(static_cast<byte>(v & 0xFF));
    };
    put_i32(1);                             // ndim
    put_i32(0);                             // hasnull
    put_i32(23);                            // elem oid (int4)
    put_i32(static_cast<std::uint32_t>(n)); // dim size
    put_i32(1);                             // lower bound
    for (int i = 0; i < n; ++i) {
        put_i32(4);                             // element length
        put_i32(static_cast<std::uint32_t>(i)); // element value
    }
    return b;
}

// The protocol layer has already removed the field length prefix. Keep the
// column metadata and its four value bytes alive outside the timed field read.
struct Int4FieldFixture {
    result_impl impl;

    Int4FieldFixture() {
        field_description desc{};
        desc.name             = "value";
        desc.table_oid        = 0;
        desc.attribute_number = 0;
        desc.type_oid         = oid::int4;
        desc.type_size        = 4;
        desc.type_mod         = -1;
        desc.format_code      = protocol_data_format::Binary;
        impl.row_description().push_back(desc);

        auto &row = impl.rows().emplace_back();
        row.offsets.push_back(0);
        row.null_map.push_back(false);
        row.data = hex_to_bytes("0000002a"); // PostgreSQL int4_send(42)
    }
};

// QB-945 comparison input. The result carries PostgreSQL's versioned JSONB
// payload, so each timed typed read includes the same field routing as a query.
std::string
jsonb_document(int shape) {
    switch (shape) {
        case 0:
            return R"({"name":"Ada","items":[true,null,"x"]})";
        case 1:
            return R"({"id":42,"ratio":1.5,"enabled":true})";
        case 2: {
            std::string text = "[";
            for (int i = 0; i < 64; ++i) {
                if (i)
                    text += ',';
                text += "0.1";
            }
            return text + ']';
        }
        case 3:
            return "12345678901234567890.123456789";
        case 4:
            return "1e400";
        case 5:
            return "[[1,2],[3,4]]";
        // Existing unit literals: numeric loss after representable values.
        case 6:
            return "[0.1,12345678901234567890.123456789]";
        case 7:
            return R"({"prefix":[0.1,1.5],"loss":1e-400})";
        default:
            return {};
    }
}

struct JsonbFieldFixture {
    result_impl impl;

    explicit JsonbFieldFixture(std::string_view text) {
        field_description desc{};
        desc.name        = "doc";
        desc.type_oid    = oid::jsonb;
        desc.format_code = protocol_data_format::Binary;
        impl.row_description().push_back(std::move(desc));

        auto &row = impl.rows().emplace_back();
        row.offsets.push_back(0);
        row.null_map.push_back(false);
        row.data.push_back(static_cast<byte>(1));
        row.data.insert(row.data.end(), text.begin(), text.end());
    }
};

// A text-format export keeps the same bytes on both sides of QB-943. SQL NULL
// and present empty text are distinct, even though neither contributes bytes.
struct TextExportFixture {
    result_impl impl;

    TextExportFixture() {
        for (const char *name : {"name", "absent", "present_empty"}) {
            field_description desc{};
            desc.name        = name;
            desc.type_oid    = oid::text;
            desc.format_code = protocol_data_format::Text;
            impl.row_description().push_back(std::move(desc));
        }
        auto &row    = impl.rows().emplace_back();
        row.offsets  = {0, 3, 3};
        row.null_map = {false, true, false};
        for (char c : std::string_view("Ada"))
            row.data.push_back(static_cast<byte>(c));
    }
};

struct BinaryExportFixture {
    result_impl impl;

    BinaryExportFixture() {
        field_description desc{};
        desc.name        = "count";
        desc.type_oid    = oid::int4;
        desc.type_size   = 4;
        desc.format_code = protocol_data_format::Binary;
        impl.row_description().push_back(std::move(desc));
        auto &row = impl.rows().emplace_back();
        row.offsets.push_back(0);
        row.null_map.push_back(false);
        row.data = hex_to_bytes("0000002a"); // int4_send(42)
    }
};

// New array types use TEXT on the old whitelist and BINARY on the new one;
// int4/text stay BINARY on both. Every role exports the identical array literal.
struct ExtendedArrayExportFixture {
    result_impl impl;
    std::string expected_text;

    ExtendedArrayExportFixture(int kind, int count) {
        struct Case {
            oid         array_oid;
            integer     element_oid;
            const char *element_hex;
            const char *array_element;
        };
        static constexpr std::string_view json_text    = R"({"n": 12345678901234567890.123456789, "s": "a\\b"})";
        static constexpr const char      *json_element = R"("{\"n\": 12345678901234567890.123456789, \"s\": \"a\\\\b\"}")";
        static const Case                 cases[]      = {
            {oid::uuid_array, 2950, "550e8400e29b41d4a716446655440000", "550e8400-e29b-41d4-a716-446655440000"},
            {oid::timestamptz_array, 1184, qb::pg::test::gt::temporal::ts_2024_03_15, R"("2024-03-15 14:30:45.123456+00")"},
            {oid::numeric_array, 1700, "0008000400000009000c0d801ed204d2162e2694153803e8", "123456789012345678.987654321"},
            {oid::json_array, 114, "", json_element},
            {oid::jsonb_array, 3802, "", json_element},
            {oid::int4_array, 23, "0000002a", "42"},
            {oid::text_array, 25, "746578742076616c7565", R"("text value")"},
        };
        const auto &sample  = cases[kind];
        auto        element = hex_to_bytes(sample.element_hex);
        if (kind == 3 || kind == 4) {
            element.assign(json_text.begin(), json_text.end());
            if (kind == 4)
                element.insert(element.begin(), static_cast<byte>(1)); // JSONB version, not array framing
        }
        // Literal array_send framing, independent of TypeConverter/array codecs.
        auto       wire    = hex_to_bytes("0000000100000001"); // ndim=1, has-NULL=1
        const auto put_i32 = [&](std::int32_t value) {
            for (int shift = 24; shift >= 0; shift -= 8)
                wire.push_back(static_cast<byte>(static_cast<std::uint32_t>(value) >> shift));
        };
        put_i32(sample.element_oid);
        put_i32(count);
        put_i32(-2);
        expected_text = "[-2:" + std::to_string(count - 3) + "]={";
        for (int i = 0; i < count; ++i) {
            if (i)
                expected_text += ',';
            expected_text += i % 2 ? "NULL" : sample.array_element;
            put_i32(i % 2 ? -1 : static_cast<std::int32_t>(element.size()));
            if (i % 2 == 0)
                wire.insert(wire.end(), element.begin(), element.end());
        }
        expected_text += '}';

        field_description desc{};
        desc.name         = "values";
        desc.type_oid     = sample.array_oid;
        desc.type_size    = -1;
        desc.type_mod     = -1;
        desc.format_code  = type_oid_prefers_binary_result_format(sample.array_oid) ? protocol_data_format::Binary : protocol_data_format::Text;
        const bool binary = desc.format_code == protocol_data_format::Binary;
        impl.row_description().push_back(std::move(desc));
        auto &row    = impl.rows().emplace_back();
        row.offsets  = {0};
        row.null_map = {false};
        row.data     = binary ? std::move(wire) : std::vector<byte>(expected_text.begin(), expected_text.end());
    }
};

bool
is_present_binary_int4_42(const resultset::row::value_type &field) {
    const auto &desc = field.description();
    return !field.is_null() && desc.type_oid == oid::int4 && desc.type_size == 4 && desc.format_code == protocol_data_format::Binary
           && field.text() == std::string_view("\x00\x00\x00\x2a", 4);
}

} // namespace

static void
BM_DecodeInt4(benchmark::State &state) {
    const std::vector<byte> wire = hex_to_bytes("0000002a"); // 42
    if (TypeConverter<integer>::from_binary(wire) != 42) {
        state.SkipWithError("int4 decode mismatch");
        return;
    }
    for (auto _ : state) {
        integer v = TypeConverter<integer>::from_binary(wire);
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DecodeInt4);

// Field-level optional read: the SQL value is present, with a binary INT4 OID.
// The plain integer read is an unchanged-path witness for the wrapper's cost.
static void
BM_FieldInt4Optional(benchmark::State &state) {
    Int4FieldFixture fixture;
    resultset        rs(&fixture.impl);
    const auto       field = rs[0][0];
    const auto       gate  = field.as<std::optional<integer>>();
    if (!is_present_binary_int4_42(field) || !gate || *gate != 42) {
        state.SkipWithError("binary int4 optional field mismatch");
        return;
    }

    for (auto _ : state) {
        auto value = field.as<std::optional<integer>>();
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * 4);
}
BENCHMARK(BM_FieldInt4Optional);

static void
BM_FieldInt4Scalar(benchmark::State &state) {
    Int4FieldFixture fixture;
    resultset        rs(&fixture.impl);
    const auto       field = rs[0][0];
    if (!is_present_binary_int4_42(field) || field.as<integer>() != 42) {
        state.SkipWithError("binary int4 scalar field mismatch");
        return;
    }

    for (auto _ : state) {
        integer value = field.as<integer>();
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * 4);
}
BENCHMARK(BM_FieldInt4Scalar);

static void
BM_DecodeInt8(benchmark::State &state) {
    // Same 8 BE bytes the temporal corpus uses for the raw PG-epoch int8 (763828245123456) those timestamptz bytes carry.
    const std::vector<byte> wire = hex_to_bytes(qb::pg::test::gt::temporal::ts_2024_03_15);
    ParamUnserializer       un;
    if (un.read_bigint(std::span<const byte>(wire)) != qb::pg::test::gt::temporal::ts_2024_03_15_pg_micros) {
        state.SkipWithError("int8 decode mismatch");
        return;
    }
    for (auto _ : state) {
        bigint v = un.read_bigint(std::span<const byte>(wire));
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DecodeInt8);

static void
BM_DecodeFloat8(benchmark::State &state) {
    // IEEE-754 big-endian 1.5 == 0x3ff8000000000000.
    const std::vector<byte> wire = hex_to_bytes("3ff8000000000000");
    ParamUnserializer       un;
    if (un.read_double(std::span<const byte>(wire)) != 1.5) {
        state.SkipWithError("float8 decode mismatch");
        return;
    }
    for (auto _ : state) {
        double v = un.read_double(std::span<const byte>(wire));
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DecodeFloat8);

static void
BM_DecodeNumeric(benchmark::State &state) {
    const auto             &c    = qb::pg::test::gt::numeric::finite[0]; // {"...", "1234.5678"}
    const std::vector<byte> wire = hex_to_bytes(c.hex);
    if (TypeConverter<numeric>::from_binary(wire).str() != c.expect) {
        state.SkipWithError("numeric decode mismatch");
        return;
    }
    for (auto _ : state) {
        numeric n = TypeConverter<numeric>::from_binary(wire);
        benchmark::DoNotOptimize(n);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DecodeNumeric);

static void
BM_DecodeInt4Array1000(benchmark::State &state) {
    const std::vector<byte> wire = build_int4_array(1000);
    auto                    gate = TypeConverter<std::vector<integer>>::from_binary(wire);
    if (gate.size() != 1000) {
        state.SkipWithError("int4[] decode count mismatch");
        return;
    }
    for (std::size_t i = 0; i < gate.size(); ++i) {
        if (gate[i] != static_cast<integer>(i)) {
            state.SkipWithError("int4[] decode value or order mismatch");
            return;
        }
    }
    for (auto _ : state) {
        auto v = TypeConverter<std::vector<integer>>::from_binary(wire);
        benchmark::DoNotOptimize(v.data());
    }
    state.SetItemsProcessed(state.iterations() * 1000); // values decoded
}
BENCHMARK(BM_DecodeInt4Array1000);

static void
BM_DecodeTimestamptz(benchmark::State &state) {
    const std::vector<byte> wire = hex_to_bytes(qb::pg::test::gt::temporal::ts_2024_03_15);
    auto                    gate = TypeConverter<qb::wall_time>::from_binary(wire);
    // Independent Unix-epoch value recorded beside the PostgreSQL wire fixture.
    const qb::wall_time expected{std::chrono::duration_cast<qb::wall_time::duration>(
        std::chrono::microseconds{qb::pg::test::gt::temporal::ts_2024_03_15_unix_micros})};
    if (gate != expected) {
        state.SkipWithError("timestamptz decode instant mismatch");
        return;
    }
    for (auto _ : state) {
        auto v = TypeConverter<qb::wall_time>::from_binary(wire);
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_DecodeTimestamptz);

static void
BM_DecodeString(benchmark::State &state) {
    // read_string consumes the whole field as text (the 1 MB auto-detect heuristic only
    // engages for very large payloads; here we exercise the common short-string path).
    const std::string       s = "the quick brown fox jumps over the lazy dog";
    const std::vector<byte> wire(reinterpret_cast<const byte *>(s.data()), reinterpret_cast<const byte *>(s.data()) + s.size());
    ParamUnserializer       un;
    if (un.read_string(std::span<const byte>(wire)) != s) {
        state.SkipWithError("string decode mismatch");
        return;
    }
    for (auto _ : state) {
        std::string v = un.read_string(std::span<const byte>(wire));
        benchmark::DoNotOptimize(v.data());
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(s.size()));
}
BENCHMARK(BM_DecodeString);

static void
BM_JsonbTypedRead(benchmark::State &state) {
    const std::string text = jsonb_document(static_cast<int>(state.range(0)));
    JsonbFieldFixture fixture(text);
    resultset         result(&fixture.impl);
    const auto        field    = result[0][0];
    const auto        expected = qb::json::parse(text);
    if (field.as<qb::jsonb>().unwrap() != expected) {
        state.SkipWithError("JSONB typed read differs from the complete wire document");
        return;
    }

    for (auto _ : state) {
        auto value = field.as<qb::jsonb>();
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(text.size()));
}
BENCHMARK(BM_JsonbTypedRead)->Arg(0)->Arg(1)->Arg(2)->Arg(5);

static void
BM_JsonExportText(benchmark::State &state) {
    TextExportFixture fixture;
    resultset         result(&fixture.impl);
    const qb::json    expected = qb::json::array({qb::json{{"name", "Ada"}, {"absent", nullptr}, {"present_empty", ""}}});
    if (result.json() != expected) {
        state.SkipWithError("TEXT result export changed a value, SQL NULL, or present empty text");
        return;
    }
    for (auto _ : state) {
        auto value = result.json();
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * 3);
}
BENCHMARK(BM_JsonExportText);

static void
BM_JsonExportInt4Binary(benchmark::State &state) {
    BinaryExportFixture fixture;
    resultset           result(&fixture.impl);
    const qb::json      expected = qb::json::array({qb::json{{"count", "42"}}});
    if (result.json() != expected) {
        state.SkipWithError("binary INT4 result export did not render decimal 42");
        return;
    }
    for (auto _ : state) {
        auto value = result.json();
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * 4);
}
BENCHMARK(BM_JsonExportInt4Binary);

// Compare complete JSON export for the same values under each role's real Bind
// result-format policy; input construction and the independent oracle are cold.
static void
BM_JsonExportExtendedArray(benchmark::State &state) {
    const int kind  = static_cast<int>(state.range(0));
    const int count = static_cast<int>(state.range(1));
    if (kind < 0 || kind > 6 || (count != 2 && count != 32)) {
        state.SkipWithError("unsupported extended-array export fixture");
        return;
    }
    ExtendedArrayExportFixture fixture(kind, count);
    resultset                  result(&fixture.impl);
    const qb::json             expected = qb::json::array({qb::json{{"values", fixture.expected_text}}});
    if (result.json() != expected) {
        state.SkipWithError("extended-array JSON export changed exact text, NULLs or bounds");
        return;
    }
    state.SetLabel(fixture.impl.row_description()[0].format_code == protocol_data_format::Binary ? "binary" : "text");
    for (auto _ : state) {
        auto value = result.json();
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations() * count);
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(fixture.expected_text.size())); // same output bytes on both roles
}
BENCHMARK(BM_JsonExportExtendedArray)
    ->Args({0, 2})
    ->Args({0, 32})
    ->Args({1, 2})
    ->Args({1, 32})
    ->Args({2, 2})
    ->Args({2, 32})
    ->Args({3, 2})
    ->Args({3, 32})
    ->Args({4, 2})
    ->Args({4, 32})
    ->Args({5, 2})
    ->Args({5, 32})
    ->Args({6, 2})
    ->Args({6, 32})
    ->ArgNames({"type", "elements"});

// PostgreSQL JSON (unlike JSONB) has no version byte. A pair-shaped JSON array
// must remain an array; the old heuristic converted it into an object.
static void
BM_JsonPairArrayDecode(benchmark::State &state) {
    constexpr std::string_view text = "[[1,2],[3,4]]";
    std::vector<byte>          wire;
    wire.reserve(text.size());
    for (char c : text)
        wire.push_back(static_cast<byte>(c));
    const auto expected = qb::json::parse(text);
    if (TypeConverter<qb::json>::from_binary(wire) != expected) {
        state.SkipWithError("binary JSON pair array became an object or lost a value");
        return;
    }
    for (auto _ : state) {
        auto value = TypeConverter<qb::json>::from_binary(wire);
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(wire.size()));
}
BENCHMARK(BM_JsonPairArrayDecode);

#ifndef QB945_BASELINE
static void
BM_JsonbTextView(benchmark::State &state) {
    const std::string text = jsonb_document(static_cast<int>(state.range(0)));
    JsonbFieldFixture fixture(text);
    resultset         result(&fixture.impl);
    const auto        field = result[0][0];
    if (field.jsonb_text() != text) {
        state.SkipWithError("JSONB text view differs from the wire text");
        return;
    }

    for (auto _ : state) {
        auto view = field.jsonb_text();
        benchmark::DoNotOptimize(view.data());
        benchmark::DoNotOptimize(view.size());
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(text.size()));
}
BENCHMARK(BM_JsonbTextView)->Arg(0)->Arg(1)->Arg(2)->Arg(3)->Arg(4);

static void
BM_JsonbTextCopy(benchmark::State &state) {
    const std::string text = jsonb_document(static_cast<int>(state.range(0)));
    JsonbFieldFixture fixture(text);
    resultset         result(&fixture.impl);
    const auto        field = result[0][0];
    if (field.jsonb_text_copy() != text) {
        state.SkipWithError("JSONB text copy differs from the wire text");
        return;
    }

    for (auto _ : state) {
        auto copy = field.jsonb_text_copy();
        benchmark::DoNotOptimize(copy.data());
        benchmark::DoNotOptimize(copy.size());
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(text.size()));
}
BENCHMARK(BM_JsonbTextCopy)->Arg(0)->Arg(1)->Arg(2)->Arg(3)->Arg(4);

// PostgreSQL NUMERIC accepts this value, but the nlohmann DOM cannot hold it.
// Keep the rejection path measurable without claiming a successful typed read.
static void
BM_JsonbTypedRejected(benchmark::State &state) {
    const int              shape = static_cast<int>(state.range(0));
    const std::string      text  = jsonb_document(shape);
    JsonbFieldFixture      fixture(text);
    resultset              result(&fixture.impl);
    const auto             field           = result[0][0];
    const std::string_view rejected_lexeme = shape == 6   ? std::string_view{"12345678901234567890.123456789"}
                                             : shape == 7 ? std::string_view{"1e-400"}
                                                          : std::string_view{text};
    const std::string      expected_error  = std::string(shape == 4 ? "JSONB numeric value exceeds the DOM floating range: "
                                                                    : "JSONB numeric value cannot be represented without decimal rounding: ")
                                             + std::string(rejected_lexeme);
    try {
        (void) field.as<qb::jsonb>();
        state.SkipWithError("unrepresentable JSONB number was accepted");
        return;
    } catch (const error::client_error &e) {
        if (std::string_view(e.what()) != expected_error) {
            state.SkipWithError("JSONB rejection did not report the expected numeric lexeme");
            return;
        }
    }

    for (auto _ : state) {
        try {
            (void) field.as<qb::jsonb>();
            state.SkipWithError("unrepresentable JSONB number was accepted");
            break;
        } catch (const error::client_error &e) {
            benchmark::DoNotOptimize(e.what());
        }
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(text.size()));
}
BENCHMARK(BM_JsonbTypedRejected)->Arg(3)->Arg(4)->Arg(6)->Arg(7);
#endif

BENCHMARK_MAIN();
