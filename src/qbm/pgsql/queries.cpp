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
    , _evicted_count(other._evicted_count) {
    _prepared_queries.reserve(other._prepared_queries.size());
    for (auto it = _lru_list.begin(); it != _lru_list.end(); ++it) {
        auto source = other._prepared_queries.find(*it);
        if (source == other._prepared_queries.end())
            throw std::logic_error("PreparedStorage LRU entry has no matching query");
        _prepared_queries.emplace(*it, LruEntry{source->second.name, source->second.query, it});
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

PreparedStorage &
PreparedStorage::operator=(PreparedStorage &&other) noexcept {
    if (this != &other) {
        _prepared_queries = std::move(other._prepared_queries);
        _lru_list         = std::move(other._lru_list);
        _max_size         = other._max_size;
        _evicted_count    = other._evicted_count;
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
}

void
PreparedStorage::set_max_size(size_t max_size) {
    _max_size = max_size > 0 ? max_size : 100;
    evict_if_needed(); // Evict immediately if over capacity
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
        return it->second.query;
    }

    // Evict if at capacity
    evict_if_needed();

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
PreparedStorage::evict_if_needed() {
    while (_prepared_queries.size() >= _max_size && !_lru_list.empty()) {
        // Get least recently used (back of list)
        std::string &lru_name = _lru_list.back();
        _prepared_queries.erase(lru_name);
        _lru_list.pop_back();
        ++_evicted_count;
    }
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
