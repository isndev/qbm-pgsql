/**
 * @file pg_reply.h
 * @brief Coroutine-friendly query result wrapper (Reply<T>) for qbm-pgsql
 */
#pragma once

#include <utility>

#include "./error.h"
#include "./resultset.h"

namespace qb::pg {

/**
 * @brief One-shot operation result: success value or database error
 *
 * Mirrors the spirit of qbm-redis Reply<T>: use `if (r)` or `r.ok()`.
 */
template <typename T>
struct Reply {
    bool            _ok{};
    T               _value{};
    error::db_error _err{"unknown error"};

    [[nodiscard]] static Reply
    success(T value) {
        Reply r;
        r._ok    = true;
        r._value = std::move(value);
        return r;
    }

    [[nodiscard]] static Reply
    failure(error::db_error err) {
        Reply r;
        r._ok  = false;
        r._err = std::move(err);
        return r;
    }

    [[nodiscard]] bool
    ok() const noexcept {
        return _ok;
    }

    [[nodiscard]] explicit
    operator bool() const noexcept {
        return _ok;
    }

    [[nodiscard]] T &
    result() & {
        return _value;
    }

    [[nodiscard]] T &&
    result() && {
        return std::move(_value);
    }

    [[nodiscard]] error::db_error const &
    error() const noexcept {
        return _err;
    }
};

/**
 * @brief Success / failure without a payload (`co_await with_transaction` for `task<void>` bodies).
 */
template <>
struct Reply<void> {
    bool            _ok{};
    error::db_error _err{"unknown error"};

    [[nodiscard]] static Reply
    success() {
        Reply r;
        r._ok = true;
        return r;
    }

    [[nodiscard]] static Reply
    failure(error::db_error err) {
        Reply r;
        r._ok  = false;
        r._err = std::move(err);
        return r;
    }

    [[nodiscard]] bool
    ok() const noexcept {
        return _ok;
    }

    [[nodiscard]] explicit
    operator bool() const noexcept {
        return _ok;
    }

    [[nodiscard]] error::db_error const &
    error() const noexcept {
        return _err;
    }
};

/**
 * @brief Graceful abort for `with_transaction`: roll back and map to `Reply::failure` (no
 * rethrow).
 *
 * Use when a coroutine body decides a statement failed (`!reply.ok()`) and the transaction must
 * not commit: `throw transaction_abort{reply.error()};`
 */
struct transaction_abort {
    error::db_error err;

    explicit transaction_abort(error::db_error e)
        : err(std::move(e)) {}
};

} // namespace qb::pg
