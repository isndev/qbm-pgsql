/**
 * @file pg_notify_sql.h
 * @brief Safe SQL builders for PostgreSQL LISTEN / UNLISTEN / NOTIFY
 *
 * Outbound only: these strings are passed to Transaction::execute() like any simple query.
 * Inbound NOTIFY (server push) is handled in pgsql.h (NotificationResponse), not here.
 */
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "./error.h"

namespace qb::pg::detail {

/// PostgreSQL NOTIFY payload size limit (bytes); see server docs.
inline constexpr std::size_t notify_payload_max_bytes = 8000;

[[nodiscard]] inline std::string
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

[[nodiscard]] inline std::string
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

[[nodiscard]] inline std::string
build_listen_sql(std::string_view channel) {
    if (channel.empty())
        throw error::client_error{"LISTEN channel name cannot be empty"};
    return std::string("LISTEN ") + quote_notify_identifier(channel);
}

[[nodiscard]] inline std::string
build_unlisten_sql(std::string_view channel) {
    if (channel.empty())
        throw error::client_error{"UNLISTEN channel name cannot be empty"};
    return std::string("UNLISTEN ") + quote_notify_identifier(channel);
}

[[nodiscard]] inline std::string
build_unlisten_all_sql() {
    return "UNLISTEN *";
}

[[nodiscard]] inline std::string
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
