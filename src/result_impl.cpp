/**
 * @file result_impl.cpp
 * @brief Implementation of PostgreSQL result set internal handling
 *
 * This file contains the implementation of the result_impl class defined in
 * result_impl.h. It provides the low-level data access and manipulation functionality
 * used by the resultset class to handle PostgreSQL query results.
 *
 * Implementation details include:
 * - Row and field data access
 * - NULL value detection
 * - Buffer management for field values
 * - Row bounds checking
 *
 * @see result_impl.h
 * @see resultset.h
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "./result_impl.h"
#include <exception>
#include <iostream>
#include <sstream>
#include <string>

namespace qb {
namespace pg {
namespace detail {

/**
 * Returns the number of rows in the result set
 * @return Row count
 */
size_t
result_impl::size() const {
    return rows_.size();
}

/**
 * Checks if the result set contains any rows
 * @return true if the result set is empty, false otherwise
 */
bool
result_impl::empty() const {
    return rows_.empty();
}

result_impl
result_impl::clone_snapshot() const {
    result_impl dup;
    dup.row_description_ = row_description_;
    dup.command_tag_     = command_tag_;
    dup.rows_affected_   = rows_affected_;
    dup.rows_.reserve(rows_.size());
    for (auto const &r : rows_) {
        row_data nr;
        nr.offsets  = r.offsets;
        nr.data     = r.data;
        nr.null_map = r.null_map;
        dup.rows_.push_back(std::move(nr));
    }
    return dup;
}

/**
 * Validates that a row index is within bounds
 * @param row The row index to check
 * @throws std::out_of_range if the row index is outside valid range
 */
void
result_impl::check_row_index(uinteger row) const {
    if (row >= rows_.size()) {
        std::ostringstream out;
        out << "Row index " << row << " is out of bounds [0.." << rows_.size() << ")";
        throw std::out_of_range(out.str().c_str());
    }
}

/**
 * Retrieves the field data at the specified row and column
 * @param row The row index
 * @param col The column index
 * @return Field buffer containing the field data
 * @throws std::out_of_range if the row index is invalid
 */
field_buffer
result_impl::at(uinteger row, usmallint col) const {
    check_row_index(row);
    row_data const &rd = rows_[row];
    return rd.field_data(col);
}

/**
 * Checks if the field at the specified position is NULL
 * @param row The row index
 * @param col The column index
 * @return true if the field is NULL, false otherwise
 * @throws std::out_of_range if the row index is invalid
 */
bool
result_impl::is_null(uinteger row, usmallint col) const {
    check_row_index(row);
    row_data const &rd = rows_[row];
    return rd.is_null(col);
}

/**
 * Gets the buffer boundaries for a field value
 * @param row The row index
 * @param col The column index
 * @return Buffer boundaries for the specified field
 * @throws std::out_of_range if the row index is invalid
 */
row_data::data_buffer_bounds
result_impl::buffer_bounds(uinteger row, usmallint col) const {
    check_row_index(row);
    row_data const &rd = rows_[row];
    return rd.field_buffer_bounds(col);
}

/**
 * @brief Build the name-to-index cache for O(1) lookups
 *
 * Populates the name_cache_ map on first call. Subsequent lookups
 * use the cache for average O(1) access instead of O(n) linear search.
 */
void
result_impl::build_name_cache() const {
    if (name_cache_built_) {
        return;
    }
    name_cache_.reserve(row_description_.size() * 2); // Load factor ~0.5
    for (usmallint i = 0; i < row_description_.size(); ++i) {
        name_cache_[row_description_[i].name] = i;
    }
    name_cache_built_ = true;
}

/**
 * @brief Get column index by field name (O(1) with cache)
 *
 * Uses a lazily-built hash map for O(1) average lookup time.
 * Falls back to linear search only if cache is not yet built.
 *
 * @param name Field name to look up
 * @return Column index, or npos (-1) if not found
 */
usmallint
result_impl::column_index_of(const std::string &name) const {
    // OPTIMIZED: O(1) lookup with hash map vs O(n) linear search (P0-11 fix)
    build_name_cache();
    auto it = name_cache_.find(name);
    if (it != name_cache_.end()) {
        return it->second;
    }
    return static_cast<usmallint>(-1); // npos
}

} /* namespace detail */
} /* namespace pg */
} /* namespace qb */
