/**
 * @file stats.h
 * @brief What one PostgreSQL connection has done: `qb::pg::stats`, and the per-query report its hook receives
 *
 * `database::stats()` returns a `qb::pg::stats` snapshot by value: monotonic `std::uint64_t` counts since the
 * object was constructed (a reconnect does not reset them) plus the duration of the last round trip.
 * `database::on_query_complete(fn)` installs a hook that receives one `qb::pg::query_info` per completed
 * query; without one the client pays a single predictable branch per completion (Huly QB-115).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#pragma once

#include <cstdint>
#include <string_view>
#include <qb/system/time.h>

#include "./error.h"

namespace qb::pg {

/**
 * @brief The kind of round trip a completed query was (`query_info::kind`).
 *
 * One query is one round trip of the connection's serial queue. The inline `query(sql, args...)`
 * is two: a `prepare` of the unnamed statement, then its `execute`.
 */
enum class query_kind : std::uint8_t {
    simple,             ///< simple-query protocol: `execute(sql)` / `query(sql)`, LISTEN / NOTIFY, COPY, a stream's cursor SQL
    prepare,            ///< Parse + Describe: `prepare()`, the first half of an inline `query(sql, args...)`
    execute,            ///< Bind + Execute of a prepared statement: `execute(name, params)`, the inline query's second half
    begin,              ///< BEGIN (with its transaction mode and statement timeout)
    commit,             ///< COMMIT
    rollback,           ///< ROLLBACK
    savepoint,          ///< SAVEPOINT name
    release_savepoint,  ///< RELEASE SAVEPOINT name
    rollback_savepoint, ///< ROLLBACK TO SAVEPOINT name
};

/**
 * @brief One completed query, as `database::on_query_complete` reports it.
 *
 * The views are valid only during the call: copy what you keep. `sql` is the text the client
 * sent or prepared and may carry literal values — a password in `ALTER ROLE ... PASSWORD`, personal
 * data in an ad-hoc INSERT: never log it raw.
 */
struct query_info {
    /// What kind of round trip it was.
    query_kind kind{};
    /// The statement (prepare / execute) or savepoint name; empty otherwise, and for the unnamed statement.
    std::string_view name;
    /// The SQL text when the client has it -- a simple query, a prepare, an execute's prepared statement;
    /// empty for begin / commit / rollback and the savepoint kinds.
    std::string_view sql;
    /// From the message's send to the query's completion; zero for a query that failed before it was sent.
    qb::duration duration{};
    /// Whether the query succeeded.
    bool ok = false;
    /// The failure when `!ok` (server, client or connection error); null on success.
    ::qb::pg::error::db_error const *error = nullptr;
};

/**
 * @brief A snapshot of what one connection has done (`database::stats()`).
 *
 * Every count is monotonic since the `database` object was constructed: two snapshots give a rate,
 * and a reconnect resets nothing. A query is one round trip of the serial queue (see `query_kind`);
 * one that never left the client — refused locally, or failed by a disconnect while still queued — is
 * counted as a query AND as an error. A call refused at submission on a disconnected handle never
 * entered the queue and is not counted.
 */
struct stats {
    std::uint64_t queries                = 0; ///< Queries completed, successfully or not
    std::uint64_t errors                 = 0; ///< Of `queries`, those that failed
    std::uint64_t bytes_sent             = 0; ///< PostgreSQL protocol bytes the client queued for the server (TLS framing excluded)
    std::uint64_t bytes_received         = 0; ///< PostgreSQL protocol bytes received from the server (TLS framing excluded)
    std::uint64_t notifications_received = 0; ///< NotificationResponse messages received
    std::uint64_t notifications_dropped  = 0; ///< Notifications a `notify_co_consumer` could not queue (its bounded channel was full)
    std::uint64_t prepared_hits          = 0; ///< Named executions found in the prepared-statement cache
    std::uint64_t prepared_misses        = 0; ///< Named executions refused because the name was not cached
    std::uint64_t prepared_evicted       = 0; ///< Statements the cache evicted (and closed on the server)
    std::uint64_t reconnects = 0;        ///< Ready connections reached after a lost one (an explicit `disconnect()` ends the session: none)
    std::uint64_t gaps       = 0;        ///< Gap markers a notify consumer emitted: after a reconnection, or where its queue overflowed
    qb::duration  last_query_duration{}; ///< Send-to-completion time of the last query that was sent
};

} // namespace qb::pg
