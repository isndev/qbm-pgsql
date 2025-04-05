/**
 * @file field_handler.h
 * @brief Modern field handling for PostgreSQL result fields
 * 
 * Provides functionality to extract and convert PostgreSQL result fields
 * without dependencies on the legacy protocol_* code.
 */

#pragma once
#ifndef QBM_PGSQL_DETAIL_FIELD_HANDLER_H
#define QBM_PGSQL_DETAIL_FIELD_HANDLER_H

#include "param_unserializer.h"
#include "../not-qb/resultset.h"
#include <tuple>
#include <utility>

namespace qb::pg {

// Forward declarations
class resultset;

namespace detail {

/**
 * @brief Modern field handler for PostgreSQL result fields
 * 
 * Provides static methods to extract and convert field values from resultsets
 * using the modern ParamUnserializer.
 */
class FieldHandler {
public:
    /**
     * @brief Deserialize a field to a specific type
     * 
     * @tparam T Type to deserialize to
     * @param field Field reference from resultset
     * @return Deserialized value
     * @throws field_is_null if field is null and T is not nullable
     * @throws field_type_mismatch if T is incompatible with the field type
     */
    template<typename T>
    static T as(const resultset::field& field) {
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
     * @tparam T Type to deserialize to
     * @param field Field reference from resultset
     * @param value Output parameter
     * @return true if conversion succeeded, false if field is null
     */
    template<typename T>
    static bool to(const resultset::field& field, T& value) {
        try {
            value = as<T>(field);
            return true;
        } catch (const field_is_null&) {
            return false;
        }
    }

    /**
     * @brief Convert field to std::optional value
     * 
     * @tparam T Type to deserialize to
     * @param field Field reference from resultset
     * @param value Output parameter
     * @return true always (optional handles null case)
     */
    template<typename T>
    static bool to(const resultset::field& field, std::optional<T>& value) {
        if (field.is_null()) {
            value = std::nullopt;
        } else {
            try {
                value = as<T>(field);
            } catch (const std::exception&) {
                value = std::nullopt;
            }
        }
        return true;
    }

    /**
     * @brief Make this handler available as a row convert implementation
     * 
     * @tparam T Tuple type
     * @param row Row reference
     * @param tuple Output tuple
     */
    template <typename... T>
    static void convert_row_to_tuple(const resultset::row& row, std::tuple<T...>& tuple) {
        convert_row_to_tuple_impl(row, tuple, std::index_sequence_for<T...>{});
    }

private:
    /**
     * @brief Implementation helper for row to tuple conversion
     * 
     * @tparam Tuple Tuple type
     * @tparam Is Index sequence
     * @param row Row reference
     * @param tuple Output tuple
     * @param is Index sequence instance
     */
    template <typename Tuple, std::size_t... Is>
    static void convert_row_to_tuple_impl(const resultset::row& row, Tuple& tuple, std::index_sequence<Is...>) {
        (void)std::initializer_list<int>{
            (to(row[Is], std::get<Is>(tuple)), 0)...
        };
    }

    /**
     * @brief Check if field data is in binary format
     * 
     * @param field Field reference
     * @return true if binary, false if text
     */
    static bool is_binary_format(const resultset::field& field) {
        // Get format code from field description
        // Format code 0 = text, 1 = binary
        return field.description().format_code == BINARY_DATA_FORMAT;
    }
};

} // namespace detail

/**
 * @brief Extension methods for resultset
 */
template <typename... T>
inline void convert_to(const resultset::row& row, std::tuple<T...>& tuple) {
    detail::FieldHandler::convert_row_to_tuple(row, tuple);
}

/**
 * @brief Get field value of specified type
 * 
 * @tparam T Type to convert to
 * @param field Field reference
 * @return Field value of type T
 * @throws field_is_null if field is null and T is not nullable
 * @throws field_type_mismatch if field cannot be converted to T
 */
template <typename T>
inline T get(const resultset::field& field) {
    return detail::FieldHandler::template as<T>(field);
}

/**
 * @brief Get field value with output parameter
 * 
 * @tparam T Type to convert to
 * @param field Field reference
 * @param value Output parameter
 * @return true if conversion succeeded, false if field is null
 */
template <typename T>
inline bool get(const resultset::field& field, T& value) {
    return detail::FieldHandler::template to<T>(field, value);
}

} // namespace qb::pg

// Add extension method to resultset::row
namespace qb::pg {

// Extend the resultset::row class with to<> method that uses our new field handler
template <typename... T>
inline void resultset::row::to(std::tuple<T...>& tuple) const {
    detail::FieldHandler::convert_row_to_tuple(*this, tuple);
}

template <typename... T>
inline void resultset::row::to(std::tuple<T&...> tuple) const {
    detail::FieldHandler::convert_row_to_tuple(*this, tuple);
}

} // namespace qb::pg

#endif // QBM_PGSQL_DETAIL_FIELD_HANDLER_H 