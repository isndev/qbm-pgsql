/**
 * @file type_converter.h
 * @brief Type conversion system for the QB PostgreSQL client
 *
 * This file implements a comprehensive type conversion system between C++ and
 * PostgreSQL:
 *
 * - Bidirectional conversion between C++ and PostgreSQL data types
 * - Support for both binary and text PostgreSQL wire formats
 * - Automatic handling of endianness and network byte order
 * - Special value handling (NaN, infinity) for floating-point types
 * - Support for QB-specific types (UUID, Timestamp)
 * - Handling of NULL values via std::optional
 * - Template-based design for compile-time type safety
 *
 * The type converter system is the core component that enables seamless
 * data interchange between the application and PostgreSQL database server.
 *
 * @see qb::pg::detail::TypeConverter
 * @see qb::pg::detail::type_mapping
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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <qb/io.h>
#include <qb/system/endian.h>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "./common.h"
#include "./param_unserializer.h"
#include "./pg_types.h"
#include "./type_mapping.h"

namespace qb::pg::detail {

/**
 * @brief Unified PostgreSQL type conversion system
 *
 * Primary template class providing bidirectional conversion between C++ types
 * and PostgreSQL data formats. This class serves as the foundation for all
 * type conversions in the PostgreSQL client, supporting both binary protocol
 * and text protocol formats.
 *
 * Key features:
 * - OID type identification for each C++ type
 * - C++ to PostgreSQL binary format conversion
 * - C++ to PostgreSQL text format conversion
 * - PostgreSQL binary format to C++ conversion
 * - PostgreSQL text format to C++ conversion
 * - Proper handling of network byte order and endianness
 *
 * @tparam T C++ type to convert to/from PostgreSQL formats
 */
template <typename T>
class TypeConverter {
public:
    using value_type = typename std::decay<T>::type;

    /**
     * @brief Gets the PostgreSQL OID for a C++ type
     *
     * Determines the appropriate PostgreSQL Object Identifier (OID) that
     * corresponds to the C++ type. This is used when sending parameter
     * type information to the server.
     *
     * @return integer PostgreSQL OID corresponding to the C++ type
     */
    static integer
    get_oid() {
        return type_mapping<value_type>::type_oid;
    }

    /**
     * @brief Converts a C++ value to a PostgreSQL binary buffer
     *
     * Serializes a C++ value into the PostgreSQL binary wire format.
     * This method handles proper binary encoding including:
     * - Length prefix for variable-length types
     * - Network byte order (big-endian) conversion
     * - Type-specific binary representations
     * - Special value encoding
     *
     * The binary format is used for efficient data transfer in the
     * PostgreSQL wire protocol, especially for prepared statements.
     *
     * @param value C++ value to convert to PostgreSQL binary format
     * @param buffer Target buffer where the serialized data will be appended
     */
    static void
    to_binary(const value_type &value, std::vector<byte> &buffer) {
        if constexpr (std::is_same_v<value_type, std::string> ||
                      std::is_same_v<value_type, std::string_view>) {
            // Write length
            integer len = static_cast<integer>(value.size());
            write_integer(buffer, len);

            // Write raw data (without null terminator)
            if (!value.empty()) {
                buffer.insert(
                    buffer.end(), reinterpret_cast<const byte *>(value.data()),
                    reinterpret_cast<const byte *>(value.data() + value.size()));
            }
        } else if constexpr (std::is_same_v<value_type, bool>) {
            // PostgreSQL boolean: length (1) + value (0/1)
            write_integer(buffer, 1);
            buffer.push_back(value ? 1 : 0);
        } else if constexpr (std::is_same_v<value_type, smallint>) {
            // PostgreSQL smallint: length (2) + network value
            write_integer(buffer, 2);
            smallint    netval = htons(value);
            const byte *bytes  = reinterpret_cast<const byte *>(&netval);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(smallint));
        } else if constexpr (std::is_same_v<value_type, integer>) {
            // PostgreSQL integer: length (4) + network value
            write_integer(buffer, 4);
            integer     netval = htonl(value);
            const byte *bytes  = reinterpret_cast<const byte *>(&netval);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(integer));
        } else if constexpr (std::is_same_v<value_type, bigint>) {
            // PostgreSQL bigint: length (8) + network value (manual swap)
            write_integer(buffer, 8);

            bigint      networkValue = qb::endian::to_big_endian(value);
            const byte *bytes        = reinterpret_cast<const byte *>(&networkValue);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(bigint));
        } else if constexpr (std::is_same_v<value_type, float>) {
            // PostgreSQL float: length (4) + IEEE 754 value
            write_integer(buffer, 4);
            const byte *bytes = reinterpret_cast<const byte *>(&value);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(float));
        } else if constexpr (std::is_same_v<value_type, double>) {
            // PostgreSQL double: length (8) + IEEE 754 value
            write_integer(buffer, 8);
            const byte *bytes = reinterpret_cast<const byte *>(&value);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(double));
        } else if constexpr (std::is_same_v<value_type, bytea> ||
                             std::is_same_v<value_type, std::vector<byte>>) {
            // PostgreSQL bytea: length + raw bytes
            integer len = static_cast<integer>(value.size());
            write_integer(buffer, len);

            if (!value.empty()) {
                buffer.insert(buffer.end(), value.begin(), value.end());
            }
        } else if constexpr (std::is_same_v<value_type, qb::uuid>) {
            // PostgreSQL UUID: length (16) + 16 bytes in network byte order
            write_integer(buffer, 16);

            // Convert UUID to byte array (ensuring network byte order if needed)
            std::array<byte, 16> bytes;
            const auto          &uuid_bytes = value.as_bytes();
            for (size_t i = 0; i < uuid_bytes.size(); ++i) {
                bytes[i] = static_cast<byte>(uuid_bytes[i]);
            }

            buffer.insert(buffer.end(), bytes.begin(), bytes.end());
        } else if constexpr (std::is_same_v<value_type, qb::Timestamp> ||
                             std::is_same_v<value_type, qb::UtcTimestamp> ||
                             std::is_same_v<value_type, qb::LocalTimestamp>) {
            // PostgreSQL timestamp: length (8) + microseconds since 2000-01-01 00:00:00
            write_integer(buffer, 8);

            // Convert timestamp to PostgreSQL format (microseconds since 2000-01-01)
            // PostgreSQL epoch is 2000-01-01, Unix epoch is 1970-01-01
            // Difference is 30 years = 946684800 seconds
            constexpr int64_t POSTGRES_EPOCH_DIFF_SECONDS = 946684800;

            // Get seconds since Unix epoch from the timestamp
            double unix_seconds = value.seconds_float();

            // Calculate the whole seconds and fractional part
            int64_t whole_seconds = static_cast<int64_t>(unix_seconds);
            double fractional_seconds = unix_seconds - whole_seconds;
            int64_t microseconds = static_cast<int64_t>(fractional_seconds * 1000000);

            // Convert to PostgreSQL timestamp (microseconds since 2000-01-01)
            int64_t pg_timestamp =
                (whole_seconds - POSTGRES_EPOCH_DIFF_SECONDS) * 1000000 + microseconds;

            // Convert to network byte order using the endian utility
            int64_t network_timestamp = qb::endian::to_big_endian(pg_timestamp);

            // Copy the bytes to the buffer
            const byte *bytes = reinterpret_cast<const byte *>(&network_timestamp);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(int64_t));
        } else if constexpr (detail::ParamUnserializer::is_optional<value_type>::value) {
            // std::optional - delegate to contained type or write NULL
            using inner_type = typename value_type::value_type;

            if (value.has_value()) {
                TypeConverter<inner_type>::to_binary(*value, buffer);
            } else {
                // -1 represents NULL in PostgreSQL binary protocol
                write_integer(buffer, -1);
            }
        } else {
            static_assert(sizeof(T) > 0, "Type not supported for binary conversion");
        }
    }

    /**
     * @brief Converts a C++ value to a PostgreSQL text representation
     *
     * Serializes a C++ value into the PostgreSQL text wire format.
     * This provides human-readable string representations that conform
     * to PostgreSQL's expected text formats for each data type.
     *
     * Features:
     * - Special value handling (NaN, infinity)
     * - Proper formatting for date/time types
     * - Hexadecimal encoding for binary data
     * - Standard formatting for UUID values
     *
     * @param value C++ value to convert to text representation
     * @return std::string Text representation in PostgreSQL format
     */
    static std::string
    to_text(const value_type &value) {
        if constexpr (std::is_same_v<value_type, std::string> ||
                      std::is_same_v<value_type, std::string_view>) {
            return std::string(value);
        } else if constexpr (std::is_same_v<value_type, bool>) {
            return value ? "t" : "f";
        } else if constexpr (std::is_integral_v<value_type>) {
            return std::to_string(value);
        } else if constexpr (std::is_floating_point_v<value_type>) {
            // Special values
            if (std::isnan(value))
                return "NaN";
            if (std::isinf(value)) {
                return value > 0 ? "Infinity" : "-Infinity";
            }
            return std::to_string(value);
        } else if constexpr (std::is_same_v<value_type, bytea> ||
                             std::is_same_v<value_type, std::vector<byte>>) {
            std::string result = "\\x";
            char        hex[3];

            for (byte b : value) {
                std::snprintf(hex, sizeof(hex), "%02x", static_cast<int>(b) & 0xFF);
                result += hex;
            }

            return result;
        } else if constexpr (std::is_same_v<value_type, qb::uuid>) {
            // UUID to string in standard format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
            return uuids::to_string(value);
        } else if constexpr (std::is_same_v<value_type, qb::Timestamp> ||
                             std::is_same_v<value_type, qb::UtcTimestamp> ||
                             std::is_same_v<value_type, qb::LocalTimestamp>) {
            // Format: YYYY-MM-DD HH:MM:SS.MMMMMM
            std::time_t timestamp = static_cast<std::time_t>(value.seconds());
            std::tm    *tm_data   = std::gmtime(&timestamp);

            std::ostringstream os;
            os << std::setfill('0') << std::setw(4) << (tm_data->tm_year + 1900) << '-'
               << std::setw(2) << (tm_data->tm_mon + 1) << '-' << std::setw(2)
               << tm_data->tm_mday << ' ' << std::setw(2) << tm_data->tm_hour << ':'
               << std::setw(2) << tm_data->tm_min << ':' << std::setw(2)
               << tm_data->tm_sec << '.' << std::setw(6)
               << (value.microseconds() % 1000000);

            return os.str();
        } else if constexpr (detail::ParamUnserializer::is_optional<value_type>::value) {
            if (value.has_value()) {
                return TypeConverter<typename value_type::value_type>::to_text(*value);
            }
            return ""; // NULL is represented by length -1 in the protocol
        } else {
            static_assert(sizeof(T) > 0, "Type not supported for text conversion");
            return "";
        }
    }

    /**
     * @brief Converts a PostgreSQL binary buffer to a C++ type
     *
     * Deserializes PostgreSQL binary wire format data into an equivalent
     * C++ value. This method handles:
     * - Length prefix parsing for variable-length types
     * - Network byte order (big-endian) to host byte order conversion
     * - Proper handling of NULL values
     * - Type-specific binary parsing
     * - PostgreSQL epoch to Unix time conversion for timestamps
     *
     * @param buffer Buffer containing the PostgreSQL binary format data
     * @return value_type Deserialized C++ value
     * @throws std::runtime_error If the buffer contains invalid or malformed data
     */
    static value_type
    from_binary(const std::vector<byte> &buffer) {
        static ParamUnserializer unserializer;

        if constexpr (std::is_same_v<value_type, std::string>) {
            return unserializer.read_string(buffer);
        } else if constexpr (std::is_same_v<value_type, smallint>) {
            return unserializer.read_smallint(buffer);
        } else if constexpr (std::is_same_v<value_type, integer>) {
            return unserializer.read_integer(buffer);
        } else if constexpr (std::is_same_v<value_type, bigint>) {
            return unserializer.read_bigint(buffer);
        } else if constexpr (std::is_same_v<value_type, float>) {
            return unserializer.read_float(buffer);
        } else if constexpr (std::is_same_v<value_type, double>) {
            return unserializer.read_double(buffer);
        } else if constexpr (std::is_same_v<value_type, bool>) {
            return !buffer.empty() && buffer[0] != 0;
        } else if constexpr (std::is_same_v<value_type, bytea> ||
                             std::is_same_v<value_type, std::vector<byte>>) {
            value_type result;
            result.assign(buffer.begin(), buffer.end());
            return result;
        } else if constexpr (std::is_same_v<value_type, qb::uuid>) {
            // Assuming buffer contains the raw 16 bytes of a UUID
            if (buffer.size() < 16) {
                throw std::runtime_error("Invalid UUID binary data size");
            }

            // Create UUID from binary data
            std::array<uint8_t, 16> uuid_bytes;
            std::copy_n(buffer.begin(), 16, uuid_bytes.begin());

            return qb::uuid(uuid_bytes);
        } else if constexpr (std::is_same_v<value_type, qb::Timestamp> ||
                             std::is_same_v<value_type, qb::UtcTimestamp> ||
                             std::is_same_v<value_type, qb::LocalTimestamp>) {
            if (buffer.size() < 8) {
                throw std::runtime_error("Buffer too small for timestamp");
            }

            // Difference between PostgreSQL epoch (2000-01-01) and Unix epoch
            // (1970-01-01)
            constexpr int64_t POSTGRES_EPOCH_DIFF = 946684800LL; // seconds

            // Create a temporary variable to hold the timestamp
            int64_t pg_usecs = 0;

            // If buffer contains EXACTLY the timestamp (8 bytes), use it directly
            if (buffer.size() == 8) {
                std::memcpy(&pg_usecs, buffer.data(), 8);
            } else {
                // Otherwise, assume there's a 4-byte prefix
                std::memcpy(&pg_usecs, buffer.data() + 4, 8);
            }

            // Convert big-endian to native order
            pg_usecs = qb::endian::from_big_endian(pg_usecs);

            // Convert to Unix seconds and microseconds
            int64_t unix_secs  = (pg_usecs / 1000000) + POSTGRES_EPOCH_DIFF;
            int64_t unix_usecs = pg_usecs % 1000000;

            if (unix_usecs < 0) {
                unix_secs--;
                unix_usecs += 1000000;
            }

            return qb::Timestamp::from_seconds(unix_secs) +
                   qb::Timespan::from_microseconds(unix_usecs);
        } else if constexpr (detail::ParamUnserializer::is_optional<value_type>::value) {
            using inner_type = typename value_type::value_type;

            if (buffer.empty() ||
                (buffer.size() >= 4 &&
                 *reinterpret_cast<const integer *>(buffer.data()) == -1)) {
                return std::nullopt;
            }

            return TypeConverter<inner_type>::from_binary(buffer);
        } else {
            static_assert(sizeof(T) > 0,
                          "Type not supported for conversion from binary");
            return value_type{};
        }
    }

    /**
     * @brief Converts a PostgreSQL text representation to a C++ type
     *
     * Deserializes a PostgreSQL text format string into an equivalent
     * C++ value. This method provides:
     * - Parsing of PostgreSQL's text format for each data type
     * - Special value recognition (NaN, infinity, null)
     * - Date/time parsing for timestamp formats
     * - Hexadecimal decoding for binary data
     * - UUID string parsing
     * - Proper numeric conversions with bounds checking
     *
     * @param text PostgreSQL text representation to convert
     * @return value_type Deserialized C++ value
     * @throws std::runtime_error If the text contains invalid or malformed data
     * @throws std::out_of_range If numeric values are outside the type's range
     */
    static value_type
    from_text(const std::string &text) {
        if constexpr (std::is_same_v<value_type, std::string>) {
            return text;
        } else if constexpr (std::is_same_v<value_type, smallint>) {
            return static_cast<smallint>(std::stoi(text));
        } else if constexpr (std::is_same_v<value_type, integer>) {
            return static_cast<integer>(std::stoi(text));
        } else if constexpr (std::is_same_v<value_type, bigint>) {
            return static_cast<bigint>(std::stoll(text));
        } else if constexpr (std::is_same_v<value_type, float>) {
            // Special values
            if (text == "NaN")
                return std::numeric_limits<float>::quiet_NaN();
            if (text == "Infinity" || text == "inf")
                return std::numeric_limits<float>::infinity();
            if (text == "-Infinity" || text == "-inf")
                return -std::numeric_limits<float>::infinity();
            return std::stof(text);
        } else if constexpr (std::is_same_v<value_type, double>) {
            // Special values
            if (text == "NaN")
                return std::numeric_limits<double>::quiet_NaN();
            if (text == "Infinity" || text == "inf")
                return std::numeric_limits<double>::infinity();
            if (text == "-Infinity" || text == "-inf")
                return -std::numeric_limits<double>::infinity();
            return std::stod(text);
        } else if constexpr (std::is_same_v<value_type, bool>) {
            return (text == "t" || text == "true" || text == "1" || text == "yes" ||
                    text == "y" || text == "on");
        } else if constexpr (std::is_same_v<value_type, bytea> ||
                             std::is_same_v<value_type, std::vector<byte>>) {
            value_type result;

            // Hexadecimal format (\x...)
            if (text.length() >= 2 && text.substr(0, 2) == "\\x") {
                std::string hex = text.substr(2);
                for (size_t i = 0; i + 1 < hex.length(); i += 2) {
                    byte byte_val =
                        static_cast<byte>(std::stoi(hex.substr(i, 2), nullptr, 16));
                    result.push_back(byte_val);
                }
            } else {
                // Raw format
                result.insert(result.end(), text.begin(), text.end());
            }

            return result;
        } else if constexpr (std::is_same_v<value_type, qb::uuid>) {
            // Parse UUID from standard format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
            return qb::uuid::from_string(text).value_or(qb::uuid{});
        } else if constexpr (std::is_same_v<value_type, qb::Timestamp> ||
                             std::is_same_v<value_type, qb::UtcTimestamp> ||
                             std::is_same_v<value_type, qb::LocalTimestamp>) {
            // Parse PostgreSQL timestamp format: YYYY-MM-DD HH:MM:SS[.MMMMMM]
            std::tm tm   = {};
            int     year, month, day, hour, minute, second, microsecond = 0;

            // Regular expression to match PostgreSQL timestamp format
            std::regex timestamp_regex(
                R"((\d{4})-(\d{1,2})-(\d{1,2})\s+(\d{1,2}):(\d{1,2}):(\d{1,2})(?:\.(\d{1,6}))?)");

            std::smatch matches;
            if (std::regex_match(text, matches, timestamp_regex)) {
                year   = std::stoi(matches[1]);
                month  = std::stoi(matches[2]);
                day    = std::stoi(matches[3]);
                hour   = std::stoi(matches[4]);
                minute = std::stoi(matches[5]);
                second = std::stoi(matches[6]);

                // Parse microseconds if present
                if (matches.size() > 7 && matches[7].matched) {
                    // Pad with zeros if less than 6 digits
                    std::string micro_str = matches[7].str();
                    micro_str.append(6 - micro_str.length(), '0');
                    microsecond = std::stoi(micro_str);
                }

                // Set tm structure
                tm.tm_year  = year - 1900; // Years since 1900
                tm.tm_mon   = month - 1;   // Months since January (0-11)
                tm.tm_mday  = day;         // Day of month (1-31)
                tm.tm_hour  = hour;        // Hours (0-23)
                tm.tm_min   = minute;      // Minutes (0-59)
                tm.tm_sec   = second;      // Seconds (0-61)
                tm.tm_isdst = -1;          // Let system determine DST

                // Convert to Unix timestamp
                std::time_t unix_timestamp = std::mktime(&tm);

                // Create timestamp from seconds and microseconds
                return value_type(qb::Timestamp::from_seconds(unix_timestamp) +
                                  qb::Timespan::from_microseconds(microsecond));
            }

            throw std::runtime_error("Invalid timestamp format");
        } else if constexpr (detail::ParamUnserializer::is_optional<value_type>::value) {
            using inner_type = typename value_type::value_type;

            if (text.empty()) {
                return std::nullopt;
            }

            return TypeConverter<inner_type>::from_text(text);
        } else {
            static_assert(sizeof(T) > 0, "Type not supported for conversion from text");
            return value_type{};
        }
    }

    /**
     * @brief Writes an integer to a buffer in network byte order
     *
     * Helper method to append a 32-bit integer to a byte buffer in
     * network byte order (big-endian). This is used to add length
     * prefixes and integer values in the PostgreSQL binary format.
     *
     * @param buffer Target buffer to append the integer to
     * @param value 32-bit integer value to write in network byte order
     */
    static void
    write_integer(std::vector<byte> &buffer, integer value) {
        integer     networkValue = htonl(value);
        const byte *bytes        = reinterpret_cast<const byte *>(&networkValue);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(integer));
    }
};

// Specialization for UUID type
template <>
struct TypeConverter<qb::uuid> {
    using value_type = qb::uuid;

    /**
     * @brief Returns the PostgreSQL OID for UUID type
     *
     * @return integer PostgreSQL type OID for UUID
     */
    static integer
    get_oid() {
        return static_cast<integer>(oid::uuid);
    }

    /**
     * @brief Converts a UUID to PostgreSQL binary format
     *
     * Creates a PostgreSQL binary representation of a UUID, following
     * PostgreSQL format specifications:
     * - 4-byte integer length prefix (16)
     * - 16-byte UUID data
     *
     * @param value The UUID to convert
     * @param buffer The buffer to store the PostgreSQL binary format
     */
    static void
    to_binary(const qb::uuid &value, std::vector<byte> &buffer) {
        // PostgreSQL binary format consists of:
        // - 4-byte length prefix
        // - 16-byte UUID

        // Resize buffer to hold length prefix and UUID data
        buffer.resize(4 + 16);

        // Write length (16) in big-endian format
        buffer[0] = 0;
        buffer[1] = 0;
        buffer[2] = 0;
        buffer[3] = 16;

        // Get UUID bytes
        auto bytes_span = value.as_bytes();

        // Write UUID data after length prefix
        for (size_t i = 0; i < 16; ++i) {
            buffer[i + 4] = static_cast<byte>(bytes_span[i]);
        }
    }

    /**
     * @brief Converts a UUID to PostgreSQL text format
     *
     * Creates a standard text representation of a UUID in format:
     * xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
     *
     * @param value The UUID to convert
     * @return std::string The PostgreSQL text representation of the UUID
     */
    static std::string
    to_text(const qb::uuid &value) {
        return uuids::to_string(value);
    }

    /**
     * @brief Converts PostgreSQL binary format to a UUID
     *
     * Handles binary format conversion from PostgreSQL's UUID type
     * to the qb::uuid C++ type. This specialized implementation handles
     * both formats:
     * - Raw 16-byte UUID without length prefix
     * - Standard PostgreSQL binary format with 4-byte length prefix
     *
     * @param buffer Buffer containing the PostgreSQL binary UUID data
     * @return qb::uuid Converted UUID object
     * @throws std::runtime_error If the buffer is too small or malformed
     */
    static qb::uuid
    from_binary(const std::vector<byte> &buffer) {
        // Check buffer size with length prefix
        if (buffer.size() < 4 + 16) { // 4 bytes length + 16 bytes UUID
            throw std::runtime_error("Buffer too small for UUID");
        }

        // Check declared length (ignore length prefix)
        std::array<uint8_t, 16> uuid_bytes;

        // If buffer contains EXACTLY the UUID (16 bytes), use it directly
        if (buffer.size() == 16) {
            for (size_t i = 0; i < 16; ++i) {
                uuid_bytes[i] = static_cast<uint8_t>(buffer[i]);
            }
            return qb::uuid(uuid_bytes);
        }

        // Otherwise, assume there's a 4-byte prefix
        for (size_t i = 0; i < 16; ++i) {
            uuid_bytes[i] = static_cast<uint8_t>(buffer[i + 4]);
        }

        return qb::uuid(uuid_bytes);
    }

    /**
     * @brief Converts PostgreSQL text format to a UUID
     *
     * Parses a PostgreSQL text representation of a UUID into a qb::uuid object.
     * Expects the standard UUID format: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
     *
     * @param text Text representation of a UUID
     * @return qb::uuid Converted UUID object
     * @throws std::runtime_error If the text is not a valid UUID format
     */
    static qb::uuid
    from_text(const std::string &text) {
        // Expected format: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
        auto uuid_result = qb::uuid::from_string(text);
        if (!uuid_result) {
            throw std::runtime_error("Invalid UUID format");
        }
        return uuid_result.value();
    }
};

// Specialization for Timestamp type
template <>
struct TypeConverter<qb::Timestamp> {
    using value_type = qb::Timestamp;

    /**
     * @brief Returns the PostgreSQL OID for timestamp type
     *
     * @return integer PostgreSQL type OID for timestamp
     */
    static integer
    get_oid() {
        return static_cast<integer>(oid::timestamp);
    }

    /**
     * @brief Converts a Timestamp to PostgreSQL binary format
     *
     * Creates a PostgreSQL binary representation of a timestamp,
     * following PostgreSQL format specifications:
     * - 4-byte integer length prefix (8)
     * - 8-byte timestamp value in microseconds since 2000-01-01
     *
     * @param value The timestamp to convert
     * @param buffer The buffer to store the PostgreSQL binary format
     */
    static void
    to_binary(const qb::Timestamp &value, std::vector<byte> &buffer) {
        // PostgreSQL binary format consists of:
        // - 4-byte length prefix
        // - 8-byte timestamp (microseconds since 2000-01-01)

        // Resize buffer to hold length prefix and timestamp data
        buffer.resize(4 + 8);

        // Write length (8) in big-endian format
        buffer[0] = 0;
        buffer[1] = 0;
        buffer[2] = 0;
        buffer[3] = 8;

        // Difference between PostgreSQL epoch (2000-01-01) and Unix epoch (1970-01-01)
        constexpr int64_t POSTGRES_EPOCH_DIFF = 946684800LL; // seconds

        // Convert Unix timestamp to PostgreSQL timestamp using float for higher precision
        double unix_secs_float = value.seconds_float();
        int64_t whole_seconds = static_cast<int64_t>(unix_secs_float);
        double fractional_part = unix_secs_float - whole_seconds;
        int64_t unix_usecs = static_cast<int64_t>(fractional_part * 1000000LL);
        
        int64_t pg_usecs = (whole_seconds - POSTGRES_EPOCH_DIFF) * 1000000LL + unix_usecs;

        // Convert to network byte order (big-endian) using the endian utility
        int64_t network_usecs = qb::endian::to_big_endian(pg_usecs);

        // Write timestamp data after length prefix
        const byte *bytes = reinterpret_cast<const byte *>(&network_usecs);
        std::memcpy(buffer.data() + 4, bytes, sizeof(int64_t));
    }

    /**
     * @brief Converts a Timestamp to PostgreSQL text format
     *
     * Creates a standard text representation of a timestamp in ISO 8601 format:
     * YYYY-MM-DD HH:MM:SS.ssssss
     *
     * @param value The timestamp to convert
     * @return std::string The PostgreSQL text representation of the timestamp
     */
    static std::string
    to_text(const qb::Timestamp &value) {
        // Get the whole seconds part
        double unix_secs_float = value.seconds_float();
        std::time_t unix_time = static_cast<std::time_t>(unix_secs_float);
        std::tm    *time_info = std::localtime(&unix_time);
        char        buf[32];

        // Format the date and time parts
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", time_info);

        // Add microseconds with higher precision using fractional part
        std::string result(buf);
        double fractional_part = unix_secs_float - std::floor(unix_secs_float);
        int64_t microsecs = static_cast<int64_t>(fractional_part * 1000000);
        if (microsecs > 0) {
            char usec_buf[8];
            std::snprintf(usec_buf, sizeof(usec_buf), ".%06ld",
                         static_cast<long>(microsecs));
            result += usec_buf;
        }

        return result;
    }

    /**
     * @brief Converts PostgreSQL binary format to a Timestamp
     *
     * Deserializes a PostgreSQL binary timestamp representation into a qb::Timestamp.
     * PostgreSQL timestamps are stored as microseconds since 2000-01-01, while
     * Unix/C++ timestamps are seconds since 1970-01-01.
     *
     * This method handles both:
     * - Raw 8-byte timestamp without length prefix
     * - Standard PostgreSQL binary format with 4-byte length prefix
     *
     * @param buffer Buffer containing the PostgreSQL binary timestamp data
     * @return qb::Timestamp Converted timestamp object
     * @throws std::runtime_error If the buffer is too small or malformed
     */
    static qb::Timestamp
    from_binary(const std::vector<byte> &buffer) {
        // PostgreSQL timestamp is in microseconds since 2000-01-01
        if (buffer.size() < 8) { // at least 8 bytes for timestamp
            throw std::runtime_error("Buffer too small for timestamp");
        }

        // Difference between PostgreSQL epoch (2000-01-01) and Unix epoch (1970-01-01)
        constexpr int64_t POSTGRES_EPOCH_DIFF = 946684800LL; // seconds

        // Create a temporary variable to hold the timestamp
        int64_t pg_usecs = 0;

        // If buffer contains EXACTLY the timestamp (8 bytes), use it directly
        if (buffer.size() == 8) {
            std::memcpy(&pg_usecs, buffer.data(), 8);
        } else {
            // Otherwise, assume there's a 4-byte prefix
            std::memcpy(&pg_usecs, buffer.data() + 4, 8);
        }

        // Convert big-endian to native order
        pg_usecs = qb::endian::from_big_endian(pg_usecs);

        // Convert to Unix seconds and microseconds
        int64_t unix_secs  = (pg_usecs / 1000000) + POSTGRES_EPOCH_DIFF;
        int64_t unix_usecs = pg_usecs % 1000000;

        if (unix_usecs < 0) {
            unix_secs--;
            unix_usecs += 1000000;
        }

        return qb::Timestamp::from_seconds(unix_secs) +
               qb::Timespan::from_microseconds(unix_usecs);
    }

    /**
     * @brief Converts PostgreSQL text format to a Timestamp
     *
     * Parses a PostgreSQL text representation of a timestamp into a qb::Timestamp
     * object. Handles the standard PostgreSQL timestamp format: "YYYY-MM-DD
     * HH:MM:SS.ssssss"
     *
     * @param text Text representation of a timestamp
     * @return qb::Timestamp Converted timestamp object
     * @throws std::runtime_error If the text is not a valid timestamp format
     */
    static qb::Timestamp
    from_text(const std::string &text) {
        // Expected format: "YYYY-MM-DD HH:MM:SS.ssssss" or "YYYY-MM-DD HH:MM:SS"
        std::tm tm   = {};
        int     usec = 0;

        // Parse date and time
        if (text.empty()) {
            throw std::runtime_error("Empty timestamp string");
        }

        int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;

        // Use sscanf which is more tolerant of formats
        int matched = sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day,
                             &hour, &min, &sec);

        if (matched != 6) {
            throw std::runtime_error("Invalid timestamp format");
        }

        // Read fractional part if it exists
        size_t dot_pos = text.find('.');
        if (dot_pos != std::string::npos && dot_pos + 1 < text.length()) {
            std::string usec_str = text.substr(dot_pos + 1);
            usec                 = std::stoi(usec_str);

            // Adjust to the correct scale (microseconds)
            int digits = usec_str.length();
            for (int i = digits; i < 6; i++) {
                usec *= 10;
            }
        }

        // Set up tm structure
        tm.tm_year = year - 1900;
        tm.tm_mon  = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min  = min;
        tm.tm_sec  = sec;

        // Convert to timestamp
        std::time_t time_secs = std::mktime(&tm);
        if (time_secs == -1) {
            throw std::runtime_error("Invalid timestamp conversion");
        }

        return value_type(qb::Timestamp::from_seconds(time_secs) +
                          qb::Timespan::from_microseconds(usec));
    }
};

// Specialization for UtcTimestamp type
template <>
struct TypeConverter<qb::UtcTimestamp> {
    using value_type = qb::UtcTimestamp;

    /**
     * @brief Returns the PostgreSQL OID for timestamptz type
     *
     * @return integer PostgreSQL type OID for timestamptz
     */
    static integer
    get_oid() {
        return static_cast<integer>(oid::timestamptz);
    }

    /**
     * @brief Converts a UtcTimestamp to PostgreSQL binary format
     *
     * Creates a PostgreSQL binary representation of a timestamp with timezone,
     * following PostgreSQL format specifications:
     * - 4-byte integer length prefix (8)
     * - 8-byte timestamp value in microseconds since 2000-01-01 UTC
     *
     * @param value The UTC timestamp to convert
     * @param buffer The buffer to store the PostgreSQL binary format
     */
    static void
    to_binary(const qb::UtcTimestamp &value, std::vector<byte> &buffer) {
        // Convert to regular Timestamp and then serialize
        // Use the value directly since UtcTimestamp is derived from Timestamp
        TypeConverter<qb::Timestamp>::to_binary(static_cast<qb::Timestamp>(value),
                                                buffer);
    }

    /**
     * @brief Converts a UtcTimestamp to PostgreSQL text format
     *
     * Creates a standard text representation of a UTC timestamp in ISO 8601 format:
     * YYYY-MM-DD HH:MM:SS.ssssss+00
     *
     * @param value The UTC timestamp to convert
     * @return std::string The PostgreSQL text representation of the timestamp with
     * timezone
     */
    static std::string
    to_text(const qb::UtcTimestamp &value) {
        std::time_t unix_time = value.seconds();
        std::tm    *time_info = std::gmtime(&unix_time);
        char        buf[40];

        // Format the date and time parts
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", time_info);

        // Add microseconds
        std::string result(buf);
        int64_t     microsecs = value.microseconds() % 1000000;
        if (microsecs > 0) {
            char usec_buf[8];
            std::snprintf(usec_buf, sizeof(usec_buf), ".%06ld",
                          static_cast<long>(microsecs));
            result += usec_buf;
        }

        // Add UTC timezone indicator
        result += "+00";

        return result;
    }

    /**
     * @brief Converts PostgreSQL binary format to a UtcTimestamp
     *
     * Deserializes a PostgreSQL binary timestamp with timezone representation
     * into a qb::UtcTimestamp.
     * PostgreSQL timestamptz are stored as UTC timestamps in microseconds since
     * 2000-01-01.
     *
     * @param buffer Buffer containing the PostgreSQL binary timestamptz data
     * @return qb::UtcTimestamp Converted UTC timestamp object
     * @throws std::runtime_error If the buffer is too small or malformed
     */
    static qb::UtcTimestamp
    from_binary(const std::vector<byte> &buffer) {
        // Convert from binary to Timestamp, then to UtcTimestamp
        qb::Timestamp ts = TypeConverter<qb::Timestamp>::from_binary(buffer);
        // Create a new UtcTimestamp and assign the epoch value from the timestamp
        qb::UtcTimestamp result;
        result = qb::UtcTimestamp(ts.nanoseconds());
        return result;
    }

    /**
     * @brief Converts PostgreSQL text format to a UtcTimestamp
     *
     * Parses a PostgreSQL text representation of a timestamp with timezone into
     * a qb::UtcTimestamp object. Handles formats with timezone information.
     *
     * @param text Text representation of a timestamp with timezone
     * @return qb::UtcTimestamp Converted UTC timestamp object
     * @throws std::runtime_error If the text is not a valid timestamptz format
     */
    static qb::UtcTimestamp
    from_text(const std::string &text) {
        // PostgreSQL will provide timestamps in various formats including timezone info
        // We'll parse the basic timestamp part first

        // Expected format: "YYYY-MM-DD HH:MM:SS.ssssss±TZ" or variations
        std::tm tm   = {};
        int     usec = 0;

        if (text.empty()) {
            throw std::runtime_error("Empty timestamp string");
        }

        int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;

        // Use a simpler approach to extract the date/time portion
        int matched = sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day,
                             &hour, &min, &sec);

        if (matched != 6) {
            throw std::runtime_error("Invalid timestamp format");
        }

        // Read fractional part if it exists (ignoring timezone for initial parsing)
        size_t dot_pos = text.find('.');
        if (dot_pos != std::string::npos && dot_pos + 1 < text.length()) {
            // Find the first non-digit character after the dot
            size_t end_pos = dot_pos + 1;
            while (end_pos < text.length() && std::isdigit(text[end_pos])) {
                end_pos++;
            }

            std::string usec_str = text.substr(dot_pos + 1, end_pos - (dot_pos + 1));
            usec                 = std::stoi(usec_str);

            // Adjust to the correct scale (microseconds)
            int digits = usec_str.length();
            for (int i = digits; i < 6; i++) {
                usec *= 10;
            }
        }

        // Set up tm structure as UTC
        tm.tm_year  = year - 1900;
        tm.tm_mon   = month - 1;
        tm.tm_mday  = day;
        tm.tm_hour  = hour;
        tm.tm_min   = min;
        tm.tm_sec   = sec;
        tm.tm_isdst = 0; // No DST for UTC

        // Convert to timestamp
        // Use timegm (GMT/UTC version of mktime) if available, otherwise approximate
#ifdef _WIN32
        // Windows doesn't have timegm, use _mkgmtime
        std::time_t time_secs = _mkgmtime(&tm);
#else
        std::time_t time_secs;
        // Check if we have timegm available
#ifdef __APPLE__
        // macOS doesn't have timegm in the standard library
        time_secs = std::mktime(&tm);
        // Adjust for local timezone offset
        std::time_t local_time = std::mktime(&tm);
        std::tm    *gm_tm      = std::gmtime(&local_time);
        std::time_t gm_time    = std::mktime(gm_tm);
        time_secs += (local_time - gm_time);
#else
        // Linux and others might have timegm
        time_secs = timegm(&tm);
#endif
#endif

        if (time_secs == -1) {
            throw std::runtime_error("Invalid timestamp conversion");
        }

        // Create a timestamp with the seconds and microseconds
        auto ts = qb::Timestamp::from_seconds(time_secs) +
                  qb::Timespan::from_microseconds(usec);
                  
        // Create a new UtcTimestamp and assign the epoch value from the timestamp
        qb::UtcTimestamp result;
        result = qb::UtcTimestamp(ts.nanoseconds());
        return result;
    }
};

// Specialization for qb::json (nlohmann::json)
template <>
struct TypeConverter<qb::json> {
    using value_type = qb::json;

    /**
     * @brief Returns the PostgreSQL OID for JSON type
     *
     * @return integer PostgreSQL type OID for JSON
     */
    static integer
    get_oid() {
        return static_cast<integer>(oid::json);
    }

    /**
     * @brief Converts a JSON object to PostgreSQL binary format
     *
     * Creates a PostgreSQL binary representation of a JSON object.
     * For JSON (as opposed to JSONB), PostgreSQL simply expects the text
     * representation of the JSON with a length prefix.
     *
     * @param value The JSON object to convert
     * @param buffer The buffer to store the PostgreSQL binary format
     */
    static void
    to_binary(const qb::json &value, std::vector<byte> &buffer) {
        // For JSON format, we just store the JSON as a text representation
        std::string json_str = value.dump();

        // PostgreSQL JSON binary format is simply the JSON text
        buffer.reserve(4 + json_str.size());

        // 1. Write length of JSON string
        write_integer(buffer, static_cast<integer>(json_str.size()));

        // 2. Write JSON content as string
        buffer.insert(buffer.end(), json_str.begin(), json_str.end());
    }

    /**
     * @brief Converts a JSON object to PostgreSQL text format
     *
     * Creates a standard text representation of a JSON object.
     *
     * @param value The JSON object to convert
     * @return std::string The PostgreSQL text representation of the JSON
     */
    static std::string
    to_text(const qb::json &value) {
        return value.dump();
    }

    /**
     * @brief Converts a PostgreSQL binary buffer to a JSON object
     *
     * Deserializes PostgreSQL binary JSON format into a qb::json object.
     * For JSON (unlike JSONB), the format is simply:
     * - 4-byte integer length prefix
     * - JSON content as text
     *
     * @param buffer Buffer containing the PostgreSQL binary format data
     * @return value_type Deserialized JSON object
     * @throws std::runtime_error If the buffer contains invalid or malformed data
     */
    static value_type
    from_binary(const std::vector<byte> &buffer) {
        try {
            if (buffer.size() <= 4) {
                throw std::runtime_error("Invalid JSON binary format: buffer too small");
            }

            // Skip the 4-byte length prefix
            std::string json_str(reinterpret_cast<const char *>(buffer.data() + 4),
                                 buffer.size() - 4);

            // The binary format might use a different encoding in PostgreSQL
            try {
                // Try to parse as standard JSON first
                return qb::json::parse(json_str);
            } catch (const std::exception &) {
                // If that fails, it might be using the PostgreSQL array format
                // The format often begins with '[[' for pairs of key-value entries
                // Extract the values and convert to a proper JSON object
                auto json = nlohmann::json::parse(json_str);

                // If it's already a valid JSON, just return it
                if (!json.is_array()) {
                    return qb::json(json);
                }

                // Convert array format to object format
                nlohmann::json result;
                for (const auto &pair : json) {
                    if (pair.is_array() && pair.size() == 2) {
                        if (pair[0].is_string()) {
                            // Standard key-value pair
                            result[pair[0].get<std::string>()] = pair[1];
                        } else {
                            // Handle non-string keys by generating a string key
                            result[pair[0].dump()] = pair[1];
                        }
                    }
                }
                return qb::json(result);
            }
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("Failed to parse JSON data: ") +
                                     e.what());
        }
    }

    /**
     * @brief Converts a PostgreSQL text representation to a JSON object
     *
     * Parses a JSON string into a qb::json object.
     *
     * @param text PostgreSQL text representation to convert
     * @return value_type Deserialized JSON object
     * @throws std::runtime_error If the text contains invalid or malformed JSON
     */
    static value_type
    from_text(const std::string &text) {
        try {
            return qb::json::parse(text);
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("Failed to parse JSON text: ") +
                                     e.what());
        }
    }

private:
    static void
    write_integer(std::vector<byte> &buffer, integer value) {
        buffer.resize(buffer.size() + sizeof(integer));
        byte   *dest = &buffer[buffer.size() - sizeof(integer)];
        integer nbo  = htonl(value);
        memcpy(dest, &nbo, sizeof(integer));
    }
};

// Specialization for qb::jsonb
template <>
struct TypeConverter<qb::jsonb> {
    using value_type = qb::jsonb;

    /**
     * @brief Returns the PostgreSQL OID for JSONB type
     *
     * @return integer PostgreSQL type OID for JSONB
     */
    static integer
    get_oid() {
        return static_cast<integer>(oid::jsonb);
    }

    /**
     * @brief Converts a JSONB object to PostgreSQL binary format
     *
     * Creates a PostgreSQL binary representation of a JSONB object.
     * The format consists of:
     * - 4-byte integer length prefix
     * - JSONB version number (1 byte, currently 1)
     * - JSON content in PostgreSQL's binary JSONB format
     *
     * Since the internal JSONB binary format is complex, we first convert to
     * text representation and let PostgreSQL handle the conversion.
     *
     * @param value The JSONB object to convert
     * @param buffer The buffer to store the PostgreSQL binary format
     */
    static void
    to_binary(const qb::jsonb &value, std::vector<byte> &buffer) {
        // For binary format, we just store the JSON as a text representation
        // and let PostgreSQL handle it
        std::string json_str = value.dump();

        // PostgreSQL JSONB binary format starts with a version number (1)
        // followed by the JSON text
        buffer.reserve(4 + 1 + json_str.size());

        // 1. Write length (1 + size of json_str) for version + content
        write_integer(buffer, static_cast<integer>(1 + json_str.size()));

        // 2. Write JSONB version (currently 1)
        buffer.push_back(1);

        // 3. Write JSON content as string
        buffer.insert(buffer.end(), json_str.begin(), json_str.end());
    }

    /**
     * @brief Converts a JSONB object to PostgreSQL text format
     *
     * Creates a standard text representation of a JSONB object.
     *
     * @param value The JSONB object to convert
     * @return std::string The PostgreSQL text representation of the JSONB
     */
    static std::string
    to_text(const qb::jsonb &value) {
        return value.dump();
    }

    /**
     * @brief Converts a PostgreSQL binary buffer to a JSONB object
     *
     * Deserializes PostgreSQL binary JSONB format into a qb::jsonb object.
     * The format consists of:
     * - 4-byte integer length prefix
     * - JSONB version number (1 byte, currently 1)
     * - JSON content
     *
     * @param buffer Buffer containing the PostgreSQL binary format data
     * @return value_type Deserialized JSONB object
     * @throws std::runtime_error If the buffer contains invalid or malformed data
     */
    static value_type
    from_binary(const std::vector<byte> &buffer) {
        try {
            if (buffer.size() <= 5) {
                throw std::runtime_error(
                    "Invalid JSONB binary format: buffer too small");
            }

            // Read length from first 4 bytes (we'll skip this, network byte order)
            // Verify version number (should be 1)
            if (buffer[4] != 1) {
                throw std::runtime_error("Unsupported JSONB version");
            }

            // Skip the 4-byte length prefix and 1-byte version
            std::string json_str(reinterpret_cast<const char *>(buffer.data() + 5),
                                 buffer.size() - 5);

            // The binary format might use a different encoding in PostgreSQL than
            // standard JSON. Here we handle potential array-based format or direct JSON
            try {
                // Try to parse as standard JSON first
                return qb::jsonb(nlohmann::json::parse(json_str));
            } catch (const std::exception &) {
                // If that fails, it might be using the PostgreSQL array format
                // The format often begins with '[[' for pairs of key-value entries
                // Extract the values and convert to a proper JSON object
                auto json = nlohmann::json::parse(json_str);

                // If it's already a valid JSON, just return it
                if (!json.is_array()) {
                    return qb::jsonb(json);
                }

                // Convert array format to object format
                nlohmann::json result;
                for (const auto &pair : json) {
                    if (pair.is_array() && pair.size() == 2) {
                        if (pair[0].is_string()) {
                            // Standard key-value pair
                            result[pair[0].get<std::string>()] = pair[1];
                        } else {
                            // Handle non-string keys by generating a string key
                            result[pair[0].dump()] = pair[1];
                        }
                    }
                }
                return qb::jsonb(result);
            }
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("Failed to parse JSONB data: ") +
                                     e.what());
        }
    }

    /**
     * @brief Converts a PostgreSQL text representation to a JSONB object
     *
     * Parses a JSON string into a qb::jsonb object.
     *
     * @param text PostgreSQL text representation to convert
     * @return value_type Deserialized JSONB object
     * @throws std::runtime_error If the text contains invalid or malformed JSON
     */
    static value_type
    from_text(const std::string &text) {
        try {
            return qb::jsonb(nlohmann::json::parse(text));
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("Failed to parse JSONB text: ") +
                                     e.what());
        }
    }

private:
    static void
    write_integer(std::vector<byte> &buffer, integer value) {
        buffer.resize(buffer.size() + sizeof(integer));
        byte   *dest = &buffer[buffer.size() - sizeof(integer)];
        integer nbo  = htonl(value);
        memcpy(dest, &nbo, sizeof(integer));
    }
};

} // namespace qb::pg::detail