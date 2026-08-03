/**
 * @file pg_notify_sql.h
 * @brief Safe SQL builders for PostgreSQL LISTEN / UNLISTEN / NOTIFY
 *
 * Outbound only: these strings are passed to Transaction::execute() like any simple query.
 * Inbound NOTIFY (server push) is handled in pgsql.h (NotificationResponse), not here.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include "./error.h"

namespace qb::pg::detail {

/// PostgreSQL NOTIFY payload size limit (bytes); see server docs.
inline constexpr std::size_t notify_payload_max_bytes = 8000;

/**
 * @brief Quote a SQL identifier (channel name) using double quotes.
 *
 * Wraps @p ident in double quotes and doubles any embedded double-quote
 * character, producing a delimited identifier safe for interpolation into
 * LISTEN / UNLISTEN / NOTIFY statements.
 *
 * @param ident Identifier to quote.
 * @return The double-quoted, escaped identifier.
 */
[[nodiscard]] std::string quote_notify_identifier(std::string_view ident);

/**
 * @brief Quote a SQL string literal using single quotes.
 *
 * Wraps @p s in single quotes and doubles any embedded single-quote
 * character, producing a string literal safe for interpolation into a
 * NOTIFY payload.
 *
 * @param s String to quote.
 * @return The single-quoted, escaped string literal.
 */
[[nodiscard]] std::string quote_notify_string_literal(std::string_view s);

/**
 * @brief Build a LISTEN statement for the given channel.
 *
 * @param channel Channel name to listen on; quoted as an identifier.
 * @return The complete `LISTEN "<channel>"` statement.
 * @throws error::client_error if @p channel is empty.
 */
[[nodiscard]] std::string build_listen_sql(std::string_view channel);

/**
 * @brief Build an UNLISTEN statement for the given channel.
 *
 * @param channel Channel name to stop listening on; quoted as an identifier.
 * @return The complete `UNLISTEN "<channel>"` statement.
 * @throws error::client_error if @p channel is empty.
 */
[[nodiscard]] std::string build_unlisten_sql(std::string_view channel);

/**
 * @brief Build an `UNLISTEN *` statement that cancels all subscriptions.
 *
 * @return The `UNLISTEN *` statement.
 */
[[nodiscard]] inline std::string
build_unlisten_all_sql() {
    return "UNLISTEN *";
}

/**
 * @brief Build a NOTIFY statement for the given channel and payload.
 *
 * The channel is quoted as an identifier; when @p payload is non-empty it is
 * appended as a quoted string literal.
 *
 * @param channel Channel name to notify; quoted as an identifier.
 * @param payload Optional payload; omitted from the statement when empty.
 * @return The complete `NOTIFY "<channel>"[, '<payload>']` statement.
 * @throws error::client_error if @p channel is empty, or if @p payload exceeds
 *         ::notify_payload_max_bytes.
 */
[[nodiscard]] std::string build_notify_sql(std::string_view channel, std::string_view payload);

} // namespace qb::pg::detail
