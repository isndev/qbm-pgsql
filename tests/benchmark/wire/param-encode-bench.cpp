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
 * @brief google-benchmark harness for the qbm-pgsql parameter encode hot path.
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
 *
 * Daemon-free. Param sets mirror the constructions in the unit serializer suite
 * (`unit/serialization/param-serializer-encode.cpp`). Setup (the argument values) is built
 * once outside the loop; the serializer is reset()-and-refilled per iteration so growth is
 * measured. ONE out-of-loop correctness gate per benchmark (state.SkipWithError). No
 * EXPECT_LT duration assertions.
 *
 * @ingroup Pgsql
 */
#include <cstdint>
#include <string>
#include <vector>
#include <benchmark/benchmark.h>

#include "../pgsql.h"

using namespace qb::pg;
using namespace qb::pg::detail;

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
        if (probe.param_count() != 5) {
            state.SkipWithError("mixed scalar param_count != 5");
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
        // A non-empty vector becomes ONE array parameter.
        if (probe.param_count() != 1 || probe.params_buffer().empty()) {
            state.SkipWithError("int4[] encode produced no array param");
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
        if (probe.empty()) {
            state.SkipWithError("numeric encode produced no bytes");
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
        if (probe.param_count() != 3) {
            state.SkipWithError("text param_count != 3");
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

BENCHMARK_MAIN();
