/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file benchmark/wire/prepared-throughput-bench.cpp
 * @brief Live statement, decode and sequential COPY costs on a warm PostgreSQL connection.
 *
 * Connection, temporary tables, preparation and correctness checks are outside timing.
 * Every measured operation must succeed; complete persisted rows or COPY bytes are also
 * checked before publishing work counters. No concurrent, cancelled or rejected COPY
 * supplies a comparable timing. The fixed COPY corpus is 1024 distinct 64-byte text rows.
 *
 * Requires QB_PG_DSN. An unavailable daemon or failed fixture calls SkipWithError before
 * any work counter is emitted. All coroutine helpers and their fixture types have named
 * namespace linkage. The same fixture must be compiled into control and candidate.
 */
#include <benchmark/benchmark.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../../shared/test_config.hpp"
#include <qbm/pgsql/pgsql.h>

namespace pgsql_prepared_throughput_bench {

using database = qb::pg::tcp::database;
using reply    = qb::pg::Reply<qb::pg::resultset>;

constexpr std::int64_t kCopyRows       = 1024;
constexpr std::size_t  kCopyChunkBytes = 1024;
constexpr char         kCopyInSql[]    = "COPY qb_pgsql_bench_copy (v) FROM STDIN";
constexpr char         kCopyOutSql[]   = "COPY (SELECT v FROM qb_pgsql_bench_copy ORDER BY left(v, 4)::int) TO STDOUT";

struct Fixture {
    database db;
    bool     ready = false;

    Fixture() {
        qb::io::async::init();
        if (!qb::io::async::run_sync(db.connect(qb::pg::test::dsn_tcp_string())))
            return;
        const auto inserts = qb::io::async::run_sync(db.query("CREATE TEMP TABLE qb_pgsql_bench_t (id int, v text)"));
        const auto copies  = qb::io::async::run_sync(db.query("CREATE TEMP TABLE qb_pgsql_bench_copy (v text)"));
        ready              = inserts.ok() && copies.ok();
    }

    ~Fixture() {
        db.disconnect();
    }
};

std::string
copy_payload() {
    std::string payload;
    payload.reserve(static_cast<std::size_t>(kCopyRows) * 64);
    for (std::int64_t row = 0; row < kCopyRows; ++row) {
        const auto digits = std::to_string(row);
        payload.append(4 - digits.size(), '0');
        payload.append(digits);
        payload.append(59, static_cast<char>('a' + row % 26));
        payload.push_back('\n');
    }
    return payload;
}

qb::io::async::task<reply>
insert_once(database *db, int id, bool prepared, std::string value) {
    qb::io::async::pin_frame_copy(value); // Guard the by-value frame parameter before read-only serialization.
    if (prepared)
        co_return co_await db->execute("qb_bench_ins", qb::pg::params{id, std::move(value)});
    co_return co_await db->query("INSERT INTO qb_pgsql_bench_t (id, v) VALUES ($1::int, $2::text)", id, std::move(value));
}

bool
insert_reply_ok(reply const &r) {
    return r.ok() && r->empty() && r->rows_affected() == 1;
}

qb::io::async::task<bool>
insert_rows_match(database *db, std::int64_t completed) {
    auto r = co_await db->query("SELECT id, v FROM qb_pgsql_bench_t ORDER BY id");
    if (!r.ok() || r.result().size() != static_cast<std::size_t>(completed + 1))
        co_return false;
    try {
        std::int64_t index = 0;
        for (const auto &row : r.result()) {
            if (row.size() != 2 || row[0].as<int>() != index + 1 || row[1].as<std::string>() != (index == 0 ? "gate" : "row"))
                co_return false;
            ++index;
        }
        co_return index == completed + 1;
    } catch (...) {
        co_return false;
    }
}

struct DecodedRows {
    bool        ok    = false;
    std::size_t count = 0;
};

qb::io::async::task<DecodedRows>
decode_rows(database *db, std::vector<int> *values) {
    auto        r = co_await db->query("SELECT g FROM generate_series(1, " + std::to_string(values->size()) + ") g");
    DecodedRows out;
    if (!r.ok())
        co_return out;
    out.ok = true;
    try {
        for (const auto &row : r.result()) {
            if (row.size() != 1 || out.count >= values->size()) {
                out.ok = false;
                break;
            }
            (*values)[out.count++] = row[0].as<int>();
        }
    } catch (...) {
        out.ok = false;
    }
    co_return out;
}

bool
decoded_rows_match(DecodedRows const &out, std::vector<int> const &values) {
    if (!out.ok || out.count != values.size())
        return false;
    for (std::size_t i = 0; i < values.size(); ++i)
        if (values[i] != static_cast<int>(i + 1))
            return false;
    return true;
}

struct CopyInput {
    std::string const *payload;
    std::size_t        offset = 0;
    std::size_t        calls  = 0;
};

qb::io::async::task<reply>
copy_in_once(database *db, CopyInput *input) {
    co_return co_await db->copy_in(kCopyInSql, [input]() -> std::optional<std::string> {
        ++input->calls;
        if (input->offset == input->payload->size())
            return std::nullopt;
        const auto take  = std::min(kCopyChunkBytes, input->payload->size() - input->offset);
        auto       chunk = input->payload->substr(input->offset, take);
        input->offset += take;
        return chunk;
    });
}

qb::io::async::task<reply>
copy_out_once(database *db, std::string *bytes) {
    co_return co_await db->copy_out(kCopyOutSql, [bytes](std::string_view chunk) { bytes->append(chunk); });
}

bool
copy_reply_ok(reply const &r) {
    return r.ok() && r->empty() && r->rows_affected() == kCopyRows;
}

bool
copy_input_complete(CopyInput const &input) {
    return input.offset == input.payload->size() && input.calls == (input.payload->size() + kCopyChunkBytes - 1) / kCopyChunkBytes + 1;
}

bool
copy_table_matches(database *db, std::string const &payload, std::string *scratch) {
    scratch->clear();
    const auto r = qb::io::async::run_sync(copy_out_once(db, scratch));
    return copy_reply_ok(r) && *scratch == payload;
}

bool
can_count(std::int64_t completed, std::int64_t units) {
    return units > 0 && completed < std::numeric_limits<std::int64_t>::max() / units;
}

} // namespace pgsql_prepared_throughput_bench

using namespace pgsql_prepared_throughput_bench;

static void
run_insert(benchmark::State &state, bool prepared) {
    Fixture fixture;
    if (!fixture.ready) {
        state.SkipWithError("postgres connection or temporary-table fixture failed (set QB_PG_DSN)");
        return;
    }
    if (prepared) {
        const auto p = qb::io::async::run_sync(fixture.db.prepare("qb_bench_ins", "INSERT INTO qb_pgsql_bench_t (id, v) VALUES ($1, $2)",
                                                                  qb::pg::type_oid_sequence{qb::pg::oid::int4, qb::pg::oid::text}));
        if (!p.ok()) {
            state.SkipWithError("prepare failed");
            return;
        }
    }
    const auto probe = qb::io::async::run_sync(insert_once(&fixture.db, 1, prepared, "gate"));
    if (!insert_reply_ok(probe) || !qb::io::async::run_sync(insert_rows_match(&fixture.db, 0))) {
        state.SkipWithError("insert preflight result or persisted row mismatch");
        return;
    }

    std::int64_t completed = 0;
    for (auto _ : state) {
        state.PauseTiming();
        if (completed >= std::numeric_limits<int>::max() - 1) {
            state.SkipWithError("insert fixture exhausted int4 IDs");
            return;
        }
        const auto id = static_cast<int>(completed + 2);
        state.ResumeTiming();
        {
            const auto r = qb::io::async::run_sync(insert_once(&fixture.db, id, prepared, "row"));
            state.PauseTiming();
            if (!insert_reply_ok(r)) {
                state.SkipWithError("measured INSERT did not complete exactly one row");
                return;
            }
            ++completed;
        }
        state.ResumeTiming();
    }
    // Range-for has stopped the timer. Check EVERY persisted id/value, not a count/sum alone.
    if (completed != state.iterations() || !qb::io::async::run_sync(insert_rows_match(&fixture.db, completed))) {
        state.SkipWithError("complete persisted INSERT roster mismatch");
        return;
    }
    state.SetItemsProcessed(completed);
}

static void
BM_AdhocInsert(benchmark::State &state) {
    run_insert(state, false);
}
BENCHMARK(BM_AdhocInsert);

static void
BM_PreparedInsert(benchmark::State &state) {
    run_insert(state, true);
}
BENCHMARK(BM_PreparedInsert);

static void
BM_RowDecodeRate(benchmark::State &state) {
    Fixture   fixture;
    const int rows = static_cast<int>(state.range(0));
    if (!fixture.ready || rows <= 0) {
        state.SkipWithError("postgres fixture failed or row count invalid");
        return;
    }
    // Storage is allocated once. Decoding/stores are timed; every value is checked while paused.
    std::vector<int> values(static_cast<std::size_t>(rows));
    const auto       probe = qb::io::async::run_sync(decode_rows(&fixture.db, &values));
    if (!decoded_rows_match(probe, values)) {
        state.SkipWithError("row decode preflight mismatch");
        return;
    }
    std::int64_t completed = 0;
    std::int64_t decoded   = 0;
    for (auto _ : state) {
        const auto out            = qb::io::async::run_sync(decode_rows(&fixture.db, &values));
        auto      *decoded_values = values.data();
        benchmark::DoNotOptimize(decoded_values);
        state.PauseTiming();
        if (!decoded_rows_match(out, values) || !can_count(completed, rows)) {
            state.SkipWithError("measured row decode result or work count mismatch");
            return;
        }
        ++completed;
        decoded += rows;
        state.ResumeTiming();
    }
    if (completed != state.iterations() || decoded != completed * rows) {
        state.SkipWithError("incomplete cumulative row decode workload");
        return;
    }
    state.SetItemsProcessed(decoded);
}
BENCHMARK(BM_RowDecodeRate)->Arg(100)->Arg(1000);

static void
BM_CopyInSequential(benchmark::State &state) {
    Fixture     fixture;
    const auto  payload = copy_payload();
    std::string scratch;
    scratch.reserve(payload.size());
    if (!fixture.ready) {
        state.SkipWithError("postgres COPY fixture failed (set QB_PG_DSN)");
        return;
    }
    CopyInput  probe{&payload};
    const auto preflight = qb::io::async::run_sync(copy_in_once(&fixture.db, &probe));
    if (!copy_reply_ok(preflight) || !copy_input_complete(probe) || !copy_table_matches(&fixture.db, payload, &scratch)) {
        state.SkipWithError("COPY IN preflight bytes or rows mismatch");
        return;
    }

    std::int64_t completed   = 0;
    std::int64_t bytes       = 0;
    std::int64_t copied_rows = 0;
    for (auto _ : state) {
        state.PauseTiming();
        const auto reset = qb::io::async::run_sync(fixture.db.query("TRUNCATE qb_pgsql_bench_copy"));
        if (!reset.ok() || !can_count(completed, static_cast<std::int64_t>(payload.size()))) {
            state.SkipWithError("COPY IN reset failed or cumulative byte count overflow");
            return;
        }
        CopyInput input{&payload};
        state.ResumeTiming();
        {
            const auto r = qb::io::async::run_sync(copy_in_once(&fixture.db, &input));
            state.PauseTiming();
            if (!copy_reply_ok(r) || !copy_input_complete(input) || !copy_table_matches(&fixture.db, payload, &scratch)) {
                state.SkipWithError("measured COPY IN did not persist the exact corpus");
                return;
            }
            ++completed;
            bytes += static_cast<std::int64_t>(input.offset);
            copied_rows += kCopyRows;
        }
        state.ResumeTiming();
    }
    if (completed != state.iterations() || bytes != completed * static_cast<std::int64_t>(payload.size())
        || copied_rows != completed * kCopyRows || !copy_table_matches(&fixture.db, payload, &scratch)) {
        state.SkipWithError("incomplete cumulative COPY IN workload");
        return;
    }
    state.SetItemsProcessed(copied_rows);
    state.SetBytesProcessed(bytes);
}
BENCHMARK(BM_CopyInSequential)->Unit(benchmark::kMicrosecond)->UseRealTime();

static void
BM_CopyOutSequential(benchmark::State &state) {
    Fixture     fixture;
    const auto  payload = copy_payload();
    std::string output;
    output.reserve(payload.size());
    if (!fixture.ready) {
        state.SkipWithError("postgres COPY fixture failed (set QB_PG_DSN)");
        return;
    }
    CopyInput  input{&payload};
    const auto seeded = qb::io::async::run_sync(copy_in_once(&fixture.db, &input));
    if (!copy_reply_ok(seeded) || !copy_input_complete(input) || !copy_table_matches(&fixture.db, payload, &output)) {
        state.SkipWithError("COPY OUT seed or preflight corpus mismatch");
        return;
    }

    std::int64_t completed   = 0;
    std::int64_t bytes       = 0;
    std::int64_t copied_rows = 0;
    for (auto _ : state) {
        state.PauseTiming();
        output.clear();
        if (!can_count(completed, static_cast<std::int64_t>(payload.size()))) {
            state.SkipWithError("COPY OUT cumulative byte count overflow");
            return;
        }
        state.ResumeTiming();
        {
            const auto r = qb::io::async::run_sync(copy_out_once(&fixture.db, &output));
            state.PauseTiming();
            if (!copy_reply_ok(r) || output != payload) {
                state.SkipWithError("measured COPY OUT returned a different corpus");
                return;
            }
            ++completed;
            bytes += static_cast<std::int64_t>(output.size());
            copied_rows += kCopyRows;
        }
        state.ResumeTiming();
    }
    if (completed != state.iterations() || bytes != completed * static_cast<std::int64_t>(payload.size())
        || copied_rows != completed * kCopyRows || !copy_table_matches(&fixture.db, payload, &output)) {
        state.SkipWithError("incomplete cumulative COPY OUT workload");
        return;
    }
    state.SetItemsProcessed(copied_rows);
    state.SetBytesProcessed(bytes);
}
BENCHMARK(BM_CopyOutSequential)->Unit(benchmark::kMicrosecond)->UseRealTime();

BENCHMARK_MAIN();
