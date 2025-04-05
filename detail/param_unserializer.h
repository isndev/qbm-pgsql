/**
 * @file param_unserializer.h
 * @brief Modern PostgreSQL result deserialization
 * 
 * Provides a clean, direct approach to deserialize PostgreSQL binary protocol
 * to C++ types without replacing existing functionality.
 */

#pragma once
#ifndef QBM_PGSQL_DETAIL_PARAM_UNSERIALIZER_H
#define QBM_PGSQL_DETAIL_PARAM_UNSERIALIZER_H

#include <vector>
#include <optional>
#include <string>

#include "../not-qb/common.h"

namespace qb::pg::detail {

/**
 * @brief Modern parameter deserializer for PostgreSQL binary protocol
 */
class ParamUnserializer {
public:
    ParamUnserializer() = default;

    // Helper for checking if a type is std::optional
    template<typename T>
    struct is_optional : std::false_type {};

    template<typename T>
    struct is_optional<std::optional<T>> : std::true_type {};

    // Low-level binary data reading functions
    smallint read_smallint(const std::vector<byte>& buffer);
    integer read_integer(const std::vector<byte>& buffer);
    bigint read_bigint(const std::vector<byte>& buffer);
    float read_float(const std::vector<byte>& buffer);
    double read_double(const std::vector<byte>& buffer);
    std::string read_string(const std::vector<byte>& buffer);
};

} // namespace qb::pg::detail

#endif // QBM_PGSQL_DETAIL_PARAM_UNSERIALIZER_H 