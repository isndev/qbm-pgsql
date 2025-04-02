/**
 * @file param_serializer.h
 * @brief Modern PostgreSQL parameter serialization
 * 
 * A complete rewrite of the PostgreSQL parameter serialization system.
 * Provides a clean, direct approach to serialize C++ types to PostgreSQL binary protocol.
 */

#pragma once
#ifndef QBM_PGSQL_DETAIL_PARAM_SERIALIZER_H
#define QBM_PGSQL_DETAIL_PARAM_SERIALIZER_H

#include <vector>
#include <string>
#include <string_view>
#include <optional>
#include <cstring>
#include <type_traits>
#include <variant>
#include <array>
#include <netinet/in.h> // for htons, htonl
#include <algorithm>
#include <sstream>
#include <iostream>
#include <qb/io.h>
#include <iomanip>
#include "../not-qb/pg_types.h"
#include "type_mapping.h"

namespace qb::pg::detail {

// Data format constants
enum class DataFormat : smallint {
    Text = 0,
    Binary = 1
};

/**
 * @brief Modern parameter serializer for PostgreSQL binary protocol
 */
class ParamSerializer {
public:
    /**
     * @brief Initialize a parameter serializer
     */
    ParamSerializer() : format_codes_buffer_{}, params_buffer_{}, param_types_{} {}

    /**
     * @brief Get the serialized format codes buffer
     */
    const std::vector<byte>& format_codes_buffer() const { return format_codes_buffer_; }

    /**
     * @brief Get the serialized parameters buffer
     */
    const std::vector<byte>& params_buffer() const { return params_buffer_; }

    /**
     * @brief Get the parameter OID types
     */
    const std::vector<integer>& param_types() const { return param_types_; }

    /**
     * @brief Get the number of parameters
     */
    smallint param_count() const {
        return param_types_.size();
    }

    /**
     * @brief Reset the serializer
     */
    void reset() {
        format_codes_buffer_.clear();
        params_buffer_.clear();
        param_types_.clear();
    }

    /**
     * @brief Get the PostgreSQL OID for a type
     * 
     * @tparam T Type to get OID for
     * @return integer PostgreSQL OID
     */
    template <typename T>
    static integer get_type_oid() {
        return type_mapping<typename std::decay<T>::type>::type_oid;
    }

    /**
     * @brief Add a NULL parameter
     */
    void add_null() {
        // Don't add format code here
        param_types_.push_back(0); // NULL type has no specific OID
        write_null();
    }

    /**
     * @brief Add a boolean parameter
     * 
     * @param value Boolean value
     */
    void add_bool(bool value) {
        // Don't add format code here
        param_types_.push_back(oid::boolean);
        write_bool(value);
    }

    /**
     * @brief Add a smallint parameter
     * 
     * @param value Smallint value
     */
    void add_smallint(smallint value) {
        // Don't add format code here
        param_types_.push_back(oid::int2);
        write_smallint(value);
    }

    /**
     * @brief Add an integer parameter
     * 
     * @param value Integer value
     */
    void add_integer(integer value) {
        // Don't add format code here
        param_types_.push_back(oid::int4);
        write_integer(value);
    }

    /**
     * @brief Add a bigint parameter
     * 
     * @param value Bigint value
     */
    void add_bigint(bigint value) {
        // Don't add format code here
        param_types_.push_back(oid::int8);
        write_bigint(value);
    }

    /**
     * @brief Add a float parameter
     * 
     * @param value Float value
     */
    void add_float(float value) {
        // Don't add format code here
        param_types_.push_back(oid::float4);
        write_float(value);
    }

    /**
     * @brief Add a double parameter
     * 
     * @param value Double value
     */
    void add_double(double value) {
        // Don't add format code here
        param_types_.push_back(oid::float8);
        write_double(value);
    }

    /**
     * @brief Add a string parameter
     * 
     * @param value String value
     */
    void add_string(const std::string& value) {
        // Don't add format code here
        param_types_.push_back(oid::text);
        write_string(value);
    }

    /**
     * @brief Add a string_view parameter
     * 
     * @param value String view value
     */
    void add_string_view(std::string_view value) {
        // Don't add format code here
        param_types_.push_back(oid::text);
        write_string_view(value);
    }

    /**
     * @brief Add a C-string parameter
     * 
     * @param value C-string value
     */
    void add_cstring(const char* value) {
        // Don't add format code here
        param_types_.push_back(oid::text);
        
        if (!value) {
            write_null();
            return;
        }
        
        write_cstring(value);
    }

    /**
     * @brief Add a byte array parameter
     * 
     * @param data Byte array
     * @param size Size of byte array
     */
    void add_byte_array(const byte* data, size_t size) {
        // Don't add format code here
        param_types_.push_back(oid::bytea);
        write_byte_array(data, size);
    }

    /**
     * @brief Add an optional parameter
     * 
     * @tparam T Type of the optional's value
     * @param value Optional value
     * @param adder Function to add the contained type
     */
    template<typename T>
    void add_optional(const std::optional<T>& value, void (ParamSerializer::*adder)(const T&)) {
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
    void add_string_vector(const std::vector<std::string>& values) {
        // For each string, we add a parameter of text type
        // to get the exact format that PostgreSQL expects for VALUES ($1),($2),...
        for (const auto& value : values) {
            // Add the OID type
            param_types_.push_back(oid::text);
            
            // Write the parameter length (4 bytes)
            integer len = static_cast<integer>(value.size());
            write_integer(params_buffer_, len);
            
            // Write the string data without terminator
            if (!value.empty()) {
                params_buffer_.insert(params_buffer_.end(),
                                     value.data(),
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
    template<typename T>
    void add_param(const T& param) {
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
            add_null();
        }
        else if constexpr (std::is_same_v<T, bool>) {
            add_bool(param);
        }
        else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
            // Integer types (excluding bool)
            if constexpr (sizeof(T) <= 2) {
                add_smallint(param);
            }
            else if constexpr (sizeof(T) <= 4) {
                add_integer(param);
            }
            else {
                add_bigint(param);
            }
        }
        else if constexpr (std::is_floating_point_v<T>) {
            // Floating point types
            if constexpr (sizeof(T) <= 4) {
                add_float(param);
            }
            else {
                add_double(param);
            }
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            add_string(param);
        }
        else if constexpr (std::is_same_v<T, std::string_view>) {
            add_string_view(param);
        }
        else if constexpr (std::is_same_v<T, const char*>) {
            add_cstring(param);
        }
        else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            // IMPORTANT: Vector of strings = one parameter PER element of the vector
            // For a query like "INSERT INTO ... VALUES ($1),($2),($3),($4)"
            add_string_vector(param);
        }
        else if constexpr (std::is_same_v<T, std::vector<char>>) {
            // Vector of char treated as byte array
            if (!param.empty()) {
                add_byte_array(reinterpret_cast<const byte*>(param.data()), param.size());
            } else {
                add_null();
            }
        }
        else if constexpr (std::is_same_v<T, std::vector<unsigned char>> || std::is_same_v<T, std::vector<byte>>) {
            // Vector of unsigned bytes treated directly as byte array
            if (!param.empty()) {
                add_byte_array(reinterpret_cast<const byte*>(param.data()), param.size());
            } else {
                add_null();
            }
        }
        else if constexpr (is_optional<T>::value) {
            using ValueType = typename T::value_type;
            if (param.has_value()) {
                add_param(*param);
            } else {
                add_null();
            }
        }
        else {
            // Fallback for unsupported types
            static_assert(!sizeof(T), "Unsupported parameter type");
        }
    }

    /**
     * @brief Finalize the format codes buffer
     * This should be called after adding all parameters and before sending to PostgreSQL
     */
    void finalize_format_codes() {
        // The format codes count should be written at the beginning of the buffer
        write_smallint_at(format_codes_buffer_, 0, param_count());
    }

    /**
     * @brief Finalize the parameters buffer
     * This should be called after adding all parameters and before sending to PostgreSQL
     */
    void finalize_params_buffer() {
        // The parameter count should be written at the beginning of the buffer
        write_smallint_at(params_buffer_, 0, param_count());
    }

    /**
     * @brief Serialize parameters for a prepared statement
     * 
     * @tparam Args Parameter types
     * @param args Parameter values
     */
    template<typename... Args>
    void serialize_params(Args&&... args) {
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

private:
    std::vector<byte> format_codes_buffer_;
    std::vector<byte> params_buffer_;
    std::vector<integer> param_types_;

    /**
     * @brief Helper for checking if a type is std::optional
     */
    template<typename T>
    struct is_optional : std::false_type {};

    template<typename T>
    struct is_optional<std::optional<T>> : std::true_type {};

    /**
     * @brief Add a format code to the format codes buffer
     * 
     * NOTE: This method is not used anymore. Format codes are handled
     * in ExecuteQuery::get() with a single format code for all parameters.
     * 
     * @param format Format code
     */
    void add_format_code(DataFormat format) {
        // NO-OP - We don't add format codes in the serializer anymore
        // This avoids the extra format codes problem
    }

    /**
     * @brief Write a smallint to a buffer
     * 
     * @param buffer Target buffer
     * @param value Smallint value
     */
    static void write_smallint(std::vector<byte>& buffer, smallint value) {
        smallint networkValue = htons(value);
        const byte* bytes = reinterpret_cast<const byte*>(&networkValue);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(smallint));
    }

    /**
     * @brief Write a smallint at a specific position in buffer
     * 
     * @param buffer Target buffer
     * @param pos Position
     * @param value Smallint value
     */
    static void write_smallint_at(std::vector<byte>& buffer, size_t pos, smallint value) {
        smallint networkValue = htons(value);
        const byte* bytes = reinterpret_cast<const byte*>(&networkValue);
        std::copy(bytes, bytes + sizeof(smallint), buffer.begin() + pos);
    }

    /**
     * @brief Write an integer to a buffer
     * 
     * @param buffer Target buffer
     * @param value Integer value
     */
    static void write_integer(std::vector<byte>& buffer, integer value) {
        integer networkValue = htonl(value);
        const byte* bytes = reinterpret_cast<const byte*>(&networkValue);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(integer));
    }

    /**
     * @brief Write a null parameter
     */
    void write_null() {
        // -1 represents NULL in PostgreSQL binary protocol
        write_integer(params_buffer_, -1);
    }

    /**
     * @brief Write a boolean parameter
     * 
     * @param value Boolean value
     */
    void write_bool(bool value) {
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
    void write_smallint(smallint value) {
        // Write length (2 bytes)
        write_integer(params_buffer_, 2);
        
        // Write value (network byte order)
        smallint networkValue = htons(value);
        const byte* bytes = reinterpret_cast<const byte*>(&networkValue);
        params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(smallint));
    }

    /**
     * @brief Write an integer parameter
     * 
     * @param value Integer value
     */
    void write_integer(integer value) {
        // Write length (4 bytes)
        write_integer(params_buffer_, 4);
        
        // Write value (network byte order)
        integer networkValue = htonl(value);
        const byte* bytes = reinterpret_cast<const byte*>(&networkValue);
        params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(integer));
    }

    /**
     * @brief Write a bigint parameter
     * 
     * @param value Bigint value
     */
    void write_bigint(bigint value) {
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
    void write_float(float value) {
        // Write length (4 bytes)
        write_integer(params_buffer_, 4);
        
        // PostgreSQL expects IEEE 754 format, which is standard for C++
        const byte* bytes = reinterpret_cast<const byte*>(&value);
        params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(float));
    }

    /**
     * @brief Write a double parameter
     * 
     * @param value Double value
     */
    void write_double(double value) {
        // Write length (8 bytes)
        write_integer(params_buffer_, 8);
        
        // PostgreSQL expects IEEE 754 format, which is standard for C++
        const byte* bytes = reinterpret_cast<const byte*>(&value);
        params_buffer_.insert(params_buffer_.end(), bytes, bytes + sizeof(double));
    }

    /**
     * @brief Write a string parameter
     * 
     * @param value String value
     */
    void write_string(const std::string& value) {
        // PostgreSQL binary format: length (int32) + raw bytes (NO null terminators)
        
        // 1. Write the 4-byte length
        integer len = static_cast<integer>(value.size());
        write_integer(params_buffer_, len);
        
        // 2. Write the raw data WITHOUT null terminator
        if (!value.empty()) {
            // Use data() + size() to avoid any potential null terminators
            params_buffer_.insert(params_buffer_.end(), 
                                 value.data(), 
                                 value.data() + value.size());
        }
    }

    /**
     * @brief Write a string_view parameter
     * 
     * @param value String view value
     */
    void write_string_view(std::string_view value) {
        // PostgreSQL binary format: length (int32) + raw bytes (NO null terminators)
        
        // 1. Write the 4-byte length
        integer len = static_cast<integer>(value.size());
        write_integer(params_buffer_, len);
        
        // 2. Write the raw data WITHOUT null terminator
        if (!value.empty()) {
            // String views have no null terminators by design
            params_buffer_.insert(params_buffer_.end(), 
                                 value.data(), 
                                 value.data() + value.size());
        }
    }

    /**
     * @brief Write a C-string parameter
     * 
     * @param value C-string value
     */
    void write_cstring(const char* value) {
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
    void write_byte_array(const byte* data, size_t size) {
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
template<typename... Args>
void serialize_params(
    std::vector<byte>& params_buffer,
    std::vector<byte>& format_codes_buffer,
    std::vector<integer>& param_types,
    Args&&... args
) {
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

#endif // QBM_PGSQL_DETAIL_PARAM_SERIALIZER_H 