/**
 * @file param_unserializer.cpp
 * @brief Implementation of binary data reading functions
 *
 * This file contains the implementation of the ParamUnserializer class methods
 * that handle the conversion from PostgreSQL binary wire format to C++ native types.
 * It includes endianness conversion, buffer validation, and format detection logic.
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

#include <cstring>
#include <iostream>
#include <netinet/in.h> // For ntohl, ntohs
#include <stdexcept>

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
ParamUnserializer::read_smallint(const std::vector<byte> &buffer) {
    // Verify minimum buffer size for a smallint
    if (buffer.size() < sizeof(smallint)) {
        throw std::runtime_error("Buffer too small for smallint");
    }

    // Convert from network byte order (big-endian)
    union {
        smallint value;
        byte bytes[sizeof(smallint)];
    } data;

    std::memcpy(data.bytes, buffer.data(), sizeof(smallint));
    return ntohs(data.value);
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
ParamUnserializer::read_integer(const std::vector<byte> &buffer) {
    // Verify minimum buffer size for an integer
    if (buffer.size() < sizeof(integer)) {
        throw std::runtime_error("Buffer too small for integer");
    }

    // Convert from network byte order (big-endian)
    union {
        integer value;
        byte bytes[sizeof(integer)];
    } data;

    std::memcpy(data.bytes, buffer.data(), sizeof(integer));
    return ntohl(data.value);
}

/**
 * @brief Reads an 8-byte integer from a binary buffer
 *
 * Performs manual byte swapping for 64-bit integers as standard
 * network functions don't handle 64-bit values. Converts from big-endian
 * to host byte order.
 *
 * @param buffer The binary buffer containing the bigint value
 * @return bigint The converted 8-byte integer in host byte order
 * @throws std::runtime_error If the buffer is too small to contain a bigint
 */
bigint
ParamUnserializer::read_bigint(const std::vector<byte> &buffer) {
    // Verify minimum buffer size for a bigint
    if (buffer.size() < sizeof(bigint)) {
        throw std::runtime_error("Buffer too small for bigint");
    }

    // Manual byte swapping for 64-bit integers (network byte order)
    union {
        bigint i;
        byte b[8];
    } src, dst;

    std::memcpy(src.b, buffer.data(), sizeof(bigint));

    // Convert from big-endian to host byte order
    dst.b[0] = src.b[7];
    dst.b[1] = src.b[6];
    dst.b[2] = src.b[5];
    dst.b[3] = src.b[4];
    dst.b[4] = src.b[3];
    dst.b[5] = src.b[2];
    dst.b[6] = src.b[1];
    dst.b[7] = src.b[0];

    return dst.i;
}

/**
 * @brief Reads a single-precision floating point value from a binary buffer
 *
 * Performs byte swapping for 32-bit floats to convert from network byte order
 * (big-endian) to host byte order.
 *
 * @param buffer The binary buffer containing the float value
 * @return float The converted single-precision floating point value
 * @throws std::runtime_error If the buffer is too small to contain a float
 */
float
ParamUnserializer::read_float(const std::vector<byte> &buffer) {
    // Verify minimum buffer size for a float
    if (buffer.size() < sizeof(float)) {
        throw std::runtime_error("Buffer too small for float");
    }

    // For floats, once we read the integer with correct endianness,
    // we can interpret it directly
    union {
        uint32_t i;
        float f;
        byte b[4];
    } src, dst;

    std::memcpy(src.b, buffer.data(), sizeof(float));

    // Convert from big-endian to host byte order
    dst.b[0] = src.b[3];
    dst.b[1] = src.b[2];
    dst.b[2] = src.b[1];
    dst.b[3] = src.b[0];

    return dst.f;
}

/**
 * @brief Reads a double-precision floating point value from a binary buffer
 *
 * Performs manual byte swapping for 64-bit doubles to convert from network byte order
 * (big-endian) to host byte order.
 *
 * @param buffer The binary buffer containing the double value
 * @return double The converted double-precision floating point value
 * @throws std::runtime_error If the buffer is too small to contain a double
 */
double
ParamUnserializer::read_double(const std::vector<byte> &buffer) {
    // Verify minimum buffer size for a double
    if (buffer.size() < sizeof(double)) {
        throw std::runtime_error("Buffer too small for double");
    }

    // Manual byte swap for 64-bit doubles
    union {
        uint64_t i;
        double d;
        byte b[8];
    } src, dst;

    std::memcpy(src.b, buffer.data(), sizeof(src.b));

    dst.b[0] = src.b[7];
    dst.b[1] = src.b[6];
    dst.b[2] = src.b[5];
    dst.b[3] = src.b[4];
    dst.b[4] = src.b[3];
    dst.b[5] = src.b[2];
    dst.b[6] = src.b[1];
    dst.b[7] = src.b[0];

    return dst.d;
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
ParamUnserializer::read_string(const std::vector<byte> &buffer) {
    // An empty buffer corresponds to an empty string
    if (buffer.empty()) {
        return "";
    }

    // Automatic format detection - only for reasonably sized buffers to avoid
    // mistaking large data buffers for binary format
    if (buffer.size() >= 4 && buffer.size() <= 1024 * 1024 &&
        (buffer[0] == 0 || buffer[1] == 0 || buffer[2] == 0)) {
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
ParamUnserializer::read_text_string(const std::vector<byte> &buffer) {
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
ParamUnserializer::read_binary_string(const std::vector<byte> &buffer) {
    // Binary format has a 4-byte length prefix
    if (buffer.size() < 4) {
        throw std::runtime_error("Buffer too small for binary string");
    }

    // Read the length (first 4 bytes)
    integer length = read_integer(std::vector<byte>(buffer.begin(), buffer.begin() + 4));

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
ParamUnserializer::read_bool(const std::vector<byte> &buffer) {
    // Text format ("true"/"false")
    if (buffer.size() >= 4 && (buffer[0] == 't' || buffer[0] == 'T' || buffer[0] == 'f' ||
                               buffer[0] == 'F' || buffer[0] == '1' || buffer[0] == '0')) {

        std::string text = read_text_string(buffer);
        return (text == "true" || text == "t" || text == "1" || text == "y" || text == "yes");
    }

    // Binary format (1 byte)
    if (buffer.size() >= 1) {
        return (buffer[0] != 0);
    }

    throw std::runtime_error("Invalid boolean format");
}

/**
 * @brief Reads binary data (bytea) from a binary buffer
 *
 * Handles both binary format (with 4-byte length prefix) and
 * text format (hex string representation). In text format,
 * supports the PostgreSQL '\x' prefix for hex strings.
 *
 * @param buffer The binary buffer containing the bytea data
 * @return std::vector<byte> The extracted binary data
 * @throws std::runtime_error If the buffer is too small or the format is invalid
 */
std::vector<byte>
ParamUnserializer::read_bytea(const std::vector<byte> &buffer) {
    // Automatic format detection
    if (buffer.size() >= 4 && (buffer[0] == 0 || buffer[1] == 0)) {
        // Binary format with length prefix
        if (buffer.size() < 4) {
            throw std::runtime_error("Buffer too small for binary bytea");
        }

        // Read the length
        integer length = read_integer(std::vector<byte>(buffer.begin(), buffer.begin() + 4));

        // Verify the length
        if (length < 0) {
            // NULL
            return {};
        }

        if (static_cast<size_t>(length) + 4 > buffer.size()) {
            throw std::runtime_error("Bytea length exceeds buffer size");
        }

        // Extract the data
        return std::vector<byte>(buffer.begin() + 4, buffer.begin() + 4 + length);
    } else {
        // Text format (hex representation)
        std::string hex_string = read_text_string(buffer);
        std::vector<byte> result;

        // Skip the "\x" prefix if present
        size_t start_pos = 0;
        if (hex_string.size() >= 2 && hex_string.substr(0, 2) == "\\x") {
            start_pos = 2;
        }

        // Convert each pair of hex bytes
        for (size_t i = start_pos; i < hex_string.size(); i += 2) {
            if (i + 1 >= hex_string.size()) {
                break;
            }

            byte value = 0;
            for (int j = 0; j < 2; ++j) {
                char c = hex_string[i + j];
                byte nibble;

                if (c >= '0' && c <= '9') {
                    nibble = c - '0';
                } else if (c >= 'a' && c <= 'f') {
                    nibble = c - 'a' + 10;
                } else if (c >= 'A' && c <= 'F') {
                    nibble = c - 'A' + 10;
                } else {
                    throw std::runtime_error("Invalid hex character in bytea");
                }

                value = (value << 4) | nibble;
            }

            result.push_back(value);
        }

        return result;
    }
}

} // namespace qb::pg::detail