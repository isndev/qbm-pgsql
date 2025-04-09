/**
 * @file field_reader.h
 * @brief Direct PostgreSQL field reader
 *
 * A direct reader for converting PostgreSQL fields to C++ types
 * without depending on protocol_io_traits. This provides a low-level
 * way to transform binary PostgreSQL data into usable C++ values.
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

#include <optional>
#include <string>
#include <type_traits>
#include "./param_unserializer.h"
#include "./pg_types.h"

namespace qb::pg {
namespace detail {

/**
 * @brief PostgreSQL field reader
 *
 * Provides functions to read PostgreSQL binary field data
 * and convert them to appropriate C++ types. This class handles
 * binary data parsing, endianness conversion, and NULL field handling.
 */
class FieldReader {
public:
    /**
     * @brief Reads data buffer and converts it to a typed value
     *
     * Handles NULL values and type conversions from PostgreSQL binary format
     * to C++ types. For NULL fields, optional types will be reset while
     * non-optional types will return a failure status.
     *
     * @tparam T Target C++ type to read into
     * @param is_null Indicates if the field is null
     * @param buffer Buffer containing the binary field data
     * @param value Reference to the value to be filled
     * @return true if reading succeeded, false otherwise
     */
    template <typename T>
    static bool
    read_buffer(bool is_null, const std::vector<byte> &buffer, T &value) {
        if (is_null) {
            // For optional types, we can handle NULL easily
            if constexpr (is_optional_v<T>) {
                value.reset();
                return true;
            } else {
                return false; // NULL value for a non-nullable type
            }
        }

        // For a simple type, use ParamUnserializer
        // to read the value from the binary buffer
        return read_value(buffer, value);
    }

private:
    /// Deserializer used for reading data
    static ParamUnserializer unserializer;

    /**
     * @brief Type trait to detect optional types
     *
     * Used to differentiate between regular types and std::optional<T> types
     * for proper NULL handling.
     */
    template <typename T>
    struct is_optional : std::false_type {};

    /**
     * @brief Specialization of is_optional for std::optional types
     */
    template <typename T>
    struct is_optional<std::optional<T>> : std::true_type {};

    /**
     * @brief Helper variable template for checking if a type is optional
     */
    template <typename T>
    static inline constexpr bool is_optional_v = is_optional<T>::value;

    /**
     * @brief Reads a value from a buffer
     *
     * Base template for reading values from binary buffers.
     * Specialized for each supported type.
     *
     * @tparam T Type of the value to read
     * @param buffer Buffer containing the data
     * @param value Value to fill
     * @return true if reading succeeded
     */
    template <typename T>
    static bool
    read_value(const std::vector<byte> &, T &) {
        // Default implementation returns false for unsupported types
        return false;
    }

    /**
     * @brief Reads a smallint (int16) value from a buffer
     *
     * Converts PostgreSQL binary representation of a 16-bit integer
     * with proper endianness handling.
     *
     * @param buffer Buffer containing the binary data
     * @param value Smallint value to fill
     * @return true if reading succeeded
     */
    static bool
    read_value(const std::vector<byte> &buffer, smallint &value) {
        try {
            value = unserializer.read_smallint(buffer);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    /**
     * @brief Reads an integer (int32) value from a buffer
     *
     * Converts PostgreSQL binary representation of a 32-bit integer
     * with proper endianness handling.
     *
     * @param buffer Buffer containing the binary data
     * @param value Integer value to fill
     * @return true if reading succeeded
     */
    static bool
    read_value(const std::vector<byte> &buffer, integer &value) {
        try {
            value = unserializer.read_integer(buffer);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    /**
     * @brief Reads a bigint (int64) value from a buffer
     *
     * Converts PostgreSQL binary representation of a 64-bit integer
     * with proper endianness handling.
     *
     * @param buffer Buffer containing the binary data
     * @param value Bigint value to fill
     * @return true if reading succeeded
     */
    static bool
    read_value(const std::vector<byte> &buffer, bigint &value) {
        try {
            value = unserializer.read_bigint(buffer);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    /**
     * @brief Reads a float (float4) value from a buffer
     *
     * Converts PostgreSQL binary representation of a single-precision
     * floating point value with proper format handling.
     *
     * @param buffer Buffer containing the binary data
     * @param value Float value to fill
     * @return true if reading succeeded
     */
    static bool
    read_value(const std::vector<byte> &buffer, float &value) {
        try {
            value = unserializer.read_float(buffer);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    /**
     * @brief Reads a double (float8) value from a buffer
     *
     * Converts PostgreSQL binary representation of a double-precision
     * floating point value with proper format handling.
     *
     * @param buffer Buffer containing the binary data
     * @param value Double value to fill
     * @return true if reading succeeded
     */
    static bool
    read_value(const std::vector<byte> &buffer, double &value) {
        try {
            value = unserializer.read_double(buffer);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    /**
     * @brief Reads a boolean value from a buffer
     *
     * In PostgreSQL binary format, booleans are represented as a single byte
     * where 0 is false and non-zero is true.
     *
     * @param buffer Buffer containing the binary data
     * @param value Boolean value to fill
     * @return true if reading succeeded
     */
    static bool
    read_value(const std::vector<byte> &buffer, bool &value) {
        try {
            // For a boolean, we just have a single byte with 0 or 1
            if (buffer.size() >= 1) {
                value = (buffer[0] != 0);
                return true;
            }
            return false;
        } catch (const std::exception &) {
            return false;
        }
    }

    /**
     * @brief Reads a string value from a buffer
     *
     * Converts PostgreSQL text representation to std::string.
     * The binary format is a sequence of bytes without null terminator.
     *
     * @param buffer Buffer containing the binary data
     * @param value String value to fill
     * @return true if reading succeeded
     */
    static bool
    read_value(const std::vector<byte> &buffer, std::string &value) {
        try {
            value = unserializer.read_string(buffer);
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }

    /**
     * @brief Reads an optional value from a buffer
     *
     * Handles reading into std::optional<T> types. The contained value
     * is filled if reading succeeds, otherwise the optional is reset.
     *
     * @tparam T Type of the value contained in the optional
     * @param buffer Buffer containing the binary data
     * @param value Optional value to fill
     * @return true if reading succeeded (even if value is reset)
     */
    template <typename T>
    static bool
    read_value(const std::vector<byte> &buffer, std::optional<T> &value) {
        T temp_value;
        if (read_value(buffer, temp_value)) {
            value = std::move(temp_value);
            return true;
        }
        value.reset();
        return false;
    }
};

/**
 * @brief Initializes the field reader
 *
 * Sets up the static deserializer used by the FieldReader.
 * Must be called before using the FieldReader class.
 */
void initialize_field_reader();

} // namespace detail
} // namespace qb::pg
