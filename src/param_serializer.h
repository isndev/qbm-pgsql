/**
 * @file param_serializer.h
 * @brief Modern PostgreSQL parameter serialization
 *
 * A complete rewrite of the PostgreSQL parameter serialization system.
 * Provides a clean, direct approach to serialize C++ types to PostgreSQL binary
 * protocol. This library handles conversion of C++ types to their PostgreSQL
 * binary representation with proper endianness handling.
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

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <netinet/in.h> // for htons, htonl
#include <optional>
#include <qb/io.h>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "./pg_types.h"
#include "./type_converter.h"
#include "./type_mapping.h"

namespace qb::pg::detail {

/**
 * @brief Modern parameter serializer for PostgreSQL binary protocol
 *
 * This class provides methods to serialize C++ types into the PostgreSQL binary protocol format.
 * It handles endianness conversion, proper binary formatting, and maintains buffers for the
 * serialized format codes, parameter data, and parameter types.
 */
class ParamSerializer {
public:
    /**
     * @brief Initialize a parameter serializer
     *
     * Constructs a new parameter serializer with empty buffers for format codes, parameters,
     * and parameter types.
     */
    ParamSerializer()
        : format_codes_buffer_{}
        , params_buffer_{}
        , param_types_{} {}

    /**
     * @brief Get the serialized format codes buffer
     *
     * @return const std::vector<byte>& Buffer containing binary format codes for parameters
     */
    const std::vector<byte> &
    format_codes_buffer() const {
        return format_codes_buffer_;
    }

    /**
     * @brief Get the serialized parameters buffer
     *
     * @return const std::vector<byte>& Buffer containing serialized parameter data
     */
    const std::vector<byte> &
    params_buffer() const {
        return params_buffer_;
    }

    /**
     * @brief Get the parameter OID types
     *
     * @return const std::vector<integer>& Vector of PostgreSQL OIDs for each parameter
     */
    const std::vector<integer> &
    param_types() const {
        return param_types_;
    }

    /**
     * @brief Get the number of parameters
     *
     * @return smallint Number of parameters currently added to the serializer
     */
    smallint
    param_count() const {
        return param_types_.size();
    }

    /**
     * @brief Reset the serializer
     *
     * Clears all internal buffers, preparing the serializer for reuse.
     */
    void
    reset() {
        format_codes_buffer_.clear();
        params_buffer_.clear();
        param_types_.clear();
    }

    /**
     * @brief Get the PostgreSQL OID for a type
     *
     * Uses type_mapping to determine the appropriate PostgreSQL OID for a C++ type.
     *
     * @tparam T Type to get OID for
     * @return integer PostgreSQL OID corresponding to the type
     */
    template <typename T>
    static integer
    get_type_oid() {
        return type_mapping<typename std::decay<T>::type>::type_oid;
    }

    /**
     * @brief Add a NULL parameter
     *
     * Adds a NULL parameter to the serializer. PostgreSQL represents NULL
     * values with a specific format in binary mode.
     */
    void
    add_null() {
        // Don't add format code here
        param_types_.push_back(0); // NULL type has no specific OID
        write_null();
    }

    /**
     * @brief Add a boolean parameter
     *
     * Adds a boolean parameter to the serializer with proper PostgreSQL binary encoding.
     * In PostgreSQL binary format, booleans are represented as a single byte.
     *
     * @param value Boolean value to serialize
     */
    void
    add_bool(bool value) {
        // Don't add format code here
        param_types_.push_back(16); // boolean (was oid::boolean)
        write_bool(value);
    }

    /**
     * @brief Add a smallint parameter
     *
     * Adds a smallint (16-bit integer) parameter to the serializer with proper
     * endianness conversion for PostgreSQL binary protocol.
     *
     * @param value Smallint value to serialize
     */
    void
    add_smallint(smallint value) {
        // Don't add format code here
        param_types_.push_back(21); // int2 (was oid::int2)
        write_smallint(value);
    }

    /**
     * @brief Add an integer parameter
     *
     * Adds a 32-bit integer parameter to the serializer with proper
     * endianness conversion for PostgreSQL binary protocol.
     *
     * @param value Integer value to serialize
     */
    void
    add_integer(integer value) {
        // Don't add format code here
        param_types_.push_back(23); // int4 (was oid::int4)
        write_integer(value);
    }

    /**
     * @brief Add a bigint parameter
     *
     * Adds a 64-bit integer parameter to the serializer with proper
     * endianness conversion for PostgreSQL binary protocol.
     *
     * @param value Bigint value to serialize
     */
    void
    add_bigint(bigint value) {
        // Don't add format code here
        param_types_.push_back(20); // int8 (was oid::int8)
        write_bigint(value);
    }

    /**
     * @brief Add a float parameter
     *
     * Adds a single-precision (32-bit) floating point parameter to the serializer
     * with proper endianness conversion for PostgreSQL binary protocol.
     *
     * @param value Float value to serialize
     */
    void
    add_float(float value) {
        // Don't add format code here
        param_types_.push_back(700); // float4 (was oid::float4)
        write_float(value);
    }

    /**
     * @brief Add a double parameter
     *
     * Adds a double-precision (64-bit) floating point parameter to the serializer
     * with proper endianness conversion for PostgreSQL binary protocol.
     *
     * @param value Double value to serialize
     */
    void
    add_double(double value) {
        // Don't add format code here
        param_types_.push_back(701); // float8 (was oid::float8)
        write_double(value);
    }

    /**
     * @brief Add a string parameter
     *
     * Adds a std::string parameter to the serializer in PostgreSQL text format.
     * The string is encoded according to PostgreSQL's text encoding rules.
     *
     * @param value String value to serialize
     */
    void
    add_string(const std::string &value) {
        // Don't add format code here
        param_types_.push_back(25); // text (was oid::text)
        write_string(value);
    }

    /**
     * @brief Add a string_view parameter
     *
     * Adds a std::string_view parameter to the serializer in PostgreSQL text format.
     * This is more efficient than add_string when the string content already exists.
     *
     * @param value String view value to serialize
     */
    void
    add_string_view(std::string_view value) {
        // Don't add format code here
        param_types_.push_back(25); // text (was oid::text)
        write_string_view(value);
    }

    /**
     * @brief Add a C-string parameter
     *
     * Adds a null-terminated C-string parameter to the serializer in PostgreSQL text format.
     * Handles null pointer case by writing a NULL value.
     *
     * @param value C-string value to serialize, can be nullptr
     */
    void
    add_cstring(const char *value) {
        // Don't add format code here
        param_types_.push_back(25); // text (was oid::text)

        if (!value) {
            write_null();
            return;
        }

        write_cstring(value);
    }

    /**
     * @brief Add a byte array parameter
     *
     * Adds a raw byte array parameter to the serializer in PostgreSQL bytea format.
     * This is useful for binary data like images or custom binary formats.
     *
     * @param data Pointer to the byte array data
     * @param size Size of the byte array in bytes
     */
    void
    add_byte_array(const byte *data, size_t size) {
        // Don't add format code here
        param_types_.push_back(17); // bytea (was oid::bytea)
        write_byte_array(data, size);
    }

    /**
     * @brief Add an optional parameter
     *
     * Adds an std::optional parameter, handling the case when it contains a value
     * or is empty (NULL). Uses the provided adder function to add the contained value.
     *
     * @tparam T Type of the optional's value
     * @param value Optional value to serialize
     * @param adder Function pointer to the appropriate add method for type T
     */
    template <typename T>
    void
    add_optional(const std::optional<T> &value, void (ParamSerializer::*adder)(const T &)) {
        if (value.has_value()) {
            (this->*adder)(*value);
        } else {
            add_null();
        }
    }

    /**
     * @brief Add a vector of strings as individual parameters
     *
     * IMPORTANT: Each element of the vector becomes a separate parameter.
     *
     * @param values Vector of strings
     */
    void
    add_string_vector(const std::vector<std::string> &values) {
        // For each string, we add a parameter of text type
        // to get the exact format that PostgreSQL expects for VALUES ($1),($2),...
        for (const auto &value : values) {
            // Add the OID type
            param_types_.push_back(25); // text (was oid::text)

            // Write the parameter length (4 bytes)
            integer len = static_cast<integer>(value.size());
            write_integer(params_buffer_, len);

            // Write the string data without terminator
            if (!value.empty()) {
                params_buffer_.insert(params_buffer_.end(), value.data(),
                                      value.data() + value.size());
            }
        }
    }

    /**
     * @brief Add a parameter based on its type
     *
     * SPECIAL CASE: When a vector<string> is passed via params{vector}, each string
     * in the vector is treated as a separate parameter for batch operations.
     *
     * @tparam T Parameter type
     * @param param Parameter value
     */
    template <typename T>
    void
    add_param(const T &param) {
        using value_type = typename std::decay<T>::type;

        // Cas spécial: vecteur de chaînes comme multiples paramètres
        if constexpr (std::is_same_v<value_type, std::vector<std::string>>) {
            add_string_vector(param);
            return;
        }

        // Cas spécial: valeur nulle (nullptr)
        if constexpr (std::is_same_v<value_type, std::nullptr_t>) {
            add_null();
            return;
        }

        // Cas standard: utiliser TypeConverter
        // 1. Ajouter l'OID du type
        param_types_.push_back(TypeConverter<value_type>::get_oid());

        // 2. Vérifier si c'est une valeur optionnelle nulle
        if constexpr (ParamUnserializer::is_optional<value_type>::value) {
            if (!param.has_value()) {
                write_null();
                return;
            }
        }

        // 3. Sérialiser en binaire
        std::vector<byte> buffer;
        TypeConverter<value_type>::to_binary(param, buffer);

        // 4. Ajouter le résultat au buffer de paramètres
        params_buffer_.insert(params_buffer_.end(), buffer.begin(), buffer.end());
    }

    /**
     * @brief Finalize the format codes buffer
     * This should be called after adding all parameters and before sending to PostgreSQL
     */
    void
    finalize_format_codes() {
        // S'assurer que le buffer a suffisamment d'espace pour le nombre de paramètres
        if (format_codes_buffer_.size() < sizeof(smallint)) {
            format_codes_buffer_.resize(sizeof(smallint));
        }

        // The format codes count should be written at the beginning of the buffer
        write_smallint_at(format_codes_buffer_, 0, param_count());
    }

    /**
     * @brief Finalize the parameters buffer
     * This should be called after adding all parameters and before sending to PostgreSQL
     */
    void
    finalize_params_buffer() {
        // The parameter count should be written at the beginning of the buffer
        write_smallint_at(params_buffer_, 0, param_count());
    }

    /**
     * @brief Serialize parameters for a prepared statement
     *
     * @tparam Args Parameter types
     * @param args Parameter values
     */
    template <typename... Args>
    void
    serialize_params(Args &&...args) {
        reset();

        // Calculate the number of parameters (approximate, may change for vectors)
        constexpr smallint expected_count = sizeof...(Args);

        // Reserve space for parameters (estimate)
        params_buffer_.reserve(sizeof(smallint) + expected_count * 32);

        // Do not write the number of parameters at the beginning of the buffer,
        // as this number may change for string vectors.
        // We will add this information later.

        // Process each parameter
        if constexpr (expected_count > 0) {
            (add_param(std::forward<Args>(args)), ...);
        }

        // Get the actual number of parameters
        smallint actual_count = param_count();

        // Create a new buffer with the correct number of parameters at the beginning
        std::vector<byte> final_buffer;
        final_buffer.reserve(sizeof(smallint) + params_buffer_.size());

        // Write the number of parameters at the beginning
        write_smallint(final_buffer, actual_count);

        // Copy the rest of the buffer
        final_buffer.insert(final_buffer.end(), params_buffer_.begin(), params_buffer_.end());

        // Replace the original buffer
        params_buffer_ = std::move(final_buffer);
    }

    /**
     * @brief Get the parameter types
     * @return Vector of parameter OIDs
     */
    const std::vector<integer> &
    get_param_types() const {
        return param_types_;
    }

    /**
     * @brief Get the serialized parameter data
     * @return Binary buffer containing all parameter data
     */
    const std::vector<byte> &
    get_params_buffer() const {
        return params_buffer_;
    }

private:
    // Traits pattern for type-based serialization

    /**
     * @brief Base template for param serializer traits
     *
     * This template is specialized for each supported type
     */
    template <typename T, typename Enable = void>
    struct param_serializer_traits {
        static void
        add_param(ParamSerializer &serializer, const T &param) {
            // Fallback for unsupported types
            static_assert(!sizeof(T), "Unsupported parameter type");
        }
    };

    // Specialization for nullptr_t
    template <>
    struct param_serializer_traits<std::nullptr_t> {
        static void
        add_param(ParamSerializer &serializer, const std::nullptr_t &) {
            serializer.add_null();
        }
    };

    // Specialization for bool
    template <>
    struct param_serializer_traits<bool> {
        static void
        add_param(ParamSerializer &serializer, const bool &param) {
            serializer.add_bool(param);
        }
    };

    // Specialization for integral types (excluding bool)
    template <typename T>
    struct param_serializer_traits<
        T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, void>> {
        static void
        add_param(ParamSerializer &serializer, const T &param) {
            if constexpr (sizeof(T) <= 2) {
                serializer.add_smallint(param);
            } else if constexpr (sizeof(T) <= 4) {
                serializer.add_integer(param);
            } else {
                serializer.add_bigint(param);
            }
        }
    };

    // Specialization for floating-point types
    template <typename T>
    struct param_serializer_traits<T, std::enable_if_t<std::is_floating_point_v<T>, void>> {
        static void
        add_param(ParamSerializer &serializer, const T &param) {
            if constexpr (sizeof(T) <= 4) {
                serializer.add_float(param);
            } else {
                serializer.add_double(param);
            }
        }
    };

    // Specialization for std::string
    template <>
    struct param_serializer_traits<std::string> {
        static void
        add_param(ParamSerializer &serializer, const std::string &param) {
            serializer.add_string(param);
        }
    };

    // Specialization for std::string_view
    template <>
    struct param_serializer_traits<std::string_view> {
        static void
        add_param(ParamSerializer &serializer, const std::string_view &param) {
            serializer.add_string_view(param);
        }
    };

    // Specialization for const char*
    template <>
    struct param_serializer_traits<const char *> {
        static void
        add_param(ParamSerializer &serializer, const char *param) {
            serializer.add_cstring(param);
        }
    };

    // Specialization for vector<std::string>
    template <>
    struct param_serializer_traits<std::vector<std::string>> {
        static void
        add_param(ParamSerializer &serializer, const std::vector<std::string> &param) {
            serializer.add_string_vector(param);
        }
    };

    // Specialization for vector<char>
    template <>
    struct param_serializer_traits<std::vector<char>> {
        static void
        add_param(ParamSerializer &serializer, const std::vector<char> &param) {
            if (!param.empty()) {
                serializer.add_byte_array(reinterpret_cast<const byte *>(param.data()),
                                          param.size());
            } else {
                serializer.add_null();
            }
        }
    };

    // Specialization for vector<unsigned char>
    template <>
    struct param_serializer_traits<std::vector<unsigned char>> {
        static void
        add_param(ParamSerializer &serializer, const std::vector<unsigned char> &param) {
            if (!param.empty()) {
                serializer.add_byte_array(reinterpret_cast<const byte *>(param.data()),
                                          param.size());
            } else {
                serializer.add_null();
            }
        }
    };

    // Specialization for std::optional
    template <typename T>
    struct param_serializer_traits<std::optional<T>> {
        static void
        add_param(ParamSerializer &serializer, const std::optional<T> &param) {
            if (param.has_value()) {
                serializer.add_param(*param);
            } else {
                serializer.add_null();
            }
        }
    };

private:
    std::vector<byte> format_codes_buffer_;
    std::vector<byte> params_buffer_;
    std::vector<integer> param_types_;

    /**
     * @brief Add a format code to the format codes buffer
     *
     * NOTE: This method is not used anymore. Format codes are handled
     * in ExecuteQuery::get() with a single format code for all parameters.
     *
     * @param format Format code
     */
    void
    add_format_code(protocol_data_format format) {
        // NO-OP - We don't add format codes in the serializer anymore
        // This avoids the extra format codes problem
    }

    /**
     * @brief Write a smallint to a buffer
     *
     * @param buffer Target buffer
     * @param value Smallint value
     */
    static void
    write_smallint(std::vector<byte> &buffer, smallint value) {
        smallint networkValue = htons(value);
        const byte *bytes = reinterpret_cast<const byte *>(&networkValue);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(smallint));
    }

    /**
     * @brief Write a smallint at a specific position in buffer
     *
     * @param buffer Target buffer
     * @param pos Position
     * @param value Smallint value
     */
    static void
    write_smallint_at(std::vector<byte> &buffer, size_t pos, smallint value) {
        smallint networkValue = htons(value);
        const byte *bytes = reinterpret_cast<const byte *>(&networkValue);
        std::copy(bytes, bytes + sizeof(smallint), buffer.begin() + pos);
    }

    /**
     * @brief Write an integer to a buffer
     *
     * @param buffer Target buffer
     * @param value Integer value
     */
    static void
    write_integer(std::vector<byte> &buffer, integer value) {
        integer networkValue = htonl(value);
        const byte *bytes = reinterpret_cast<const byte *>(&networkValue);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(integer));
    }

    /**
     * @brief Write a null parameter
     */
    void
    write_null() {
        // -1 represents NULL in PostgreSQL binary protocol
        write_integer(params_buffer_, -1);
    }

    /**
     * @brief Write a boolean parameter
     *
     * @param value Boolean value
     */
    void
    write_bool(bool value) {
        // Write length (1 byte)
        write_integer(params_buffer_, 1);

        // Write value (PostgreSQL boolean is 1 byte)
        params_buffer_.push_back(value ? 1 : 0);
    }

    /**
     * @brief Write a smallint parameter
     *
     * @param value Smallint value
     */
    void
    write_smallint(smallint value) {
        // Write length (2 bytes)
        write_integer(params_buffer_, 2);

        // Write value (network byte order)
        smallint networkValue = htons(value);
        const byte *bytes = reinterpret_cast<const byte *>(&networkValue);
        params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(smallint));
    }

    /**
     * @brief Write an integer parameter
     *
     * @param value Integer value
     */
    void
    write_integer(integer value) {
        // Write length (4 bytes)
        write_integer(params_buffer_, 4);

        // Write value (network byte order)
        integer networkValue = htonl(value);
        const byte *bytes = reinterpret_cast<const byte *>(&networkValue);
        params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(integer));
    }

    /**
     * @brief Write a bigint parameter
     *
     * @param value Bigint value
     */
    void
    write_bigint(bigint value) {
        // Write length (8 bytes)
        write_integer(params_buffer_, 8);

        // Manual byte swapping for 64-bit integers
        union {
            bigint i;
            byte b[8];
        } src, dst;

        src.i = value;
        dst.b[0] = src.b[7];
        dst.b[1] = src.b[6];
        dst.b[2] = src.b[5];
        dst.b[3] = src.b[4];
        dst.b[4] = src.b[3];
        dst.b[5] = src.b[2];
        dst.b[6] = src.b[1];
        dst.b[7] = src.b[0];

        params_buffer_.insert(params_buffer_.end(), dst.b, dst.b + sizeof(bigint));
    }

    /**
     * @brief Write a float parameter
     *
     * @param value Float value
     */
    void
    write_float(float value) {
        // Write length (4 bytes)
        write_integer(params_buffer_, 4);

        // PostgreSQL expects IEEE 754 format, which is standard for C++
        const byte *bytes = reinterpret_cast<const byte *>(&value);
        params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(float));
    }

    /**
     * @brief Write a double parameter
     *
     * @param value Double value
     */
    void
    write_double(double value) {
        // Write length (8 bytes)
        write_integer(params_buffer_, 8);

        // PostgreSQL expects IEEE 754 format, which is standard for C++
        const byte *bytes = reinterpret_cast<const byte *>(&value);
        params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(double));
    }

    /**
     * @brief Write a string parameter
     *
     * @param value String value
     */
    void
    write_string(const std::string &value) {
        // PostgreSQL binary format: length (int32) + raw bytes (NO null terminators)

        // 1. Write the 4-byte length
        integer len = static_cast<integer>(value.size());
        write_integer(params_buffer_, len);

        // 2. Write the raw data WITHOUT null terminator
        if (!value.empty()) {
            // Use data() + size() to avoid any potential null terminators
            params_buffer_.insert(params_buffer_.end(), value.data(), value.data() + value.size());
        }
    }

    /**
     * @brief Write a string_view parameter
     *
     * @param value String view value
     */
    void
    write_string_view(std::string_view value) {
        // PostgreSQL binary format: length (int32) + raw bytes (NO null terminators)

        // 1. Write the 4-byte length
        integer len = static_cast<integer>(value.size());
        write_integer(params_buffer_, len);

        // 2. Write the raw data WITHOUT null terminator
        if (!value.empty()) {
            // String views have no null terminators by design
            params_buffer_.insert(params_buffer_.end(), value.data(), value.data() + value.size());
        }
    }

    /**
     * @brief Write a C-string parameter
     *
     * @param value C-string value
     */
    void
    write_cstring(const char *value) {
        // Get length WITHOUT null terminator
        size_t len = strlen(value);

        // PostgreSQL binary format: length (int32) + raw bytes (NO null terminators)

        // 1. Write the 4-byte length
        write_integer(params_buffer_, static_cast<integer>(len));

        // 2. Write the raw data WITHOUT null terminator
        if (len > 0) {
            // Copy exactly len bytes (excluding the null terminator)
            params_buffer_.insert(params_buffer_.end(), value, value + len);
        }
    }

    /**
     * @brief Write a byte array parameter
     *
     * @param data Byte array
     * @param size Size of byte array
     */
    void
    write_byte_array(const byte *data, size_t size) {
        // Write length
        write_integer(params_buffer_, static_cast<integer>(size));

        // Write byte array data
        params_buffer_.insert(params_buffer_.end(), data, data + size);
    }
};

/**
 * @brief Helper function to serialize parameters and get the resulting buffers
 *
 * @tparam Args Parameter types
 * @param params_buffer Output buffer for serialized parameters
 * @param format_codes_buffer Output buffer for format codes (OBSOLÈTE, non utilisé)
 * @param param_types Output vector for parameter OID types
 * @param args Parameter values
 */
template <typename... Args>
void
serialize_params(std::vector<byte> &params_buffer, std::vector<byte> &format_codes_buffer,
                 std::vector<integer> &param_types, Args &&...args) {
    // Créer un nouveau sérialiseur
    ParamSerializer serializer;

    // Sérialiser les paramètres
    serializer.serialize_params(std::forward<Args>(args)...);

    // Récupérer les données sérialisées
    params_buffer = serializer.params_buffer();
    param_types = serializer.param_types();

    // Le buffer de codes de format n'est plus utilisé
    format_codes_buffer.clear();
}

} // namespace qb::pg::detail
