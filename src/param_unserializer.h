/**
 * @file param_unserializer.h
 * @brief Modern PostgreSQL result deserialization
 *
 * Provides a clean, direct approach to deserialize PostgreSQL binary protocol
 * to C++ types without replacing existing functionality. This class handles
 * the conversion from wire format to native C++ types, supporting both
 * text and binary data formats from PostgreSQL.
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
#include <vector>

#include "./common.h"
#include "./pg_types.h"

namespace qb::pg::detail {

/**
 * @brief Modern parameter deserializer for PostgreSQL binary protocol
 *
 * This class provides functionality to extract and convert values from
 * PostgreSQL's binary wire format into C++ native types. It handles the
 * low-level binary data parsing and conversion, supporting both text and
 * binary protocol formats.
 */
class ParamUnserializer {
public:
    /**
     * @brief Default constructor
     */
    ParamUnserializer() = default;

    /**
     * @brief Default destructor
     */
    ~ParamUnserializer() = default;

    /**
     * @brief Type trait to detect if a type is std::optional
     *
     * Used to provide specialized handling for nullable types.
     *
     * @tparam T The type to check
     */
    template <typename T>
    struct is_optional : std::false_type {};

    /**
     * @brief Specialization of is_optional for std::optional types
     *
     * @tparam T The type wrapped by std::optional
     */
    template <typename T>
    struct is_optional<std::optional<T>> : std::true_type {};

    /**
     * @brief Read a 2-byte integer from a binary buffer
     *
     * Extracts a smallint (int16_t) value from the provided binary buffer,
     * handling endianness conversion if necessary.
     *
     * @param buffer The buffer containing binary data
     * @return smallint The extracted 2-byte integer value
     */
    smallint read_smallint(const std::vector<byte> &buffer);

    /**
     * @brief Read a 4-byte integer from a binary buffer
     *
     * Extracts an integer (int32_t) value from the provided binary buffer,
     * handling endianness conversion if necessary.
     *
     * @param buffer The buffer containing binary data
     * @return integer The extracted 4-byte integer value
     */
    integer read_integer(const std::vector<byte> &buffer);

    /**
     * @brief Read an 8-byte integer from a binary buffer
     *
     * Extracts a bigint (int64_t) value from the provided binary buffer,
     * handling endianness conversion if necessary.
     *
     * @param buffer The buffer containing binary data
     * @return bigint The extracted 8-byte integer value
     */
    bigint read_bigint(const std::vector<byte> &buffer);

    /**
     * @brief Read a single-precision floating point value from a binary buffer
     *
     * Extracts a float value from the provided binary buffer,
     * handling endianness conversion if necessary.
     *
     * @param buffer The buffer containing binary data
     * @return float The extracted single-precision floating point value
     */
    float read_float(const std::vector<byte> &buffer);

    /**
     * @brief Read a double-precision floating point value from a binary buffer
     *
     * Extracts a double value from the provided binary buffer,
     * handling endianness conversion if necessary.
     *
     * @param buffer The buffer containing binary data
     * @return double The extracted double-precision floating point value
     */
    double read_double(const std::vector<byte> &buffer);

    /**
     * @brief Read a string from a binary buffer
     *
     * Extracts a string value from the provided binary buffer.
     * This is a generic method that can handle both text and binary formats.
     *
     * @param buffer The buffer containing string data
     * @return std::string The extracted string value
     */
    std::string read_string(const std::vector<byte> &buffer);

    /**
     * @brief Read a string in text format from a binary buffer
     *
     * Extracts a string value from the provided binary buffer,
     * specifically handling PostgreSQL's text format.
     *
     * @param buffer The buffer containing text-formatted string data
     * @return std::string The extracted string value
     */
    std::string read_text_string(const std::vector<byte> &buffer);

    /**
     * @brief Read a string in binary format from a binary buffer
     *
     * Extracts a string value from the provided binary buffer,
     * specifically handling PostgreSQL's binary format.
     *
     * @param buffer The buffer containing binary-formatted string data
     * @return std::string The extracted string value
     */
    std::string read_binary_string(const std::vector<byte> &buffer);

    /**
     * @brief Read a boolean value from a binary buffer
     *
     * Extracts a boolean value from the provided binary buffer.
     * In PostgreSQL's binary format, booleans are represented as a single byte.
     *
     * @param buffer The buffer containing boolean data
     * @return bool The extracted boolean value
     */
    bool read_bool(const std::vector<byte> &buffer);

    /**
     * @brief Read binary data (bytea) from a binary buffer
     *
     * Extracts a bytea value from the provided binary buffer.
     * This handles PostgreSQL's binary format for bytea data.
     *
     * @param buffer The buffer containing bytea data
     * @return std::vector<byte> The extracted binary data
     */
    std::vector<byte> read_bytea(const std::vector<byte> &buffer);
};

} // namespace qb::pg::detail
