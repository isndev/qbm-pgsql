/**
 * @file param_unserializer.cpp
 * @brief Implementation of binary data reading functions
 *
 * This file contains the implementation of the ParamUnserializer class methods
 * that handle the conversion from PostgreSQL binary wire format to C++ native types.
 * It includes endianness conversion, buffer validation, and format detection logic.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <qb/system/endian.h>

#include "./param_unserializer.h"

namespace qb::pg::detail {

/**
 * @brief Reads a 2-byte integer from a binary buffer
 *
 * Converts the network byte order (big-endian) representation to host byte order.
 * Validates that the buffer contains at least enough bytes for a smallint.
 *
 * @param buffer The binary buffer containing the smallint value
 * @return smallint The converted 2-byte integer in host byte order
 * @throws std::runtime_error If the buffer is too small to contain a smallint
 */
smallint
ParamUnserializer::read_smallint(std::span<const byte> buffer) {
    // Verify minimum buffer size for a smallint
    if (buffer.size() < sizeof(smallint)) {
        throw std::runtime_error("Buffer too small for smallint");
    }

    // Convert from network byte order (big-endian). memcpy + qb::endian (not a
    // type-punning union, whose cross-member read is UB) — matches read_bigint.
    smallint value;
    std::memcpy(&value, buffer.data(), sizeof(smallint));
    return qb::endian::from_big_endian(value);
}

/**
 * @brief Reads a 4-byte integer from a binary buffer
 *
 * Converts the network byte order (big-endian) representation to host byte order.
 * Validates that the buffer contains at least enough bytes for an integer.
 *
 * @param buffer The binary buffer containing the integer value
 * @return integer The converted 4-byte integer in host byte order
 * @throws std::runtime_error If the buffer is too small to contain an integer
 */
integer
ParamUnserializer::read_integer(std::span<const byte> buffer) {
    // Verify minimum buffer size for an integer
    if (buffer.size() < sizeof(integer)) {
        throw std::runtime_error("Buffer too small for integer");
    }

    // Convert from network byte order (big-endian). memcpy + qb::endian (not a
    // type-punning union, whose cross-member read is UB) — matches read_bigint.
    integer value;
    std::memcpy(&value, buffer.data(), sizeof(integer));
    return qb::endian::from_big_endian(value);
}

/**
 * @brief Reads an 8-byte integer from a binary buffer
 *
 * Uses endian utility to convert from big-endian (network byte order)
 * to host byte order.
 *
 * @param buffer The binary buffer containing the bigint value
 * @return bigint The converted 8-byte integer in host byte order
 * @throws std::runtime_error If the buffer is too small to contain a bigint
 */
bigint
ParamUnserializer::read_bigint(std::span<const byte> buffer) {
    // Verify minimum buffer size for a bigint
    if (buffer.size() < sizeof(bigint)) {
        throw std::runtime_error("Buffer too small for bigint");
    }

    // Create a temporary bigint value from the buffer
    bigint value;
    std::memcpy(&value, buffer.data(), sizeof(bigint));

    // Convert from big-endian to host byte order
    return qb::endian::from_big_endian(value);
}

/**
 * @brief Reads a single-precision floating point value from a binary buffer
 *
 * Uses endian utility for 32-bit floats to convert from network byte order
 * (big-endian) to host byte order.
 *
 * @param buffer The binary buffer containing the float value
 * @return float The converted single-precision floating point value
 * @throws std::runtime_error If the buffer is too small to contain a float
 */
float
ParamUnserializer::read_float(std::span<const byte> buffer) {
    // Verify minimum buffer size for a float
    if (buffer.size() < sizeof(float)) {
        throw std::runtime_error("Buffer too small for float");
    }

    // Create a temporary value to hold the bytes
    uint32_t value;
    std::memcpy(&value, buffer.data(), sizeof(float));

    // Convert from big-endian to host byte order
    uint32_t host_value = qb::endian::from_big_endian(value);

    // Convert to float
    float result;
    std::memcpy(&result, &host_value, sizeof(float));

    return result;
}

/**
 * @brief Reads a double-precision floating point value from a binary buffer
 *
 * Uses endian utility for 64-bit doubles to convert from network byte order
 * (big-endian) to host byte order.
 *
 * @param buffer The binary buffer containing the double value
 * @return double The converted double-precision floating point value
 * @throws std::runtime_error If the buffer is too small to contain a double
 */
double
ParamUnserializer::read_double(std::span<const byte> buffer) {
    // Verify minimum buffer size for a double
    if (buffer.size() < sizeof(double)) {
        throw std::runtime_error("Buffer too small for double");
    }

    // Create a temporary value to hold the bytes
    uint64_t value;
    std::memcpy(&value, buffer.data(), sizeof(double));

    // Convert from big-endian to host byte order
    uint64_t host_value = qb::endian::from_big_endian(value);

    // Convert to double
    double result;
    std::memcpy(&result, &host_value, sizeof(double));

    return result;
}

/**
 * @brief Reads a string from a binary buffer with format auto-detection
 *
 * Attempts to determine whether the buffer contains a string in binary
 * or text format, and delegates to the appropriate specialized method.
 * This is the main string reader that handles automatic format detection.
 *
 * @param buffer The binary buffer containing the string data
 * @return std::string The extracted string value
 */
std::string
ParamUnserializer::read_string(std::span<const byte> buffer) {
    // An empty buffer corresponds to an empty string
    if (buffer.empty()) {
        return "";
    }

    // Automatic format detection - only for reasonably sized buffers to avoid
    // mistaking large data buffers for binary format
    if (buffer.size() >= 4 && buffer.size() <= 1024 * 1024 && (buffer[0] == 0 || buffer[1] == 0 || buffer[2] == 0)) {
        // This is likely a binary format with a 4-byte length prefix
        try {
            return read_binary_string(buffer);
        } catch (const std::exception &) {
            // If binary reading fails, try as text
            return read_text_string(buffer);
        }
    } else {
        // Text format - use data directly
        return read_text_string(buffer);
    }
}

/**
 * @brief Reads a string in text format from a binary buffer
 *
 * Interprets the buffer content directly as a string without any
 * format conversion. This is used for PostgreSQL text format strings.
 *
 * @param buffer The binary buffer containing the text-formatted string
 * @return std::string The extracted string value
 */
std::string
ParamUnserializer::read_text_string(std::span<const byte> buffer) {
    // In TEXT format, we simply take the content as is
    return std::string(reinterpret_cast<const char *>(buffer.data()), buffer.size());
}

/**
 * @brief Reads a string in binary format from a binary buffer
 *
 * Extracts a string from PostgreSQL binary format, which includes
 * a 4-byte length prefix. Handles NULL values (indicated by negative length).
 *
 * @param buffer The binary buffer containing the binary-formatted string
 * @return std::string The extracted string value
 * @throws std::runtime_error If the buffer is too small or the length is invalid
 */
std::string
ParamUnserializer::read_binary_string(std::span<const byte> buffer) {
    // Binary format has a 4-byte length prefix
    if (buffer.size() < 4) {
        throw std::runtime_error("Buffer too small for binary string");
    }

    // Read the length (first 4 bytes)
    integer length = read_integer(buffer.subspan(0, 4));

    // Verify the length is consistent
    if (length < 0) {
        // A negative length indicates NULL
        return "";
    }

    if (static_cast<size_t>(length) + 4 > buffer.size()) {
        throw std::runtime_error("String length exceeds buffer size");
    }

    // Extract the string
    return std::string(reinterpret_cast<const char *>(buffer.data() + 4), length);
}

/**
 * @brief Reads a boolean value from a binary buffer
 *
 * Handles both text format ("true"/"false", "t"/"f", "1"/"0", etc.)
 * and binary format (single byte where non-zero is true).
 *
 * @param buffer The binary buffer containing the boolean value
 * @return bool The extracted boolean value
 * @throws std::runtime_error If the format is invalid for a boolean
 */
bool
ParamUnserializer::read_bool(std::span<const byte> buffer) {
    // Empty buffer check
    if (buffer.empty()) {
        throw std::runtime_error("Empty buffer for boolean value");
    }

    // PostgreSQL binary format for bool (length + value)
    // Buffer format: [length (4 bytes)][value (1 byte)]
    if (buffer.size() >= 5) {
        // Check if this is a binary format with length prefix
        integer length = read_integer(buffer.subspan(0, 4));

        if (length == 1) {
            // This is the formal binary format with length=1
            return buffer[4] != 0;
        }
    }

    // Text format ("true"/"false", "t"/"f", "1"/"0", etc.)
    if (buffer.size() >= 1
        && (buffer[0] == 't' || buffer[0] == 'T' || buffer[0] == 'f' || buffer[0] == 'F' || buffer[0] == '1' || buffer[0] == '0'
            || buffer[0] == 'y' || buffer[0] == 'n')) {
        std::string text = read_text_string(buffer);
        return (text == "true" || text == "t" || text == "1" || text == "y" || text == "yes" || text == "on");
    }

    // Raw binary format (single byte) - last resort
    return buffer[0] != 0;
}

} // namespace qb::pg::detail