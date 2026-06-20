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
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
#include <optional>
#include <qb/io.h>
#include <qb/system/endian.h>
#include <qb/system/time.h> // qb::safe_gmtime / safe_timegm / civil date helpers
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
 * @brief Reentrant, range-checked replacements for std::gmtime / std::localtime.
 *
 * std::gmtime / std::localtime return a pointer into a process-wide static
 * std::tm — a data race when more than one qb VirtualCore formats a timestamp
 * concurrently — and return nullptr for an out-of-range time_t (whereupon the
 * historical callers dereferenced it or passed it to strftime/mktime, i.e. UB
 * / crash). These wrappers use the thread-safe *_r / *_s variants and report
 * failure via the return value.
 */
[[nodiscard]] inline bool
safe_gmtime(std::time_t t, std::tm &out) noexcept {
    // Portable, thread-safe UTC breakdown using pure integer arithmetic
    // (Howard Hinnant's civil-from-days algorithm). We deliberately do NOT
    // delegate to the platform gmtime_s / gmtime_r: the Windows CRT gmtime_s
    // rejects any negative time_t, so every instant before 1970-01-01 would
    // fail there (while POSIX gmtime_r accepts it) — a cross-platform
    // divergence for PostgreSQL TIMESTAMP/DATE values before the Unix epoch.
    // UTC has no timezone/DST state, so the integer computation is exact.
    return qb::safe_gmtime(t, out); // canonical impl in <qb/system/time.h>
}

[[nodiscard]] inline bool
safe_localtime(std::time_t t, std::tm &out) noexcept {
    return qb::safe_localtime(t, out); // canonical impl in <qb/system/time.h>
}

/**
 * @brief Portable UTC `std::tm` -> `time_t` (inverse of @ref safe_gmtime).
 *
 * Pure integer arithmetic (Howard Hinnant's days_from_civil). Used instead of
 * `_mkgmtime` / `timegm`: the Windows CRT `_mkgmtime` returns -1 for any instant
 * before 1970-01-01, so PostgreSQL TIMESTAMP values before the Unix epoch would
 * fail to parse on Windows while working on POSIX. UTC has no DST/timezone
 * state, so the computation is exact and cannot fail for in-range input.
 */
[[nodiscard]] inline std::time_t
safe_timegm(const std::tm &in) noexcept {
    return qb::safe_timegm(in); // canonical impl in <qb/system/time.h>
}

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
        if constexpr (std::is_same_v<value_type, std::string> || std::is_same_v<value_type, std::string_view>) {
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
            // PostgreSQL float4: length (4) + big-endian IEEE 754
            write_integer(buffer, 4);
            uint32_t raw;
            std::memcpy(&raw, &value, sizeof(float));
            uint32_t    be    = qb::endian::to_big_endian(raw);
            const byte *bytes = reinterpret_cast<const byte *>(&be);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(float));
        } else if constexpr (std::is_same_v<value_type, double>) {
            // PostgreSQL float8: length (8) + big-endian IEEE 754
            write_integer(buffer, 8);
            uint64_t raw;
            std::memcpy(&raw, &value, sizeof(double));
            uint64_t    be    = qb::endian::to_big_endian(raw);
            const byte *bytes = reinterpret_cast<const byte *>(&be);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(double));
        } else if constexpr (std::is_same_v<value_type, bytea> || std::is_same_v<value_type, std::vector<byte>>) {
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
        } else if constexpr (std::is_same_v<value_type, qb::wall_time>) {
            // PostgreSQL timestamptz: length (8) + microseconds since 2000-01-01 UTC
            write_integer(buffer, 8);

            // PostgreSQL epoch is 2000-01-01, Unix epoch is 1970-01-01
            // Difference is 30 years = 946684800 seconds
            constexpr int64_t POSTGRES_EPOCH_DIFF_SECONDS = 946684800;

            // Exact integer micros since the PostgreSQL epoch (no double rounding).
            int64_t pg_timestamp = qb::unix_micros(value) - POSTGRES_EPOCH_DIFF_SECONDS * 1000000LL;

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
        if constexpr (std::is_same_v<value_type, std::string> || std::is_same_v<value_type, std::string_view>) {
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
        } else if constexpr (std::is_same_v<value_type, bytea> || std::is_same_v<value_type, std::vector<byte>>) {
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
        } else if constexpr (std::is_same_v<value_type, qb::wall_time>) {
            // Format: YYYY-MM-DD HH:MM:SS.MMMMMM (UTC). Floored seconds/fraction
            // split: system_clock is signed, so a pre-1970 instant with a
            // sub-second part must borrow a second rather than truncate toward
            // zero (else the seconds field is one too high).
            const int64_t total_us  = qb::unix_micros(value);
            int64_t       whole_sec = total_us / 1000000;
            int64_t       frac      = total_us % 1000000;
            if (frac < 0) {
                frac += 1000000;
                --whole_sec;
            }
            std::time_t timestamp = static_cast<std::time_t>(whole_sec);
            std::tm     tm_data{};
            if (!safe_gmtime(timestamp, tm_data))
                throw error::client_error("timestamp out of range for text conversion");

            std::ostringstream os;
            os << std::setfill('0') << std::setw(4) << (tm_data.tm_year + 1900) << '-' << std::setw(2) << (tm_data.tm_mon + 1) << '-'
               << std::setw(2) << tm_data.tm_mday << ' ' << std::setw(2) << tm_data.tm_hour << ':' << std::setw(2) << tm_data.tm_min << ':'
               << std::setw(2) << tm_data.tm_sec << '.' << std::setw(6) << frac;

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
            // int2: 2-byte BE; int4: 4-byte BE; COUNT(*) and other aggregates: int8 (8-byte).
            if (buffer.size() == sizeof(smallint)) {
                return static_cast<integer>(unserializer.read_smallint(buffer));
            }
            if (buffer.size() == sizeof(bigint)) {
                const bigint wide = unserializer.read_bigint(buffer);
                if (wide > static_cast<bigint>(std::numeric_limits<integer>::max())
                    || wide < static_cast<bigint>(std::numeric_limits<integer>::min())) {
                    throw std::runtime_error("Integer value out of range for int32");
                }
                return static_cast<integer>(wide);
            }
            return unserializer.read_integer(buffer);
        } else if constexpr (std::is_same_v<value_type, bigint>) {
            return unserializer.read_bigint(buffer);
        } else if constexpr (std::is_same_v<value_type, float>) {
            return unserializer.read_float(buffer);
        } else if constexpr (std::is_same_v<value_type, double>) {
            return unserializer.read_double(buffer);
        } else if constexpr (std::is_same_v<value_type, bool>) {
            // PostgreSQL binary boolean: exactly 1 raw byte (0 = false, non-zero = true).
            // The field value buffer never contains a length prefix — that is stripped
            // by the protocol layer before this function is called.
            if (buffer.empty()) {
                throw std::runtime_error("Empty buffer for boolean value");
            }
            return buffer[0] != 0;
        } else if constexpr (std::is_same_v<value_type, bytea> || std::is_same_v<value_type, std::vector<byte>>) {
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
        } else if constexpr (std::is_same_v<value_type, qb::wall_time>) {
            if (buffer.size() < 8) {
                throw std::runtime_error("Buffer too small for timestamp");
            }

            // Difference between PostgreSQL epoch (2000-01-01) and Unix epoch
            // (1970-01-01)
            constexpr int64_t POSTGRES_EPOCH_DIFF = 946684800LL; // seconds

            // Create a temporary variable to hold the timestamp
            int64_t pg_usecs = 0;

            // The field value is normally EXACTLY 8 bytes — the protocol layer
            // strips any length prefix. A legacy/defensive path also accepts a
            // 4-byte prefix (>= 12 bytes total). A size of 9..11 would make the
            // prefixed read run 1..3 bytes past the buffer on attacker-controlled
            // col_size, so reject it rather than read out of bounds.
            if (buffer.size() == 8) {
                std::memcpy(&pg_usecs, buffer.data(), 8);
            } else if (buffer.size() >= 12) {
                std::memcpy(&pg_usecs, buffer.data() + 4, 8);
            } else {
                throw std::runtime_error("Malformed timestamp buffer size");
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

            return qb::wall_from_unix_seconds(unix_secs) + std::chrono::microseconds(unix_usecs);
        } else if constexpr (detail::ParamUnserializer::is_optional<value_type>::value) {
            using inner_type = typename value_type::value_type;

            // A 4-byte all-ones length prefix is PostgreSQL's NULL sentinel.
            // Read it with memcpy rather than a reinterpret_cast through the
            // byte pointer, which is an unaligned load + strict-aliasing UB.
            if (buffer.empty() || [&] {
                    if (buffer.size() < 4)
                        return false;
                    integer len_prefix = 0;
                    std::memcpy(&len_prefix, buffer.data(), sizeof(len_prefix));
                    return len_prefix == -1;
                }()) {
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
            try {
                return static_cast<smallint>(std::stoi(text));
            } catch (const std::exception &e) {
                throw error::client_error("Cannot parse smallint from text: " + text + " (" + e.what() + ")");
            }
        } else if constexpr (std::is_same_v<value_type, integer>) {
            try {
                return static_cast<integer>(std::stoi(text));
            } catch (const std::exception &e) {
                throw error::client_error("Cannot parse integer from text: " + text + " (" + e.what() + ")");
            }
        } else if constexpr (std::is_same_v<value_type, bigint>) {
            try {
                return static_cast<bigint>(std::stoll(text));
            } catch (const std::exception &e) {
                throw error::client_error("Cannot parse bigint from text: " + text + " (" + e.what() + ")");
            }
        } else if constexpr (std::is_same_v<value_type, float>) {
            // Special values
            if (text == "NaN")
                return std::numeric_limits<float>::quiet_NaN();
            if (text == "Infinity" || text == "inf")
                return std::numeric_limits<float>::infinity();
            if (text == "-Infinity" || text == "-inf")
                return -std::numeric_limits<float>::infinity();
            try {
                return std::stof(text);
            } catch (const std::exception &e) {
                throw error::client_error("Cannot parse float from text: " + text + " (" + e.what() + ")");
            }
        } else if constexpr (std::is_same_v<value_type, double>) {
            // Special values
            if (text == "NaN")
                return std::numeric_limits<double>::quiet_NaN();
            if (text == "Infinity" || text == "inf")
                return std::numeric_limits<double>::infinity();
            if (text == "-Infinity" || text == "-inf")
                return -std::numeric_limits<double>::infinity();
            try {
                return std::strtod(text.c_str(), nullptr);
            } catch (const std::exception &e) {
                throw error::client_error("Cannot parse double from text: " + text + " (" + e.what() + ")");
            }
        } else if constexpr (std::is_same_v<value_type, bool>) {
            return (text == "t" || text == "true" || text == "1" || text == "yes" || text == "y" || text == "on");
        } else if constexpr (std::is_same_v<value_type, bytea> || std::is_same_v<value_type, std::vector<byte>>) {
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
        } else if constexpr (std::is_same_v<value_type, qb::wall_time>) {
            // Parse PostgreSQL timestamp format: YYYY-MM-DD HH:MM:SS[.MMMMMM]
            std::tm tm = {};
            int     year, month, day, hour, minute, second, microsecond = 0;

            // Regular expression to match PostgreSQL timestamp / timestamptz text.
            // The optional trailing timezone (e.g. "+00", "+02:00", "-0530") is
            // tolerated so timestamptz values parse via std::regex_match.
            std::regex timestamp_regex(
                R"((\d{4})-(\d{1,2})-(\d{1,2})\s+(\d{1,2}):(\d{1,2}):(\d{1,2})(?:\.(\d{1,6}))?(?:[+-]\d{2}(?::?\d{2})?)?)");

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
                tm.tm_isdst = 0;           // UTC-native: no DST

                // Convert the broken-down UTC time to an epoch (UTC-native, portable:
                // handles instants before 1970 that the Windows CRT _mkgmtime rejects).
                std::time_t unix_timestamp = safe_timegm(tm);

                // Create timestamp from seconds and microseconds
                return qb::wall_from_unix_seconds(unix_timestamp) + std::chrono::microseconds(microsecond);
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
        // PostgreSQL UUID binary format: 4-byte length prefix (16) + 16 raw bytes
        write_integer(buffer, 16);
        const auto &uuid_bytes = value.as_bytes();
        for (size_t i = 0; i < 16; ++i) {
            buffer.push_back(static_cast<byte>(uuid_bytes[i]));
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
        // Wire / typreceive: 16 raw bytes (uuid_recv). Some paths include a 4-byte
        // length prefix before the 16 bytes — check the unambiguous case first.
        std::array<uint8_t, 16> uuid_bytes;

        if (buffer.size() == 16) {
            for (size_t i = 0; i < 16; ++i) {
                uuid_bytes[i] = static_cast<uint8_t>(buffer[i]);
            }
            return qb::uuid(uuid_bytes);
        }

        if (buffer.size() < 4 + 16) {
            throw std::runtime_error("Buffer too small for UUID");
        }

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
        auto uuid_result = qb::uuid::from_string(text);
        if (!uuid_result) {
            throw std::runtime_error("Invalid UUID format");
        }
        return uuid_result.value();
    }

private:
    static void
    write_integer(std::vector<byte> &buffer, integer value) {
        integer     nbo   = htonl(value);
        const byte *bytes = reinterpret_cast<const byte *>(&nbo);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(integer));
    }
};

// Specialization for the canonical UTC instant (qb::wall_time == system_clock).
// Maps to PostgreSQL timestamptz; reads of both timestamp and timestamptz columns
// decode into wall_time (identical micros-since-2000 wire representation).
template <>
struct TypeConverter<qb::wall_time> {
    using value_type = qb::wall_time;

    /**
     * @brief Returns the PostgreSQL OID for the timestamptz type
     *
     * @return integer PostgreSQL type OID for timestamptz
     */
    static integer
    get_oid() {
        return static_cast<integer>(oid::timestamptz);
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
    to_binary(const qb::wall_time &value, std::vector<byte> &buffer) {
        // PostgreSQL timestamptz binary: 4-byte length (8) + 8-byte microseconds
        // since 2000-01-01 UTC in big-endian.

        // Difference between PostgreSQL epoch (2000-01-01) and Unix epoch (1970-01-01)
        constexpr int64_t POSTGRES_EPOCH_DIFF = 946684800LL;

        // Exact integer micros since the PostgreSQL epoch (no double rounding).
        const int64_t pg_usecs = qb::unix_micros(value) - POSTGRES_EPOCH_DIFF * 1000000LL;

        write_integer(buffer, 8);
        int64_t     be    = qb::endian::to_big_endian(pg_usecs);
        const byte *bytes = reinterpret_cast<const byte *>(&be);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(int64_t));
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
    to_text(const qb::wall_time &value) {
        // Floored seconds/fraction split (system_clock is signed): a pre-1970
        // sub-second instant must borrow a second, not truncate toward zero.
        const int64_t total_us  = qb::unix_micros(value);
        int64_t       whole_sec = total_us / 1000000;
        int64_t       microsecs = total_us % 1000000;
        if (microsecs < 0) {
            microsecs += 1000000;
            --whole_sec;
        }
        std::time_t unix_time = static_cast<std::time_t>(whole_sec);
        std::tm     time_info{};
        if (!safe_gmtime(unix_time, time_info))
            throw error::client_error("timestamp out of range for text conversion");
        char buf[40];

        // Format the date and time parts (UTC)
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &time_info);

        // Add microseconds (already normalized to [0, 1e6) above)
        std::string result(buf);
        if (microsecs > 0) {
            char usec_buf[8];
            std::snprintf(usec_buf, sizeof(usec_buf), ".%06ld", static_cast<long>(microsecs));
            result += usec_buf;
        }

        result += "+00";
        return result;
    }

    /**
     * @brief Converts PostgreSQL binary format to a Timestamp
     *
     * Deserializes a PostgreSQL binary timestamp representation into a qb::wall_time.
     * PostgreSQL timestamps are stored as microseconds since 2000-01-01, while
     * Unix/C++ timestamps are seconds since 1970-01-01.
     *
     * This method handles both:
     * - Raw 8-byte timestamp without length prefix
     * - Standard PostgreSQL binary format with 4-byte length prefix
     *
     * @param buffer Buffer containing the PostgreSQL binary timestamp data
     * @return qb::wall_time Converted timestamp object
     * @throws std::runtime_error If the buffer is too small or malformed
     */
    static qb::wall_time
    from_binary(const std::vector<byte> &buffer) {
        // PostgreSQL timestamp is in microseconds since 2000-01-01
        if (buffer.size() < 8) { // at least 8 bytes for timestamp
            throw std::runtime_error("Buffer too small for timestamp");
        }

        // Difference between PostgreSQL epoch (2000-01-01) and Unix epoch (1970-01-01)
        constexpr int64_t POSTGRES_EPOCH_DIFF = 946684800LL; // seconds

        // Create a temporary variable to hold the timestamp
        int64_t pg_usecs = 0;

        // The field value is normally EXACTLY 8 bytes — the protocol layer strips
        // any length prefix. A legacy/defensive path also accepts a 4-byte prefix
        // (>= 12 bytes total). A size of 9..11 would make the prefixed read run
        // 1..3 bytes past the buffer on attacker-controlled col_size, so reject it
        // rather than read out of bounds.
        if (buffer.size() == 8) {
            std::memcpy(&pg_usecs, buffer.data(), 8);
        } else if (buffer.size() >= 12) {
            std::memcpy(&pg_usecs, buffer.data() + 4, 8);
        } else {
            throw std::runtime_error("Malformed timestamp buffer size");
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

        return qb::wall_from_unix_seconds(unix_secs) + std::chrono::microseconds(unix_usecs);
    }

    /**
     * @brief Converts PostgreSQL text format to a Timestamp
     *
     * Parses a PostgreSQL text representation of a timestamp into a qb::wall_time
     * object. Handles the standard PostgreSQL timestamp format: "YYYY-MM-DD
     * HH:MM:SS.ssssss"
     *
     * @param text Text representation of a timestamp
     * @return qb::wall_time Converted timestamp object
     * @throws std::runtime_error If the text is not a valid timestamp format
     */
    static qb::wall_time
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
        int matched = sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &min, &sec);

        if (matched != 6) {
            throw std::runtime_error("Invalid timestamp format");
        }

        // Read fractional part if it exists. timestamptz text carries a trailing
        // timezone (e.g. ".789+00"), so the fractional digits must be delimited at
        // the first non-digit — not the rest of the string — otherwise the digit
        // count is wrong and the value is mis-scaled.
        size_t dot_pos = text.find('.');
        if (dot_pos != std::string::npos && dot_pos + 1 < text.length()) {
            size_t end_pos = dot_pos + 1;
            while (end_pos < text.length() && std::isdigit(static_cast<unsigned char>(text[end_pos]))) {
                end_pos++;
            }
            std::string usec_str = text.substr(dot_pos + 1, end_pos - (dot_pos + 1));
            usec                 = std::stoi(usec_str);

            // Adjust to the correct scale (microseconds)
            int digits = static_cast<int>(usec_str.length());
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

        // Convert the broken-down UTC time to an epoch (UTC-native, portable:
        // handles pre-1970 instants that the Windows CRT _mkgmtime rejects).
        tm.tm_isdst = 0;
        std::time_t time_secs = safe_timegm(tm);

        return qb::wall_from_unix_seconds(time_secs) + std::chrono::microseconds(usec);
    }

private:
    static void
    write_integer(std::vector<byte> &buffer, integer value) {
        integer     nbo   = htonl(value);
        const byte *bytes = reinterpret_cast<const byte *>(&nbo);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(integer));
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
            std::string json_str(reinterpret_cast<const char *>(buffer.data() + 4), buffer.size() - 4);

            // OPTIMIZED: Single parse with format detection (P0-10 fix)
            // Previously called parse TWICE on error path - wasteful CPU usage
            // Parse once, then check if it's array format that needs conversion
            auto json = nlohmann::json::parse(json_str);

            // If it's not an array, return as-is (standard JSON)
            if (!json.is_array()) {
                return qb::json(json);
            }

            // Check if it might be PostgreSQL array format with key-value pairs
            // The format often begins with '[[' for pairs of key-value entries
            bool is_key_value_format = false;
            if (!json.empty() && json[0].is_array() && json[0].size() == 2) {
                is_key_value_format = true;
            }

            if (!is_key_value_format) {
                // Regular array, return as-is
                return qb::json(json);
            }

            // Convert key-value array format to object format
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
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("Failed to parse JSON data: ") + e.what());
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
            throw std::runtime_error(std::string("Failed to parse JSON text: ") + e.what());
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
        // ParamSerializer expects typbinary layout: Int32 byte length of payload,
        // then jsonb_recv payload: version 1 + UTF-8 JSON text (see jsonb_send/recv).
        std::string json_str = value.dump();
        buffer.reserve(sizeof(integer) + 1 + json_str.size());
        write_integer(buffer, static_cast<integer>(1 + json_str.size()));
        buffer.push_back(static_cast<byte>(1));
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
     * Accepts jsonb_send payload: version byte 1 then UTF-8 JSON, or a 4-byte
     * varlena-style header before that version byte (see jsonb.c).
     *
     * @param buffer Buffer containing the PostgreSQL binary format data
     * @return value_type Deserialized JSONB object
     * @throws std::runtime_error If the buffer contains invalid or malformed data
     */
    static value_type
    from_binary(const std::vector<byte> &buffer) {
        try {
            if (buffer.size() < 2) {
                throw std::runtime_error("Invalid JSONB binary format: buffer too small");
            }

            // Result values are typically jsonb_send's bytea payload (VARDATA): version
            // byte 1 then UTF-8 JSON. Some stacks may prefix a 4-byte varlena header.
            std::size_t json_off;
            if (static_cast<unsigned char>(buffer[0]) == 1U) {
                json_off = 1;
            } else if (buffer.size() >= 5 && static_cast<unsigned char>(buffer[4]) == 1U) {
                json_off = 5;
            } else {
                throw std::runtime_error("Unsupported JSONB binary format or version");
            }

            std::string json_str(reinterpret_cast<const char *>(buffer.data() + json_off), buffer.size() - json_off);

            // OPTIMIZED: Single parse with format detection (P0-10 fix)
            // Previously called parse TWICE on error path - wasteful CPU usage
            // Parse once, then check if it's array format that needs conversion
            auto json = nlohmann::json::parse(json_str);

            // If it's not an array, return as-is (standard JSON)
            if (!json.is_array()) {
                return qb::jsonb(json);
            }

            // Check if it might be PostgreSQL array format with key-value pairs
            // The format often begins with '[[' for pairs of key-value entries
            bool is_key_value_format = false;
            if (!json.empty() && json[0].is_array() && json[0].size() == 2) {
                is_key_value_format = true;
            }

            if (!is_key_value_format) {
                // Regular array, return as-is
                return qb::jsonb(json);
            }

            // Convert key-value array format to object format
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
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("Failed to parse JSONB data: ") + e.what());
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
            throw std::runtime_error(std::string("Failed to parse JSONB text: ") + e.what());
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

// ============================================================================
// P2-2: Additional PostgreSQL type specializations
// ============================================================================

/**
 * @brief Type converter for std::chrono::duration as PostgreSQL INTERVAL
 *
 * PostgreSQL stores intervals as 16 bytes:
 * - 8 bytes: time in microseconds (int64)
 * - 4 bytes: days (int32)
 * - 4 bytes: months (int32)
 */
template <typename Rep, typename Period>
struct TypeConverter<std::chrono::duration<Rep, Period>> {
    using value_type = std::chrono::duration<Rep, Period>;

    static integer
    get_oid() {
        return static_cast<integer>(oid::interval);
    }

    static void
    to_binary(const value_type &value, std::vector<byte> &buffer) {
        // Write total length (16 bytes for interval)
        buffer.resize(buffer.size() + sizeof(integer));
        byte   *dest = &buffer[buffer.size() - sizeof(integer)];
        integer nbo  = htonl(16);
        memcpy(dest, &nbo, sizeof(integer));

        // Convert to microseconds
        auto        micros    = std::chrono::duration_cast<std::chrono::microseconds>(value);
        int64_t     count     = micros.count();
        int64_t     net_count = qb::endian::to_big_endian(count);
        const byte *bytes     = reinterpret_cast<const byte *>(&net_count);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(int64_t));

        // Days = 0
        int32_t zero     = 0;
        int32_t net_zero = qb::endian::to_big_endian(zero);
        bytes            = reinterpret_cast<const byte *>(&net_zero);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(int32_t));
        buffer.insert(buffer.end(), bytes, bytes + sizeof(int32_t)); // months = 0
    }

    static std::string
    to_text(const value_type &value) {
        // Convert to seconds and format
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(value);
        return std::to_string(secs.count()) + " seconds";
    }

    static value_type
    from_binary(const std::vector<byte> &buffer) {
        if (buffer.size() < 16) {
            throw std::runtime_error("Invalid INTERVAL binary format");
        }
        int64_t net_count, count;
        std::memcpy(&net_count, buffer.data(), sizeof(int64_t));
        count = qb::endian::from_big_endian(net_count);
        // Convert microseconds to target duration
        auto micros = std::chrono::microseconds(count);
        return std::chrono::duration_cast<value_type>(micros);
    }

    static value_type
    from_text(const std::string &text) {
        // Simple parsing for "X seconds" format
        // This is a simplified implementation
        try {
            size_t  pos   = 0;
            int64_t value = std::stoll(text, &pos);
            return std::chrono::duration_cast<value_type>(std::chrono::seconds(value));
        } catch (...) {
            return value_type::zero();
        }
    }
};

// ============================================================================
// P0: PostgreSQL NUMERIC/DECIMAL type support
// ============================================================================

/**
 * @brief Type converter for PostgreSQL NUMERIC/DECIMAL type
 *
 * PostgreSQL NUMERIC is an arbitrary precision decimal type.
 * We use std::string to preserve exact precision (financial calculations).
 *
 * Binary format: Complex structure with sign, weight, and digits.
 * For simplicity, we use text format which is more reliable.
 */
template <>
struct TypeConverter<std::string> {
    using value_type = std::string;

    static integer
    get_oid() {
        // Note: This returns text OID by default
        // For NUMERIC, the caller should specify oid::numeric (1700)
        return static_cast<integer>(oid::text);
    }

    static void
    to_binary(const value_type &value, std::vector<byte> &buffer) {
        // For NUMERIC, we send as text to preserve exact precision
        // Write length
        integer len = static_cast<integer>(value.size());
        buffer.resize(buffer.size() + sizeof(integer));
        byte   *dest = &buffer[buffer.size() - sizeof(integer)];
        integer nbo  = htonl(len);
        memcpy(dest, &nbo, sizeof(integer));

        // Write raw data (without null terminator)
        if (!value.empty()) {
            buffer.insert(buffer.end(), reinterpret_cast<const byte *>(value.data()),
                          reinterpret_cast<const byte *>(value.data() + value.size()));
        }
    }

    static std::string
    to_text(const value_type &value) {
        return value;
    }

    static value_type
    from_binary(const std::vector<byte> &buffer) {
        // Read as text (length-prefixed)
        if (buffer.size() < 4) {
            return "";
        }
        integer len;
        std::memcpy(&len, buffer.data(), sizeof(integer));
        len = ntohl(len);
        if (len <= 0 || static_cast<size_t>(len) > buffer.size() - 4) {
            return "";
        }
        return std::string(reinterpret_cast<const char *>(buffer.data() + 4), len);
    }

    static value_type
    from_text(const std::string &text) {
        return text;
    }
};

/**
 * @brief Specialized NUMERIC converter for financial precision
 *
 * This is a marker type to distinguish NUMERIC from regular TEXT.
 * Usage: TypeConverter<qb::pg::numeric>::from_text("123.456")
 */
struct numeric {
    std::string value;

    numeric() = default;
    explicit numeric(const std::string &v)
        : value(v) {}
    explicit numeric(const char *v)
        : value(v) {}

    // Arithmetic operators for convenience
    numeric
    operator+(const numeric &other) const {
        // Simple string-based addition would require a decimal library
        // For now, just concatenate for demonstration
        return numeric("(" + value + "+" + other.value + ")");
    }

    bool
    operator==(const numeric &other) const {
        return value == other.value;
    }

    const std::string &
    str() const {
        return value;
    }
};

template <>
struct TypeConverter<numeric> {
    using value_type = numeric;

    static integer
    get_oid() {
        return static_cast<integer>(oid::numeric); // 1700
    }

    static void
    to_binary(const value_type &num, std::vector<byte> &buffer) {
        // Send as text for precision
        const std::string &value = num.str();
        integer            len   = static_cast<integer>(value.size());
        buffer.resize(buffer.size() + sizeof(integer));
        byte   *dest = &buffer[buffer.size() - sizeof(integer)];
        integer nbo  = htonl(len);
        memcpy(dest, &nbo, sizeof(integer));

        if (!value.empty()) {
            buffer.insert(buffer.end(), reinterpret_cast<const byte *>(value.data()),
                          reinterpret_cast<const byte *>(value.data() + value.size()));
        }
    }

    static std::string
    to_text(const value_type &num) {
        return num.str();
    }

    static value_type
    from_binary(const std::vector<byte> &buffer) {
        if (buffer.size() < 4) {
            return numeric("0");
        }
        integer len;
        std::memcpy(&len, buffer.data(), sizeof(integer));
        len = ntohl(len);
        if (len <= 0 || static_cast<size_t>(len) > buffer.size() - 4) {
            return numeric("0");
        }
        return numeric(std::string(reinterpret_cast<const char *>(buffer.data() + 4), len));
    }

    static value_type
    from_text(const std::string &text) {
        return numeric(text);
    }
};

// ============================================================================
// P1: PostgreSQL DATE type support
// ============================================================================

/**
 * @brief Simple date type for PostgreSQL DATE (days since 2000-01-01)
 *
 * PostgreSQL stores DATE as 4-byte integer (days since 2000-01-01).
 * Positive = after 2000-01-01, negative = before.
 */
struct pgdate {
    int32_t days_since_pg_epoch; // Days since 2000-01-01

    pgdate()
        : days_since_pg_epoch(0) {}
    explicit pgdate(int32_t days)
        : days_since_pg_epoch(days) {}

    // Convert from Unix timestamp (seconds since 1970-01-01)
    static pgdate
    from_unix_time(time_t unix_seconds) {
        // Days from 1970-01-01 to 2000-01-01 = 10957 days
        constexpr int32_t DAYS_1970_TO_2000 = 10957;
        return pgdate(static_cast<int32_t>(unix_seconds / 86400) - DAYS_1970_TO_2000);
    }

    // Convert to Unix timestamp (midnight of that day)
    time_t
    to_unix_time() const {
        constexpr int32_t DAYS_1970_TO_2000 = 10957;
        return static_cast<time_t>(days_since_pg_epoch + DAYS_1970_TO_2000) * 86400;
    }

    // Simple to_string (YYYY-MM-DD format) - UTC. Pure integer civil conversion
    // (qb::detail::civil_from_days) — exact for all dates incl. pre-1970, which
    // the C library gmtime_s rejects on Windows.
    std::string
    to_string() const {
        constexpr int32_t DAYS_1970_TO_2000 = 10957;
        const auto        c = qb::detail::civil_from_days(static_cast<int64_t>(days_since_pg_epoch) + DAYS_1970_TO_2000);
        char              buf[16]; // room for negative/large years
        std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u", static_cast<long long>(c.year), c.month, c.day);
        return std::string(buf);
    }

    // Parse from string (YYYY-MM-DD) - UTC.
    static pgdate
    from_string(const std::string &str) {
        if (str.size() < 10)
            return pgdate(0);
        int year, month, day;
        if (std::sscanf(str.c_str(), "%d-%d-%d", &year, &month, &day) != 3) {
            return pgdate(0);
        }
        const int64_t days_since_1970 = qb::detail::days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
        return from_unix_time(days_since_1970 * 86400);
    }

    bool
    operator==(const pgdate &other) const {
        return days_since_pg_epoch == other.days_since_pg_epoch;
    }

    bool
    operator<(const pgdate &other) const {
        return days_since_pg_epoch < other.days_since_pg_epoch;
    }
};

template <>
struct TypeConverter<pgdate> {
    using value_type = pgdate;

    static integer
    get_oid() {
        return static_cast<integer>(oid::date); // 1082
    }

    static void
    to_binary(const value_type &date, std::vector<byte> &buffer) {
        // PostgreSQL DATE: 4 bytes, network byte order
        buffer.resize(buffer.size() + sizeof(integer));
        byte   *dest = &buffer[buffer.size() - sizeof(integer)];
        integer nbo  = htonl(4); // Length = 4
        memcpy(dest, &nbo, sizeof(integer));

        int32_t     net_days = qb::endian::to_big_endian(date.days_since_pg_epoch);
        const byte *bytes    = reinterpret_cast<const byte *>(&net_days);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(int32_t));
    }

    static std::string
    to_text(const value_type &date) {
        return date.to_string();
    }

    static value_type
    from_binary(const std::vector<byte> &buffer) {
        if (buffer.size() < 8) { // 4 bytes length + 4 bytes data
            return pgdate(0);
        }
        // Skip length prefix (already parsed by caller usually)
        int32_t net_days;
        std::memcpy(&net_days, buffer.data() + 4, sizeof(int32_t));
        return pgdate(qb::endian::from_big_endian(net_days));
    }

    static value_type
    from_text(const std::string &text) {
        return pgdate::from_string(text);
    }
};

// ============================================================================
// TIME and TIMETZ Types
// ============================================================================

/**
 * @brief Structure for PostgreSQL TIME type
 *
 * Stores time as microseconds since midnight (0-86399999999)
 * PostgreSQL epoch starts at 2000-01-01, but TIME is relative to midnight
 */
struct pgtime {
    int64_t microseconds; // Microseconds since midnight (0 to 86399999999)

    pgtime()
        : microseconds(0) {}
    explicit pgtime(int64_t micros)
        : microseconds(micros) {}

    // Construct from hours, minutes, seconds, microseconds
    static pgtime
    from_hmsu(int hour, int min, int sec, int microsec = 0) {
        int64_t total_micros = ((hour * 3600LL) + (min * 60LL) + sec) * 1000000LL + microsec;
        return pgtime(total_micros);
    }

    // Parse from string (HH:MM:SS or HH:MM:SS.uuuuuu)
    static pgtime
    from_string(const std::string &str) {
        int hour = 0, min = 0, sec = 0, microsec = 0;
        // Try format HH:MM:SS.uuuuuu
        if (std::sscanf(str.c_str(), "%d:%d:%d.%d", &hour, &min, &sec, &microsec) >= 3) {
            return from_hmsu(hour, min, sec, microsec);
        }
        return pgtime(0);
    }

    std::string
    to_string() const {
        int64_t total_seconds = microseconds / 1000000;
        int     hour          = static_cast<int>(total_seconds / 3600);
        int     min           = static_cast<int>((total_seconds % 3600) / 60);
        int     sec           = static_cast<int>(total_seconds % 60);
        int     microsec      = static_cast<int>(microseconds % 1000000);

        char buf[32];
        if (microsec > 0) {
            std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06d", hour, min, sec, microsec);
        } else {
            std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hour, min, sec);
        }
        return std::string(buf);
    }

    bool
    operator==(const pgtime &other) const {
        return microseconds == other.microseconds;
    }

    bool
    operator<(const pgtime &other) const {
        return microseconds < other.microseconds;
    }
};

/**
 * @brief Structure for PostgreSQL TIMETZ type
 *
 * Stores time with timezone offset
 */
struct pgtimetz {
    int64_t microseconds; // Microseconds since midnight
    int32_t tz_offset;    // Timezone offset in seconds (e.g., +02:00 = 7200)

    pgtimetz()
        : microseconds(0)
        , tz_offset(0) {}
    pgtimetz(int64_t micros, int32_t offset)
        : microseconds(micros)
        , tz_offset(offset) {}

    static pgtimetz
    from_hmsu_tz(int hour, int min, int sec, int microsec, int tz_seconds) {
        int64_t total_micros = ((hour * 3600LL) + (min * 60LL) + sec) * 1000000LL + microsec;
        return pgtimetz(total_micros, tz_seconds);
    }

    // Parse from string (HH:MM:SS+TZ or HH:MM:SS-TZ)
    static pgtimetz
    from_string(const std::string &str) {
        int  hour = 0, min = 0, sec = 0, microsec = 0;
        int  tz_hour = 0, tz_min = 0;
        char tz_sign = '+';

        // Try format with timezone HH:MM:SS+HH:MM or HH:MM:SS-HH:MM
        if (std::sscanf(str.c_str(), "%d:%d:%d%c%d:%d", &hour, &min, &sec, &tz_sign, &tz_hour, &tz_min) >= 5) {
            int tz_seconds = (tz_hour * 3600) + (tz_min * 60);
            if (tz_sign == '-')
                tz_seconds = -tz_seconds;
            return from_hmsu_tz(hour, min, sec, microsec, tz_seconds);
        }
        // Try without timezone
        if (std::sscanf(str.c_str(), "%d:%d:%d", &hour, &min, &sec) == 3) {
            return from_hmsu_tz(hour, min, sec, 0, 0);
        }
        return pgtimetz(0, 0);
    }

    std::string
    to_string() const {
        int64_t total_seconds = microseconds / 1000000;
        int     hour          = static_cast<int>(total_seconds / 3600);
        int     min           = static_cast<int>((total_seconds % 3600) / 60);
        int     sec           = static_cast<int>(total_seconds % 60);

        int  abs_offset = std::abs(tz_offset);
        int  tz_h       = abs_offset / 3600;
        int  tz_m       = (abs_offset % 3600) / 60;
        char sign       = tz_offset >= 0 ? '+' : '-';

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d%c%02d:%02d", hour, min, sec, sign, tz_h, tz_m);
        return std::string(buf);
    }

    bool
    operator==(const pgtimetz &other) const {
        return microseconds == other.microseconds && tz_offset == other.tz_offset;
    }
};

template <>
struct TypeConverter<pgtime> {
    using value_type = pgtime;

    static integer
    get_oid() {
        return static_cast<integer>(oid::time); // 1083
    }

    static void
    to_binary(const value_type &time, std::vector<byte> &buffer) {
        // Length: 8 bytes
        buffer.resize(buffer.size() + sizeof(integer));
        byte   *dest = &buffer[buffer.size() - sizeof(integer)];
        integer nbo  = htonl(8);
        memcpy(dest, &nbo, sizeof(integer));

        // Microseconds since midnight (int64 big-endian)
        int64_t     net_micros = qb::endian::to_big_endian(time.microseconds);
        const byte *bytes      = reinterpret_cast<const byte *>(&net_micros);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(int64_t));
    }

    static std::string
    to_text(const value_type &time) {
        return time.to_string();
    }

    static value_type
    from_binary(const std::vector<byte> &buffer) {
        if (buffer.size() < 12) { // 4 bytes length + 8 bytes data
            return pgtime(0);
        }
        int64_t net_micros;
        std::memcpy(&net_micros, buffer.data() + 4, sizeof(int64_t));
        return pgtime(qb::endian::from_big_endian(net_micros));
    }

    static value_type
    from_text(const std::string &text) {
        return pgtime::from_string(text);
    }
};

template <>
struct TypeConverter<pgtimetz> {
    using value_type = pgtimetz;

    static integer
    get_oid() {
        return static_cast<integer>(oid::timetz); // 1266
    }

    static void
    to_binary(const value_type &timetz, std::vector<byte> &buffer) {
        // Length: 12 bytes (8 for time + 4 for tz offset)
        buffer.resize(buffer.size() + sizeof(integer));
        byte   *dest = &buffer[buffer.size() - sizeof(integer)];
        integer nbo  = htonl(12);
        memcpy(dest, &nbo, sizeof(integer));

        // Microseconds since midnight (int64 big-endian)
        int64_t     net_micros = qb::endian::to_big_endian(timetz.microseconds);
        const byte *bytes      = reinterpret_cast<const byte *>(&net_micros);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(int64_t));

        // Timezone offset in seconds (int32 big-endian)
        int32_t net_offset = qb::endian::to_big_endian(timetz.tz_offset);
        bytes              = reinterpret_cast<const byte *>(&net_offset);
        buffer.insert(buffer.end(), bytes, bytes + sizeof(int32_t));
    }

    static std::string
    to_text(const value_type &timetz) {
        return timetz.to_string();
    }

    static value_type
    from_binary(const std::vector<byte> &buffer) {
        if (buffer.size() < 16) { // 4 bytes length + 8 bytes time + 4 bytes tz
            return pgtimetz(0, 0);
        }
        int64_t net_micros;
        int32_t net_offset;
        std::memcpy(&net_micros, buffer.data() + 4, sizeof(int64_t));
        std::memcpy(&net_offset, buffer.data() + 12, sizeof(int32_t));
        return pgtimetz(qb::endian::from_big_endian(net_micros), qb::endian::from_big_endian(net_offset));
    }

    static value_type
    from_text(const std::string &text) {
        return pgtimetz::from_string(text);
    }
};

} // namespace qb::pg::detail