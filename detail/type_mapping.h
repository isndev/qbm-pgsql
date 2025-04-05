/**
 * @file type_mapping.h
 * @brief C++ to PostgreSQL type mapping
 * 
 * Defines the mapping between C++ types and PostgreSQL OID types
 * for automatic parameter type deduction.
 */

#pragma once
#ifndef QBM_PGSQL_DETAIL_TYPE_MAPPING_H
#define QBM_PGSQL_DETAIL_TYPE_MAPPING_H

#include <vector>
#include <string>
#include <string_view>
#include <optional>
#include <type_traits>
#include "../not-qb/pg_types.h"

namespace qb::pg::detail {

/**
 * @brief Type mapping from C++ to PostgreSQL OID types
 * 
 * Provides mapping between C++ types and PostgreSQL OID types
 * for automatic parameter type deduction.
 */
template <typename T, typename Enable = void>
struct type_mapping {
    static constexpr integer type_oid = 705; // unknown
};

// Specializations for common C++ types
template <> struct type_mapping<bool> { static constexpr integer type_oid = 16; };        // boolean
template <> struct type_mapping<int16_t> { static constexpr integer type_oid = 21; };     // int2
template <> struct type_mapping<int32_t> { static constexpr integer type_oid = 23; };     // int4
template <> struct type_mapping<int64_t> { static constexpr integer type_oid = 20; };     // int8
template <> struct type_mapping<float> { static constexpr integer type_oid = 700; };      // float4
template <> struct type_mapping<double> { static constexpr integer type_oid = 701; };     // float8

// String types
template <> struct type_mapping<std::string> { static constexpr integer type_oid = 25; };     // text
template <> struct type_mapping<const char*> { static constexpr integer type_oid = 25; };     // text
template <> struct type_mapping<std::string_view> { static constexpr integer type_oid = 25; }; // text
template <size_t N> struct type_mapping<char[N]> { static constexpr integer type_oid = 25; }; // text

// Binary data
template <> struct type_mapping<std::vector<char>> { static constexpr integer type_oid = 17; };        // bytea
template <> struct type_mapping<std::vector<unsigned char>> { static constexpr integer type_oid = 17; }; // bytea

// Optional types (use the underlying type's OID)
template <typename T>
struct type_mapping<std::optional<T>> {
    static constexpr integer type_oid = type_mapping<T>::type_oid;
};

/**
 * @brief Get the PostgreSQL OID for a C++ type
 * 
 * @tparam T C++ type
 * @return integer PostgreSQL OID for the type
 */
template <typename T>
inline integer get_type_oid() {
    return type_mapping<typename std::decay<T>::type>::type_oid;
}

/**
 * @brief Fill a vector with PostgreSQL OID types based on C++ template types
 * 
 * Helper function that populates a vector with the corresponding PostgreSQL
 * OID types for each C++ type provided as template parameters.
 * 
 * @tparam T C++ types to convert to PostgreSQL OIDs
 * @param types_to_fill Vector to populate with OID types
 */
template <typename... T>
void fill_types(std::vector<integer> &types_to_fill) {
    types_to_fill.reserve(sizeof...(T));
    if constexpr (sizeof...(T) > 0) {
        (types_to_fill.push_back(get_type_oid<T>()), ...);
    }
}

} // namespace qb::pg::detail

#endif // QBM_PGSQL_DETAIL_TYPE_MAPPING_H 