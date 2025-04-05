/**
 * @file field_handler.h
 * @brief Modern field handling for PostgreSQL result fields
 *
 * Provides functionality to extract and convert PostgreSQL result fields
 * without dependencies on the legacy protocol_* code. This file implements
 * a clean interface for accessing and converting field data from PostgreSQL
 * result sets to native C++ types, with proper handling of NULL values and
 * type conversions.
 *
 * Key features:
 * - Type-safe field value extraction
 * - Automatic handling of NULL values
 * - Support for std::optional for nullable fields
 * - Binary and text format field conversion
 * - Row-to-tuple conversion for structured binding
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

#include "./param_unserializer.h"
#include "./resultset.h"

namespace qb::pg {

// Forward declarations
class resultset;

namespace detail {

/**
 * @brief Modern field handler for PostgreSQL result fields
 *
 * Provides static methods to extract and convert field values from resultsets
 * using the modern ParamUnserializer. This class serves as a bridge between
 * raw PostgreSQL binary data and strongly-typed C++ values, handling format
 * detection, NULL value handling, and type conversions.
 *
 * The handler supports:
 * - Automatic conversion between PostgreSQL and C++ types
 * - Proper handling of NULL values with std::optional
 * - Binary and text format detection and processing
 * - Row-to-tuple conversion for destructuring query results
 * - Exception handling for type mismatches and NULL values
 */
class FieldHandler {
public:
    /**
     * @brief Deserialize a field to a specific type
     *
     * Converts a PostgreSQL field to the requested C++ type, handling both
     * binary and text format data. For NULL fields, either returns an empty
     * optional (if T is an optional type) or throws field_is_null exception.
     *
     * The conversion process:
     * 1. Checks if the field is NULL and handles accordingly
     * 2. Determines if the field data is in binary or text format
     * 3. Extracts the raw field data into a buffer
     * 4. Passes the buffer to the appropriate deserializer
     *
     * @tparam T Target C++ type to deserialize to
     * @param field Field reference from resultset
     * @return Deserialized value of type T
     * @throws field_is_null if field is null and T is not nullable
     * @throws field_type_mismatch if T is incompatible with the field type
     * @throws std::exception for other conversion errors
     */
    template <typename T>
    static T
    as(const typename resultset::field &field) {
        if (field.is_null()) {
            if constexpr (ParamUnserializer::template is_optional<T>::value) {
                return T{};
            } else {
                throw field_is_null();
            }
        }

        field_buffer buffer = field.input_buffer();
        bool is_binary = is_binary_format(field);

        return ParamUnserializer::template deserialize<T>(buffer, is_binary);
    }

    /**
     * @brief Convert field to a value, with null handling
     *
     * Attempts to convert the field to the specified type T and store it in the
     * provided value reference. If the field is NULL, returns false instead of
     * throwing an exception, providing a more graceful handling option for NULL values.
     *
     * Use this method when you want to:
     * - Handle NULL values without exceptions
     * - Modify an existing variable with the field value
     * - Get a boolean indication of NULL status
     *
     * @tparam T Target C++ type to deserialize to
     * @param field Field reference from resultset
     * @param value Output parameter to receive the converted value
     * @return true if conversion succeeded, false if field is null
     * @throws field_type_mismatch if conversion to T is not possible
     */
    template <typename T>
    static bool
    to(const typename resultset::field &field, T &value) {
        try {
            value = as<T>(field);
            return true;
        } catch (const field_is_null &) {
            return false;
        }
    }

    /**
     * @brief Convert field to std::optional value
     *
     * Specialized version for optional types that handles NULL fields by setting
     * the optional to std::nullopt. This method always returns true because
     * converting to an optional never fails due to NULL.
     *
     * This is the preferred method for handling potentially NULL fields as it provides:
     * - Exception-free handling of NULL values
     * - Clear semantics through the std::optional interface
     * - Protection against unexpected exceptions during conversion
     *
     * @tparam T Type contained within the optional
     * @param field Field reference from resultset
     * @param value Output parameter to receive the optional value
     * @return true always (optional handles both value and null cases)
     */
    template <typename T>
    static bool
    to(const typename resultset::field &field, std::optional<T> &value) {
        if (field.is_null()) {
            value = std::nullopt;
        } else {
            try {
                value = as<T>(field);
            } catch (const std::exception &) {
                value = std::nullopt;
            }
        }
        return true;
    }

    /**
     * @brief Convert an entire row to a tuple of values
     *
     * Converts each field in the row to the corresponding type in the tuple.
     * This enables structured decomposition of query results into strongly-typed
     * C++ variables, making it easier to work with complex query results.
     *
     * Benefits of using this method:
     * - Type-safe extraction of multiple fields at once
     * - Support for structured binding in modern C++
     * - Automatic handling of NULL values for each tuple element
     * - Consistent conversion behavior across all fields
     *
     * @tparam T Pack of types for each tuple element
     * @param row Row reference from the resultset
     * @param tuple Output tuple to receive the converted values
     * @throws std::exception if conversion fails and the target type is not optional
     */
    template <typename... T>
    static void
    convert_row_to_tuple(const resultset::row &row, std::tuple<T...> &tuple) {
        convert_row_to_tuple_impl(row, tuple, std::index_sequence_for<T...>{});
    }

private:
    /**
     * @brief Implementation helper for row to tuple conversion
     *
     * Uses index sequence expansion to convert each field in the row to
     * the corresponding tuple element. This template metaprogramming technique
     * allows for compile-time generation of the conversion calls.
     *
     * The implementation:
     * 1. Uses a compile-time index sequence to match fields to tuple elements
     * 2. Expands into individual calls to to() for each field/tuple element pair
     * 3. Discards the return values using an initializer list trick
     *
     * @tparam Tuple Tuple type to fill
     * @tparam Is Index sequence for the tuple elements
     * @param row Row reference containing the field data
     * @param tuple Output tuple to receive the converted values
     * @param is Index sequence instance (compile-time parameter)
     */
    template <typename Tuple, std::size_t... Is>
    static void
    convert_row_to_tuple_impl(const resultset::row &row, Tuple &tuple, std::index_sequence<Is...>) {
        (void)std::initializer_list<int>{(to(row[Is], std::get<Is>(tuple)), 0)...};
    }

    /**
     * @brief Check if field data is in binary format
     *
     * Determines whether the field's data is in PostgreSQL binary format or text format
     * by examining the format code in the field description. This affects how the
     * data should be interpreted during conversion.
     *
     * Format codes:
     * - 0: Text format (ASCII representation)
     * - 1: Binary format (native PostgreSQL binary representation)
     *
     * @param field Field reference to check
     * @return true if binary format (1), false if text format (0)
     */
    static bool
    is_binary_format(const typename resultset::field &field) {
        // Get format code from field description
        // Format code 0 = text, 1 = binary
        return field.description().format_code == pg::protocol_data_format::Binary;
    }
};

} // namespace detail

/**
 * @brief Convert a row to a tuple of values
 *
 * Global function that converts an entire row from a resultset to a tuple
 * of the specified types. This enables convenient unpacking of query results
 * into multiple variables, supporting the following patterns:
 *
 * ```cpp
 * // Convert directly to a tuple
 * std::tuple<int, std::string, std::optional<double>> values;
 * convert_to(row, values);
 *
 * // Use with structured binding (C++17)
 * std::tuple<int, std::string, std::optional<double>> values;
 * convert_to(row, values);
 * auto& [id, name, score] = values;
 * ```
 *
 * @tparam T Pack of types for each tuple element
 * @param row Row reference from the resultset
 * @param tuple Output tuple to receive the converted values
 */
template <typename... T>
inline void
convert_to(const resultset::row &row, std::tuple<T...> &tuple) {
    detail::FieldHandler::convert_row_to_tuple(row, tuple);
}

/**
 * @brief Get field value of specified type
 *
 * Global function that extracts and converts a field value to the requested type.
 * This is a convenient wrapper around FieldHandler::as() that provides a simpler
 * interface for common field extraction tasks.
 *
 * Example usage:
 * ```cpp
 * int id = get<int>(row[0]);
 * std::string name = get<std::string>(row[1]);
 * std::optional<double> score = get<std::optional<double>>(row[2]);
 * ```
 *
 * @tparam T Type to convert the field value to
 * @param field Field reference to extract from
 * @return Field value converted to type T
 * @throws field_is_null if field is null and T is not nullable
 * @throws field_type_mismatch if field cannot be converted to T
 */
template <typename T>
inline T
get(const typename resultset::field &field) {
    return detail::FieldHandler::template as<T>(field);
}

/**
 * @brief Get field value with output parameter
 *
 * Global function that attempts to convert a field value to the specified type
 * and store it in the provided reference. Returns false if the field is NULL.
 * This is a convenient wrapper around FieldHandler::to() for situations where
 * you want to handle NULL values without exceptions.
 *
 * Example usage:
 * ```cpp
 * int id;
 * std::string name;
 * std::optional<double> score;
 *
 * if (get(row[0], id) && get(row[1], name)) {
 *     // Use id and name here
 * }
 * // Optional values always succeed
 * get(row[2], score);
 * ```
 *
 * @tparam T Type to convert the field value to
 * @param field Field reference to extract from
 * @param value Output parameter to receive the converted value
 * @return true if conversion succeeded, false if field is null
 */
template <typename T>
inline bool
get(const typename resultset::field &field, T &value) {
    return detail::FieldHandler::template to<T>(field, value);
}

} // namespace qb::pg

// Add extension method to resultset::row
namespace qb::pg {

/**
 * @brief Extension method for resultset::row to convert to a tuple
 *
 * Adds a to<>() method to the resultset::row class that converts the
 * entire row to a tuple of the specified types. This enables structured
 * binding and direct conversion of query results.
 *
 * Example usage:
 * ```cpp
 * // Convert directly to a tuple
 * std::tuple<int, std::string, std::optional<double>> values;
 * row.to(values);
 *
 * // Use with structured binding (C++17)
 * auto [id, name, score] = std::tuple<int, std::string, std::optional<double>>{};
 * row.to(std::tie(id, name, score));
 * ```
 *
 * @tparam T Pack of types for each tuple element
 * @param tuple Output tuple to receive the converted values
 */
template <typename... T>
inline void
resultset::row::to(std::tuple<T...> &tuple) const {
    detail::FieldHandler::convert_row_to_tuple(*this, tuple);
}

/**
 * @brief Extension method for resultset::row to convert to a tuple of references
 *
 * Variant that supports tuples of references, allowing direct placement of
 * field values into existing variables. This is particularly useful with
 * std::tie() to unpack row values into separate variables.
 *
 * Example usage:
 * ```cpp
 * int id;
 * std::string name;
 * std::optional<double> score;
 *
 * // Unpack row into variables
 * row.to(std::tie(id, name, score));
 * ```
 *
 * @tparam T Pack of reference types
 * @param tuple Tuple of references to receive the converted values
 */
template <typename... T>
inline void
resultset::row::to(std::tuple<T &...> tuple) const {
    detail::FieldHandler::convert_row_to_tuple(*this, tuple);
}

} // namespace qb::pg
