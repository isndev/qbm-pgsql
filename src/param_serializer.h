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

        // C-string / string-literal params: the decayed type of a string literal is `const char*`,
        // which has no TypeConverter. Route it through the std::string path so `params("text")`
        // serializes as a text/varchar parameter (PostgreSQL infers the column type / cast). This
        // is the live, tested replacement for the old never-wired param_serializer_traits<const char*>.
        else if constexpr (std::is_same_v<value_type, const char *> || std::is_same_v<value_type, char *>) {
            add_param(std::string(param));
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

        const integer element_oid = TypeConverter<element_type>::get_oid();

        // Map the scalar element OID to its concrete PostgreSQL array OID. The previous
        // anyarray (2277) fallback was wrong: anyarray is a pseudo-type PostgreSQL rejects
        // as a Bind parameter type, so any vector of an unlisted element (uuid, numeric,
        // temporal, json, ...) produced a wire error. An element type with no array
        // companion now fails loudly here instead of emitting an invalid Bind.
        const oid array_oid = array_oid_for_element(static_cast<oid>(element_oid));
        if (array_oid == oid::invalid) {
            throw std::invalid_argument(
                "pgsql: cannot bind std::vector parameter - element type OID " +
                std::to_string(element_oid) + " has no PostgreSQL array type mapping");
        }
        param_types_.push_back(static_cast<integer>(array_oid));

        // Build the array value bytes via the shared encoder (the exact inverse of
        // decode_pg_array). An empty vector yields a valid EMPTY ARRAY ('{}', ndim=1
        // size=0), NOT SQL NULL — binding NULL would be a different value and break
        // `col = ANY($1)` / array_length / cardinality semantics.
        const std::vector<byte> body = encode_pg_array<element_type>(vector);
        write_integer(params_buffer_, static_cast<integer>(body.size()));
        params_buffer_.insert(params_buffer_.end(), body.begin(), body.end());
    }
};

} // namespace qb::pg::detail
