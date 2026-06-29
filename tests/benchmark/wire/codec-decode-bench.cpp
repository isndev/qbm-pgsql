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
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../shared/pg_wire_ground_truth.hpp"
#include "../pgsql.h"

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
    if (gate.size() != 1000 || gate.front() != 0 || gate.back() != 999) {
        state.SkipWithError("int4[] decode mismatch");
        return;
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
    benchmark::DoNotOptimize(gate);
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

BENCHMARK_MAIN();
