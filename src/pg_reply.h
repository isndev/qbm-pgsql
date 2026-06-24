/**
 * @file pg_reply.h
 * @brief Coroutine-friendly query result wrapper (Reply<T>) for qbm-pgsql
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

    // --- std::expected-style ergonomics (additive; the .ok()/.result()/.error()
    // API above is unchanged). Value access is non-throwing — check ok() first. ---

    [[nodiscard]] bool
    has_value() const noexcept {
        return _ok;
    }
    [[nodiscard]] T &
    operator*() & noexcept {
        return _value;
    }
    [[nodiscard]] T const &
    operator*() const & noexcept {
        return _value;
    }
    [[nodiscard]] T &&
    operator*() && noexcept {
        return std::move(_value);
    }
    [[nodiscard]] T *
    operator->() noexcept {
        return &_value;
    }
    [[nodiscard]] T const *
    operator->() const noexcept {
        return &_value;
    }

    /// The value if ok, else @p fallback.
    template <typename U>
    [[nodiscard]] T
    value_or(U &&fallback) const & {
        return _ok ? _value : static_cast<T>(std::forward<U>(fallback));
    }
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
    error::db_error err;

    explicit transaction_abort(error::db_error e)
        : err(std::move(e)) {}
};

} // namespace qb::pg
