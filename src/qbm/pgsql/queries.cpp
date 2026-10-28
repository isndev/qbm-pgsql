/**
 * @file queries.cpp
 * @brief Implementation of PostgreSQL query representation and management
 *
 * This file implements the non-template members declared in queries.h that are
 * substantial enough to live in a translation unit rather than inline in the
 * header, in particular the LRU-eviction logic of PreparedStorage and the
 * parameter-count decoding of QueryParams.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#include "./queries.h"
#include <stdexcept>
#include <qb/system/endian.h> // qb::endian::from_big_endian

namespace qb::pg::detail {

PreparedStorage::PreparedStorage(const PreparedStorage &other)
    : _lru_list(other._lru_list)
    , _max_size(other._max_size)
    , _evicted_count(other._evicted_count)
    , _stale_count(other._stale_count)
    , _pending_close(other._pending_close)
    , _hits(other._hits)
    , _misses(other._misses) {
    _prepared_queries.reserve(other._prepared_queries.size());
    for (auto it = _lru_list.begin(); it != _lru_list.end(); ++it) {
        auto source = other._prepared_queries.find(*it);
        if (source == other._prepared_queries.end())
            throw std::logic_error("PreparedStorage LRU entry has no matching query");
        _prepared_queries.emplace(*it, LruEntry{source->second.name, source->second.query, it, source->second.stale});
    }
}

PreparedStorage &
PreparedStorage::operator=(const PreparedStorage &other) {
    if (this != &other) {
        PreparedStorage copy(other);
        swap(copy);
    }
    return *this;
}

// A moved-from container is valid but unspecified, and the count is a plain integer the move copies:
// the source is cleared, so that its has_stale() agrees with its marks (Huly QB-114).
PreparedStorage::PreparedStorage(PreparedStorage &&other) noexcept
    : _prepared_queries(std::move(other._prepared_queries))
    , _lru_list(std::move(other._lru_list))
    , _max_size(other._max_size)
    , _evicted_count(other._evicted_count)
    , _stale_count(other._stale_count)
    , _pending_close(std::move(other._pending_close))
    , _hits(other._hits)
    , _misses(other._misses) {
    other.clear();
}

PreparedStorage &
PreparedStorage::operator=(PreparedStorage &&other) noexcept {
    if (this != &other) {
        _prepared_queries = std::move(other._prepared_queries);
        _lru_list         = std::move(other._lru_list);
        _max_size         = other._max_size;
        _evicted_count    = other._evicted_count;
        _stale_count      = other._stale_count;
        _pending_close    = std::move(other._pending_close);
        _hits             = other._hits;
        _misses           = other._misses;
        other.clear();
    }
    return *this;
}

void
PreparedStorage::swap(PreparedStorage &other) {
    using std::swap;
    _prepared_queries.swap(other._prepared_queries);
    _lru_list.swap(other._lru_list);
    swap(_max_size, other._max_size);
    swap(_evicted_count, other._evicted_count);
    swap(_stale_count, other._stale_count);
    _pending_close.swap(other._pending_close);
    swap(_hits, other._hits);
    swap(_misses, other._misses);
}

void
PreparedStorage::set_max_size(size_t max_size) {
    _max_size = max_size > 0 ? max_size : 100;
    // Down to the new bound, not one below it: the pre-insertion rule (make room for ONE more)
    // applied here left a shrunk cache one entry short of its capacity.
    evict_down_to(_max_size);
}

void
PreparedStorage::make_room_for(std::string_view name) {
    if (!has(name))
        evict_down_to(_max_size - 1);
}

const PreparedQuery &
PreparedStorage::push(PreparedQuery &&query) {
    std::string key = query.name;

    // Check if already exists - update it and move to front
    auto it = _prepared_queries.find(key);
    if (it != _prepared_queries.end()) {
        // Relink the existing node; its stored iterator stays valid.
        _lru_list.splice(_lru_list.begin(), _lru_list, it->second.lru_iter);
        it->second.query = std::move(query);
        if (std::exchange(it->second.stale, false))
            --_stale_count;
        return it->second.query;
    }

    // Evict if at capacity. A Parse sent by the client made room already (make_room_for), so
    // in a session this finds a free slot; direct users of the cache still get the bound.
    evict_down_to(_max_size - 1);

    // Add to front of LRU list
    _lru_list.push_front(key);

    // Store in map with LRU iterator
    LruEntry entry{key, std::move(query), _lru_list.begin()};
    auto     result = _prepared_queries.emplace(std::move(key), std::move(entry));

    return result.first->second.query;
}

PreparedQuery const &
PreparedStorage::get(std::string_view name) const {
    std::string key(name);
    auto        it = _prepared_queries.find(key);
    if (it == _prepared_queries.end()) {
        throw std::out_of_range("Prepared query not found: " + key);
    }

    // Recency is observable cache state and may change on a const lookup.
    _lru_list.splice(_lru_list.begin(), _lru_list, it->second.lru_iter);

    return it->second.query;
}

void
PreparedStorage::mark_stale(std::string_view name) {
    auto it = _prepared_queries.find(std::string(name));
    if (it != _prepared_queries.end() && !std::exchange(it->second.stale, true))
        ++_stale_count;
}

bool
PreparedStorage::is_stale(std::string_view name) const {
    const auto it = _prepared_queries.find(std::string(name));
    return it != _prepared_queries.cend() && it->second.stale;
}

void
PreparedStorage::refresh(std::string_view name, row_description_type &&description) {
    auto it = _prepared_queries.find(std::string(name));
    if (it == _prepared_queries.end())
        return;
    it->second.query.row_description = std::move(description);
    if (std::exchange(it->second.stale, false))
        --_stale_count;
}

void
PreparedStorage::evict_down_to(size_t limit) {
    while (_prepared_queries.size() > limit && !_lru_list.empty())
        evict_lru();
}

void
PreparedStorage::evict_lru() {
    // Least recently used = back of the list. The backend keeps a named statement until it is
    // closed; the unnamed one is replaced by the next unnamed Parse and never needs a Close.
    // The Close is queued first: if that allocation throws, nothing has been evicted yet.
    std::string const &lru_name = _lru_list.back();
    if (!lru_name.empty())
        _pending_close.push_back(lru_name);
    const auto entry = _prepared_queries.find(lru_name);
    if (entry != _prepared_queries.end() && entry->second.stale)
        --_stale_count;
    _prepared_queries.erase(lru_name);
    _lru_list.pop_back();
    ++_evicted_count;
}

message
close_evicted_statements_before(PreparedStorage &storage, message &&tail) {
    const auto names = storage.take_pending_close();
    if (names.empty())
        return std::move(tail);
    message out(close_tag);
    out.write('S');
    out.write(names.front());
    for (std::size_t i = 1; i < names.size(); ++i) {
        message close(close_tag);
        close.write('S');
        close.write(names[i]);
        out.pack(close);
    }
    out.pack(tail);
    return out;
}

message
parse_describe_sync(PreparedQuery const &query) {
    message cmd(parse_tag);
    cmd.write(query.name);
    cmd.write(query.expression);
    cmd.write((smallint) query.param_types.size());
    for (auto oid_val : query.param_types) {
        cmd.write(static_cast<integer>(oid_val));
    }

    message describe(describe_tag);
    describe.write('S');
    describe.write(query.name);
    cmd.pack(describe);
    cmd.pack(message(sync_tag));

    return cmd;
}

message
reprepare_statement(PreparedStorage &storage, PreparedQuery const &query) {
    message close(close_tag);
    close.write('S');
    close.write(query.name);
    close.pack(parse_describe_sync(query));
    if (storage.has_pending_close())
        return close_evicted_statements_before(storage, std::move(close));
    return close;
}

smallint
QueryParams::param_count() const {
    if (_params.size() >= sizeof(smallint)) {
        // Extract the number of parameters from the buffer
        smallint count;
        std::memcpy(&count, _params.data(), sizeof(smallint));
        return qb::endian::from_big_endian(count); // network (big-endian) -> host
    }
    return 0;
}

} // namespace qb::pg::detail
