/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/wire/param-encode-bench.cpp
 * @brief google-benchmark harness for parameter encoding and NOTIFY SQL construction.
 *
 * Every parameterized statement (`db.query(sql, args...)`, `execute(stmt, params)`) builds
 * its Bind-message parameter block through `ParamSerializer::serialize_params` — once per
 * execution — so the variadic fold, the per-type `add_param` dispatch, the NUMERIC binary
 * encoder, and the `std::vector<byte>` growth are all on the write hot path:
 *
 *   - serialize_params over a wide MIXED scalar param set (int/bigint/double/text/bool).
 *   - serialize_params of a 1000-element int4[] (the array-header + per-element encode +
 *     buffer growth path in add_vector).
 *   - serialize_params of a wide NUMERIC (decode_pg_numeric's inverse, set_var_from_str).
 *   - build_notify_sql with valid ASCII/UTF-8 payloads, including the 7999-byte boundary.
 *
 * Daemon-free. Param sets mirror the constructions in the unit serializer suite
 * (`unit/serialization/param-serializer-encode.cpp`). Setup (the argument values) is built
 * once outside the loop; the serializer is reset()-and-refilled per iteration so growth is
 * measured. Out-of-loop correctness gates report mismatches through state.SkipWithError. No
 * EXPECT_LT duration assertions.
 *
 * @ingroup Pgsql
 */
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "../../shared/pg_wire_ground_truth.hpp"
#include <qbm/pgsql/pg_notify_sql.h>
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using namespace qb::pg::detail;
using qb::pg::test::hex_to_bytes;

static void
BM_EncodeMixedScalars(benchmark::State &state) {
    const integer     i   = 42;
    const bigint      b   = 9'000'000'000LL;
    const double      d   = 3.14159;
    const std::string txt = "the quick brown fox";
    const bool        flg = true;

    {
        ParamSerializer probe;
        probe.serialize_params(i, b, d, txt, flg);
        // Count, per-parameter lengths, network-order values and OIDs are literal
        // PostgreSQL wire expectations, independent of the serializer/decoder.
        const auto expected = hex_to_bytes("0005"                                           // five parameters
                                           "000000040000002a"                               // int4: 42
                                           "000000080000000218711a00"                       // int8: 9000000000
                                           "00000008400921f9f01b866e"                       // IEEE-754 binary64: 3.14159
                                           "0000001374686520717569636b2062726f776e20666f78" // text: the quick brown fox
                                           "0000000101");                                   // bool: true
        if (probe.param_count() != 5 || probe.param_types() != std::vector<integer>{23, 20, 701, 25, 16} || probe.params_buffer() != expected) {
            state.SkipWithError("mixed scalar OID, order or exact wire bytes mismatch");
            return;
        }
    }

    ParamSerializer ser;
    for (auto _ : state) {
        ser.serialize_params(i, b, d, txt, flg);
        benchmark::DoNotOptimize(ser.params_buffer().data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * 5); // params encoded
}
BENCHMARK(BM_EncodeMixedScalars);

static void
BM_EncodeInt4Array1000(benchmark::State &state) {
    std::vector<integer> arr;
    arr.reserve(1000);
    for (int k = 0; k < 1000; ++k)
        arr.push_back(k);

    {
        ParamSerializer probe;
        probe.serialize_params(arr);
        // One int4[] parameter: 8020-byte body, one dimension, no NULLs,
        // element OID 23, length 1000, lower bound 1, then ordered int4 values.
        auto expected = hex_to_bytes("000100001f54000000010000000000000017000003e800000001");
        for (std::uint32_t k = 0; k < 1000; ++k) {
            expected.insert(expected.end(), {0, 0, 0, 4, 0, 0, static_cast<byte>(k >> 8), static_cast<byte>(k & 0xff)});
        }
        if (probe.param_count() != 1 || probe.param_types() != std::vector<integer>{1007} || probe.params_buffer() != expected) {
            state.SkipWithError("int4[] OID, order or exact wire bytes mismatch");
            return;
        }
    }

    ParamSerializer ser;
    for (auto _ : state) {
        ser.serialize_params(arr);
        benchmark::DoNotOptimize(ser.params_buffer().data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * 1000); // elements encoded
}
BENCHMARK(BM_EncodeInt4Array1000);

static void
BM_EncodeWideNumeric(benchmark::State &state) {
    const numeric n("123456789012345678.987654321");

    {
        std::vector<byte> probe;
        TypeConverter<numeric>::to_binary(n, probe);
        // Length 24; ndigits=8, weight=4, positive, dscale=9; base-10000
        // digits {12,3456,7890,1234,5678,9876,5432,1000}. No codec round trip.
        const auto expected = hex_to_bytes("000000180008000400000009000c0d801ed204d2162e2694153803e8");
        if (probe != expected) {
            state.SkipWithError("numeric exact wire bytes mismatch");
            return;
        }
    }

    std::vector<byte> buf;
    for (auto _ : state) {
        buf.clear();
        TypeConverter<numeric>::to_binary(n, buf);
        benchmark::DoNotOptimize(buf.data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EncodeWideNumeric);

static void
BM_EncodeTextOnly(benchmark::State &state) {
    const std::string a = "alpha";
    const std::string b = "beta-gamma-delta";
    const std::string c = "a slightly longer text value used as a bind parameter";

    {
        ParamSerializer probe;
        probe.serialize_params(a, b, c);
        const auto expected =
            hex_to_bytes("0003"                                     // three parameters
                         "00000005616c706861"                       // alpha
                         "00000010626574612d67616d6d612d64656c7461" // beta-gamma-delta
                         "000000356120736c696768746c79206c6f6e67657220746578742076616c7565207573656420617320612062696e6420706172616d65746572");
        if (probe.param_count() != 3 || probe.param_types() != std::vector<integer>{25, 25, 25} || probe.params_buffer() != expected) {
            state.SkipWithError("text OID, order or exact wire bytes mismatch");
            return;
        }
    }

    ParamSerializer ser;
    for (auto _ : state) {
        ser.serialize_params(a, b, c);
        benchmark::DoNotOptimize(ser.params_buffer().data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * 3);
}
BENCHMARK(BM_EncodeTextOnly);

// Only SQL construction is measured: no database, socket, callback or coroutine.
// Both comparison roles accept these payloads and produce the same literal bytes;
// rejection at 8000 bytes belongs to correctness tests, not an equivalent-work pair.
static void
BM_BuildNotifySql(benchmark::State &state) {
    const auto payload_bytes = state.range(0);
    const auto utf8          = state.range(1);
    if ((payload_bytes != 16 && payload_bytes != 512 && payload_bytes != 7999) || (utf8 != 0 && utf8 != 1)) {
        state.SkipWithError("unsupported NOTIFY payload fixture");
        return;
    }

    const auto  bytes = static_cast<std::size_t>(payload_bytes);
    std::string channel{"qb_notify_bench"};
    std::string payload;
    payload.reserve(bytes);
    if (utf8) {
        // U+00E9 is two UTF-8 bytes. Keep complete code points; an odd byte count
        // ends in ASCII, so 7999 bytes never contains a truncated UTF-8 character.
        for (std::size_t i = 0; i < bytes / 2; ++i)
            payload.append("\xC3\xA9");
        if (bytes % 2)
            payload.push_back('x');
    } else {
        payload.assign(bytes, 'x');
    }
    // Independent oracle: no quoting helper is reused to derive the expectation.
    const std::string expected = "NOTIFY \"qb_notify_bench\", $qb_notify$" + payload + "$qb_notify$";
    auto              sql      = build_notify_sql(channel, payload);
    if (payload.size() != bytes || sql != expected) {
        state.SkipWithError("NOTIFY payload size or exact SQL bytes mismatch");
        return;
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(channel);
        benchmark::DoNotOptimize(payload);
        sql = build_notify_sql(channel, payload);
        benchmark::DoNotOptimize(sql.data());
        benchmark::DoNotOptimize(sql.size());
        benchmark::ClobberMemory();
    }
    if (sql != expected) {
        state.SkipWithError("NOTIFY exact SQL bytes changed after timed loop");
        return;
    }
    if (state.iterations() > std::numeric_limits<std::int64_t>::max() / payload_bytes) {
        state.SkipWithError("NOTIFY payload byte counter overflow");
        return;
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * payload_bytes);
}
BENCHMARK(BM_BuildNotifySql)
    ->Args({16, 0})
    ->Args({512, 0})
    ->Args({7999, 0})
    ->Args({16, 1})
    ->Args({512, 1})
    ->Args({7999, 1})
    ->ArgNames({"payload_bytes", "utf8"})
    ->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
