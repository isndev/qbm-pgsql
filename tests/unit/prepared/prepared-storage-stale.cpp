/**
 * @file unit/prepared/prepared-storage-stale.cpp
 * @brief Unit tests for the stale marks of the prepared-statement storage (Huly QB-114).
 *
 * Daemon-free, parallel-safe (no `RESOURCE_LOCK`, no event loop). A statement whose cached plan the
 * server reported stale is marked (`mark_stale()`) until it is prepared again (`refresh()`), and every
 * execution pays one `has_stale()` test -- a count of the marked entries. That count must agree with
 * the marks through every operation that adds, drops or moves an entry:
 *  - a mark is counted once, and a name that is not cached is not marked;
 *  - `refresh()` clears the mark and stores the new columns; a Parse of the same name (`push()`)
 *    replaces the statement and its mark;
 *  - an LRU eviction -- an insertion over capacity, a shrink, the room a Parse makes -- takes the
 *    entry's mark with it, and only that one; `clear()` (a new backend session) drops them all;
 *  - copy construction carries each entry's mark and the count, independently of the source; copy
 *    assignment -- a copy swapped in, `swap()` being private -- replaces the target's marks with the
 *    source's; move construction and assignment carry them, and leave the source empty with no mark
 *    counted -- usable as it is, as after `clear()`.
 * The wire behaviour the marks drive is in `system/query/prepared-replan-wire.cpp`.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using namespace qb::pg::detail;

namespace {

/// Build a minimal PreparedQuery (name + SQL only; types/row-description empty).
[[nodiscard]] PreparedQuery
make_query(std::string name, std::string sql) {
    return PreparedQuery{std::move(name), std::move(sql), {}, {}};
}

/// The text columns a re-preparation's Describe returns.
[[nodiscard]] row_description_type
columns(std::vector<std::string> const &names) {
    row_description_type out;
    for (auto const &name : names) {
        field_description fd{};
        fd.name        = name;
        fd.type_oid    = oid::text;
        fd.format_code = protocol_data_format::Text;
        out.push_back(std::move(fd));
    }
    return out;
}

/// The state a moved-from storage must be left in: what `clear()` leaves -- no entry, no pending Close,
/// no mark counted -- and usable as it is: a mark set and cleared again leaves nothing counted.
void
expect_left_empty(PreparedStorage &from) {
    EXPECT_EQ(from.size(), 0u);
    EXPECT_FALSE(from.has("a"));
    EXPECT_FALSE(from.is_stale("a"));
    EXPECT_FALSE(from.has_pending_close());
    EXPECT_FALSE(from.has_stale()) << "a count with no marked entry behind it: every execution would test the marks";
    from.push(make_query("a", "SELECT 1"));
    from.mark_stale("a");
    EXPECT_TRUE(from.has_stale());
    from.refresh("a", {});
    EXPECT_FALSE(from.has_stale()) << "the count did not restart from zero";
}

} // namespace

/// A mark is set on a cached name only, and counted once however often the server reports it.
TEST(PreparedStorageStale, AMarkIsSetOnACachedNameAndCountedOnce) {
    PreparedStorage storage(4);
    storage.push(make_query("a", "SELECT * FROM t"));
    storage.push(make_query("b", "SELECT * FROM u"));
    EXPECT_FALSE(storage.has_stale());
    EXPECT_FALSE(storage.is_stale("a"));

    storage.mark_stale("absent"); // not cached: nothing to mark
    EXPECT_FALSE(storage.has_stale());
    EXPECT_FALSE(storage.is_stale("absent"));
    EXPECT_FALSE(storage.has("absent"));

    storage.mark_stale("a");
    storage.mark_stale("a"); // a second report of the same statement
    EXPECT_TRUE(storage.has_stale());
    EXPECT_TRUE(storage.is_stale("a"));
    EXPECT_FALSE(storage.is_stale("b"));

    storage.refresh("a", columns({"id"})); // one re-preparation clears it
    EXPECT_FALSE(storage.is_stale("a"));
    EXPECT_FALSE(storage.has_stale());
}

/// `refresh()` clears the mark of its statement only and stores the new columns; a Parse of the same
/// name replaces the statement, mark included.
TEST(PreparedStorageStale, RefreshAndAParseOfTheSameNameClearTheMark) {
    PreparedStorage storage(4);
    storage.push(make_query("a", "SELECT * FROM t"));
    storage.push(make_query("b", "SELECT * FROM u"));
    storage.mark_stale("a");
    storage.mark_stale("b");
    EXPECT_TRUE(storage.has_stale());

    storage.refresh("a", columns({"id", "v"}));
    EXPECT_FALSE(storage.is_stale("a"));
    EXPECT_TRUE(storage.is_stale("b"));
    EXPECT_TRUE(storage.has_stale()) << "b is still marked";
    ASSERT_NE(storage.find("a"), nullptr);
    EXPECT_EQ(storage.find("a")->row_description.size(), 2u);

    storage.refresh("a", {}); // not marked: its columns change, the count does not
    EXPECT_TRUE(storage.find("a")->row_description.empty());
    EXPECT_FALSE(storage.is_stale("a"));
    EXPECT_TRUE(storage.has_stale());
    storage.refresh("absent", columns({"x"})); // not cached: a no-op
    EXPECT_FALSE(storage.has("absent"));
    EXPECT_TRUE(storage.has_stale());

    storage.push(make_query("b", "SELECT * FROM u")); // ParseComplete of the same name
    EXPECT_FALSE(storage.is_stale("b"));
    EXPECT_FALSE(storage.has_stale());
}

/// An eviction takes the evicted entry's mark with it, and leaves the others counted.
TEST(PreparedStorageStale, AnEvictionTakesTheMarkOfTheEvictedEntryOnly) {
    PreparedStorage storage(2);
    storage.push(make_query("a", "SELECT 1"));
    storage.push(make_query("b", "SELECT 2"));
    storage.mark_stale("a"); // a mark does not promote: "a" is still the LRU

    storage.push(make_query("c", "SELECT 3")); // over capacity: evicts "a"
    EXPECT_FALSE(storage.has("a"));
    EXPECT_FALSE(storage.is_stale("a"));
    EXPECT_FALSE(storage.has_stale()) << "the evicted entry took its mark";

    storage.mark_stale("c");
    storage.push(make_query("d", "SELECT 4")); // evicts "b", unmarked
    EXPECT_FALSE(storage.has("b"));
    EXPECT_TRUE(storage.is_stale("c"));
    EXPECT_TRUE(storage.has_stale()) << "an unmarked eviction leaves the other marks counted";

    storage.mark_stale("d");
    storage.set_max_size(1); // a shrink evicts at once: "c", marked
    EXPECT_FALSE(storage.has("c"));
    EXPECT_TRUE(storage.is_stale("d"));
    EXPECT_TRUE(storage.has_stale());

    storage.make_room_for("e"); // the room a Parse makes: evicts "d", marked
    EXPECT_FALSE(storage.has("d"));
    EXPECT_EQ(storage.size(), 0u);
    EXPECT_FALSE(storage.has_stale());
    EXPECT_EQ(storage.evicted_count(), 4u);
}

/// `clear()` -- a new backend session -- drops every mark; the names prepared again start unmarked.
TEST(PreparedStorageStale, ClearDropsEveryMark) {
    PreparedStorage storage(4);
    storage.push(make_query("a", "SELECT 1"));
    storage.push(make_query("b", "SELECT 2"));
    storage.mark_stale("a");
    storage.mark_stale("b");

    storage.clear();
    EXPECT_FALSE(storage.has_stale());
    EXPECT_FALSE(storage.is_stale("a"));

    storage.push(make_query("a", "SELECT 1"));
    EXPECT_FALSE(storage.is_stale("a"));
    EXPECT_FALSE(storage.has_stale());
    storage.mark_stale("a");
    storage.refresh("a", {});
    EXPECT_FALSE(storage.has_stale()) << "the count restarted from zero";
}

/// A copy carries each entry's mark and the count, and the two then go their own way.
TEST(PreparedStorageStale, ACopyCarriesTheMarksIndependentlyOfItsSource) {
    PreparedStorage source(4);
    source.push(make_query("a", "SELECT 1"));
    source.push(make_query("b", "SELECT 2"));
    source.mark_stale("a");

    PreparedStorage copy(source);
    EXPECT_TRUE(copy.has_stale());
    EXPECT_TRUE(copy.is_stale("a"));
    EXPECT_FALSE(copy.is_stale("b"));
    copy.refresh("a", {});
    EXPECT_FALSE(copy.has_stale());
    EXPECT_TRUE(source.is_stale("a"));
    EXPECT_TRUE(source.has_stale()) << "the copy's refresh is its own";

    // Copy assignment builds a copy and swaps it in (`swap()` is private), here over a storage with
    // two marks of its own -- not one, so a count left unswapped would show: they leave with its old
    // entries.
    PreparedStorage assigned(4);
    assigned.push(make_query("x", "SELECT 8"));
    assigned.push(make_query("y", "SELECT 9"));
    assigned.mark_stale("x");
    assigned.mark_stale("y");
    assigned = source;
    EXPECT_FALSE(assigned.has("x"));
    EXPECT_TRUE(assigned.is_stale("a"));
    EXPECT_FALSE(assigned.is_stale("b"));
    EXPECT_TRUE(assigned.has_stale());
    assigned.refresh("a", {});
    EXPECT_FALSE(assigned.has_stale()) << "the old entries' marks did not come along";
    EXPECT_TRUE(source.has_stale());
}

/// A move carries the entries with their marks, the count and the pending Closes, and leaves the
/// source empty.
TEST(PreparedStorageStale, AMoveCarriesTheMarksAndLeavesTheSourceEmpty) {
    PreparedStorage source(2);
    source.push(make_query("z", "SELECT 0"));
    source.push(make_query("a", "SELECT 1"));
    source.push(make_query("b", "SELECT 2")); // over capacity: evicts "z", its Close pending
    source.mark_stale("a");
    source.mark_stale("b");

    PreparedStorage moved(std::move(source));
    EXPECT_TRUE(moved.has_stale());
    EXPECT_TRUE(moved.is_stale("a"));
    EXPECT_TRUE(moved.is_stale("b"));
    EXPECT_TRUE(moved.has_pending_close());
    expect_left_empty(source);
    moved.refresh("a", {});
    EXPECT_TRUE(moved.has_stale()) << "b is still marked";

    // Assigned over a storage with two marks of its own: they go with its old entries.
    PreparedStorage assigned(4);
    assigned.push(make_query("x", "SELECT 8"));
    assigned.push(make_query("y", "SELECT 9"));
    assigned.mark_stale("x");
    assigned.mark_stale("y");
    assigned = std::move(moved);
    EXPECT_FALSE(assigned.has("x"));
    EXPECT_FALSE(assigned.is_stale("a"));
    EXPECT_TRUE(assigned.is_stale("b"));
    EXPECT_TRUE(assigned.has_stale());
    EXPECT_TRUE(assigned.has_pending_close());
    assigned.refresh("b", {});
    EXPECT_FALSE(assigned.has_stale()) << "the count came with the entries";
    expect_left_empty(moved);
}
