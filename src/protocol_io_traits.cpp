/**
 * @file protocol_io_traits.cpp
 * @brief Implementation of PostgreSQL protocol I/O traits
 *
 * This file implements the specializations for reading PostgreSQL protocol
 * data, particularly for UUID types in both binary and text formats.
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

#include <array>
#include <ctime>
#ifndef _WIN32
#include <netinet/in.h> // For htons, htonl
#endif
#include <regex>
#include <stdexcept>

#include "./protocol_io_traits.h"

namespace qb {
namespace pg {
namespace io {

/**
 * @brief Helper functions for text format conversion
 *
 * This anonymous namespace contains utility functions for converting
 * PostgreSQL text format data to C++ types.
 */
namespace {
/**
 * @brief Convert data from PostgreSQL text format to C++ type
 *
 * Template function that specializes for different C++ types.
 *
 * @tparam T The target C++ type
 * @param data Pointer to the text data
 * @param size Size of the text data
 * @return T The converted value
 * @throws std::runtime_error If conversion fails or type is unsupported
 */
template <typename T>
T
convert_from_text(const char *data, size_t size) {
    if (!data || size == 0)
        throw std::runtime_error("Empty data for text conversion");

    std::string text(data, size);

    if constexpr (std::is_same_v<T, smallint>) {
        return static_cast<smallint>(std::stoi(text));
    } else if constexpr (std::is_same_v<T, integer>) {
        return std::stoi(text);
    } else if constexpr (std::is_same_v<T, bigint>) {
        return std::stoll(text);
    } else if constexpr (std::is_same_v<T, float>) {
        return std::stof(text);
    } else if constexpr (std::is_same_v<T, double>) {
        return std::stod(text);
    } else if constexpr (std::is_same_v<T, bool>) {
        return (text == "t" || text == "true" || text == "y" || text == "yes" ||
                text == "1");
    } else {
        throw std::runtime_error("Unsupported type for text conversion");
    }
}
} // namespace

/**
 * @brief Implementation of protocol_read for UUID in binary format
 *
 * Specialized implementation for reading UUID values from PostgreSQL binary wire format.
 * A UUID in binary format is a consecutive sequence of 16 bytes.
 *
 * @param begin Start iterator for the binary data
 * @param end End iterator for the binary data
 * @param value UUID value to fill
 * @return detail::message::const_iterator Iterator after the read value
 */
template <>
detail::message::const_iterator
protocol_read<pg::protocol_data_format::Binary, qb::uuid>(
    detail::message::const_iterator begin, detail::message::const_iterator end,
    qb::uuid &value) {
    if (std::distance(begin, end) < 16) {
        return begin;
    }

    // Create UUID from raw 16 bytes
    std::array<uint8_t, 16> uuid_bytes;
    for (size_t i = 0; i < 16; ++i) {
        uuid_bytes[i] = static_cast<uint8_t>(*(begin + i));
    }

    value = qb::uuid(uuid_bytes);
    return begin + 16;
}

/**
 * @brief Implementation of protocol_read for UUID in text format
 *
 * Specialized implementation for reading UUID values from PostgreSQL text wire format.
 * In text format, a UUID is represented as a string in the standard format:
 * xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
 *
 * @param begin Start iterator for the text data
 * @param end End iterator for the text data
 * @param value UUID value to fill
 * @return detail::message::const_iterator Iterator after the read value
 */
template <>
detail::message::const_iterator
protocol_read<pg::protocol_data_format::Text, qb::uuid>(
    detail::message::const_iterator begin, detail::message::const_iterator end,
    qb::uuid &value) {
    if (begin == end) {
        return begin;
    }

    try {
        // Extract data into a temporary vector
        auto data = detail::copy_to_vector(begin, end);

        // Parse UUID from string
        auto uuid_opt = qb::uuid::from_string(std::string(data.data(), data.size()));
        if (!uuid_opt) {
            return begin;
        }

        value = *uuid_opt;
        return end;
    } catch (const std::exception &) {
        return begin;
    }
}

} // namespace io
} // namespace pg
} // namespace qb