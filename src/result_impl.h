/**
 * @file result_impl.h
 * @brief Implementation details for PostgreSQL result set handling
 *
 * This file contains the implementation details for handling PostgreSQL query results.
 * It provides the internal storage and access mechanisms used by the public resultset
 * interface defined in resultset.h.
 *
 * Key components:
 * - Low-level data storage for query results
 * - Row and column access functions
 * - Result set metadata handling
 *
 * This class is not intended for direct use by application code, but is used internally
 * by the resultset class to provide its functionality.
 *
 * @see resultset.h
 * @see protocol.h
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

#pragma once

#include <vector>

#include "./common.h"
#include "./protocol.h"

namespace qb {
namespace pg {
namespace detail {

/**
 * @brief Internal implementation class for PostgreSQL result sets
 *
 * Provides the storage and access mechanisms for PostgreSQL query results.
 * This class is used by the resultset class to implement its functionality
 * while hiding implementation details from public API users.
 */
class result_impl {
public:
    /// Type definition for a collection of row data
    typedef std::vector<row_data> row_set_type;

public:
    /// Default constructor
    result_impl() = default;

    /// Copy constructor
    result_impl(result_impl &) = default;

    /// Move constructor
    result_impl(result_impl &&) = default;

    /// Copy assignment operator
    result_impl &operator=(result_impl &) = default;

    /// Move assignment operator
    result_impl &operator=(result_impl &&) = default;

    /**
     * @brief Get mutable reference to row description
     * @return Reference to row description metadata
     */
    row_description_type &
    row_description() {
        return row_description_;
    }

    /**
     * @brief Get const reference to row description
     * @return Const reference to row description metadata
     */
    row_description_type const &
    row_description() const {
        return row_description_;
    }

    /**
     * @brief Get mutable reference to the rows collection
     * @return Reference to rows collection
     */
    row_set_type &
    rows() {
        return rows_;
    }

    /**
     * @brief Get the number of rows in the result set
     * @return Number of rows
     */
    size_t size() const;

    /**
     * @brief Check if the result set is empty
     * @return true if there are no rows, false otherwise
     */
    bool empty() const;

    /**
     * @brief Get field value at the specified row and column
     * @param row Row index
     * @param col Column index
     * @return Buffer containing the field value
     */
    field_buffer at(uinteger row, usmallint col) const;

    /**
     * @brief Get the buffer bounds for a field value
     * @param row Row index
     * @param col Column index
     * @return Buffer bounds for the specified field
     */
    row_data::data_buffer_bounds buffer_bounds(uinteger row, usmallint col) const;

    /**
     * @brief Check if a field value is NULL
     * @param row Row index
     * @param col Column index
     * @return true if the field is NULL, false otherwise
     */
    bool is_null(uinteger row, usmallint col) const;

private:
    /**
     * @brief Verify that a row index is valid
     * @param row Row index to check
     * @throws std::out_of_range if the index is invalid
     */
    void check_row_index(uinteger row) const;

    row_description_type row_description_; ///< Metadata about the result columns
    row_set_type         rows_;            ///< Collection of result rows
};

} /* namespace detail */
} /* namespace pg */
} /* namespace qb */
