/**
 * @file type_converter.cpp
 * @brief Out-of-line definitions for the QB PostgreSQL type conversion system
 *
 * This file holds the non-template, non-trivial member and free-function bodies
 * of the type conversion layer declared in type_converter.h. The header keeps
 * the templates, constexpr/trivial accessors and the public declarations; the
 * substantial wire-format codecs (binary/text serialization and parsing for the
 * fully-specialized converters, plus the NUMERIC limb codec) live here so that
 * including the header does not pull these bodies into every translation unit.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#include "./type_converter.h"

#include <qb/system/parse.h> // qb::to_number (locale-free, throw-free string->number)

namespace qb::pg::detail {

// ============================================================================
// TypeConverter<qb::uuid>
// ============================================================================

void
TypeConverter<qb::uuid>::to_binary(const qb::uuid &value, std::vector<byte> &buffer) {
    // PostgreSQL UUID binary format: 4-byte length prefix (16) + 16 raw bytes
    write_integer(buffer, 16);
    const auto &uuid_bytes = value.as_bytes();
    for (size_t i = 0; i < 16; ++i) {
        buffer.push_back(static_cast<byte>(uuid_bytes[i]));
    }
}

qb::uuid
TypeConverter<qb::uuid>::from_binary(std::span<const byte> buffer) {
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

qb::uuid
TypeConverter<qb::uuid>::from_text(const std::string &text) {
    auto uuid_result = qb::uuid::from_string(text);
    if (!uuid_result) {
        throw std::runtime_error("Invalid UUID format");
    }
    return uuid_result.value();
}

// ============================================================================
// TypeConverter<qb::wall_time>
// ============================================================================

void
TypeConverter<qb::wall_time>::to_binary(const qb::wall_time &value, std::vector<byte> &buffer) {
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

std::string
TypeConverter<qb::wall_time>::to_text(const qb::wall_time &value) {
    // Floored seconds/fraction split (system_clock is signed): a pre-1970
    // sub-second instant must borrow a second, not truncate toward zero.
    const int64_t total_us  = qb::unix_micros(value);
    int64_t       whole_sec = total_us / 1000000;
    int64_t       microsecs = total_us % 1000000;
    if (microsecs < 0) {
        microsecs += 1000000;
        --whole_sec;
    }
    // Date and time parts (UTC) via the canonical formatter.
    std::string result = qb::format_utc(qb::wall_from_unix_seconds(whole_sec), "%Y-%m-%d %H:%M:%S");
    if (result.empty())
        throw error::client_error("timestamp out of range for text conversion");

    // Add microseconds (already normalized to [0, 1e6) above)
    if (microsecs > 0) {
        char usec_buf[8];
        std::snprintf(usec_buf, sizeof(usec_buf), ".%06ld", static_cast<long>(microsecs));
        result += usec_buf;
    }

    result += "+00";
    return result;
}

qb::wall_time
TypeConverter<qb::wall_time>::from_binary(std::span<const byte> buffer) {
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

qb::wall_time
TypeConverter<qb::wall_time>::from_text(const std::string &text) {
    // Expected format: "YYYY-MM-DD HH:MM:SS.ssssss" or "YYYY-MM-DD HH:MM:SS"
    std::tm tm   = {};
    int     usec = 0;

    // Parse date and time
    if (text.empty()) {
        throw std::runtime_error("Empty timestamp string");
    }

    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;

    // Faithful, locale-free, overflow-safe replacement for
    // sscanf("%d-%d-%d %d:%d:%d") == 6. Each take() parses a signed integer prefix
    // (skipping leading whitespace, like %d), so the date/time separator space is
    // absorbed by the hour field's own whitespace skip; the '-'/':' separators are
    // matched literally. All six fields are required.
    const std::string_view sv   = text;
    std::size_t            pos  = 0;
    const auto             take = [&](int &out) noexcept -> bool {
        if (pos >= sv.size())
            return false;
        std::size_t used = 0;
        const auto  v    = qb::to_number_prefix<int>(sv.substr(pos), &used);
        if (!v)
            return false;
        out = *v;
        pos += used;
        return true;
    };
    const auto lit = [&](char c) noexcept -> bool {
        if (pos >= sv.size() || sv[pos] != c)
            return false;
        ++pos;
        return true;
    };
    if (!(take(year) && lit('-') && take(month) && lit('-') && take(day) && take(hour) && lit(':') && take(min) && lit(':') && take(sec))) {
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
        // usec_str is the run of ASCII digits after the '.' (delimited by the
        // first non-digit), i.e. an exact, sign-free, whitespace-free whole field
        // -> STRICT parse. The only failure mode is an overflow (more digits than
        // fit in int) on hostile wire input; fail loud with the module's domain
        // error instead of std::stoi's raw std::out_of_range.
        if (auto parsed = qb::to_number<int>(usec_str))
            usec = *parsed;
        else
            throw error::client_error("invalid microsecond fraction in timestamp text: '" + usec_str + "'");

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
    tm.tm_isdst           = 0;
    std::time_t time_secs = safe_timegm(tm);

    // Apply the trailing timestamptz offset if present (east-positive): the parsed
    // fields are local to that zone, so the UTC instant is (local - offset). The
    // date's '-' separators sit at fixed early positions, so the zone sign is the
    // first +/- at index >= 10. Without this, "12:00:00+02" decoded as "12:00:00Z".
    if (const auto sign_pos = text.find_first_of("+-", 10); sign_pos != std::string::npos) {
        if (auto off = qb::parse_utc_offset(text.substr(sign_pos)))
            time_secs -= *off;
    }

    return qb::wall_from_unix_seconds(time_secs) + std::chrono::microseconds(usec);
}

// ============================================================================
// TypeConverter<qb::json>
// ============================================================================

void
TypeConverter<qb::json>::to_binary(const qb::json &value, std::vector<byte> &buffer) {
    // For JSON format, we just store the JSON as a text representation
    std::string json_str = value.dump();

    // PostgreSQL JSON binary format is simply the JSON text
    buffer.reserve(4 + json_str.size());

    // 1. Write length of JSON string
    write_integer(buffer, checked_param_length(json_str.size()));

    // 2. Write JSON content as string
    buffer.insert(buffer.end(), json_str.begin(), json_str.end());
}

TypeConverter<qb::json>::value_type
TypeConverter<qb::json>::from_binary(std::span<const byte> buffer) {
    try {
        if (buffer.empty()) {
            throw std::runtime_error("Invalid JSON binary value: empty buffer");
        }

        // The protocol layer already stripped the per-field length prefix, and `json`
        // (unlike `jsonb`) has no version byte — so the buffer IS the JSON text value.
        // (The old `+ 4` dropped the first 4 chars; latent today because json columns
        // route to the text result format, but a real bug if ever binary-routed.)
        std::string json_str(reinterpret_cast<const char *>(buffer.data()), buffer.size());

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

TypeConverter<qb::json>::value_type
TypeConverter<qb::json>::from_text(const std::string &text) {
    try {
        return qb::json::parse(text);
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Failed to parse JSON text: ") + e.what());
    }
}

// ============================================================================
// TypeConverter<qb::jsonb>
// ============================================================================

void
TypeConverter<qb::jsonb>::to_binary(const qb::jsonb &value, std::vector<byte> &buffer) {
    // ParamSerializer expects typbinary layout: Int32 byte length of payload,
    // then jsonb_recv payload: version 1 + UTF-8 JSON text (see jsonb_send/recv).
    std::string json_str = value.dump();
    buffer.reserve(sizeof(integer) + 1 + json_str.size());
    // +1 for the jsonb version byte; guard the full payload length against the int32 wire prefix.
    write_integer(buffer, checked_param_length(1 + json_str.size()));
    buffer.push_back(static_cast<byte>(1));
    buffer.insert(buffer.end(), json_str.begin(), json_str.end());
}

TypeConverter<qb::jsonb>::value_type
TypeConverter<qb::jsonb>::from_binary(std::span<const byte> buffer) {
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

TypeConverter<qb::jsonb>::value_type
TypeConverter<qb::jsonb>::from_text(const std::string &text) {
    try {
        return qb::jsonb(nlohmann::json::parse(text));
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Failed to parse JSONB text: ") + e.what());
    }
}

// ============================================================================
// TypeConverter<std::string>
// ============================================================================

void
TypeConverter<std::string>::to_binary(const value_type &value, std::vector<byte> &buffer) {
    // Bind framing for a text value: [int32 byte-length][raw bytes], no NUL terminator.
    integer len = checked_param_length(value.size());
    buffer.resize(buffer.size() + sizeof(integer));
    byte   *dest = &buffer[buffer.size() - sizeof(integer)];
    integer nbo  = qb::endian::to_big_endian(len);
    memcpy(dest, &nbo, sizeof(integer));

    // Write raw data (without null terminator)
    if (!value.empty()) {
        buffer.insert(buffer.end(), reinterpret_cast<const byte *>(value.data()), reinterpret_cast<const byte *>(value.data() + value.size()));
    }
}

TypeConverter<std::string>::value_type
TypeConverter<std::string>::from_binary(std::span<const byte> buffer) {
    // The protocol layer has already stripped the per-field length prefix, so the buffer IS
    // the value bytes (a text/varchar value, or a bytea read as a string). Read them VERBATIM,
    // preserving any embedded/leading NULs.
    //
    // (The old code reinterpreted the first 4 bytes as an int32 length prefix and stripped them
    //  -> for real value bytes that prefix-as-length was garbage, so e.g. a 6-byte bytea
    //  {00 00 01 02 'h' 'i'} decoded its first 4 bytes to len=258 > remaining 2 and returned ""
    //  -- as<std::string>() on any binary value silently produced an empty/wrong string.)
    return std::string(reinterpret_cast<const char *>(buffer.data()), buffer.size());
}

// ============================================================================
// NUMERIC limb codec (free functions)
// ============================================================================

std::string
decode_pg_numeric(const byte *data, std::size_t size) {
    auto rd16 = [](const byte *p) -> std::uint16_t {
        std::uint16_t be;
        std::memcpy(&be, p, sizeof(be));
        return qb::endian::from_big_endian(be);
    };
    if (size < 8)
        return "0";
    const std::uint16_t ndigits = rd16(data + 0);
    const std::int16_t  weight  = static_cast<std::int16_t>(rd16(data + 2));
    const std::uint16_t sign    = rd16(data + 4);
    const std::uint16_t dscale  = rd16(data + 6);

    if (sign == 0xC000)
        return "NaN";
    if (sign == 0xD000)
        return "Infinity";
    if (sign == 0xF000)
        return "-Infinity";
    if (size < static_cast<std::size_t>(8) + static_cast<std::size_t>(ndigits) * 2)
        return "0"; // truncated/malformed

    std::string out;
    if (sign == 0x4000)
        out += '-';

    // Integer part: base-10000 limbs at positions weight..0 (digit indices 0..weight).
    if (weight < 0) {
        out += '0';
    } else {
        for (int i = 0; i <= weight; ++i) {
            const int limb = (i < ndigits) ? rd16(data + 8 + i * 2) : 0;
            char      tmp[8];
            std::snprintf(tmp, sizeof(tmp), i == 0 ? "%d" : "%04d", limb); // first limb: no pad
            out += tmp;
        }
    }

    // Fraction part: exactly dscale digits, limbs from index weight+1 onward.
    if (dscale > 0) {
        out += '.';
        std::string frac;
        for (int idx = weight + 1; static_cast<int>(frac.size()) < dscale; ++idx) {
            const int limb = (idx >= 0 && idx < ndigits) ? rd16(data + 8 + idx * 2) : 0;
            char      tmp[8];
            std::snprintf(tmp, sizeof(tmp), "%04d", limb);
            frac += tmp;
        }
        frac.resize(static_cast<std::size_t>(dscale));
        out += frac;
    }
    return out;
}

std::vector<byte>
encode_pg_numeric(const std::string &in) {
    auto wr16 = [](std::vector<byte> &b, std::uint16_t v) {
        std::uint16_t be = qb::endian::to_big_endian(v);
        const byte   *p  = reinterpret_cast<const byte *>(&be);
        b.insert(b.end(), p, p + sizeof(be));
    };
    std::vector<byte> out;

    // Non-finite values.
    if (in == "NaN") {
        wr16(out, 0);
        wr16(out, 0);
        wr16(out, 0xC000);
        wr16(out, 0);
        return out;
    }
    if (in == "Infinity" || in == "+Infinity") {
        wr16(out, 0);
        wr16(out, 0);
        wr16(out, 0xD000);
        wr16(out, 0);
        return out;
    }
    if (in == "-Infinity") {
        wr16(out, 0);
        wr16(out, 0);
        wr16(out, 0xF000);
        wr16(out, 0);
        return out;
    }

    std::size_t i   = 0;
    bool        neg = false;
    if (i < in.size() && (in[i] == '+' || in[i] == '-')) {
        neg = (in[i] == '-');
        ++i;
    }
    // Strict: every character of the integer/fraction part must be a digit (or the single
    // decimal point). Previously any non-digit was SILENTLY skipped, so "abc" encoded as the
    // zero NUMERIC, "12abc" as 12, and "1e5" as 15 — silent data corruption sent to the server.
    // A malformed value must fail loudly, like every other from_text/encode path.
    const auto reject = [&in]() {
        throw error::client_error("invalid NUMERIC text representation: '" + in + "'");
    };
    std::string intpart, fracpart;
    for (; i < in.size() && in[i] != '.'; ++i) {
        if (in[i] < '0' || in[i] > '9')
            reject();
        intpart += in[i];
    }
    if (i < in.size() && in[i] == '.') {
        ++i;
        for (; i < in.size(); ++i) {
            if (in[i] < '0' || in[i] > '9')
                reject();
            fracpart += in[i];
        }
    }
    // No digits at all ("", "+", "-", ".", "+.") is not a number.
    if (intpart.empty() && fracpart.empty())
        reject();
    const int dscale = static_cast<int>(fracpart.size());

    // Align to base-10000 limb boundaries: integer part left-padded, fraction
    // part right-padded, each to a multiple of 4 decimal digits.
    const std::string int_al  = std::string((4 - (intpart.size() % 4)) % 4, '0') + intpart;
    const std::string frac_al = fracpart + std::string((4 - (fracpart.size() % 4)) % 4, '0');

    const int n_int  = static_cast<int>(int_al.size()) / 4;
    const int n_frac = static_cast<int>(frac_al.size()) / 4;

    auto limb_of = [](const std::string &s, int k) {
        int v = 0;
        for (int j = 0; j < 4; ++j)
            v = v * 10 + (s[static_cast<std::size_t>(k) * 4 + j] - '0');
        return v;
    };
    std::vector<int> limbs;
    limbs.reserve(static_cast<std::size_t>(n_int) + static_cast<std::size_t>(n_frac));
    for (int k = 0; k < n_int; ++k)
        limbs.push_back(limb_of(int_al, k));
    for (int k = 0; k < n_frac; ++k)
        limbs.push_back(limb_of(frac_al, k));

    int weight = n_int - 1;
    // Strip leading zero limbs (each lowers the weight) and trailing zero limbs
    // (dscale is preserved regardless).
    int start = 0;
    while (start < static_cast<int>(limbs.size()) && limbs[start] == 0) {
        ++start;
        --weight;
    }
    int end = static_cast<int>(limbs.size());
    while (end > start && limbs[end - 1] == 0)
        --end;

    const int     ndigits = end - start;
    std::uint16_t sign    = neg ? 0x4000 : 0x0000;
    if (ndigits == 0) { // exact zero: normalize weight/sign, keep dscale
        weight = 0;
        sign   = 0x0000;
    }

    wr16(out, static_cast<std::uint16_t>(ndigits));
    wr16(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(weight)));
    wr16(out, sign);
    wr16(out, static_cast<std::uint16_t>(dscale));
    for (int k = start; k < end; ++k)
        wr16(out, static_cast<std::uint16_t>(limbs[k]));
    return out;
}

// ============================================================================
// TypeConverter<numeric>
// ============================================================================

void
TypeConverter<numeric>::to_binary(const value_type &num, std::vector<byte> &buffer) {
    // Real PostgreSQL binary NUMERIC, framed with the 4-byte length prefix the
    // Bind/param wire expects (parameters are sent in binary format, see
    // ExecuteQuery). The previous implementation sent the decimal text bytes,
    // which PostgreSQL then mis-parsed as binary.
    const std::vector<byte> body = encode_pg_numeric(num.str());
    const integer           len  = checked_param_length(body.size());
    buffer.resize(buffer.size() + sizeof(integer));
    byte   *dest = &buffer[buffer.size() - sizeof(integer)];
    integer nbo  = qb::endian::to_big_endian(len);
    std::memcpy(dest, &nbo, sizeof(integer));
    buffer.insert(buffer.end(), body.begin(), body.end());
}

TypeConverter<numeric>::value_type
TypeConverter<numeric>::from_binary(std::span<const byte> buffer) {
    // The result path hands us the value bytes only — the per-field length
    // prefix is stripped by the protocol layer. The to_binary round-trip path
    // instead includes the 4-byte length prefix; detect it (prefix value equals
    // the remaining byte count, impossible for a real value-first buffer) and
    // skip it before decoding.
    if (buffer.size() >= sizeof(integer) + 8) {
        integer len = 0;
        std::memcpy(&len, buffer.data(), sizeof(integer));
        len = qb::endian::from_big_endian(len);
        if (len >= 0 && static_cast<size_t>(len) == buffer.size() - sizeof(integer)) {
            return numeric(decode_pg_numeric(buffer.data() + sizeof(integer), buffer.size() - sizeof(integer)));
        }
    }
    return numeric(decode_pg_numeric(buffer.data(), buffer.size()));
}

// ============================================================================
// TypeConverter<qb::date>
// ============================================================================

void
TypeConverter<qb::date>::to_binary(const value_type &d, std::vector<byte> &buffer) {
    buffer.resize(buffer.size() + sizeof(integer));
    byte   *dest = &buffer[buffer.size() - sizeof(integer)];
    integer nbo  = qb::endian::to_big_endian(4);
    std::memcpy(dest, &nbo, sizeof(integer));
    int32_t     net_days = qb::endian::to_big_endian(static_cast<int32_t>(d.days_since_epoch() - DAYS_1970_TO_2000));
    const byte *bytes    = reinterpret_cast<const byte *>(&net_days);
    buffer.insert(buffer.end(), bytes, bytes + sizeof(int32_t));
}

TypeConverter<qb::date>::value_type
TypeConverter<qb::date>::from_binary(std::span<const byte> buffer) {
    int32_t net_days = 0;
    if (buffer.size() == sizeof(int32_t))
        std::memcpy(&net_days, buffer.data(), sizeof(int32_t));
    else if (buffer.size() >= sizeof(integer) + sizeof(int32_t))
        std::memcpy(&net_days, buffer.data() + sizeof(integer), sizeof(int32_t));
    else
        return qb::date{};
    return qb::date::from_days_since_epoch(static_cast<std::int64_t>(qb::endian::from_big_endian(net_days)) + DAYS_1970_TO_2000);
}

// ============================================================================
// TypeConverter<qb::time_of_day>
// ============================================================================

void
TypeConverter<qb::time_of_day>::to_binary(const value_type &t, std::vector<byte> &buffer) {
    buffer.resize(buffer.size() + sizeof(integer));
    byte   *dest = &buffer[buffer.size() - sizeof(integer)];
    integer nbo  = qb::endian::to_big_endian(8);
    std::memcpy(dest, &nbo, sizeof(integer));
    int64_t     net_micros = qb::endian::to_big_endian(static_cast<int64_t>(t.since_midnight().count()));
    const byte *bytes      = reinterpret_cast<const byte *>(&net_micros);
    buffer.insert(buffer.end(), bytes, bytes + sizeof(int64_t));
}

TypeConverter<qb::time_of_day>::value_type
TypeConverter<qb::time_of_day>::from_binary(std::span<const byte> buffer) {
    int64_t net_micros = 0;
    if (buffer.size() == sizeof(int64_t))
        std::memcpy(&net_micros, buffer.data(), sizeof(int64_t));
    else if (buffer.size() >= sizeof(integer) + sizeof(int64_t))
        std::memcpy(&net_micros, buffer.data() + sizeof(integer), sizeof(int64_t));
    else
        return qb::time_of_day{};
    return qb::time_of_day::from_micros(qb::endian::from_big_endian(net_micros));
}

// ============================================================================
// TypeConverter<qb::time_of_day_tz>
// ============================================================================

void
TypeConverter<qb::time_of_day_tz>::to_binary(const value_type &z, std::vector<byte> &buffer) {
    buffer.resize(buffer.size() + sizeof(integer));
    byte   *dest = &buffer[buffer.size() - sizeof(integer)];
    integer nbo  = qb::endian::to_big_endian(12);
    std::memcpy(dest, &nbo, sizeof(integer));
    int64_t     net_micros = qb::endian::to_big_endian(static_cast<int64_t>(z.tod.since_midnight().count()));
    const byte *b1         = reinterpret_cast<const byte *>(&net_micros);
    buffer.insert(buffer.end(), b1, b1 + sizeof(int64_t));
    int32_t     net_offset = qb::endian::to_big_endian(-static_cast<int32_t>(z.offset.count())); // east+ -> west+ wire
    const byte *b2         = reinterpret_cast<const byte *>(&net_offset);
    buffer.insert(buffer.end(), b2, b2 + sizeof(int32_t));
}

TypeConverter<qb::time_of_day_tz>::value_type
TypeConverter<qb::time_of_day_tz>::from_binary(std::span<const byte> buffer) {
    std::size_t base = 0;
    if (buffer.size() == sizeof(int64_t) + sizeof(int32_t))
        base = 0;
    else if (buffer.size() >= sizeof(integer) + sizeof(int64_t) + sizeof(int32_t))
        base = sizeof(integer);
    else
        return qb::time_of_day_tz{};
    int64_t net_micros = 0;
    int32_t net_offset = 0;
    std::memcpy(&net_micros, buffer.data() + base, sizeof(int64_t));
    std::memcpy(&net_offset, buffer.data() + base + sizeof(int64_t), sizeof(int32_t));
    return qb::time_of_day_tz{
        qb::time_of_day::from_micros(qb::endian::from_big_endian(net_micros)),
        std::chrono::seconds{-static_cast<int>(qb::endian::from_big_endian(net_offset))}
    };
}

TypeConverter<qb::time_of_day_tz>::value_type
TypeConverter<qb::time_of_day_tz>::from_text(const std::string &text) {
    // PostgreSQL TIMETZ text: "HH:MM:SS[.ffffff]±HH[:MM[:SS]]" (e.g.
    // "14:30:45.123456+02:00", "08:00:00-05"). Split the time from the
    // trailing signed offset (the first +/- after position 0), parse each.
    const auto      sign_pos  = text.find_first_of("+-", 1);
    std::string     time_part = (sign_pos == std::string::npos) ? text : text.substr(0, sign_pos);
    qb::time_of_day tod{};
    if (auto micros = qb::parse_time_of_day(time_part))
        tod = qb::time_of_day::from_micros(*micros);
    std::chrono::seconds offset{0};
    if (sign_pos != std::string::npos) {
        if (auto secs = qb::parse_utc_offset(std::string_view{text}.substr(sign_pos)))
            offset = std::chrono::seconds{*secs};
    }
    return qb::time_of_day_tz{tod, offset};
}

// ============================================================================
// TypeConverter<qb::calendar_interval>
// ============================================================================

void
TypeConverter<qb::calendar_interval>::to_binary(const value_type &iv, std::vector<byte> &buffer) {
    buffer.resize(buffer.size() + sizeof(integer));
    byte   *dest = &buffer[buffer.size() - sizeof(integer)];
    integer nbo  = qb::endian::to_big_endian(16);
    std::memcpy(dest, &nbo, sizeof(integer));
    int64_t     net_micros = qb::endian::to_big_endian(static_cast<int64_t>(iv.micros.count()));
    const byte *b          = reinterpret_cast<const byte *>(&net_micros);
    buffer.insert(buffer.end(), b, b + sizeof(int64_t));
    int32_t net_days = qb::endian::to_big_endian(iv.days);
    b                = reinterpret_cast<const byte *>(&net_days);
    buffer.insert(buffer.end(), b, b + sizeof(int32_t));
    int32_t net_months = qb::endian::to_big_endian(iv.months);
    b                  = reinterpret_cast<const byte *>(&net_months);
    buffer.insert(buffer.end(), b, b + sizeof(int32_t));
}

TypeConverter<qb::calendar_interval>::value_type
TypeConverter<qb::calendar_interval>::from_binary(std::span<const byte> buffer) {
    std::size_t base = 0;
    if (buffer.size() == 16)
        base = 0;
    else if (buffer.size() >= sizeof(integer) + 16)
        base = sizeof(integer);
    else
        return qb::calendar_interval{};
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
    const byte *p = buffer.data() + base;
    return qb::calendar_interval{rd32(p + 12), rd32(p + 8), std::chrono::microseconds{rd64(p + 0)}};
}

TypeConverter<qb::calendar_interval>::value_type
TypeConverter<qb::calendar_interval>::from_text(const std::string &) {
    // PostgreSQL INTERVAL text (e.g. "1 year 2 mons 3 days 04:05:06.789") is
    // IntervalStyle-dependent and lossy to parse field-by-field. The binary
    // codec is the supported decode (INTERVAL routes binary by default). Fail
    // loudly rather than silently returning a zero interval.
    throw std::runtime_error("qb::calendar_interval: text-format INTERVAL decode is not supported; read this column "
                             "in binary (the default for interval) or as std::string");
}

// ============================================================================
// TypeConverter<std::vector<std::byte>>
// ============================================================================

void
TypeConverter<std::vector<std::byte>>::to_binary(const value_type &v, std::vector<byte> &buffer) {
    const integer len = checked_param_length(v.size());
    buffer.resize(buffer.size() + sizeof(integer));
    byte   *dest = &buffer[buffer.size() - sizeof(integer)];
    integer nbo  = qb::endian::to_big_endian(len);
    std::memcpy(dest, &nbo, sizeof(integer));
    for (std::byte b : v)
        buffer.push_back(static_cast<byte>(b));
}

TypeConverter<std::vector<std::byte>>::value_type
TypeConverter<std::vector<std::byte>>::from_binary(std::span<const byte> buffer) {
    value_type out;
    out.reserve(buffer.size());
    for (byte b : buffer)
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(b)));
    return out;
}

TypeConverter<std::vector<std::byte>>::value_type
TypeConverter<std::vector<std::byte>>::from_text(const std::string &text) {
    // PostgreSQL bytea hex output: "\xDEADBEEF". Strip the "\x" marker and decode
    // with qb::crypto::hex_to_string (the same codec pgsql uses for MD5 auth; it
    // rejects odd-length / non-hex input by returning "").
    std::string_view hex{text};
    if (hex.size() >= 2 && hex[0] == '\\' && hex[1] == 'x')
        hex.remove_prefix(2);
    const std::string decoded = qb::crypto::hex_to_string(std::string{hex});
    value_type        out;
    out.reserve(decoded.size());
    for (char c : decoded)
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return out;
}

std::string
TypeConverter<std::vector<std::byte>>::to_text(const value_type &v) {
    // Emit PostgreSQL hex format "\x..." via qb::crypto::to_hex_string (lowercase).
    std::string raw;
    raw.reserve(v.size());
    for (std::byte b : v)
        raw.push_back(static_cast<char>(b));
    return "\\x" + qb::crypto::to_hex_string(raw, qb::crypto::range_hex_lower);
}

} // namespace qb::pg::detail
