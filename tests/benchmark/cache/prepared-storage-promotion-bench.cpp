/**
 * @file benchmark/cache/prepared-storage-promotion-bench.cpp
 * @brief Daemon-free prepared-storage hit benchmarks for the QB-994 A/B/A comparison.
 *
 * Use this identical fixture against both the pre-fix and fixed qbm-pgsql sources.
 * It measures only existing-entry promotions; setup and replacement-query creation
 * are outside the timed region. Neither case connects to PostgreSQL.
 */

#include <benchmark/benchmark.h>
#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <qbm/pgsql/pgsql.h>

namespace {

using qb::pg::detail::PreparedQuery;
using qb::pg::detail::PreparedStorage;

constexpr std::size_t kEntries = 64;
using Names                    = std::array<std::string, kEntries>;

Names
make_names() {
    Names names;
    for (std::size_t i = 0; i < names.size(); ++i)
        names[i] = "q" + std::to_string(i);
    return names;
}

PreparedStorage
make_storage(const Names &names) {
    PreparedStorage storage(kEntries);
    for (const auto &name : names)
        storage.push(PreparedQuery{name, "SELECT 1", {}, {}});
    return storage;
}

bool
has_all_names(const PreparedStorage &storage, const Names &names) {
    if (storage.size() != kEntries)
        return false;
    for (const auto &name : names)
        if (!storage.has(name))
            return false;
    return true;
}

void
BM_PreparedStorageGetHit(benchmark::State &state) {
    const Names            names   = make_names();
    PreparedStorage        storage = make_storage(names);
    const PreparedStorage &lookup  = storage;
    if (!has_all_names(storage, names)) {
        state.SkipWithError("prepared-storage setup lost an entry");
        return;
    }

    std::size_t index = 0;
    for (auto _ : state) {
        const auto &query = lookup.get(names[index]);
        benchmark::DoNotOptimize(query.name.data());
        index = (index + 1) & (kEntries - 1);
    }
    if (!has_all_names(storage, names)) {
        state.SkipWithError("prepared-storage lookup lost an entry");
        return;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PreparedStorageGetHit);

void
BM_PreparedStorageExistingPush(benchmark::State &state) {
    const Names     names   = make_names();
    PreparedStorage storage = make_storage(names);
    if (!has_all_names(storage, names)) {
        state.SkipWithError("prepared-storage setup lost an entry");
        return;
    }

    for (auto _ : state) {
        state.PauseTiming();
        std::array<PreparedQuery, kEntries> replacements;
        for (std::size_t i = 0; i < kEntries; ++i)
            replacements[i] = PreparedQuery{names[i], "SELECT 2", {}, {}};
        state.ResumeTiming();

        for (auto &replacement : replacements) {
            const auto &stored = storage.push(std::move(replacement));
            benchmark::DoNotOptimize(stored.expression.data());
        }
        benchmark::ClobberMemory();
    }
    if (!has_all_names(storage, names)) {
        state.SkipWithError("prepared-storage update lost an entry");
        return;
    }
    for (const auto &name : names) {
        if (storage.get(name).expression != "SELECT 2") {
            state.SkipWithError("prepared-storage update kept an old query");
            return;
        }
    }
    state.SetItemsProcessed(state.iterations() * kEntries);
}
BENCHMARK(BM_PreparedStorageExistingPush);

} // namespace

BENCHMARK_MAIN();
