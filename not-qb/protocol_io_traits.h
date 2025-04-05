#pragma once

#include "common.h"
#include <string>
#include <optional>
#include <type_traits>
#include <limits>
#include <algorithm>
#include "../detail/param_unserializer.h"
#include <regex>
#include <array>

namespace qb {
namespace pg {

namespace io {

/**
 * @brief Traits for PostgreSQL protocol I/O operations
 */
namespace traits {

/**
 * @brief Checks if a type has a parser for a given format
 */
template <typename T, protocol_data_format F>
struct has_parser : std::false_type {};

/**
 * @brief Checks if a type is nullable
 */
template <typename T>
struct is_nullable : std::false_type {};

/**
 * @brief Specialization for std::optional
 */
template <typename T>
struct is_nullable<std::optional<T>> : std::true_type {};

/**
 * @brief Traits for nullable types
 */
template <typename T>
struct nullable_traits {
    static void set_null(T& val) {
        val = T{};
    }
};

/**
 * @brief Specialization for std::optional
 */
template <typename T>
struct nullable_traits<std::optional<T>> {
    static void set_null(std::optional<T>& val) {
        val = std::nullopt;
    }
};

// Specializations for common types
template <> struct has_parser<smallint, pg::protocol_data_format::Text> : std::true_type {};
template <> struct has_parser<smallint, pg::protocol_data_format::Binary> : std::true_type {};
template <> struct has_parser<integer, pg::protocol_data_format::Text> : std::true_type {};
template <> struct has_parser<integer, pg::protocol_data_format::Binary> : std::true_type {};
template <> struct has_parser<bigint, pg::protocol_data_format::Text> : std::true_type {};
template <> struct has_parser<bigint, pg::protocol_data_format::Binary> : std::true_type {};
template <> struct has_parser<float, pg::protocol_data_format::Text> : std::true_type {};
template <> struct has_parser<float, pg::protocol_data_format::Binary> : std::true_type {};
template <> struct has_parser<double, pg::protocol_data_format::Text> : std::true_type {};
template <> struct has_parser<double, pg::protocol_data_format::Binary> : std::true_type {};
template <> struct has_parser<bool, pg::protocol_data_format::Text> : std::true_type {};
template <> struct has_parser<bool, pg::protocol_data_format::Binary> : std::true_type {};
template <> struct has_parser<std::string, pg::protocol_data_format::Text> : std::true_type {};
template <> struct has_parser<std::string, pg::protocol_data_format::Binary> : std::true_type {};
template <> struct has_parser<qb::uuid, pg::protocol_data_format::Text> : std::true_type {};
template <> struct has_parser<qb::uuid, pg::protocol_data_format::Binary> : std::true_type {};

// Specializations for std::optional
template <typename T>
struct has_parser<std::optional<T>, pg::protocol_data_format::Text>
    : has_parser<T, pg::protocol_data_format::Text> {};

template <typename T>
struct has_parser<std::optional<T>, pg::protocol_data_format::Binary>
    : has_parser<T, pg::protocol_data_format::Binary> {};

// Specializations for tuples
template <typename... Args>
struct has_parser<std::tuple<Args...>, pg::protocol_data_format::Text> : std::true_type {};

template <typename... Args>
struct has_parser<std::tuple<Args...>, pg::protocol_data_format::Binary> : std::true_type {};

/**
 * @brief Base reader for binary data that uses ParamUnserializer
 */
template <typename T>
struct binary_reader {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, T& value) {
        // Default version - does nothing
        return begin;
    }
};

/**
 * @brief Specialization for smallint
 */
template <>
struct binary_reader<smallint> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, smallint& value) {
        if (std::distance(begin, end) < sizeof(smallint))
            return begin;
        
        std::vector<byte> buffer(begin, begin + sizeof(smallint));
        static detail::ParamUnserializer unserializer;
        value = unserializer.read_smallint(buffer);
        return begin + sizeof(smallint);
    }
};

/**
 * @brief Specialization for integer
 */
template <>
struct binary_reader<integer> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, integer& value) {
        if (std::distance(begin, end) < sizeof(integer))
            return begin;
        
        std::vector<byte> buffer(begin, begin + sizeof(integer));
        static detail::ParamUnserializer unserializer;
        value = unserializer.read_integer(buffer);
        return begin + sizeof(integer);
    }
};

/**
 * @brief Specialization for bigint
 */
template <>
struct binary_reader<bigint> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, bigint& value) {
        if (std::distance(begin, end) < sizeof(bigint))
            return begin;
        
        std::vector<byte> buffer(begin, begin + sizeof(bigint));
        static detail::ParamUnserializer unserializer;
        value = unserializer.read_bigint(buffer);
        return begin + sizeof(bigint);
    }
};

/**
 * @brief Specialization for float
 */
template <>
struct binary_reader<float> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, float& value) {
        if (std::distance(begin, end) < sizeof(float))
            return begin;
        
        std::vector<byte> buffer(begin, begin + sizeof(float));
        static detail::ParamUnserializer unserializer;
        value = unserializer.read_float(buffer);
        return begin + sizeof(float);
    }
};

/**
 * @brief Specialization for double
 */
template <>
struct binary_reader<double> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, double& value) {
        if (std::distance(begin, end) < sizeof(double))
            return begin;
        
        std::vector<byte> buffer(begin, begin + sizeof(double));
        static detail::ParamUnserializer unserializer;
        value = unserializer.read_double(buffer);
        return begin + sizeof(double);
    }
};

/**
 * @brief Specialization for bool
 */
template <>
struct binary_reader<bool> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, bool& value) {
        if (begin == end)
            return begin;
        
        value = (*begin != 0);
        return begin + 1;
    }
};

/**
 * @brief Specialization for std::string
 */
template <>
struct binary_reader<std::string> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, std::string& value) {
        if (begin == end) {
            value.clear();
            return begin;
        }
        
        std::vector<byte> buffer(begin, end);
        static detail::ParamUnserializer unserializer;
        value = unserializer.read_string(buffer);
        return end;
    }
};

/**
 * @brief Specialization for UUID (binary)
 */
template <>
struct binary_reader<qb::uuid> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, qb::uuid& value) {
        if (std::distance(begin, end) < 16) // UUID is 16 bytes
            return begin;
        
        // Create a UUID from the raw 16 bytes
        std::array<uint8_t, 16> uuid_bytes;
        for (size_t i = 0; i < 16; ++i) {
            uuid_bytes[i] = static_cast<uint8_t>(*(begin + i));
        }
        
        value = qb::uuid(uuid_bytes);
        return begin + 16;
    }
};

/**
 * @brief Text data reader
 */
template <typename T>
struct text_reader {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, T& value) {
        // Default version - does nothing
        return begin;
    }
};

/**
 * @brief Specialization for smallint
 */
template <>
struct text_reader<smallint> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, smallint& value) {
        // For TEXT format, we look for a NULL-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        try {
            value = std::stoi(text_value);
            return null_terminator + 1; // Skip the \0
        } catch (const std::exception&) {
            return begin;
        }
    }
};

/**
 * @brief Specialization for integer
 */
template <>
struct text_reader<integer> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, integer& value) {
        // For TEXT format, we look for a NULL-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        try {
            value = std::stoi(text_value);
            return null_terminator + 1; // Skip the \0
        } catch (const std::exception&) {
            return begin;
        }
    }
};

/**
 * @brief Specialization for bigint
 */
template <>
struct text_reader<bigint> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, bigint& value) {
        // For TEXT format, we look for a NULL-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        try {
            value = std::stoll(text_value);
            return null_terminator + 1; // Skip the \0
        } catch (const std::exception&) {
            return begin;
        }
    }
};

/**
 * @brief Specialization for float
 */
template <>
struct text_reader<float> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, float& value) {
        // For TEXT format, we look for a NULL-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        try {
            value = std::stof(text_value);
            return null_terminator + 1; // Skip the \0
        } catch (const std::exception&) {
            return begin;
        }
    }
};

/**
 * @brief Specialization for double
 */
template <>
struct text_reader<double> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, double& value) {
        // For TEXT format, we look for a NULL-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        try {
            value = std::stod(text_value);
            return null_terminator + 1; // Skip the \0
        } catch (const std::exception&) {
            return begin;
        }
    }
};

/**
 * @brief Specialization for bool
 */
template <>
struct text_reader<bool> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, bool& value) {
        // For TEXT format, we look for a NULL-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        
        // PostgreSQL uses 'true'/'false' or 't'/'f'
        value = (text_value == "true" || text_value == "t" || text_value == "1" || text_value == "yes" || text_value == "y");
        return null_terminator + 1; // Skip the \0
    }
};

/**
 * @brief Specialization for std::string
 */
template <>
struct text_reader<std::string> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, std::string& value) {
        // For TEXT format, we look for a NULL-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        
        value.assign(begin, null_terminator);
        
        if (null_terminator == end)
            return end;
        
        return null_terminator + 1; // Skip the \0
    }
};

/**
 * @brief Specialization for UUID
 */
template <>
struct text_reader<qb::uuid> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, qb::uuid& value) {
        // For TEXT format, we look for a null-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        try {
            // Parse UUID from standard format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
            auto uuid_opt = qb::uuid::from_string(text_value);
            if (uuid_opt) {
                value = *uuid_opt;
                return null_terminator + 1; // Skip the \0
            }
        } catch (const std::exception&) {
            // Parsing failed
        }
        
        return begin;
    }
};

} // namespace traits

namespace detail {
    // Type for PostgreSQL protocol messages
    using message = std::vector<byte>;
    
    /**
     * @brief Converts a data segment to a vector
     * 
     * @tparam InputIterator Input iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @return std::vector<byte> Data as a vector
     */
    template <typename InputIterator>
    std::vector<byte> copy_to_vector(InputIterator begin, InputIterator end) {
        std::vector<byte> result;
        result.reserve(std::distance(begin, end));
        std::copy(begin, end, std::back_inserter(result));
        return result;
    }
}

/**
 * @brief Main read function for PostgreSQL protocol
 * 
 * This function is an entry point that selects the right implementation
 * based on format and type.
 * 
 * @tparam F Data format (TEXT or BINARY)
 * @tparam T Type to read
 * @tparam InputIterator Input iterator type
 * @param begin Start iterator
 * @param end End iterator
 * @param value Value to fill
 * @return InputIterator New start iterator (after reading)
 */
template <protocol_data_format F, typename T, typename InputIterator>
InputIterator protocol_read(InputIterator begin, InputIterator end, T &value) {
    if (begin == end)
        return begin;
        
    if constexpr (F == pg::protocol_data_format::Binary) {
        return traits::binary_reader<T>::read(begin, end, value);
    } else { // pg::protocol_data_format::Text
        return traits::text_reader<T>::read(begin, end, value);
    }
}

// UUID specializations
template<>
detail::message::const_iterator
protocol_read<pg::protocol_data_format::Binary, qb::uuid>(
    detail::message::const_iterator begin,
    detail::message::const_iterator end,
    qb::uuid& value);

template<>
detail::message::const_iterator
protocol_read<pg::protocol_data_format::Text, qb::uuid>(
    detail::message::const_iterator begin,
    detail::message::const_iterator end,
    qb::uuid& value);

} // namespace io
} // namespace pg
} // namespace qb 