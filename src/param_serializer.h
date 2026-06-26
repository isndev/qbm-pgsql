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
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>
#include <qb/io.h>
#include <qb/system/endian.h>

#include "./pg_types.h"
#include "./type_converter.h"
#include "./type_mapping.h"

namespace qb::pg::detail {

/**
 * @brief Modern parameter serializer for PostgreSQL binary protocol
 *
 * This class provides methods to serialize C++ types into the PostgreSQL binary protocol
 * format. It handles endianness conversion, proper binary formatting, and maintains
 * buffers for the serialized format codes, parameter data, and parameter types.
 */
class ParamSerializer {
public:
    /**
     * @brief Initialize a parameter serializer
     *
     * Constructs a new parameter serializer with empty buffers for format codes,
     * parameters, and parameter types.
     */
    ParamSerializer()
        : params_buffer_{}
        , param_types_{} {}

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
        return static_cast<smallint>(param_types_.size());
    }

    /// PostgreSQL's Bind/Describe parameter count is a 16-bit wire field; more
    /// than this many parameters cannot be represented and must be rejected
    /// rather than silently wrapped into a corrupt (negative) count.
    static constexpr size_t MAX_PARAMS = 32767;

    /**
     * @brief Throw if the accumulated parameter count exceeds the wire limit.
     *
     * Called before the count is written into the Bind message so an
     * over-large parameter list fails loudly instead of desynchronizing the
     * protocol stream with a wrapped int16 count.
     */
    void
    ensure_param_count_fits() const {
        if (param_types_.size() > MAX_PARAMS) {
            throw std::length_error("pgsql: too many bind parameters (" + std::to_string(param_types_.size()) + " > "
                                    + std::to_string(MAX_PARAMS) + ")");
        }
    }

    /**
     * @brief Reset the serializer
     *
     * Clears all internal buffers, preparing the serializer for reuse.
     */
    void
    reset() {
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
     * Adds a null-terminated C-string parameter to the serializer in PostgreSQL text
     * format. Handles null pointer case by writing a NULL value.
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
    void add_string_vector(const std::vector<std::string> &values);

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

        // Special case: Generic vector handling
        if constexpr (is_std_vector<value_type>::value) {
            // Special case for vector<string> keeping its original behavior
            if constexpr (std::is_same_v<value_type, std::vector<std::string>>) {
                add_string_vector(param);
                return;
            }
            // Special cases for byte arrays (bytea).
            else if constexpr (std::is_same_v<value_type, std::vector<char>> || std::is_same_v<value_type, std::vector<unsigned char>>
                               || std::is_same_v<value_type, std::vector<std::byte>>) {
                // An EMPTY byte vector is a valid zero-length bytea (''::bytea, length 0), NOT
                // a SQL NULL. write_byte_array() encodes a 0-length value correctly (length 0,
                // no payload); routing empty to add_null() (a -1 length sentinel) was a bug that
                // made `length($1::bytea)` come back NULL for an empty input. param.data() may be
                // nullptr when size()==0, but add_byte_array copies a zero-length range, so the
                // pointer is never dereferenced.
                add_byte_array(reinterpret_cast<const byte *>(param.data()), param.size());
                return;
            }
            // General case: handle as a PostgreSQL array
            else {
                add_vector(param);
                return;
            }
        }

        // Special case: null value (nullptr)
        else if constexpr (std::is_same_v<value_type, std::nullptr_t>) {
            add_null();
        }

        // Standard scalar case: serialize via TypeConverter. Guarded as the final
        // `else` so it is NOT instantiated for vector/nullptr types — otherwise
        // TypeConverter<std::vector<T>>::to_binary would be ODR-used here (and now
        // hard-error via the loud static_assert) even though the vector branch above
        // handles arrays through add_vector / add_byte_array.
        else {
            // 1. Add the OID type
            param_types_.push_back(TypeConverter<value_type>::get_oid());

            // 2. Optional NULL value -> -1 length sentinel, no payload
            if constexpr (ParamUnserializer::is_optional<value_type>::value) {
                if (!param.has_value()) {
                    write_null();
                    return;
                }
            }

            // 3. Serialize to binary and append to the parameter buffer
            std::vector<byte> buffer;
            TypeConverter<value_type>::to_binary(param, buffer);
            params_buffer_.insert(params_buffer_.end(), buffer.begin(), buffer.end());
        }
    }

    /**
     * @brief Finalize the parameters buffer
     *
     * Writes the actual parameter count at the beginning of the buffer.
     * Must be called after adding all parameters and before sending to PostgreSQL.
     */
    void
    finalize_params_buffer() {
        ensure_param_count_fits();
        write_smallint_at(params_buffer_, 0, param_count());
    }

    /**
     * @brief Serialize parameters for a prepared statement
     *
     * Builds the params buffer in a single pass: reserves space for the 2-byte
     * count prefix at the beginning, appends each parameter's binary encoding,
     * then fills in the actual count in-place — no secondary copy needed.
     *
     * @tparam Args Parameter types
     * @param args  Parameter values (forwarded)
     */
    template <typename... Args>
    void
    serialize_params(Args &&...args) {
        reset();

        constexpr smallint expected_count = sizeof...(Args);
        params_buffer_.reserve(sizeof(smallint) + expected_count * 32);

        // Reserve space for the count prefix; it will be filled in below.
        params_buffer_.resize(sizeof(smallint));

        if constexpr (expected_count > 0) {
            (add_param(std::forward<Args>(args)), ...);
        }

        // Write the actual parameter count (may differ from expected when
        // vector<string> expands into multiple parameters).
        ensure_param_count_fits();
        smallint actual_count_be = htons(param_count());
        std::memcpy(params_buffer_.data(), &actual_count_be, sizeof(smallint));
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
        add_param(ParamSerializer &, const T &) {
            static_assert(!sizeof(T), "Unsupported parameter type");
        }
    };

private:
    std::vector<byte>    params_buffer_;
    std::vector<integer> param_types_;

    /**
     * @brief Write a smallint to a buffer
     *
     * @param buffer Target buffer
     * @param value Smallint value
     */
    static void write_smallint(std::vector<byte> &buffer, smallint value);

    /**
     * @brief Write a smallint at a specific position in buffer
     *
     * @param buffer Target buffer
     * @param pos Position
     * @param value Smallint value
     */
    static void write_smallint_at(std::vector<byte> &buffer, size_t pos, smallint value);

    /**
     * @brief Write an integer to a buffer
     *
     * @param buffer Target buffer
     * @param value Integer value
     */
    static void write_integer(std::vector<byte> &buffer, integer value);

    /**
     * @brief Write a null parameter
     */
    void write_null();

    /**
     * @brief Write a boolean parameter
     *
     * @param value Boolean value
     */
    void write_bool(bool value);

    /**
     * @brief Write a smallint parameter
     *
     * @param value Smallint value
     */
    void write_smallint(smallint value);

    /**
     * @brief Write an integer parameter
     *
     * @param value Integer value
     */
    void write_integer(integer value);

    /**
     * @brief Write a bigint parameter
     *
     * @param value Bigint value
     */
    void write_bigint(bigint value);

    /**
     * @brief Write a float parameter in big-endian IEEE 754 format
     *
     * PostgreSQL requires all numeric values in network byte order (big-endian).
     * We reinterpret the IEEE 754 bit pattern as uint32_t and byte-swap it.
     *
     * @param value Float value
     */
    void write_float(float value);

    /**
     * @brief Write a double parameter in big-endian IEEE 754 format
     *
     * PostgreSQL requires all numeric values in network byte order (big-endian).
     * We reinterpret the IEEE 754 bit pattern as uint64_t and byte-swap it.
     *
     * @param value Double value
     */
    void write_double(double value);

    /**
     * @brief Write a string parameter
     *
     * @param value String value
     */
    void write_string(const std::string &value);

    /**
     * @brief Write a string_view parameter
     *
     * @param value String view value
     */
    void write_string_view(std::string_view value);

    /**
     * @brief Write a C-string parameter
     *
     * @param value C-string value
     */
    void write_cstring(const char *value);

    /**
     * @brief Write a byte array parameter
     *
     * @param data Byte array
     * @param size Size of byte array
     */
    void write_byte_array(const byte *data, size_t size);

    // Add type trait to detect std::vector
    template <typename T>
    struct is_std_vector : std::false_type {};

    template <typename T, typename Alloc>
    struct is_std_vector<std::vector<T, Alloc>> : std::true_type {};

    /**
     * @brief Add a generic vector as a PostgreSQL array
     *
     * This method serializes any std::vector into a PostgreSQL array format
     *
     * @tparam VecType The vector type to serialize
     * @param vector The vector to serialize
     */
    template <typename VecType>
    void
    add_vector(const VecType &vector) {
        using element_type = typename VecType::value_type;

        // Get array element OID type from the element type
        integer element_oid = TypeConverter<element_type>::get_oid();

        // Determine the array OID based on element type OID
        // This is a simplification; PostgreSQL array OIDs typically follow a pattern
        // but a proper implementation would use a mapping from element OID to array OID
        integer array_oid = 0;

        // Array type determination - common array OIDs
        switch (element_oid) {
            case 16:
                array_oid = 1000;
                break; // boolean array
            case 21:
                array_oid = 1005;
                break; // int2 array
            case 23:
                array_oid = 1007;
                break; // int4 array
            case 20:
                array_oid = 1016;
                break; // int8 array
            case 700:
                array_oid = 1021;
                break; // float4 array
            case 701:
                array_oid = 1022;
                break; // float8 array
            case 25:
                array_oid = 1009;
                break; // text array
            default:
                array_oid = 2277;
                break; // Use anyarray as fallback
        }

        // Add the array OID type
        param_types_.push_back(array_oid);

        // For empty vectors, write NULL
        if (vector.empty()) {
            write_null();
            return;
        }

        // OPTIMIZED: Reserve space for param_types_ (P0-3 fix)
        param_types_.reserve(param_types_.size() + 1);

        // Prepare a binary buffer for the array
        std::vector<byte> array_buffer;

        // PostgreSQL array binary format:
        // int32 number of dimensions (1 for 1D array)
        // int32 has nulls flag (1 if array has nulls, 0 otherwise)
        // int32 element type OID
        // int32 dimension size
        // int32 dimension lower bound (typically 1)
        // followed by each element with int32 length prefix and data

        // OPTIMIZED: Reserve space for header + estimated elements (P0-3 fix)
        // Header = 20 bytes, each element = 4 bytes length prefix + data
        size_t estimated_element_size = sizeof(integer); // length prefix
        if constexpr (std::is_same_v<element_type, smallint>)
            estimated_element_size += sizeof(smallint);
        else if constexpr (std::is_same_v<element_type, integer>)
            estimated_element_size += sizeof(integer);
        else if constexpr (std::is_same_v<element_type, bigint>)
            estimated_element_size += sizeof(bigint);
        else if constexpr (std::is_same_v<element_type, float>)
            estimated_element_size += sizeof(float);
        else if constexpr (std::is_same_v<element_type, double>)
            estimated_element_size += sizeof(double);
        else if constexpr (std::is_same_v<element_type, bool>)
            estimated_element_size += sizeof(byte);
        else
            estimated_element_size += 32; // default estimate for strings/complex types
        array_buffer.reserve(20 + vector.size() * estimated_element_size);

        // We'll start with a 1D array header (20 bytes)
        // Number of dimensions
        write_integer(array_buffer, 1);

        // Has nulls flag (0 = no nulls, check or implement if needed)
        write_integer(array_buffer, 0);

        // Element type OID
        write_integer(array_buffer, element_oid);

        // Dimension size
        write_integer(array_buffer, static_cast<integer>(vector.size()));

        // Lower bound (typically 1 for PostgreSQL arrays)
        write_integer(array_buffer, 1);

        // Now serialize each element
        for (const auto &elem : vector) {
            // For each element, use TypeConverter to serialize it
            std::vector<byte> elem_buffer;
            TypeConverter<element_type>::to_binary(elem, elem_buffer);

            // Add element data to array buffer
            array_buffer.insert(array_buffer.end(), elem_buffer.begin(), elem_buffer.end());
        }

        // Write the total array length
        write_integer(params_buffer_, static_cast<integer>(array_buffer.size()));

        // Write the array data
        params_buffer_.insert(params_buffer_.end(), array_buffer.begin(), array_buffer.end());
    }
};

} // namespace qb::pg::detail

// Include the template specializations
#include "./param_serializer.tpp"
