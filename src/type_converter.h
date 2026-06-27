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
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#pragma once

#include <algorithm>
#include <array>
#include <charconv> // std::to_chars / std::from_chars (locale-independent, round-trip exact)
#include <chrono>
#include <cmath>
#include <cstddef>
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
#include <regex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <qb/io.h>
#include <qb/io/crypto.h> // qb::crypto::to_hex_string / hex_to_string (bytea hex codec)
#include <qb/system/endian.h>
#include <qb/system/time.h> // qb::safe_gmtime / safe_timegm / civil date helpers

#include "./common.h"
#include "./error.h" // qb::pg::error::client_error (thrown by text/binary converters)
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
            smallint    netval = qb::endian::to_big_endian(value);
            const byte *bytes  = reinterpret_cast<const byte *>(&netval);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(smallint));
        } else if constexpr (std::is_same_v<value_type, integer>) {
            // PostgreSQL integer: length (4) + network value
            write_integer(buffer, 4);
            integer     netval = qb::endian::to_big_endian(value);
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
            // Dependent-false: a hard compile error if this primary template is
            // ever instantiated for an unsupported type (instead of the old vacuous
            // sizeof(T)>0 which silently let unsupported types through).
            static_assert(!sizeof(T), "Type not supported for binary conversion");
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
            // PostgreSQL spells the non-finite values out in full.
            if (std::isnan(value))
                return "NaN";
            if (std::isinf(value)) {
                return value > 0 ? "Infinity" : "-Infinity";
            }
            // std::to_chars gives the SHORTEST round-trip-exact decimal. std::to_string
            // is fixed at 6 fractional digits, which both loses precision (1234.56789012
            // -> "1234.567890") and pads trailing zeros — wrong for a value sent to the DB.
            char        buf[64];
            const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
            if (ec != std::errc())
                throw error::client_error("failed to format floating-point value as text");
            return std::string(buf, ptr);
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
            static_assert(!sizeof(T), "Type not supported for text conversion");
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
    from_binary(std::span<const byte> buffer) {
        static ParamUnserializer unserializer;

        if constexpr (std::is_same_v<value_type, std::string>) {
            // The protocol layer already stripped the per-field length prefix, so the
            // buffer IS the value bytes — read them verbatim. read_string()'s legacy
            // binary-vs-text auto-detection (strip a phantom 4-byte prefix when a NUL
            // is in the first 3 bytes) MISFIRES here and would corrupt a value that
            // begins with a NUL (e.g. a bytea read as a string).
            return unserializer.read_text_string(buffer);
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
            // The buffer here is the field's VALUE bytes (the protocol strips the
            // per-field length prefix), and SQL NULL is decided by the caller via
            // field::as -> is_null() BEFORE any converter runs. So a value is always
            // present here — decode the inner type directly. (The previous code sniffed
            // the first 4 bytes for a "-1 NULL prefix" and wrongly turned a genuine
            // all-0xFF value — e.g. int4/int8 == -1 — into std::nullopt.)
            using inner_type = typename value_type::value_type;
            return TypeConverter<inner_type>::from_binary(buffer);
        } else {
            static_assert(!sizeof(T), "Type not supported for conversion from binary");
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
            int v;
            try {
                v = std::stoi(text);
            } catch (const std::exception &e) {
                throw error::client_error("Cannot parse smallint from text: " + text + " (" + e.what() + ")");
            }
            // std::stoi fits an int, so 40000 would NOT throw but would silently wrap on
            // the int16 cast (-> -25536). Range-check before narrowing.
            if (v < std::numeric_limits<smallint>::min() || v > std::numeric_limits<smallint>::max())
                throw error::client_error("smallint value out of range: " + text);
            return static_cast<smallint>(v);
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
            // std::stod (not strtod) so malformed input throws instead of silently
            // yielding 0.0 — strtod cannot throw, making the catch below dead.
            try {
                return std::stod(text);
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
                R"((\d{4})-(\d{1,2})-(\d{1,2})\s+(\d{1,2}):(\d{1,2}):(\d{1,2})(?:\.(\d{1,6}))?([+-]\d{2}(?::?\d{2})?)?)");

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

                // Convert the broken-down time to an epoch (UTC-native, portable:
                // handles instants before 1970 that the Windows CRT _mkgmtime rejects).
                std::time_t unix_timestamp = safe_timegm(tm);

                // Apply the timestamptz offset, if printed (group 8, east-positive):
                // the broken-down fields are local to that zone, so the UTC instant is
                // (local - offset). Without this, "12:00:00+02" decoded as "12:00:00Z".
                if (matches.size() > 8 && matches[8].matched) {
                    if (auto off = qb::parse_utc_offset(matches[8].str()))
                        unix_timestamp -= *off;
                }

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
            static_assert(!sizeof(T), "Type not supported for conversion from text");
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
        integer     networkValue = qb::endian::to_big_endian(value);
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
    static void to_binary(const qb::uuid &value, std::vector<byte> &buffer);

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
    static qb::uuid from_binary(std::span<const byte> buffer);

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
    static qb::uuid from_text(const std::string &text);

private:
    static void
    write_integer(std::vector<byte> &buffer, integer value) {
        integer     nbo   = qb::endian::to_big_endian(value);
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
    static void to_binary(const qb::wall_time &value, std::vector<byte> &buffer);

    /**
     * @brief Converts a Timestamp to PostgreSQL text format
     *
     * Creates a standard text representation of a timestamp in ISO 8601 format:
     * YYYY-MM-DD HH:MM:SS.ssssss
     *
     * @param value The timestamp to convert
     * @return std::string The PostgreSQL text representation of the timestamp
     */
    static std::string to_text(const qb::wall_time &value);

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
    static qb::wall_time from_binary(std::span<const byte> buffer);

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
    static qb::wall_time from_text(const std::string &text);

private:
    static void
    write_integer(std::vector<byte> &buffer, integer value) {
        integer     nbo   = qb::endian::to_big_endian(value);
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
    static void to_binary(const qb::json &value, std::vector<byte> &buffer);

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
    static value_type from_binary(std::span<const byte> buffer);

    /**
     * @brief Converts a PostgreSQL text representation to a JSON object
     *
     * Parses a JSON string into a qb::json object.
     *
     * @param text PostgreSQL text representation to convert
     * @return value_type Deserialized JSON object
     * @throws std::runtime_error If the text contains invalid or malformed JSON
     */
    static value_type from_text(const std::string &text);

private:
    static void
    write_integer(std::vector<byte> &buffer, integer value) {
        buffer.resize(buffer.size() + sizeof(integer));
        byte   *dest = &buffer[buffer.size() - sizeof(integer)];
        integer nbo  = qb::endian::to_big_endian(value);
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
    static void to_binary(const qb::jsonb &value, std::vector<byte> &buffer);

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
    static value_type from_binary(std::span<const byte> buffer);

    /**
     * @brief Converts a PostgreSQL text representation to a JSONB object
     *
     * Parses a JSON string into a qb::jsonb object.
     *
     * @param text PostgreSQL text representation to convert
     * @return value_type Deserialized JSONB object
     * @throws std::runtime_error If the text contains invalid or malformed JSON
     */
    static value_type from_text(const std::string &text);

private:
    static void
    write_integer(std::vector<byte> &buffer, integer value) {
        buffer.resize(buffer.size() + sizeof(integer));
        byte   *dest = &buffer[buffer.size() - sizeof(integer)];
        integer nbo  = qb::endian::to_big_endian(value);
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
        integer nbo  = qb::endian::to_big_endian(16);
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
    from_binary(std::span<const byte> buffer) {
        // PostgreSQL INTERVAL value is 16 bytes: int64 micros (time), int32 days,
        // int32 months. The protocol layer strips the per-field length prefix, so
        // the result buffer is exactly 16 bytes read from offset 0; the to_binary
        // round-trip path includes the 4-byte length prefix (>= 20 bytes). The
        // previous decoder read only the micros field and silently dropped days
        // and months (e.g. INTERVAL '1 day' decoded to 0).
        std::size_t base;
        if (buffer.size() == 16) {
            base = 0;
        } else if (buffer.size() >= sizeof(integer) + 16) {
            base = sizeof(integer);
        } else {
            throw std::runtime_error("Invalid INTERVAL binary format");
        }

        auto rd64 = [](const byte *p) {
            int64_t v;
            std::memcpy(&v, p, sizeof(v));
            return qb::endian::from_big_endian(v);
        };
        auto rd32 = [](const byte *p) {
            int32_t v;
            std::memcpy(&v, p, sizeof(v));
            return qb::endian::from_big_endian(v);
        };
        const byte   *p      = buffer.data() + base;
        const int64_t micros = rd64(p + 0);
        const int32_t days   = rd32(p + 8);
        const int32_t months = rd32(p + 12);

        // std::chrono::duration is a fixed-length span; PostgreSQL days/months are
        // calendar units. Fold them exactly as PostgreSQL's EXTRACT(EPOCH) does, so
        // as<duration>() in seconds equals EXTRACT(EPOCH FROM the interval): a day is
        // 24h, a whole year (12 months) is 365.25 days, and each residual month is
        // 30 days. (C++ truncating /,% keep the split consistent for negatives.)
        constexpr int64_t USECS_PER_DAY  = 86400LL * 1000000LL;
        constexpr int64_t USECS_PER_YEAR = 31557600LL * 1000000LL; // 365.25 * 86400 s
        const int64_t total_micros = micros + static_cast<int64_t>(days) * USECS_PER_DAY + static_cast<int64_t>(months / 12) * USECS_PER_YEAR
                                     + static_cast<int64_t>(months % 12) * 30 * USECS_PER_DAY;
        return std::chrono::duration_cast<value_type>(std::chrono::microseconds(total_micros));
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
// std::string <-> PostgreSQL text/varchar
// ============================================================================

/**
 * @brief Type converter for std::string, bound as a PostgreSQL text value.
 *
 * Maps to the `text` OID (25). PostgreSQL implicitly coerces a text Bind value to the
 * target column's actual type (varchar, bpchar, and — because NUMERIC accepts its
 * canonical decimal spelling — numeric, etc.), so a plain std::string parameter works
 * against most textual/decimal columns without an explicit cast. For an EXACT decimal
 * with a guaranteed numeric OID and binary NUMERIC framing, use qb::pg::numeric
 * (TypeConverter<numeric>) instead of a raw std::string.
 *
 * from_binary reads the field value bytes verbatim (see type_converter.cpp); to_binary
 * writes the Bind [int32 len][bytes] framing.
 */
template <>
struct TypeConverter<std::string> {
    using value_type = std::string;

    static integer
    get_oid() {
        // Always the text OID. PostgreSQL coerces text -> the column's real type on Bind;
        // for a value that must carry the numeric OID/format, bind qb::pg::numeric.
        return static_cast<integer>(oid::text);
    }

    static void to_binary(const value_type &value, std::vector<byte> &buffer);

    static std::string
    to_text(const value_type &value) {
        return value;
    }

    static value_type from_binary(std::span<const byte> buffer);

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

    // NOTE: numeric is an EXACT decimal carried as its canonical text form
    // (string in, string out). It is intentionally NOT an arithmetic type — a
    // faithful arbitrary-precision decimal belongs in a dedicated type, not here.
    // A previous operator+ did "(" + a + "+" + b + ")" string concatenation
    // masquerading as addition, which silently corrupted the value; it was removed.

    bool
    operator==(const numeric &other) const {
        return value == other.value;
    }

    const std::string &
    str() const {
        return value;
    }
};

// numeric's type_mapping lives here (not in type_mapping.h) because the numeric type is
// only declared above. Keeps the two OID sources (type_mapping<T> and
// TypeConverter<T>::get_oid) in agreement for the exact-decimal type.
template <>
struct type_mapping<numeric> {
    static constexpr integer type_oid = 1700;
}; // numeric

/**
 * @brief Decode PostgreSQL binary NUMERIC (value bytes, no length prefix) into a
 *        canonical decimal string. Mirrors PostgreSQL's numeric_out.
 *
 * Wire layout, all big-endian: int16 ndigits, int16 weight (signed; base-10000
 * limb position of the first digit), uint16 sign (0x0000 = positive, 0x4000 =
 * negative, 0xC000 = NaN, 0xD000 = +Infinity, 0xF000 = -Infinity), uint16 dscale
 * (display scale), then ndigits × int16 base-10000 "digits" (each 0..9999).
 */
std::string decode_pg_numeric(const byte *data, std::size_t size);

/**
 * @brief Encode a decimal string into PostgreSQL binary NUMERIC value bytes (no
 *        length prefix). Inverse of decode_pg_numeric; mirrors set_var_from_str.
 *
 * @param in Canonical decimal text (optionally signed, optional fraction); the
 *           special tokens "NaN", "Infinity"/"+Infinity" and "-Infinity" map to
 *           the corresponding NUMERIC sign words.
 * @return std::vector<byte> Big-endian NUMERIC value bytes (header + limbs), no
 *         length prefix.
 */
std::vector<byte> encode_pg_numeric(const std::string &in);

template <>
struct TypeConverter<numeric> {
    using value_type = numeric;

    static integer
    get_oid() {
        return static_cast<integer>(oid::numeric); // 1700
    }

    static void to_binary(const value_type &num, std::vector<byte> &buffer);

    static std::string
    to_text(const value_type &num) {
        return num.str();
    }

    static value_type from_binary(std::span<const byte> buffer);

    static value_type
    from_text(const std::string &text) {
        return numeric(text);
    }
};

// ============================================================================
// DATE / TIME / TIMETZ / INTERVAL → qb core civil vocabulary (canonical)
//
// These map the PostgreSQL wire formats onto qb::date / qb::time_of_day /
// qb::time_of_day_tz / qb::calendar_interval (qb/system/time.h) — the canonical
// (and only) way to read/bind temporal columns.
//
// Contract: to_binary appends [int32 length][value]; from_binary gets the value
// bytes only (the protocol strips the per-field length prefix), with a defensive
// path that also accepts the prefixed form so to_binary→from_binary round-trips.
// ============================================================================

/// PostgreSQL DATE: int32 days since 2000-01-01; qb::date counts days since the
/// Unix epoch, hence the 10957-day epoch offset.
template <>
struct TypeConverter<qb::date> {
    using value_type                                = qb::date;
    static constexpr std::int32_t DAYS_1970_TO_2000 = 10957;

    static integer
    get_oid() {
        return static_cast<integer>(oid::date);
    }
    static void to_binary(const value_type &d, std::vector<byte> &buffer);
    static std::string
    to_text(const value_type &d) {
        return d.to_string();
    }
    static value_type from_binary(std::span<const byte> buffer);
    static value_type
    from_text(const std::string &text) {
        return qb::date::parse(text).value_or(qb::date{});
    }
};

/// PostgreSQL TIME: int64 microseconds since midnight; maps 1:1 to qb::time_of_day.
template <>
struct TypeConverter<qb::time_of_day> {
    using value_type = qb::time_of_day;

    static integer
    get_oid() {
        return static_cast<integer>(oid::time);
    }
    static void to_binary(const value_type &t, std::vector<byte> &buffer);
    static std::string
    to_text(const value_type &t) {
        return t.to_string();
    }
    static value_type from_binary(std::span<const byte> buffer);
    static value_type
    from_text(const std::string &text) {
        return qb::time_of_day::parse(text).value_or(qb::time_of_day{});
    }
};

/// PostgreSQL TIMETZ: int64 micros + int32 zone (seconds WEST of UTC). qb::time_of_day_tz
/// stores an east-positive offset, so the zone is negated in both directions.
template <>
struct TypeConverter<qb::time_of_day_tz> {
    using value_type = qb::time_of_day_tz;

    static integer
    get_oid() {
        return static_cast<integer>(oid::timetz);
    }
    static void to_binary(const value_type &z, std::vector<byte> &buffer);
    static std::string
    to_text(const value_type &z) {
        return z.to_string();
    }
    static value_type from_binary(std::span<const byte> buffer);
    static value_type from_text(const std::string &text);
};

/// PostgreSQL INTERVAL: int64 micros + int32 days + int32 months. qb::calendar_interval
/// keeps the three fields separate (lossless); the std::chrono::duration converter
/// above remains as a lossy "total span" convenience.
template <>
struct TypeConverter<qb::calendar_interval> {
    using value_type = qb::calendar_interval;

    static integer
    get_oid() {
        return static_cast<integer>(oid::interval);
    }
    static void to_binary(const value_type &iv, std::vector<byte> &buffer);
    static std::string
    to_text(const value_type &iv) {
        return iv.to_string();
    }
    static value_type from_binary(std::span<const byte> buffer);
    static value_type from_text(const std::string &);
};

/**
 * @brief bytea converter for std::vector<std::byte>.
 *
 * The generic primary template only matches std::vector<byte> (== std::vector<char>);
 * std::byte is a distinct type, so without this as<std::vector<std::byte>>() would hit
 * the unsupported-type fallback and silently return an empty vector. PostgreSQL bytea
 * is raw bytes: the result value carries no length prefix; a parameter is length-prefixed.
 */
template <>
struct TypeConverter<std::vector<std::byte>> {
    using value_type = std::vector<std::byte>;

    static integer
    get_oid() {
        return static_cast<integer>(oid::bytea);
    }

    static void to_binary(const value_type &v, std::vector<byte> &buffer);

    static value_type from_binary(std::span<const byte> buffer);

    static value_type  from_text(const std::string &text);
    static std::string to_text(const value_type &v);
};

// ============================================================================
// One-dimensional array result decoding (std::vector<T>)
// ============================================================================

/**
 * @brief Decode a PostgreSQL binary array (value bytes, no length prefix) into a
 *        flat std::vector<Elem>.
 *
 * Wire layout, all big-endian: int32 ndim, int32 flags (has-null), int32 element
 * OID, then per dimension { int32 size, int32 lower_bound }, then each element as
 * { int32 length (-1 = NULL), value bytes }. Elements are stored row-major; this
 * flattens multi-dimensional arrays into a single vector. NULL elements become a
 * default-constructed Elem (the vector cannot represent SQL NULL otherwise).
 */
template <typename Elem>
std::vector<Elem>
decode_pg_array(std::span<const byte> buffer) {
    std::vector<Elem> result;
    auto              rd32 = [](const byte *p) -> std::int32_t {
        std::int32_t be;
        std::memcpy(&be, p, sizeof(be));
        return qb::endian::from_big_endian(be);
    };

    const std::size_t size = buffer.size();
    if (size < 12) // ndim + flags + element_oid
        return result;
    const byte        *p    = buffer.data();
    const std::int32_t ndim = rd32(p + 0);
    // p+4 = has-null flags, p+8 = element OID — both implied by Elem here.
    if (ndim <= 0)
        return result; // empty / zero-dimensional array

    std::size_t  off   = 12;
    std::int64_t total = 1;
    for (std::int32_t d = 0; d < ndim; ++d) {
        if (off + 8 > size)
            return result;
        const std::int32_t dim_size = rd32(p + off);
        off += 8; // dim size + lower bound
        if (dim_size < 0)
            return result;
        total *= dim_size;
        if (total > static_cast<std::int64_t>(size)) // guard against bogus dims
            return result;
    }
    result.reserve(static_cast<std::size_t>(total));

    for (std::int64_t e = 0; e < total; ++e) {
        if (off + 4 > size)
            break;
        const std::int32_t elem_len = rd32(p + off);
        off += 4;
        if (elem_len == -1) {
            result.emplace_back(); // SQL NULL -> default-constructed element
            continue;
        }
        if (elem_len < 0 || off + static_cast<std::size_t>(elem_len) > size)
            break;
        const byte *ev = p + off;
        if constexpr (std::is_same_v<Elem, std::string>) {
            result.emplace_back(reinterpret_cast<const char *>(ev), static_cast<std::size_t>(elem_len));
        } else {
            // Element value carries no length prefix — exactly the contract the
            // scalar from_binary decoders expect.
            result.push_back(TypeConverter<Elem>::from_binary(std::span<const byte>(ev, static_cast<std::size_t>(elem_len))));
        }
        off += static_cast<std::size_t>(elem_len);
    }
    return result;
}

/**
 * @brief Encode a std::vector<Elem> into PostgreSQL binary array value bytes (no
 *        length prefix). Inverse of decode_pg_array. The real parameter path uses
 *        ParamSerializer::add_vector(); this mirrors it so the converter is also
 *        correct if used directly (and lets ParamSerializer ODR-use to_binary).
 */
template <typename Elem>
std::vector<byte>
encode_pg_array(const std::vector<Elem> &vec) {
    std::vector<byte> buf;
    auto              wr32 = [](std::vector<byte> &b, std::int32_t v) {
        std::int32_t be = qb::endian::to_big_endian(v);
        const byte  *p  = reinterpret_cast<const byte *>(&be);
        b.insert(b.end(), p, p + sizeof(be));
    };
    wr32(buf, 1);                                     // ndim (1-D)
    wr32(buf, 0);                                     // has-null flags
    wr32(buf, TypeConverter<Elem>::get_oid());        // element OID
    wr32(buf, static_cast<std::int32_t>(vec.size())); // dimension size
    wr32(buf, 1);                                     // lower bound
    for (const Elem &e : vec) {
        std::vector<byte> elem; // each scalar to_binary emits [int32 length][value]
        TypeConverter<Elem>::to_binary(e, elem);
        buf.insert(buf.end(), elem.begin(), elem.end());
    }
    return buf;
}

/**
 * @brief Result/param converter for one-dimensional PostgreSQL arrays.
 *
 * `from_binary` decodes the binary result (arrays are requested in binary).
 * `to_binary` length-prefixes encode_pg_array (the live parameter path actually
 * goes through ParamSerializer::add_vector, but a compiling to_binary is required
 * because add_param ODR-uses it). Restricted to non-byte element types so
 * std::vector<byte>/std::vector<char> stay on the bytea path.
 */
#define QB_PG_DEFINE_ARRAY_CONVERTER(ELEM, ARRAY_OID)                         \
    template <>                                                               \
    struct TypeConverter<std::vector<ELEM>> {                                 \
        using value_type = std::vector<ELEM>;                                 \
        static integer                                                        \
        get_oid() {                                                           \
            return (ARRAY_OID);                                               \
        }                                                                     \
        static void                                                           \
        to_binary(const value_type &vec, std::vector<byte> &buffer) {         \
            const std::vector<byte> body = encode_pg_array<ELEM>(vec);        \
            integer                 len  = static_cast<integer>(body.size()); \
            buffer.resize(buffer.size() + sizeof(integer));                   \
            byte   *dest = &buffer[buffer.size() - sizeof(integer)];          \
            integer nbo  = qb::endian::to_big_endian(len);                                        \
            std::memcpy(dest, &nbo, sizeof(integer));                         \
            buffer.insert(buffer.end(), body.begin(), body.end());            \
        }                                                                     \
        static value_type                                                     \
        from_binary(std::span<const byte> buffer) {                           \
            return decode_pg_array<ELEM>(buffer);                             \
        }                                                                     \
        static value_type                                                     \
        from_text(const std::string &) {                                      \
            return {};                                                        \
        }                                                                     \
        static std::string                                                    \
        to_text(const value_type &) {                                         \
            return {};                                                        \
        }                                                                     \
    };

QB_PG_DEFINE_ARRAY_CONVERTER(bool, 1000)        // boolean[]
QB_PG_DEFINE_ARRAY_CONVERTER(smallint, 1005)    // int2[]
QB_PG_DEFINE_ARRAY_CONVERTER(integer, 1007)     // int4[]
QB_PG_DEFINE_ARRAY_CONVERTER(bigint, 1016)      // int8[]
QB_PG_DEFINE_ARRAY_CONVERTER(float, 1021)       // float4[]
QB_PG_DEFINE_ARRAY_CONVERTER(double, 1022)      // float8[]
QB_PG_DEFINE_ARRAY_CONVERTER(std::string, 1009) // text[]

#undef QB_PG_DEFINE_ARRAY_CONVERTER

} // namespace qb::pg::detail