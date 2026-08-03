/**
 * @file pg_reply.h
 * @brief Coroutine-friendly query result wrapper (Reply<T>) for qbm-pgsql
 *
 * Defines `Reply<T>`, a lightweight success-or-error result type returned by the
 * coroutine query API. It carries either a value of type `T` or a
 * @ref qb::pg::error::db_error, and offers `std::expected`-style ergonomics
 * (`operator bool`, `operator*`, `value_or`, `and_then`, `transform`, `or_else`)
 * in addition to the explicit `ok()`/`result()`/`error()` accessors. A
 * `Reply<void>` specialization covers payload-less results, and
 * @ref qb::pg::transaction_abort lets a transaction body request a graceful
 * rollback that is surfaced as a `Reply::failure`.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#pragma once

#include <type_traits>
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
    bool            _ok{};                 ///< True when the result holds a value.
    T               _value{};              ///< The payload (valid only when `_ok`).
    error::db_error _err{"unknown error"}; ///< The error (valid only when `!_ok`).

    /**
     * @brief Build a successful result holding @p value.
     * @param value The payload to move into the result.
     * @return A `Reply` with `ok() == true`.
     */
    [[nodiscard]] static Reply
    success(T value) {
        Reply r;
        r._ok    = true;
        r._value = std::move(value);
        return r;
    }

    /**
     * @brief Build a failed result carrying @p err.
     * @param err The database error to move into the result.
     * @return A `Reply` with `ok() == false`.
     */
    [[nodiscard]] static Reply
    failure(error::db_error err) {
        Reply r;
        r._ok  = false;
        r._err = std::move(err);
        return r;
    }

    /// @return True if the operation succeeded.
    [[nodiscard]] bool
    ok() const noexcept {
        return _ok;
    }

    /// @return True if the operation succeeded (enables `if (r)`).
    [[nodiscard]] explicit
    operator bool() const noexcept {
        return _ok;
    }

    /**
     * @brief Access the payload by reference (does not check `ok()`).
     * @return Reference to the stored value.
     */
    [[nodiscard]] T &
    result() & {
        return _value;
    }

    /**
     * @brief Move the payload out of an rvalue result (does not check `ok()`).
     * @return Rvalue reference to the stored value.
     */
    [[nodiscard]] T &&
    result() && {
        return std::move(_value);
    }

    /// @return The stored database error (meaningful only when `!ok()`).
    [[nodiscard]] error::db_error const &
    error() const noexcept {
        return _err;
    }

    // --- std::expected-style ergonomics (additive; the .ok()/.result()/.error()
    // API above is unchanged). Value access is non-throwing — check ok() first. ---

    /// @return True if a value is present (alias for `ok()`).
    [[nodiscard]] bool
    has_value() const noexcept {
        return _ok;
    }
    /// @return Reference to the value (non-throwing; check `ok()` first).
    [[nodiscard]] T &
    operator*() & noexcept {
        return _value;
    }
    /// @return Const reference to the value (non-throwing; check `ok()` first).
    [[nodiscard]] T const &
    operator*() const & noexcept {
        return _value;
    }
    /// @return Rvalue reference to the value (non-throwing; check `ok()` first).
    [[nodiscard]] T &&
    operator*() && noexcept {
        return std::move(_value);
    }
    /// @return Pointer to the value for member access (check `ok()` first).
    [[nodiscard]] T *
    operator->() noexcept {
        return &_value;
    }
    /// @return Const pointer to the value for member access (check `ok()` first).
    [[nodiscard]] T const *
    operator->() const noexcept {
        return &_value;
    }

    /**
     * @brief Return the value if `ok()`, otherwise @p fallback.
     * @param fallback Value converted to `T` and returned on error.
     * @return The stored value, or @p fallback.
     */
    template <typename U>
    [[nodiscard]] T
    value_or(U &&fallback) const & {
        return _ok ? _value : static_cast<T>(std::forward<U>(fallback));
    }
    /// @copydoc value_or
    template <typename U>
    [[nodiscard]] T
    value_or(U &&fallback) && {
        return _ok ? std::move(_value) : static_cast<T>(std::forward<U>(fallback));
    }

    /// Monadic bind: @p f takes the value and returns a `Reply<U>`; on error the
    /// error is propagated as `Reply<U>::failure`.
    template <typename F>
    [[nodiscard]] auto
    and_then(F &&f) const & {
        using R = std::invoke_result_t<F, T const &>;
        return _ok ? std::forward<F>(f)(_value) : R::failure(_err);
    }
    template <typename F>
    [[nodiscard]] auto
    and_then(F &&f) && {
        using R = std::invoke_result_t<F, T &&>;
        return _ok ? std::forward<F>(f)(std::move(_value)) : R::failure(std::move(_err));
    }

    /// Functor map: @p f takes the value and returns a plain `U`; the result is
    /// wrapped as `Reply<U>::success`, errors propagate as `Reply<U>::failure`.
    template <typename F>
    [[nodiscard]] auto
    transform(F &&f) const & {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T const &>>;
        return _ok ? Reply<U>::success(std::forward<F>(f)(_value)) : Reply<U>::failure(_err);
    }
    template <typename F>
    [[nodiscard]] auto
    transform(F &&f) && {
        using U = std::remove_cvref_t<std::invoke_result_t<F, T &&>>;
        return _ok ? Reply<U>::success(std::forward<F>(f)(std::move(_value))) : Reply<U>::failure(std::move(_err));
    }

    /// Recover from error: @p f takes the db_error and returns a `Reply<T>`.
    template <typename F>
    [[nodiscard]] Reply
    or_else(F &&f) const & {
        return _ok ? *this : std::forward<F>(f)(_err);
    }
    template <typename F>
    [[nodiscard]] Reply
    or_else(F &&f) && {
        return _ok ? std::move(*this) : std::forward<F>(f)(std::move(_err));
    }
};

/**
 * @brief Success / failure without a payload (`co_await with_transaction` for `task<void>` bodies).
 */
template <>
struct Reply<void> {
    bool            _ok{};                 ///< True when the operation succeeded.
    error::db_error _err{"unknown error"}; ///< The error (valid only when `!_ok`).

    /// @return A successful payload-less result (`ok() == true`).
    [[nodiscard]] static Reply
    success() {
        Reply r;
        r._ok = true;
        return r;
    }

    /**
     * @brief Build a failed result carrying @p err.
     * @param err The database error to move into the result.
     * @return A `Reply<void>` with `ok() == false`.
     */
    [[nodiscard]] static Reply
    failure(error::db_error err) {
        Reply r;
        r._ok  = false;
        r._err = std::move(err);
        return r;
    }

    /// @return True if the operation succeeded.
    [[nodiscard]] bool
    ok() const noexcept {
        return _ok;
    }

    /// @return True if the operation succeeded (enables `if (r)`).
    [[nodiscard]] explicit
    operator bool() const noexcept {
        return _ok;
    }

    /// @return The stored database error (meaningful only when `!ok()`).
    [[nodiscard]] error::db_error const &
    error() const noexcept {
        return _err;
    }

    /// @return True if the operation succeeded (alias for `ok()`).
    [[nodiscard]] bool
    has_value() const noexcept {
        return _ok;
    }
    /// Monadic bind: @p f takes no argument and returns a `Reply<U>`.
    template <typename F>
    [[nodiscard]] auto
    and_then(F &&f) const {
        using R = std::invoke_result_t<F>;
        return _ok ? std::forward<F>(f)() : R::failure(_err);
    }
    /// Recover from error: @p f takes the db_error and returns a `Reply<void>`.
    template <typename F>
    [[nodiscard]] Reply
    or_else(F &&f) const {
        return _ok ? *this : std::forward<F>(f)(_err);
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
    error::db_error err; ///< Error reported as the transaction's `Reply::failure`.

    /**
     * @brief Construct an abort request carrying @p e.
     * @param e The database error to surface to the caller after rollback.
     */
    explicit transaction_abort(error::db_error e)
        : err(std::move(e)) {}
};

} // namespace qb::pg
