/**
 * @file pg_notify_sql.cpp
 * @brief Safe SQL builders for PostgreSQL LISTEN / UNLISTEN / NOTIFY
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#include "./pg_notify_sql.h"

namespace qb::pg::detail {

std::string
quote_notify_identifier(std::string_view ident) {
    std::string out;
    out.reserve(ident.size() + 2);
    out.push_back('"');
    for (char c : ident) {
        if (c == '"')
            out.append("\"\"");
        else
            out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string
quote_notify_string_literal(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'')
            out.append("''");
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string
build_listen_sql(std::string_view channel) {
    if (channel.empty())
        throw error::client_error{"LISTEN channel name cannot be empty"};
    return std::string("LISTEN ") + quote_notify_identifier(channel);
}

std::string
build_unlisten_sql(std::string_view channel) {
    if (channel.empty())
        throw error::client_error{"UNLISTEN channel name cannot be empty"};
    return std::string("UNLISTEN ") + quote_notify_identifier(channel);
}

std::string
build_notify_sql(std::string_view channel, std::string_view payload) {
    if (channel.empty())
        throw error::client_error{"NOTIFY channel name cannot be empty"};
    if (payload.size() > notify_payload_max_bytes)
        throw error::client_error{"NOTIFY payload exceeds maximum length"};
    std::string sql = std::string("NOTIFY ") + quote_notify_identifier(channel);
    if (!payload.empty())
        sql.append(", ").append(quote_notify_string_literal(payload));
    return sql;
}

} // namespace qb::pg::detail
