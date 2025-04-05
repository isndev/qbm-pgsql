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

#include <boost/endian/conversion.hpp>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <netinet/in.h>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <qb/system/timestamp.h>
#include <qb/uuid.h>

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
                buffer.insert(buffer.end(), reinterpret_cast<const byte *>(value.data()),
                              reinterpret_cast<const byte *>(value.data() + value.size()));
            }
        } else if constexpr (std::is_same_v<value_type, bool>) {
            // PostgreSQL boolean: length (1) + value (0/1)
            write_integer(buffer, 1);
            buffer.push_back(value ? 1 : 0);
        } else if constexpr (std::is_same_v<value_type, smallint>) {
            // PostgreSQL smallint: length (2) + network value
            write_integer(buffer, 2);
            smallint netval = htons(value);
            const byte *bytes = reinterpret_cast<const byte *>(&netval);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(smallint));
        } else if constexpr (std::is_same_v<value_type, integer>) {
            // PostgreSQL integer: length (4) + network value
            write_integer(buffer, 4);
            integer netval = htonl(value);
            const byte *bytes = reinterpret_cast<const byte *>(&netval);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(integer));
        } else if constexpr (std::is_same_v<value_type, bigint>) {
            // PostgreSQL bigint: length (8) + network value (manual swap)
            write_integer(buffer, 8);

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

            buffer.insert(buffer.end(), dst.b, dst.b + sizeof(bigint));
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
            const auto &uuid_bytes = value.as_bytes();
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
            int64_t unix_seconds = value.seconds();

            // Convert to PostgreSQL timestamp (microseconds since 2000-01-01)
            int64_t pg_timestamp = (unix_seconds - POSTGRES_EPOCH_DIFF_SECONDS) * 1000000 +
                                   (value.microseconds() % 1000000);

            // Convert to network byte order
            union {
                int64_t i;
                byte b[8];
            } src, dst;

            src.i = pg_timestamp;
            dst.b[0] = src.b[7];
            dst.b[1] = src.b[6];
            dst.b[2] = src.b[5];
            dst.b[3] = src.b[4];
            dst.b[4] = src.b[3];
            dst.b[5] = src.b[2];
            dst.b[6] = src.b[1];
            dst.b[7] = src.b[0];

            buffer.insert(buffer.end(), dst.b, dst.b + sizeof(int64_t));
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
            char hex[3];

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
            std::tm *tm_data = std::gmtime(&timestamp);

            std::ostringstream os;
            os << std::setfill('0') << std::setw(4) << (tm_data->tm_year + 1900) << '-'
               << std::setw(2) << (tm_data->tm_mon + 1) << '-' << std::setw(2) << tm_data->tm_mday
               << ' ' << std::setw(2) << tm_data->tm_hour << ':' << std::setw(2) << tm_data->tm_min
               << ':' << std::setw(2) << tm_data->tm_sec << '.' << std::setw(6)
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
                throw std::runtime_error("Invalid timestamp binary data size");
            }

            // Read the 8-byte int64 from the buffer (microseconds since 2000-01-01)
            int64_t pg_timestamp = 0;
            union {
                int64_t i;
                byte b[8];
            } val;

            // Convert from network byte order
            val.b[0] = buffer[7];
            val.b[1] = buffer[6];
            val.b[2] = buffer[5];
            val.b[3] = buffer[4];
            val.b[4] = buffer[3];
            val.b[5] = buffer[2];
            val.b[6] = buffer[1];
            val.b[7] = buffer[0];

            pg_timestamp = val.i;

            // Convert PostgreSQL timestamp to Unix timestamp
            // PostgreSQL epoch is 2000-01-01, Unix epoch is 1970-01-01
            // Difference is 30 years = 946684800 seconds
            constexpr int64_t POSTGRES_EPOCH_DIFF_SECONDS = 946684800;

            // Extract seconds and microseconds
            int64_t seconds = (pg_timestamp / 1000000) + POSTGRES_EPOCH_DIFF_SECONDS;
            int64_t microseconds = pg_timestamp % 1000000;
            if (microseconds < 0) {
                seconds--;
                microseconds += 1000000;
            }

            // Create the timestamp from the components
            return value_type(qb::Timestamp::seconds(seconds) +
                              qb::Timespan::microseconds(microseconds));
        } else if constexpr (detail::ParamUnserializer::is_optional<value_type>::value) {
            using inner_type = typename value_type::value_type;

            if (buffer.empty() ||
                (buffer.size() >= 4 && *reinterpret_cast<const integer *>(buffer.data()) == -1)) {
                return std::nullopt;
            }

            return TypeConverter<inner_type>::from_binary(buffer);
        } else {
            static_assert(sizeof(T) > 0, "Type not supported for conversion from binary");
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
            return (text == "t" || text == "true" || text == "1" || text == "yes" || text == "y" ||
                    text == "on");
        } else if constexpr (std::is_same_v<value_type, bytea> ||
                             std::is_same_v<value_type, std::vector<byte>>) {
            value_type result;

            // Hexadecimal format (\x...)
            if (text.length() >= 2 && text.substr(0, 2) == "\\x") {
                std::string hex = text.substr(2);
                for (size_t i = 0; i + 1 < hex.length(); i += 2) {
                    byte byte_val = static_cast<byte>(std::stoi(hex.substr(i, 2), nullptr, 16));
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
            std::tm tm = {};
            int year, month, day, hour, minute, second, microsecond = 0;

            // Regular expression to match PostgreSQL timestamp format
            std::regex timestamp_regex(
                R"((\d{4})-(\d{1,2})-(\d{1,2})\s+(\d{1,2}):(\d{1,2}):(\d{1,2})(?:\.(\d{1,6}))?)");

            std::smatch matches;
            if (std::regex_match(text, matches, timestamp_regex)) {
                year = std::stoi(matches[1]);
                month = std::stoi(matches[2]);
                day = std::stoi(matches[3]);
                hour = std::stoi(matches[4]);
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
                tm.tm_year = year - 1900; // Years since 1900
                tm.tm_mon = month - 1;    // Months since January (0-11)
                tm.tm_mday = day;         // Day of month (1-31)
                tm.tm_hour = hour;        // Hours (0-23)
                tm.tm_min = minute;       // Minutes (0-59)
                tm.tm_sec = second;       // Seconds (0-61)
                tm.tm_isdst = -1;         // Let system determine DST

                // Convert to Unix timestamp
                std::time_t unix_timestamp = std::mktime(&tm);

                // Create timestamp from seconds and microseconds
                return value_type(qb::Timestamp::seconds(unix_timestamp) +
                                  qb::Timespan::microseconds(microsecond));
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

private:
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
        integer networkValue = htonl(value);
        const byte *bytes = reinterpret_cast<const byte *>(&networkValue);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(integer));
    }
};

// Specialization for UUID type
template <>
struct TypeConverter<qb::uuid> {
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
        if (buffer.size() < 4 + 8) { // 4 bytes length + 8 bytes timestamp
            throw std::runtime_error("Buffer too small for timestamp");
        }

        // Difference between PostgreSQL epoch (2000-01-01) and Unix epoch (1970-01-01)
        constexpr int64_t POSTGRES_EPOCH_DIFF = 946684800LL; // seconds

        // Assemble timestamp value (8 bytes in big-endian)
        union {
            int64_t i;
            byte b[8];
        } src, dst;

        // If buffer contains EXACTLY the timestamp (8 bytes), use it directly
        if (buffer.size() == 8) {
            std::memcpy(src.b, buffer.data(), 8);
        } else {
            // Otherwise, assume there's a 4-byte prefix
            std::memcpy(src.b, buffer.data() + 4, 8);
        }

        // Convert big-endian to native order
        dst.b[0] = src.b[7];
        dst.b[1] = src.b[6];
        dst.b[2] = src.b[5];
        dst.b[3] = src.b[4];
        dst.b[4] = src.b[3];
        dst.b[5] = src.b[2];
        dst.b[6] = src.b[1];
        dst.b[7] = src.b[0];

        int64_t pg_usecs = dst.i;

        // Convert to Unix seconds and microseconds
        int64_t unix_secs = (pg_usecs / 1000000) + POSTGRES_EPOCH_DIFF;
        int64_t unix_usecs = pg_usecs % 1000000;

        if (unix_usecs < 0) {
            unix_secs--;
            unix_usecs += 1000000;
        }

        return qb::Timestamp::seconds(unix_secs) + qb::Timespan::microseconds(unix_usecs);
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
        std::tm tm = {};
        int usec = 0;

        // Parse date and time
        if (text.empty()) {
            throw std::runtime_error("Empty timestamp string");
        }

        int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;

        // Use sscanf which is more tolerant of formats
        int matched =
            sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &min, &sec);

        if (matched != 6) {
            throw std::runtime_error("Invalid timestamp format");
        }

        // Read fractional part if it exists
        size_t dot_pos = text.find('.');
        if (dot_pos != std::string::npos && dot_pos + 1 < text.length()) {
            std::string usec_str = text.substr(dot_pos + 1);
            usec = std::stoi(usec_str);

            // Adjust to the correct scale (microseconds)
            int digits = usec_str.length();
            for (int i = digits; i < 6; i++) {
                usec *= 10;
            }
        }

        // Set up tm structure
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;

        // Convert to timestamp
        std::time_t time_secs = std::mktime(&tm);
        if (time_secs == -1) {
            throw std::runtime_error("Invalid timestamp conversion");
        }

        return qb::Timestamp::seconds(time_secs) + qb::Timespan::microseconds(usec);
    }
};

} // namespace qb::pg::detail