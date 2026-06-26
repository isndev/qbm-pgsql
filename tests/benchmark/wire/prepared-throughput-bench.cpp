/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/wire/prepared-throughput-bench.cpp
 * @brief LIVE google-benchmark harness: prepared-vs-adhoc INSERT throughput + decode rate.
 *
 * Low-priority, live: REQUIRES a reachable postgres (`QB_PG_DSN`). When the daemon is down
 * every benchmark short-circuits via `state.SkipWithError` (google-benchmark has no
 * GTEST_SKIP), so a daemon-less run reports skipped-with-error rather than crashing.
 *
 * Measures the end-to-end statement paths that the correctness suite used to time inline
 * (`PerformanceComparison`/`AsyncPerformanceComparison` in prepared-statements and
 * `QueryPerformance`/`mass_insert` in queries) — timing extracted OUT of ctest so the
 * integration tests stay deterministic:
 *
 *   - BM_AdhocInsert      : one ad-hoc parameterized INSERT per iteration (re-parses each time).
 *   - BM_PreparedInsert   : one execute() of a once-prepared INSERT per iteration (no re-parse).
 *   - BM_RowDecodeRate    : SELECT of N rows, decoding every int4 via field::as<int> (rows/sec).
 *
 * Connection + schema setup is hoisted out of the timed loop. Each iteration runs through
 * `qb::io::async::run_sync` on the connected database.
 *
 * @ingroup Pgsql
 */
#include <cstdint>
#include <memory>
#include <string>
#include <benchmark/benchmark.h>

#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../pgsql.h"
#include "../../shared/test_config.hpp"

using namespace qb::pg;
using namespace qb::pg::detail;

namespace {

/// Process-wide live connection, lazily created. Returns nullptr if postgres is unreachable
/// (benchmarks then SkipWithError). The work table is (re)created on first successful connect.
qb::pg::tcp::database *
shared_db() {
    static std::unique_ptr<qb::pg::tcp::database> db;
    static bool                                   tried = false;
    if (!tried) {
        tried = true;
        qb::io::async::init();
        auto candidate = std::make_unique<qb::pg::tcp::database>();
        if (qb::io::async::run_sync(candidate->connect(qb::pg::test::dsn_tcp_string()))) {
            db = std::move(candidate);
            (void) db->execute("DROP TABLE IF EXISTS qb_pgsql_bench_t", discard_query, discard_error).await();
            (void) db->execute("CREATE TABLE qb_pgsql_bench_t (id INT, v TEXT)", discard_query, discard_error).await();
        }
    }
    return db.get();
}

} // namespace

static void
BM_AdhocInsert(benchmark::State &state) {
    auto *db = shared_db();
    if (!db) {
        state.SkipWithError("postgres unreachable (set QB_PG_DSN)");
        return;
    }
    // Correctness gate: one insert must land.
    bool ok = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await db->query("INSERT INTO qb_pgsql_bench_t (id, v) VALUES ($1::int, $2::text)", 1, std::string("gate"));
        ok     = r.ok();
        co_return;
    }());
    if (!ok) {
        state.SkipWithError("ad-hoc insert failed");
        return;
    }

    int n = 2;
    for (auto _ : state) {
        qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
            auto r = co_await db->query("INSERT INTO qb_pgsql_bench_t (id, v) VALUES ($1::int, $2::text)", n, std::string("row"));
            benchmark::DoNotOptimize(r.ok());
            co_return;
        }());
        ++n;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AdhocInsert);

static void
BM_PreparedInsert(benchmark::State &state) {
    auto *db = shared_db();
    if (!db) {
        state.SkipWithError("postgres unreachable (set QB_PG_DSN)");
        return;
    }
    // The statement is parsed server-side ONCE for the lifetime of the shared connection:
    // google-benchmark re-invokes this function (estimation + measured passes), and a second
    // Parse of the same name on the persistent connection raises 42P05
    // (prepared statement already exists). Guard so we only prepare on the first call.
    static bool prepared = false;
    if (!prepared) {
        qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
            auto p = co_await db->prepare("qb_bench_ins", "INSERT INTO qb_pgsql_bench_t (id, v) VALUES ($1, $2)",
                                          type_oid_sequence{oid::int4, oid::text});
            prepared = p.ok();
            co_return;
        }());
    }
    if (!prepared) {
        state.SkipWithError("prepare failed");
        return;
    }

    int n = 1'000'000;
    for (auto _ : state) {
        qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
            auto r = co_await db->execute("qb_bench_ins", params{n, std::string("row")});
            benchmark::DoNotOptimize(r.ok());
            co_return;
        }());
        ++n;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PreparedInsert);

static void
BM_RowDecodeRate(benchmark::State &state) {
    auto *db = shared_db();
    if (!db) {
        state.SkipWithError("postgres unreachable (set QB_PG_DSN)");
        return;
    }
    const int rows = static_cast<int>(state.range(0));

    // Correctness gate: the row count and the decoded sum must be right once.
    bool          ok  = false;
    std::uint64_t sum = 0;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto r = co_await db->query("SELECT g FROM generate_series(1, " + std::to_string(rows) + ") g");
        if (r.ok() && static_cast<int>(r.result().size()) == rows) {
            for (const auto &row : r.result())
                sum += static_cast<std::uint64_t>(row[0].as<int>());
            ok = (sum == static_cast<std::uint64_t>(rows) * (rows + 1) / 2);
        }
        co_return;
    }());
    if (!ok) {
        state.SkipWithError("row decode correctness gate failed");
        return;
    }

    for (auto _ : state) {
        std::uint64_t s = 0;
        qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
            auto r = co_await db->query("SELECT g FROM generate_series(1, " + std::to_string(rows) + ") g");
            if (r.ok())
                for (const auto &row : r.result())
                    s += static_cast<std::uint64_t>(row[0].as<int>());
            co_return;
        }());
        benchmark::DoNotOptimize(s);
    }
    state.SetItemsProcessed(state.iterations() * rows); // rows decoded
}
BENCHMARK(BM_RowDecodeRate)->Arg(100)->Arg(1000);

BENCHMARK_MAIN();
