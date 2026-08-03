/**
 * @file protocol_io_traits.h
 * @brief PostgreSQL protocol I/O traits for type conversion
 *
 * This file provides traits and utility functions for converting between
 * PostgreSQL wire protocol data formats and C++ types, including specializations
 * for binary and text data formats for standard types.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#pragma once

#include <memory>
#include <iterator>
#include <span>
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <type_traits>

#include <qb/system/parse.h>

#include "./common.h"
#include "./param_unserializer.h"

namespace qb {
namespace pg {

namespace io {

/**
 * @brief Traits for PostgreSQL protocol I/O operations
 *
 * This namespace contains type traits, parsers, and serializers for converting
 * between PostgreSQL wire protocol data formats and C++ types.
 */
namespace traits {

/**
 * @brief Checks if a type has a parser for a given format
 *
 * @tparam T The type to check
 * @tparam F The protocol data format (Binary or Text)
 */
template <typename T, protocol_data_format F>
struct has_parser : std::false_type {};

/**
 * @brief Checks if a type is nullable
 *
 * @tparam T The type to check
 */
template <typename T>
struct is_nullable : std::false_type {};

/**
 * @brief Specialization for std::optional
 *
 * @tparam T The type contained in std::optional
 */
template <typename T>
struct is_nullable<std::optional<T>> : std::true_type {};

/**
 * @brief Traits for nullable types
 *
 * Provides operations specific to nullable types, such as setting them to null.
 *
 * @tparam T The nullable type
 */
template <typename T>
struct nullable_traits {
    /**
     * @brief Set a value to its null representation
     *
     * @param val The value to set to null
     */
    static void
    set_null(T &val) {
        val = T{};
    }
};

/**
 * @brief Specialization for std::optional
 *
 * @tparam T The type contained in std::optional
 */
template <typename T>
struct nullable_traits<std::optional<T>> {
    /**
     * @brief Set an optional value to nullopt
     *
     * @param val The optional value to set to null
     */
    static void
    set_null(std::optional<T> &val) {
        val = std::nullopt;
    }
};

// Specializations for common types that declare they have parsers for both text and
// binary formats

/**
 * @brief smallint (int16) can be parsed from text format
 */
template <>
struct has_parser<smallint, pg::protocol_data_format::Text> : std::true_type {};

/**
 * @brief smallint (int16) can be parsed from binary format
 */
template <>
struct has_parser<smallint, pg::protocol_data_format::Binary> : std::true_type {};

/**
 * @brief integer (int32) can be parsed from text format
 */
template <>
struct has_parser<integer, pg::protocol_data_format::Text> : std::true_type {};

/**
 * @brief integer (int32) can be parsed from binary format
 */
template <>
struct has_parser<integer, pg::protocol_data_format::Binary> : std::true_type {};

/**
 * @brief bigint (int64) can be parsed from text format
 */
template <>
struct has_parser<bigint, pg::protocol_data_format::Text> : std::true_type {};

/**
 * @brief bigint (int64) can be parsed from binary format
 */
template <>
struct has_parser<bigint, pg::protocol_data_format::Binary> : std::true_type {};

/**
 * @brief float (float4) can be parsed from text format
 */
template <>
struct has_parser<float, pg::protocol_data_format::Text> : std::true_type {};

/**
 * @brief float (float4) can be parsed from binary format
 */
template <>
struct has_parser<float, pg::protocol_data_format::Binary> : std::true_type {};

/**
 * @brief double (float8) can be parsed from text format
 */
template <>
struct has_parser<double, pg::protocol_data_format::Text> : std::true_type {};

/**
 * @brief double (float8) can be parsed from binary format
 */
template <>
struct has_parser<double, pg::protocol_data_format::Binary> : std::true_type {};

/**
 * @brief bool can be parsed from text format
 */
template <>
struct has_parser<bool, pg::protocol_data_format::Text> : std::true_type {};

/**
 * @brief bool can be parsed from binary format
 */
template <>
struct has_parser<bool, pg::protocol_data_format::Binary> : std::true_type {};

/**
 * @brief std::string can be parsed from text format
 */
template <>
struct has_parser<std::string, pg::protocol_data_format::Text> : std::true_type {};

/**
 * @brief std::string can be parsed from binary format
 */
template <>
struct has_parser<std::string, pg::protocol_data_format::Binary> : std::true_type {};

/**
 * @brief UUID can be parsed from text format
 */
template <>
struct has_parser<qb::uuid, pg::protocol_data_format::Text> : std::true_type {};

/**
 * @brief UUID can be parsed from binary format
 */
template <>
struct has_parser<qb::uuid, pg::protocol_data_format::Binary> : std::true_type {};

/**
 * @brief Specialization for optional types in text format
 *
 * An optional<T> can be parsed if T can be parsed
 *
 * @tparam T The type contained in std::optional
 */
template <typename T>
struct has_parser<std::optional<T>, pg::protocol_data_format::Text> : has_parser<T, pg::protocol_data_format::Text> {};

/**
 * @brief Specialization for optional types in binary format
 *
 * An optional<T> can be parsed if T can be parsed
 *
 * @tparam T The type contained in std::optional
 */
template <typename T>
struct has_parser<std::optional<T>, pg::protocol_data_format::Binary> : has_parser<T, pg::protocol_data_format::Binary> {};

/**
 * @brief Tuples can be parsed from text format
 *
 * @tparam Args The types contained in the tuple
 */
template <typename... Args>
struct has_parser<std::tuple<Args...>, pg::protocol_data_format::Text> : std::true_type {};

/**
 * @brief Tuples can be parsed from binary format
 *
 * @tparam Args The types contained in the tuple
 */
template <typename... Args>
struct has_parser<std::tuple<Args...>, pg::protocol_data_format::Binary> : std::true_type {};

/**
 * @brief View @p n bytes starting at @p begin as a `std::span`, without copying when possible.
 * @details The unserializer reads take `std::span<const byte>`, but every call site used to
 *          materialise a `std::vector<byte>` first — a heap allocation and a copy purely to produce
 *          a view. On the scalar reads (2-8 bytes, i.e. every smallint/integer/bigint/float/double
 *          of every column of every row) that measured **20-25x** the cost of the read itself
 *          (~20 ns vs ~1 ns, allocation-dominated); on the text read it copied the whole field a
 *          second time. `detail::message::const_iterator` is a `std::vector<char>::const_iterator`,
 *          so the bytes are contiguous and a span is exact. The non-contiguous branch keeps the
 *          template usable with any InputIterator.
 *
 *          Deliberately NOT `static`: at namespace scope that is internal linkage, so every
 *          translation unit including this header would get its own copy, and the externally-linked
 *          `binary_reader<T>::read` templates below would name a different entity in each of them.
 */
template <typename InputIterator, typename Fn>
inline auto
with_byte_span(InputIterator begin, std::size_t n, Fn &&fn) {
    if constexpr (std::contiguous_iterator<InputIterator>) {
        return fn(std::span<const byte>(std::to_address(begin), n));
    } else {
        std::vector<byte> buffer(begin, std::next(begin, static_cast<std::ptrdiff_t>(n)));
        return fn(std::span<const byte>(buffer.data(), buffer.size()));
    }
}

/**
 * @brief Base reader for binary data that uses ParamUnserializer
 *
 * Default implementation that does nothing - specialized for specific types.
 *
 * @tparam T Type to read
 */
template <typename T>
struct binary_reader {
    /**
     * @brief Read binary data into a value
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator, T &) {
        // Default version - does nothing
        return begin;
    }
};

/**
 * @brief Specialization for smallint (int16)
 */
template <>
struct binary_reader<smallint> {
    /**
     * @brief Read binary smallint (2 bytes)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, smallint &value) {
        if (static_cast<std::size_t>(std::distance(begin, end)) < sizeof(smallint))
            return begin;

        static detail::ParamUnserializer unserializer;
        value = with_byte_span(begin, sizeof(smallint), [](std::span<const byte> b) { return unserializer.read_smallint(b); });
        return begin + sizeof(smallint);
    }
};

/**
 * @brief Specialization for integer (int32)
 */
template <>
struct binary_reader<integer> {
    /**
     * @brief Read binary integer (4 bytes)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, integer &value) {
        if (static_cast<std::size_t>(std::distance(begin, end)) < sizeof(integer))
            return begin;

        static detail::ParamUnserializer unserializer;
        value = with_byte_span(begin, sizeof(integer), [](std::span<const byte> b) { return unserializer.read_integer(b); });
        return begin + sizeof(integer);
    }
};

/**
 * @brief Specialization for bigint (int64)
 */
template <>
struct binary_reader<bigint> {
    /**
     * @brief Read binary bigint (8 bytes)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, bigint &value) {
        if (std::distance(begin, end) < sizeof(bigint))
            return begin;

        static detail::ParamUnserializer unserializer;
        value = with_byte_span(begin, sizeof(bigint), [](std::span<const byte> b) { return unserializer.read_bigint(b); });
        return begin + sizeof(bigint);
    }
};

/**
 * @brief Specialization for float (float4)
 */
template <>
struct binary_reader<float> {
    /**
     * @brief Read binary float (4 bytes)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, float &value) {
        if (std::distance(begin, end) < sizeof(float))
            return begin;

        static detail::ParamUnserializer unserializer;
        value = with_byte_span(begin, sizeof(float), [](std::span<const byte> b) { return unserializer.read_float(b); });
        return begin + sizeof(float);
    }
};

/**
 * @brief Specialization for double (float8)
 */
template <>
struct binary_reader<double> {
    /**
     * @brief Read binary double (8 bytes)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, double &value) {
        if (std::distance(begin, end) < sizeof(double))
            return begin;

        static detail::ParamUnserializer unserializer;
        value = with_byte_span(begin, sizeof(double), [](std::span<const byte> b) { return unserializer.read_double(b); });
        return begin + sizeof(double);
    }
};

/**
 * @brief Specialization for bool
 */
template <>
struct binary_reader<bool> {
    /**
     * @brief Read binary bool (1 byte)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, bool &value) {
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
    /**
     * @brief Read binary string
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, std::string &value) {
        if (begin == end) {
            value.clear();
            return begin;
        }

        static detail::ParamUnserializer unserializer;
        // Field value bytes (length prefix already stripped) -> read verbatim; read_string()'s
        // leading-NUL heuristic would corrupt a value beginning with a NUL.
        value = with_byte_span(begin, static_cast<std::size_t>(std::distance(begin, end)),
                               [](std::span<const byte> b) { return unserializer.read_text_string(b); });
        return end;
    }
};

/**
 * @brief Specialization for UUID (binary)
 */
template <>
struct binary_reader<qb::uuid> {
    /**
     * @brief Read binary UUID (16 bytes)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, qb::uuid &value) {
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
 *
 * Default implementation that does nothing - specialized for specific types.
 *
 * @tparam T Type to read
 */
template <typename T>
struct text_reader {
    /**
     * @brief Read text data into a value
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator, T &) {
        // Default version - does nothing
        return begin;
    }
};

/**
 * @brief Specialization for smallint (int16)
 */
template <>
struct text_reader<smallint> {
    /**
     * @brief Read text smallint (null-terminated string)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, smallint &value) {
        // For TEXT format, we look for a NULL-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;

        std::string text_value(begin, null_terminator);
        // std::stoi parsed into int then narrowed to smallint; preserve that
        // exact range/narrowing behaviour by parsing into int.
        if (auto parsed = qb::to_number_prefix<int>(text_value)) {
            value = static_cast<smallint>(*parsed);
            return null_terminator + 1; // Skip the \0
        }
        return begin;
    }
};

/**
 * @brief Specialization for integer (int32)
 */
template <>
struct text_reader<integer> {
    /**
     * @brief Read text integer (null-terminated string)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, integer &value) {
        // For TEXT format, we look for a NULL-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;

        std::string text_value(begin, null_terminator);
        if (auto parsed = qb::to_number_prefix<integer>(text_value)) {
            value = *parsed;
            return null_terminator + 1; // Skip the \0
        }
        return begin;
    }
};

/**
 * @brief Specialization for bigint (int64)
 */
template <>
struct text_reader<bigint> {
    /**
     * @brief Read text bigint (null-terminated string)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, bigint &value) {
        // For TEXT format, we look for a NULL-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;

        std::string text_value(begin, null_terminator);
        if (auto parsed = qb::to_number_prefix<bigint>(text_value)) {
            value = *parsed;
            return null_terminator + 1; // Skip the \0
        }
        return begin;
    }
};

/**
 * @brief Specialization for float (float4)
 */
template <>
struct text_reader<float> {
    /**
     * @brief Read text float (null-terminated string)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, float &value) {
        // For TEXT format, we look for a NULL-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;

        std::string text_value(begin, null_terminator);
        if (auto parsed = qb::to_number_prefix<float>(text_value)) {
            value = *parsed;
            return null_terminator + 1; // Skip the \0
        }
        return begin;
    }
};

/**
 * @brief Specialization for double (float8)
 */
template <>
struct text_reader<double> {
    /**
     * @brief Read text double (null-terminated string)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, double &value) {
        // For TEXT format, we look for a NULL-terminated string
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;

        std::string text_value(begin, null_terminator);
        if (auto parsed = qb::to_number_prefix<double>(text_value)) {
            value = *parsed;
            return null_terminator + 1; // Skip the \0
        }
        return begin;
    }
};

/**
 * @brief Specialization for bool
 */
template <>
struct text_reader<bool> {
    /**
     * @brief Read text bool (null-terminated string)
     *
     * Recognizes various PostgreSQL text representations for boolean:
     * 'true'/'false', 't'/'f', '1'/'0', 'yes'/'no', 'y'/'n'
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, bool &value) {
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
    /**
     * @brief Read text string (null-terminated string)
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, std::string &value) {
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
    /**
     * @brief Read text UUID (null-terminated string)
     *
     * Parses a UUID from standard format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
     *
     * @tparam InputIterator Iterator type
     * @param begin Start iterator
     * @param end End iterator
     * @param value Value to read into
     * @return InputIterator Iterator after the read value
     */
    template <typename InputIterator>
    static InputIterator
    read(InputIterator begin, InputIterator end, qb::uuid &value) {
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
        } catch (const std::exception &) {
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
 * Helper function to copy a range of values into a byte vector.
 *
 * @tparam InputIterator Input iterator type
 * @param begin Start iterator
 * @param end End iterator
 * @return std::vector<byte> Data as a vector
 */
template <typename InputIterator>
std::vector<byte>
copy_to_vector(InputIterator begin, InputIterator end) {
    std::vector<byte> result;
    result.reserve(std::distance(begin, end));
    std::copy(begin, end, std::back_inserter(result));
    return result;
}
} // namespace detail

// Adding a namespace detail to contain utility components and make ParamUnserializer available
namespace detail {
// Import ParamUnserializer from the parent namespace to make it available here
using qb::pg::detail::ParamUnserializer;
} // namespace detail

/**
 * @brief Main read function for PostgreSQL protocol
 *
 * This function is an entry point that selects the right implementation
 * based on format and type. It dispatches to the appropriate binary_reader
 * or text_reader specialization.
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
InputIterator
protocol_read(InputIterator begin, InputIterator end, T &value) {
    if (begin == end)
        return begin;

    if constexpr (F == pg::protocol_data_format::Binary) {
        return traits::binary_reader<T>::read(begin, end, value);
    } else { // pg::protocol_data_format::Text
        return traits::text_reader<T>::read(begin, end, value);
    }
}

/**
 * @brief Specialization for reading UUID from binary format
 *
 * @param begin Start iterator
 * @param end End iterator
 * @param value UUID to fill
 * @return detail::message::const_iterator New start iterator (after reading)
 */
template <>
detail::message::const_iterator protocol_read<pg::protocol_data_format::Binary, qb::uuid>(detail::message::const_iterator begin,
                                                                                          detail::message::const_iterator end, qb::uuid &value);

/**
 * @brief Specialization for reading UUID from text format
 *
 * @param begin Start iterator
 * @param end End iterator
 * @param value UUID to fill
 * @return detail::message::const_iterator New start iterator (after reading)
 */
template <>
detail::message::const_iterator protocol_read<pg::protocol_data_format::Text, qb::uuid>(detail::message::const_iterator begin,
                                                                                        detail::message::const_iterator end, qb::uuid &value);

} // namespace io
} // namespace pg
} // namespace qb