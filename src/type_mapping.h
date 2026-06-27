/**
 * @file type_mapping.h
 * @brief C++ to PostgreSQL type mapping for the QB Actor Framework
 *
 * This file provides the mapping system between C++ types and PostgreSQL OID types:
 *
 * - Template-based type mapping from C++ types to PostgreSQL OIDs
 * - Specializations for standard C++ types (integral, floating-point, strings)
 * - Support for QB-specific types like UUID and Timestamp
 * - Handling of binary data with bytea type
 * - Support for std::optional types
 * - Utility functions for OID type retrieval and vector population
 *
 * The type mapping system enables automatic parameter type deduction when binding
 * parameters to prepared statements, making the client more type-safe and convenient.
 *
 * @see qb::pg::detail::type_mapping
 * @see qb::pg::detail::get_type_oid
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <qb/json.h>
#include <qb/system/time.h>
#include <qb/uuid.h>

#include "./common.h"
#include "./pg_types.h"

namespace qb::pg::detail {

/**
 * @brief Type mapping from C++ to PostgreSQL OID types
 *
 * Primary template for mapping C++ types to their corresponding PostgreSQL OID types.
 * This template structure provides a consistent way to determine the appropriate
 * PostgreSQL type OID for any C++ type used in parameter binding.
 *
 * The primary template is INTENTIONALLY ill-formed: a C++ type with no mapping must
 * fail at compile time rather than silently bind the PostgreSQL 'unknown' OID (705),
 * which masked unsupported types and let, e.g., std::optional<numeric> ship as 705
 * instead of numeric. Add a type_mapping<> specialization for any new supported type
 * (this mirrors the fail-loud static_assert fallbacks in TypeConverter<T>'s
 * to_binary/to_text/from_binary/from_text).
 *
 * @tparam T The C++ type to map to a PostgreSQL OID
 * @tparam Enable Optional SFINAE enabler for conditional specializations
 */
template <typename T, typename Enable = void>
struct type_mapping {
    static_assert(!sizeof(T),
                  "qbm-pgsql: no PostgreSQL OID mapping for this C++ type. Bind a supported "
                  "type or add a type_mapping<> specialization. The generic fallback no longer "
                  "silently sends OID 705 'unknown'.");
};

// Specializations for common C++ types
template <>
struct type_mapping<bool> {
    static constexpr integer type_oid = 16;
}; // boolean
template <>
struct type_mapping<int16_t> {
    static constexpr integer type_oid = 21;
}; // int2
template <>
struct type_mapping<int32_t> {
    static constexpr integer type_oid = 23;
}; // int4
template <>
struct type_mapping<int64_t> {
    static constexpr integer type_oid = 20;
}; // int8
template <>
struct type_mapping<float> {
    static constexpr integer type_oid = 700;
}; // float4
template <>
struct type_mapping<double> {
    static constexpr integer type_oid = 701;
}; // float8

// String types
template <>
struct type_mapping<std::string> {
    static constexpr integer type_oid = 25;
}; // text
template <>
struct type_mapping<const char *> {
    static constexpr integer type_oid = 25;
}; // text
template <>
struct type_mapping<std::string_view> {
    static constexpr integer type_oid = 25;
}; // text
template <size_t N>
struct type_mapping<char[N]> {
    static constexpr integer type_oid = 25;
}; // text

// Binary data
template <>
struct type_mapping<std::vector<char>> {
    static constexpr integer type_oid = 17;
}; // bytea
template <>
struct type_mapping<std::vector<unsigned char>> {
    static constexpr integer type_oid = 17;
}; // bytea
template <>
struct type_mapping<bytea> {
    static constexpr integer type_oid = 17;
}; // bytea

// UUID type
template <>
struct type_mapping<qb::uuid> {
    static constexpr integer type_oid = 2950;
}; // uuid

// JSON types
template <>
struct type_mapping<qb::json> {
    static constexpr integer type_oid = 114;
}; // json type

template <>
struct type_mapping<qb::jsonb> {
    static constexpr integer type_oid = 3802;
}; // jsonb type

// Date and timestamp types
// qb::wall_time is a UTC instant (system_clock) -> PostgreSQL timestamptz.
template <>
struct type_mapping<qb::wall_time> {
    static constexpr integer type_oid = 1184;
}; // timestamptz

// Civil calendar / time-of-day types (qb/system/time.h).
template <>
struct type_mapping<qb::date> {
    static constexpr integer type_oid = 1082;
}; // date
template <>
struct type_mapping<qb::time_of_day> {
    static constexpr integer type_oid = 1083;
}; // time
template <>
struct type_mapping<qb::time_of_day_tz> {
    static constexpr integer type_oid = 1266;
}; // timetz
template <>
struct type_mapping<qb::calendar_interval> {
    static constexpr integer type_oid = 1186;
}; // interval
// A std::chrono::duration is bound as INTERVAL, matching its TypeConverter<> get_oid.
template <typename Rep, typename Period>
struct type_mapping<std::chrono::duration<Rep, Period>> {
    static constexpr integer type_oid = 1186;
}; // interval

// Optional types (use the underlying type's OID)
template <typename T>
struct type_mapping<std::optional<T>> {
    static constexpr integer type_oid = type_mapping<T>::type_oid;
};

/**
 * @brief Get the PostgreSQL OID for a C++ type
 *
 * Retrieves the PostgreSQL OID for a given C++ type, applying
 * std::decay to handle reference types correctly. This function
 * provides a convenient interface to the type_mapping template.
 *
 * @tparam T C++ type to convert to PostgreSQL OID
 * @return integer PostgreSQL OID for the type
 */
template <typename T>
inline integer
get_type_oid() {
    return type_mapping<typename std::decay<T>::type>::type_oid;
}

/**
 * @brief Fill a vector with PostgreSQL OID types based on C++ template types
 *
 * Helper function that populates a vector with the corresponding PostgreSQL
 * OID types for each C++ type provided as template parameters. This is useful
 * for creating type information for prepared statements with multiple parameters.
 *
 * Uses parameter pack expansion to efficiently handle any number of types.
 *
 * @tparam T Parameter pack of C++ types to convert to PostgreSQL OIDs
 * @param types_to_fill Vector to populate with OID types
 */
template <typename... T>
void
fill_types(std::vector<integer> &types_to_fill) {
    types_to_fill.reserve(sizeof...(T));
    if constexpr (sizeof...(T) > 0) {
        (types_to_fill.push_back(get_type_oid<T>()), ...);
    }
}

} // namespace qb::pg::detail