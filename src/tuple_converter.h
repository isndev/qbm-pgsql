/**
 * @file tuple_converter.h
 * @brief PostgreSQL to C++ tuple conversion system for the QB Actor Framework
 *
 * This file provides utilities for converting PostgreSQL query results to C++ tuples:
 *
 * - Generic conversion between different tuple types
 * - Row-to-tuple conversion for query results
 * - Support for references to avoid unnecessary copies
 * - Field-by-field extraction with type checking
 * - Integration with the FieldReader system for type conversions
 * - Compile-time size validation for tuples
 *
 * The tuple converter system enables more idiomatic C++ usage patterns
 * when working with PostgreSQL query results, allowing direct mapping
 * of query results to strongly-typed C++ data structures.
 *
 * @see qb::pg::detail::row_to_tuple
 * @see qb::pg::detail::row_to_refs
 * @see qb::pg::detail::row_to_values
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

#include <tuple>
#include <utility>

#include "./field_reader.h"
#include "./resultset.h"

namespace qb::pg::detail {

/**
 * @brief Implementation helper for tuple conversion
 *
 * Low-level utility function that implements the actual element-by-element
 * conversion between tuples using C++17 fold expressions and index sequences.
 *
 * @tparam FromTuple Source tuple type
 * @tparam ToTuple Destination tuple type
 * @tparam Indices Parameter pack of indices for tuple elements
 * @param from Source tuple
 * @param to Destination tuple
 * @param Index sequence for tuple elements
 */
template <typename FromTuple, typename ToTuple, std::size_t... Indices>
void
convert_tuple_impl(const FromTuple &from, ToTuple &to, std::index_sequence<Indices...>) {
    // Use pack expansion to convert each element
    ((std::get<Indices>(to) = std::get<Indices>(from)), ...);
}

/**
 * @brief Converts a source tuple to a destination tuple
 *
 * Converts elements from one tuple type to another, handling
 * different-sized tuples by using the smaller of the two sizes.
 * Uses compile-time size calculation for efficiency.
 *
 * @tparam FromTuple Source tuple type
 * @tparam ToTuple Destination tuple type
 * @param from Source tuple
 * @param to Destination tuple
 */
template <typename FromTuple, typename ToTuple>
void
convert_tuple(const FromTuple &from, ToTuple &to) {
    constexpr std::size_t size =
        std::min(std::tuple_size_v<FromTuple>, std::tuple_size_v<ToTuple>);

    convert_tuple_impl(from, to, std::make_index_sequence<size>{});
}

/**
 * @brief Converts a PostgreSQL row to a C++ tuple
 *
 * Transforms a row from a PostgreSQL query result into a C++ tuple
 * with strongly-typed elements. Validates that the row has at least
 * as many columns as the tuple has elements.
 *
 * @tparam Ts Types of the tuple elements
 * @param row PostgreSQL row from a query result
 * @param tuple Destination tuple to store the converted values
 * @return true if the conversion was successful, false otherwise
 */
template <typename... Ts>
bool
row_to_tuple(const resultset::row &row, std::tuple<Ts...> &tuple) {
    // Check that the number of columns matches
    if (row.size() < sizeof...(Ts)) {
        return false; // Not enough columns
    }

    return convert_tuple(row, tuple);
}

/**
 * @brief Converts a PostgreSQL row to a tuple of references
 *
 * Similar to row_to_tuple, but works with a tuple of references
 * to avoid copying values. This is useful when working with existing
 * variables that need to be populated from a query result.
 *
 * @tparam Ts Types of the references in the tuple
 * @param row PostgreSQL row from a query result
 * @param refs Tuple of references to populate
 * @return true if the conversion was successful, false otherwise
 */
template <typename... Ts>
bool
row_to_refs(const resultset::row &row, std::tuple<Ts &...> refs) {
    // Create a temporary tuple and convert it
    std::tuple<std::remove_reference_t<Ts>...> temp;

    if (!row_to_tuple(row, temp)) {
        return false;
    }

    // Assign values to the references
    (void) std::initializer_list<int>{
        (std::get<Ts &>(refs) = std::get<std::remove_reference_t<Ts>>(temp), 0)...};

    return true;
}

/**
 * @brief Converts individual PostgreSQL fields to values
 *
 * Extracts and converts values directly from a PostgreSQL row into
 * separate variables. This provides a simpler interface when working
 * with individual values rather than tuples.
 *
 * @tparam Ts Types of the values to extract
 * @param row PostgreSQL row from a query result
 * @param values Destination variables to store the converted values
 * @return true if the conversion was successful, false otherwise
 */
template <typename... Ts>
bool
row_to_values(const resultset::row &row, Ts &...values) {
    // Check that the number of columns matches
    if (row.size() < sizeof...(Ts)) {
        return false; // Not enough columns
    }

    // Convert each value using an index sequence
    std::size_t index = 0;
    return (read_field(row[index++], values) && ...);
}

} // namespace qb::pg::detail