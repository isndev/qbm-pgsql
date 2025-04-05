/**
 * @file result_impl.cpp
 * @brief Implementation of PostgreSQL result set internal handling
 *
 * This file contains the implementation of the result_impl class defined in result_impl.h.
 * It provides the low-level data access and manipulation functionality used by the
 * resultset class to handle PostgreSQL query results.
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
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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

} /* namespace detail */
} /* namespace pg */
} /* namespace qb */
