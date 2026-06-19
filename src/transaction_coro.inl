/**
 * @file transaction_coro.inl
 * @brief No-callback overloads: same names as the callback API, return awaiters (included from transaction.inl)
 *
 * Coroutine helpers delegate to the callback overloads on Transaction (see transaction.inl). LISTEN/NOTIFY
 * coroutine paths share pg_execute_void_sql / pg_try_build_and_execute_void_sql below.
 */

#pragma once

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "./pg_awaiter.h"
#include "./pg_notify_sql.h"

namespace {

/** Append `; SET LOCAL statement_timeout = N` (ms) when @p timeout_ms &gt; 0. */
inline void
pg_append_set_local_statement_timeout(std::string &sql, qb::duration timeout) {
    const auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
    if (timeout_ms <= 0)
        return;
    sql += "; SET LOCAL statement_timeout = ";
    sql += std::to_string(timeout_ms);
}

[[nodiscard]] inline bool
pg_savepoint_name_ok(std::string_view name) noexcept {
    if (name.empty() || name.size() > 63)
        return false;
    for (unsigned char const c : name) {
        if (std::isalnum(c) != 0 || c == '_')
            continue;
        return false;
    }
    return true;
}

[[nodiscard]] inline pg_reply_awaiter<resultset>
pg_fail_resultset(error::db_error err) {
    return pg_reply_awaiter<resultset>{[e = std::move(err)](pg_coro_complete<resultset> complete) mutable {
        complete(::qb::pg::Reply<resultset>::failure(std::move(e)));
    }};
}

[[nodiscard]] inline pg_reply_awaiter<void>
pg_fail_void(error::db_error err) {
    return pg_reply_awaiter<void>{[e = std::move(err)](pg_coro_complete<void> complete) mutable {
        complete(::qb::pg::Reply<void>::failure(std::move(e)));
    }};
}

/** Simple query that returns no meaningful rowset: completion → Reply<void>. */
[[nodiscard]] inline pg_reply_awaiter<void>
pg_execute_void_sql(Transaction *self, std::string sql) {
    return pg_reply_awaiter<void>{[self, sql = std::move(sql)](pg_coro_complete<void> complete) mutable {
        self->execute(
            std::string_view(sql), [complete](Transaction &, resultset) mutable { complete(::qb::pg::Reply<void>::success()); },
            [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<void>::failure(e)); });
    }};
}

template <typename Build>
[[nodiscard]] inline pg_reply_awaiter<void>
pg_try_build_and_execute_void_sql(Transaction *self, Build &&build) {
    std::string sql;
    try {
        sql = std::forward<Build>(build)();
    } catch (error::db_error const &e) {
        return pg_fail_void(e);
    }
    return pg_execute_void_sql(self, std::move(sql));
}

} // namespace

inline pg_reply_awaiter<resultset>
Transaction::execute(std::string_view expr) {
    return pg_reply_awaiter<resultset>{[this, sql = std::string(expr)](pg_coro_complete<resultset> complete) {
        this->execute(
            std::string_view(sql),
            [complete](Transaction &, resultset rs) mutable { complete(::qb::pg::Reply<resultset>::success(rs.deep_snapshot())); },
            [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<resultset>::failure(e)); });
    }};
}

inline pg_reply_awaiter<PreparedQuery>
Transaction::prepare(std::string_view query_name, std::string_view expr, type_oid_sequence types) {
    return pg_reply_awaiter<PreparedQuery>{[this, qn = std::string(query_name), ex = std::string(expr),
                                            t = std::move(types)](pg_coro_complete<PreparedQuery> complete) mutable {
        this->prepare(
            std::string_view(qn), std::string_view(ex), std::move(t),
            [complete](Transaction &, PreparedQuery const &pq) mutable {
                complete(::qb::pg::Reply<PreparedQuery>::success(PreparedQuery{pq}));
            },
            [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<PreparedQuery>::failure(e)); });
    }};
}

inline pg_reply_awaiter<resultset>
Transaction::execute(std::string_view query_name, QueryParams &&params) {
    return pg_reply_awaiter<resultset>{[this, qn = std::string(query_name),
                                        p = std::move(params)](pg_coro_complete<resultset> complete) mutable {
        this->execute(
            std::string_view(qn), std::move(p),
            [complete](Transaction &, resultset rs) mutable { complete(::qb::pg::Reply<resultset>::success(rs.deep_snapshot())); },
            [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<resultset>::failure(e)); });
    }};
}

inline pg_reply_awaiter<resultset>
Transaction::begin(transaction_mode mode) {
    std::string sql = "BEGIN ";
    sql += to_string(mode);
    pg_append_set_local_statement_timeout(sql, get_timeout());
    return execute(std::string_view(sql));
}

inline pg_reply_awaiter<resultset>
Transaction::begin() {
    return begin(transaction_mode{});
}

inline pg_reply_awaiter<resultset>
Transaction::commit() {
    return execute("COMMIT");
}

inline pg_reply_awaiter<resultset>
Transaction::rollback() {
    return execute("ROLLBACK");
}

inline pg_reply_awaiter<resultset>
Transaction::savepoint(std::string_view name) {
    if (!pg_savepoint_name_ok(name))
        return pg_fail_resultset(error::client_error{"invalid savepoint name (use non-empty alphanumeric/underscore, max 63)"});
    std::string sql = "SAVEPOINT ";
    sql.append(name);
    return execute(std::string_view(sql));
}

inline pg_reply_awaiter<resultset>
Transaction::rollback_savepoint(std::string_view name) {
    if (!pg_savepoint_name_ok(name))
        return pg_fail_resultset(error::client_error{"invalid savepoint name (use non-empty alphanumeric/underscore, max 63)"});
    std::string sql = "ROLLBACK TO SAVEPOINT ";
    sql.append(name);
    return execute(std::string_view(sql));
}

inline pg_reply_awaiter<resultset>
Transaction::release_savepoint(std::string_view name) {
    if (!pg_savepoint_name_ok(name))
        return pg_fail_resultset(error::client_error{"invalid savepoint name (use non-empty alphanumeric/underscore, max 63)"});
    std::string sql = "RELEASE SAVEPOINT ";
    sql.append(name);
    return execute(std::string_view(sql));
}

inline pg_reply_awaiter<resultset>
Transaction::execute_file(const std::filesystem::path &file_path) {
    return pg_reply_awaiter<resultset>{[this, path = file_path](pg_coro_complete<resultset> complete) mutable {
        try {
            if (!std::filesystem::exists(path)) {
                complete(::qb::pg::Reply<resultset>::failure(error::query_error("SQL file not found: " + path.string())));
                return;
            }
            std::ifstream file(path);
            if (!file.is_open()) {
                complete(::qb::pg::Reply<resultset>::failure(error::query_error("Cannot open SQL file: " + path.string())));
                return;
            }
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string sql_query = buffer.str();
            this->execute(
                std::string_view(sql_query),
                [complete](Transaction &, resultset rs) mutable { complete(::qb::pg::Reply<resultset>::success(rs.deep_snapshot())); },
                [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<resultset>::failure(e)); });
        } catch (std::exception const &e) {
            complete(::qb::pg::Reply<resultset>::failure(error::query_error("Error reading SQL file: " + std::string(e.what()))));
        }
    }};
}

inline pg_reply_awaiter<PreparedQuery>
Transaction::prepare_file(std::string_view query_name, const std::filesystem::path &file_path, type_oid_sequence types) {
    return pg_reply_awaiter<PreparedQuery>{[this, qn = std::string(query_name), path = file_path,
                                            t = std::move(types)](pg_coro_complete<PreparedQuery> complete) mutable {
        try {
            if (!std::filesystem::exists(path)) {
                complete(::qb::pg::Reply<PreparedQuery>::failure(error::query_error("SQL file not found: " + path.string())));
                return;
            }
            std::ifstream file(path);
            if (!file.is_open()) {
                complete(::qb::pg::Reply<PreparedQuery>::failure(error::query_error("Cannot open SQL file: " + path.string())));
                return;
            }
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string sql_query = buffer.str();
            this->prepare(
                std::string_view(qn), std::string_view(sql_query), std::move(t),
                [complete](Transaction &, PreparedQuery const &pq) mutable {
                    complete(::qb::pg::Reply<PreparedQuery>::success(PreparedQuery{pq}));
                },
                [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<PreparedQuery>::failure(e)); });
        } catch (std::exception const &e) {
            complete(::qb::pg::Reply<PreparedQuery>::failure(error::query_error("Error reading SQL file: " + std::string(e.what()))));
        }
    }};
}

inline pg_reply_awaiter<void>
Transaction::notify(std::string_view channel, std::string_view payload) {
    return pg_try_build_and_execute_void_sql(this, [channel, payload] { return build_notify_sql(channel, payload); });
}

inline pg_reply_awaiter<void>
Transaction::listen(std::string_view channel) {
    return pg_try_build_and_execute_void_sql(this, [channel] { return build_listen_sql(channel); });
}

inline pg_reply_awaiter<void>
Transaction::unlisten(std::string_view channel) {
    return pg_try_build_and_execute_void_sql(this, [channel] { return build_unlisten_sql(channel); });
}

inline pg_reply_awaiter<void>
Transaction::unlisten_all() {
    return pg_execute_void_sql(this, std::string(build_unlisten_all_sql()));
}
