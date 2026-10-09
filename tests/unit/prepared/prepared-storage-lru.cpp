/**
 * @file unit/prepared/prepared-storage-lru.cpp
 * @brief Unit tests for the prepared-statement LRU storage + result name-cache.
 *
 * Daemon-free, parallel-safe (no `RESOURCE_LOCK`, no event loop). Extracted from the
 * four bare `TEST()`s in `test-prepared-statements.cpp`:
 *  - `PreparedStorageLRUTest.EvictionPolicy` — capacity, access-promotion, eviction count;
 *  - `PreparedStorageStressTest.HighVolumeEviction` — 1000 inserts / 100-slot cache;
 *  - `PreparedStorageStressTest.AccessPatternPromotion` — accessed entries survive;
 *  - copied/moved storage keeps its own LRU index, including self-move; const lookup promotes recency;
 *  - `NameCacheTest.LazyInitialization` — REWRITTEN from a zero-assertion placeholder
 *    (`std::cout "placeholder"`) into a real check of `result_impl::column_index_of`'s
 *    lazy O(1) name-cache (built on first lookup; miss → npos).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <gtest/gtest.h>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <qbm/pgsql/pgsql.h>

namespace {
thread_local bool fail_next_allocation = false;
}

// This test binary contains only the prepared-storage and name-cache unit tests.
// The fault is armed only around one cache call on the current thread.
void *
operator new(std::size_t size) {
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc{};
    }
    if (void *memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}

void
operator delete(void *memory) noexcept {
    std::free(memory);
}

void
operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

using namespace qb::pg;
using namespace qb::pg::detail;

namespace {

struct FailNextAllocation {
    FailNextAllocation() {
        fail_next_allocation = true;
    }
    ~FailNextAllocation() {
        fail_next_allocation = false;
    }
};

/// Build a minimal PreparedQuery (name + SQL only; types/row-description empty).
[[nodiscard]] PreparedQuery
make_query(std::string name, std::string sql) {
    return PreparedQuery{std::move(name), std::move(sql), {}, {}};
}

/// Append a named result column (only `name` and `type_oid` matter for the name cache).
void
add_column(result_impl &r, std::string name, oid type_oid) {
    field_description fd{};
    fd.name        = std::move(name);
    fd.type_oid    = type_oid;
    fd.format_code = protocol_data_format::Text;
    r.row_description().push_back(std::move(fd));
}

} // namespace

// ---------------------------------------------------------------------------
// PreparedStorage — LRU eviction
// ---------------------------------------------------------------------------

/**
 * @brief LRU eviction respects access order, capacity, and the eviction counter.
 */
TEST(PreparedStorageLRU, EvictionPolicy) {
    PreparedStorage storage(3); // Max 3 entries.

    storage.push(make_query("q1", "SELECT 1"));
    storage.push(make_query("q2", "SELECT 2"));
    storage.push(make_query("q3", "SELECT 3"));

    EXPECT_EQ(storage.size(), 3u);
    EXPECT_EQ(storage.max_size(), 3u);
    EXPECT_TRUE(storage.has("q1"));
    EXPECT_TRUE(storage.has("q2"));
    EXPECT_TRUE(storage.has("q3"));
    EXPECT_EQ(storage.evicted_count(), 0u);

    // Touch q1 so it becomes most-recently-used; q2 is now the LRU victim.
    EXPECT_EQ(storage.get("q1").expression, "SELECT 1");

    // Insert q4 over capacity → evicts q2 (least recently used).
    storage.push(make_query("q4", "SELECT 4"));

    EXPECT_EQ(storage.size(), 3u);   // still at capacity
    EXPECT_TRUE(storage.has("q1"));  // accessed → survives
    EXPECT_FALSE(storage.has("q2")); // LRU → evicted
    EXPECT_TRUE(storage.has("q3"));
    EXPECT_TRUE(storage.has("q4")); // newly inserted
    EXPECT_EQ(storage.evicted_count(), 1u);

    // Grow capacity: a further insert no longer evicts.
    storage.set_max_size(5);
    EXPECT_EQ(storage.max_size(), 5u);
    storage.push(make_query("q5", "SELECT 5"));
    EXPECT_EQ(storage.size(), 4u);
    EXPECT_EQ(storage.evicted_count(), 1u); // unchanged
    EXPECT_TRUE(storage.has("q5"));
}

/// The stored value is the exact query that was pushed (push returns/keeps it intact).
TEST(PreparedStorageLRU, PushReturnsStoredQueryAndGetReadsItBack) {
    PreparedStorage storage(2);
    const auto     &stored = storage.push(make_query("named", "SELECT name FROM t"));

    EXPECT_EQ(stored.name, "named");
    EXPECT_EQ(stored.expression, "SELECT name FROM t");
    EXPECT_EQ(storage.get("named").expression, "SELECT name FROM t");
}

/// `get()` on an absent name throws (documented `std::out_of_range`).
TEST(PreparedStorageLRU, GetMissingThrows) {
    PreparedStorage storage(2);
    storage.push(make_query("present", "SELECT 1"));
    EXPECT_FALSE(storage.has("absent"));
    EXPECT_THROW((void) storage.get("absent"), std::out_of_range);
}

// ---------------------------------------------------------------------------
// PreparedStorage — stress
// ---------------------------------------------------------------------------

/**
 * @brief 1000 inserts into a 100-slot cache: size capped, 900 evicted, last 100 kept.
 */
TEST(PreparedStorageStress, HighVolumeEviction) {
    constexpr size_t CACHE_SIZE  = 100;
    constexpr size_t NUM_QUERIES = 1000;

    PreparedStorage storage(CACHE_SIZE);

    for (size_t i = 0; i < NUM_QUERIES; ++i) {
        storage.push(make_query("query_" + std::to_string(i), "SELECT " + std::to_string(i)));
        ASSERT_LE(storage.size(), CACHE_SIZE); // never exceeds capacity
    }

    EXPECT_EQ(storage.size(), CACHE_SIZE);
    EXPECT_EQ(storage.evicted_count(), NUM_QUERIES - CACHE_SIZE);

    // Exactly the final 100 (query_900..query_999) remain.
    for (size_t i = 0; i < NUM_QUERIES; ++i) {
        const std::string name = "query_" + std::to_string(i);
        if (i >= NUM_QUERIES - CACHE_SIZE)
            EXPECT_TRUE(storage.has(name)) << name << " should be retained";
        else
            EXPECT_FALSE(storage.has(name)) << name << " should be evicted";
    }
}

/**
 * @brief Accessed entries are promoted and survive; never-accessed entries are evicted.
 */
TEST(PreparedStorageStress, AccessPatternPromotion) {
    PreparedStorage storage(5);

    for (int i = 0; i < 5; ++i)
        storage.push(make_query("q" + std::to_string(i), "SELECT " + std::to_string(i)));

    // Promote q0, q1, q2 to most-recently-used.
    EXPECT_EQ(storage.get("q0").expression, "SELECT 0");
    EXPECT_EQ(storage.get("q1").expression, "SELECT 1");
    EXPECT_EQ(storage.get("q2").expression, "SELECT 2");

    // Two inserts over capacity evict the two never-touched entries (q3, q4).
    storage.push(make_query("q5", "SELECT 5"));
    storage.push(make_query("q6", "SELECT 6"));

    EXPECT_TRUE(storage.has("q0"));
    EXPECT_TRUE(storage.has("q1"));
    EXPECT_TRUE(storage.has("q2"));
    EXPECT_FALSE(storage.has("q3"));
    EXPECT_FALSE(storage.has("q4"));
    EXPECT_TRUE(storage.has("q5"));
    EXPECT_TRUE(storage.has("q6"));
    EXPECT_EQ(storage.evicted_count(), 2u);
}

TEST(PreparedStorageLRU, CopyConstructionKeepsQueriesAndOrderIndependent) {
    PreparedStorage source(2);
    source.push(make_query("q1", "SELECT 1"));
    source.push(make_query("q2", "SELECT 2"));

    PreparedStorage copy(source);
    EXPECT_EQ(copy.get("q1").expression, "SELECT 1"); // promote only in copy
    source.push(make_query("q1", "SELECT 11"));
    EXPECT_EQ(copy.get("q1").expression, "SELECT 1");
    copy.push(make_query("q3", "SELECT 3"));
    EXPECT_FALSE(copy.has("q2"));
    EXPECT_TRUE(source.has("q2"));
    EXPECT_EQ(source.get("q1").expression, "SELECT 11");
}

TEST(PreparedStorageLRU, CopyAssignmentSurvivesSourceDestruction) {
    PreparedStorage copy(2);
    copy.push(make_query("old", "SELECT 9"));
    {
        PreparedStorage source(2);
        source.push(make_query("q1", "SELECT 1"));
        source.push(make_query("q2", "SELECT 2"));
        copy = source;
    }

    EXPECT_FALSE(copy.has("old"));
    EXPECT_EQ(copy.get("q1").expression, "SELECT 1");
    copy.push(make_query("q3", "SELECT 3"));
    EXPECT_FALSE(copy.has("q2"));
    EXPECT_TRUE(copy.has("q1"));
    const auto &alias = copy;
    copy              = alias;
    EXPECT_EQ(copy.get("q1").expression, "SELECT 1");
}

TEST(PreparedStorageLRU, ConstLookupPromotesRecency) {
    PreparedStorage source(2);
    source.push(make_query("q1", "SELECT 1"));
    source.push(make_query("q2", "SELECT 2"));
    const PreparedStorage storage(std::move(source));

    EXPECT_EQ(storage.get("q1").expression, "SELECT 1");
    PreparedStorage copy(storage);
    copy.push(make_query("q3", "SELECT 3"));
    EXPECT_TRUE(copy.has("q1"));
    EXPECT_FALSE(copy.has("q2"));
}

TEST(PreparedStorageLRU, ConstLookupPromotionSurvivesAllocationFailure) {
    PreparedStorage source(2);
    source.push(make_query("a", "SELECT 1"));
    source.push(make_query("b", "SELECT 2"));
    const PreparedStorage &storage = source;

    bool threw = false;
    {
        FailNextAllocation fault;
        try {
            (void) storage.get("a");
        } catch (const std::bad_alloc &) {
            threw = true;
        }
    }
    ASSERT_FALSE(threw);
    EXPECT_EQ(storage.size(), 2u);

    PreparedStorage copy(storage);
    copy.push(make_query("c", "SELECT 3"));
    EXPECT_TRUE(copy.has("a"));
    EXPECT_FALSE(copy.has("b"));
    EXPECT_TRUE(copy.has("c"));
}

TEST(PreparedStorageLRU, ExistingPushPromotionSurvivesAllocationFailure) {
    PreparedStorage storage(2);
    storage.push(make_query("a", "SELECT 1"));
    storage.push(make_query("b", "SELECT 2"));
    auto replacement = make_query("a", "SELECT 11");

    bool threw = false;
    {
        FailNextAllocation fault;
        try {
            (void) storage.push(std::move(replacement));
        } catch (const std::bad_alloc &) {
            threw = true;
        }
    }
    ASSERT_FALSE(threw);
    EXPECT_EQ(storage.size(), 2u);
    EXPECT_EQ(storage.get("a").expression, "SELECT 11");

    storage.push(make_query("c", "SELECT 3"));
    EXPECT_TRUE(storage.has("a"));
    EXPECT_FALSE(storage.has("b"));
    EXPECT_TRUE(storage.has("c"));
}

TEST(PreparedStorageLRU, MoveConstructionAndAssignmentRetainOrder) {
    PreparedStorage source(2);
    source.push(make_query("q1", "SELECT 1"));
    source.push(make_query("q2", "SELECT 2"));
    PreparedStorage moved(std::move(source));
    source.clear();
    EXPECT_EQ(moved.get("q1").expression, "SELECT 1");
    moved.push(make_query("q3", "SELECT 3"));
    EXPECT_FALSE(moved.has("q2"));

    PreparedStorage assigned(1);
    assigned.push(make_query("old", "SELECT 9"));
    assigned = std::move(moved);
    if (moved.has("old")) {
        ASSERT_EQ(moved.size(), 1u);
        EXPECT_EQ(moved.get("old").expression, "SELECT 9");
    }
    moved.clear();
    EXPECT_EQ(assigned.max_size(), 2u);
    EXPECT_EQ(assigned.get("q1").expression, "SELECT 1");
    EXPECT_TRUE(assigned.has("q3"));
}

TEST(PreparedStorageLRU, SelfMoveKeepsIndexAndOrderUsable) {
    PreparedStorage storage(2);
    storage.push(make_query("q1", "SELECT 1"));
    storage.push(make_query("q2", "SELECT 2"));

    auto &same = storage;
    storage    = std::move(same);
    EXPECT_EQ(storage.size(), 2u);
    EXPECT_EQ(storage.get("q1").expression, "SELECT 1");

    PreparedStorage copy(storage);
    copy.push(make_query("q3", "SELECT 3"));
    EXPECT_TRUE(copy.has("q1"));
    EXPECT_FALSE(copy.has("q2"));

    storage.push(make_query("q3", "SELECT 3"));
    EXPECT_TRUE(storage.has("q1"));
    EXPECT_FALSE(storage.has("q2"));
}

// ---------------------------------------------------------------------------
// result_impl — lazy O(1) column name cache (rewritten from a no-assert placeholder)
// ---------------------------------------------------------------------------

/**
 * @brief `column_index_of` builds its name→index map lazily and returns npos on miss.
 *
 * The old `NameCacheTest.LazyInitialization` asserted nothing (a `std::cout` placeholder
 * deferred "to integration"). The cache is in fact pure-logic: `result_impl` holds a
 * `mutable` name map built on the first `column_index_of` call from the row description.
 * Here we populate the description directly, then verify every name resolves to its
 * 0-based position and an unknown name resolves to npos (`(usmallint)-1`).
 */
TEST(NameCache, LazyInitializationResolvesColumnsAndMisses) {
    result_impl r;
    add_column(r, "id", oid::int4);
    add_column(r, "name", oid::text);
    add_column(r, "created_at", oid::timestamptz);

    // First lookup triggers the lazy build; the index is the column's position.
    EXPECT_EQ(r.column_index_of("id"), 0u);
    EXPECT_EQ(r.column_index_of("name"), 1u);
    EXPECT_EQ(r.column_index_of("created_at"), 2u);

    // Unknown column → npos sentinel (static_cast<usmallint>(-1)).
    EXPECT_EQ(r.column_index_of("missing"), static_cast<usmallint>(-1));

    // Repeated lookups (cache already built) stay consistent.
    EXPECT_EQ(r.column_index_of("name"), 1u);
}

/// An empty result set has no columns: every lookup is a npos miss.
TEST(NameCache, EmptyDescriptionAlwaysMisses) {
    result_impl r;
    EXPECT_EQ(r.column_index_of("anything"), static_cast<usmallint>(-1));
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
