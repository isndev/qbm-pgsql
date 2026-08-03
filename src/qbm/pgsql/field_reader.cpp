/**
 * @file field_reader.cpp
 * @brief Direct PostgreSQL field reader implementation
 *
 * Out-of-line definitions for the non-template scalar/string overloads of
 * FieldReader::read_value. Each overload delegates to the shared
 * ParamUnserializer to parse a single PostgreSQL binary field and converts
 * any parsing exception into a false return value.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#include "./field_reader.h"

namespace qb::pg {
namespace detail {

bool
FieldReader::read_value(const std::vector<byte> &buffer, smallint &value) {
    try {
        value = unserializer.read_smallint(buffer);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool
FieldReader::read_value(const std::vector<byte> &buffer, integer &value) {
    try {
        value = unserializer.read_integer(buffer);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool
FieldReader::read_value(const std::vector<byte> &buffer, bigint &value) {
    try {
        value = unserializer.read_bigint(buffer);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool
FieldReader::read_value(const std::vector<byte> &buffer, float &value) {
    try {
        value = unserializer.read_float(buffer);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool
FieldReader::read_value(const std::vector<byte> &buffer, double &value) {
    try {
        value = unserializer.read_double(buffer);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

bool
FieldReader::read_value(const std::vector<byte> &buffer, bool &value) {
    try {
        // For a boolean, we just have a single byte with 0 or 1
        if (buffer.size() >= 1) {
            value = (buffer[0] != 0);
            return true;
        }
        return false;
    } catch (const std::exception &) {
        return false;
    }
}

bool
FieldReader::read_value(const std::vector<byte> &buffer, std::string &value) {
    try {
        // Field value bytes (length prefix already stripped) -> verbatim; read_string()'s
        // leading-NUL heuristic would corrupt a value beginning with a NUL.
        value = unserializer.read_text_string(buffer);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

} // namespace detail
} // namespace qb::pg
