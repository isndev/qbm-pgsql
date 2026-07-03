/**
 * @file param_serializer.cpp
 * @brief Implementation of the PostgreSQL parameter serializer
 *
 * Out-of-line definitions for the non-template members of
 * qb::pg::detail::ParamSerializer. These bodies handle the low-level encoding
 * of C++ values into the PostgreSQL binary wire format (length prefixes and
 * big-endian payloads). The template members (add_param, serialize_params,
 * add_vector, ...) remain in the header.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#include <limits>
#include <stdexcept>

#include "./param_serializer.h"

namespace qb::pg::detail {

// checked_param_length(...) is the shared length-prefix guard defined in pg_types.h (qb::pg scope,
// found here via enclosing-namespace lookup) — used at every var-length write so the invariant holds
// uniformly across the param serializer AND the TypeConverter binary encoders.

void
ParamSerializer::add_string_vector(const std::vector<std::string> &values) {
    // OPTIMIZED: Reserve space to avoid O(n²) reallocations (P0-3 fix)
    // Estimate: average 4 bytes for length + 20 bytes per string
    param_types_.reserve(param_types_.size() + values.size());
    size_t estimated_bytes = values.size() * 4; // length headers
    for (const auto &value : values) {
        estimated_bytes += value.size();
    }
    params_buffer_.reserve(params_buffer_.size() + estimated_bytes);

    // For each string, we add a parameter of text type
    // to get the exact format that PostgreSQL expects for VALUES ($1),($2),...
    for (const auto &value : values) {
        // Add the OID type
        param_types_.push_back(25); // text (was oid::text)

        // Write the parameter length (4 bytes)
        integer len = checked_param_length(value.size());
        write_integer(params_buffer_, len);

        // Write the string data without terminator
        if (!value.empty()) {
            params_buffer_.insert(params_buffer_.end(), value.data(), value.data() + value.size());
        }
    }
}

void
ParamSerializer::write_smallint(std::vector<byte> &buffer, smallint value) {
    smallint    networkValue = qb::endian::to_big_endian(value);
    const byte *bytes        = reinterpret_cast<const byte *>(&networkValue);
    buffer.insert(buffer.end(), bytes, bytes + sizeof(smallint));
}

void
ParamSerializer::write_smallint_at(std::vector<byte> &buffer, size_t pos, smallint value) {
    smallint    networkValue = qb::endian::to_big_endian(value);
    const byte *bytes        = reinterpret_cast<const byte *>(&networkValue);
    std::copy(bytes, bytes + sizeof(smallint), buffer.begin() + pos);
}

void
ParamSerializer::write_integer(std::vector<byte> &buffer, integer value) {
    integer     networkValue = qb::endian::to_big_endian(value);
    const byte *bytes        = reinterpret_cast<const byte *>(&networkValue);
    buffer.insert(buffer.end(), bytes, bytes + sizeof(integer));
}

void
ParamSerializer::write_null() {
    // -1 represents NULL in PostgreSQL binary protocol
    write_integer(params_buffer_, -1);
}

void
ParamSerializer::write_bool(bool value) {
    // Write length (1 byte)
    write_integer(params_buffer_, 1);

    // Write value (PostgreSQL boolean is 1 byte)
    params_buffer_.push_back(value ? 1 : 0);
}

void
ParamSerializer::write_smallint(smallint value) {
    // Write length (2 bytes)
    write_integer(params_buffer_, 2);

    // Write value (network byte order)
    smallint    networkValue = qb::endian::to_big_endian(value);
    const byte *bytes        = reinterpret_cast<const byte *>(&networkValue);
    params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(smallint));
}

void
ParamSerializer::write_integer(integer value) {
    // Write length (4 bytes)
    write_integer(params_buffer_, 4);

    // Write value (network byte order)
    integer     networkValue = qb::endian::to_big_endian(value);
    const byte *bytes        = reinterpret_cast<const byte *>(&networkValue);
    params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(integer));
}

void
ParamSerializer::write_bigint(bigint value) {
    // Write length (8 bytes)
    write_integer(params_buffer_, 8);

    // Use endian utility for 64-bit conversion
    bigint      networkValue = qb::endian::to_big_endian(value);
    const byte *bytes        = reinterpret_cast<const byte *>(&networkValue);
    params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(bigint));
}

void
ParamSerializer::write_float(float value) {
    write_integer(params_buffer_, 4);
    uint32_t raw;
    std::memcpy(&raw, &value, sizeof(float));
    uint32_t    be    = qb::endian::to_big_endian(raw);
    const byte *bytes = reinterpret_cast<const byte *>(&be);
    params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(float));
}

void
ParamSerializer::write_double(double value) {
    write_integer(params_buffer_, 8);
    uint64_t raw;
    std::memcpy(&raw, &value, sizeof(double));
    uint64_t    be    = qb::endian::to_big_endian(raw);
    const byte *bytes = reinterpret_cast<const byte *>(&be);
    params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(double));
}

void
ParamSerializer::write_string(const std::string &value) {
    // PostgreSQL binary format: length (int32) + raw bytes (NO null terminators)

    // 1. Write the 4-byte length
    integer len = checked_param_length(value.size());
    write_integer(params_buffer_, len);

    // 2. Write the raw data WITHOUT null terminator
    if (!value.empty()) {
        // Use data() + size() to avoid any potential null terminators
        params_buffer_.insert(params_buffer_.end(), value.data(), value.data() + value.size());
    }
}

void
ParamSerializer::write_string_view(std::string_view value) {
    // PostgreSQL binary format: length (int32) + raw bytes (NO null terminators)

    // 1. Write the 4-byte length
    integer len = checked_param_length(value.size());
    write_integer(params_buffer_, len);

    // 2. Write the raw data WITHOUT null terminator
    if (!value.empty()) {
        // String views have no null terminators by design
        params_buffer_.insert(params_buffer_.end(), value.data(), value.data() + value.size());
    }
}

void
ParamSerializer::write_cstring(const char *value) {
    // Get length WITHOUT null terminator
    size_t len = strlen(value);

    // PostgreSQL binary format: length (int32) + raw bytes (NO null terminators)

    // 1. Write the 4-byte length
    write_integer(params_buffer_, checked_param_length(len));

    // 2. Write the raw data WITHOUT null terminator
    if (len > 0) {
        // Copy exactly len bytes (excluding the null terminator)
        params_buffer_.insert(params_buffer_.end(), value, value + len);
    }
}

void
ParamSerializer::write_byte_array(const byte *data, size_t size) {
    // Write length
    write_integer(params_buffer_, checked_param_length(size));

    // Write byte array data
    params_buffer_.insert(params_buffer_.end(), data, data + size);
}

} // namespace qb::pg::detail
