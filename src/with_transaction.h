/**
 * @file with_transaction.h
 * @brief Coroutine sugar: `co_await with_transaction(tr, body)` — BEGIN, body, COMMIT or
 *
 *        ROLLBACK.
 *
 * Included from `pgsql.h`. Requires `<qb/io/async/coroutine.h>` usage in the translation
 * unit.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#pragma once

#include <concepts>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

#include <qb/io/async/coroutine.h>
#include <qb/utility/compat.h>

#include "./common.h"
#include "./pg_reply.h"
#include "./transaction.h"

namespace qb::pg::detail {

/**
 * @brief Extracts the value type carried by a `qb::io::async::task<T>`.
 *
 * Primary template is intentionally left undefined; only the `task<T>` specialization is
 * valid, so any non-`task` argument fails substitution.
 *
 * @tparam Task An instantiation of `qb::io::async::task`.
 */
template <typename Task>
struct pg_task_result;

/**
 * @brief Specialization yielding the awaited value type of a `qb::io::async::task<T>`.
 *
 * @tparam T The value produced by the task (`type == T`).
 */
template <typename T>
struct pg_task_result<qb::io::async::task<T>> {
    using type = T;
};

/**
 * @brief Value type produced by calling @p F with a `Transaction&`.
 *
 * Equals `T` where `f(tr)` returns `qb::io::async::task<T>` (`void` for `task<void>`).
 *
 * @tparam F A callable invocable as `f(Transaction&)` returning a `qb::io::async::task`.
 */
template <typename F>
using pg_with_transaction_value_t = typename pg_task_result<std::invoke_result_t<std::decay_t<F> &, Transaction &>>::type;

/**
 * @brief Constrains @p F to a transaction body usable by `with_transaction`.
 *
 * Satisfied when @p F is invocable as `f(Transaction&)` and the result is a
 * `qb::io::async::task<T>` (so `pg_with_transaction_value_t<F>` is well-formed).
 *
 * @tparam F The candidate transaction-body callable.
 */
template <typename F>
concept pg_with_transaction_fn = std::invocable<std::decay_t<F> &, Transaction &>
                                 && requires { typename pg_task_result<std::invoke_result_t<std::decay_t<F> &, Transaction &>>::type; };

/**
 * @brief Shared implementation: `co_await begin_op(tr)`, run `f(tr)`, then COMMIT / ROLLBACK.
 *
 * `BeginOp` is invoked as `begin_op(tr)` and must return the same awaitable as `tr.begin()` /
 * `tr.begin(mode)` (`pg_reply_awaiter<resultset>`).
 */
template <typename T, typename F, typename BeginOp>
qb::io::async::task<::qb::pg::Reply<T>>
with_transaction_impl(Transaction &tr, F &&f, BeginOp &&begin_op) {
    auto b = co_await std::invoke(std::forward<BeginOp>(begin_op), tr);
    if (!b.ok()) {
        co_return ::qb::pg::Reply<T>::failure(b.error());
    }

    enum class Catch : std::uint8_t { none, transaction_abort, rethrow };
    Catch                          which = Catch::none;
    std::optional<error::db_error> abort_err;
    std::exception_ptr             rethrow_ex;

    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::invoke(std::forward<F>(f), tr);
        } else {
            T    value = co_await std::invoke(std::forward<F>(f), tr);
            auto c     = co_await tr.commit();
            if (!c.ok()) {
                (void) co_await tr.rollback();
                co_return ::qb::pg::Reply<T>::failure(c.error());
            }
            co_return ::qb::pg::Reply<T>::success(std::move(value));
        }
    } catch (::qb::pg::transaction_abort const &ab) {
        which     = Catch::transaction_abort;
        abort_err = ab.err;
    } catch (...) {
        which      = Catch::rethrow;
        rethrow_ex = std::current_exception();
    }

    if (which == Catch::transaction_abort) {
        (void) co_await tr.rollback();
        co_return ::qb::pg::Reply<T>::failure(std::move(*abort_err));
    }
    if (which == Catch::rethrow) {
        (void) co_await tr.rollback();
        std::rethrow_exception(rethrow_ex);
    }

    if constexpr (std::is_void_v<T>) {
        auto c = co_await tr.commit();
        if (!c.ok()) {
            (void) co_await tr.rollback();
            co_return ::qb::pg::Reply<void>::failure(c.error());
        }
        co_return ::qb::pg::Reply<void>::success();
    } else {
        qb::unreachable();
    }
}

} // namespace qb::pg::detail

namespace qb::pg {

/**
 * @brief Run a coroutine body inside a single SQL transaction (BEGIN → body → COMMIT).
 *
 * On success the body’s `task<T>` result is wrapped in `Reply<T>::success`. On `begin` failure,
 * `commit` failure, `transaction_abort`, or a C++ exception after `begin`, the client issues
 * `ROLLBACK` (best-effort) before returning or rethrowing.
 *
 * If an awaited `execute` / `query` returns `!ok()`, throw `transaction_abort{reply.error()}`
 * so the scope rolls back and you get a failed `Reply` instead of attempting `COMMIT` on an
 * aborted transaction.
 *
 * @param tr Connection / transaction object (`tcp::database&`, etc.).
 * @param f Callable such that `f(tr)` returns `qb::io::async::task<T>` (or `task<void>`).
 */
template <detail::pg_with_transaction_fn F>
[[nodiscard]] inline qb::io::async::task<Reply<detail::pg_with_transaction_value_t<F>>>
with_transaction(detail::Transaction &tr, F &&f) {
    using T = detail::pg_with_transaction_value_t<F>;
    co_return co_await detail::with_transaction_impl<T>(tr, std::forward<F>(f), [](detail::Transaction &t) { return t.begin(); });
}

/**
 * @brief Same as `with_transaction(tr, f)` but opens the block with `BEGIN` using @p mode
 *        (isolation, read-only, deferrable).
 */
template <detail::pg_with_transaction_fn F>
[[nodiscard]] inline qb::io::async::task<Reply<detail::pg_with_transaction_value_t<F>>>
with_transaction(detail::Transaction &tr, transaction_mode mode, F &&f) {
    using T = detail::pg_with_transaction_value_t<F>;
    co_return co_await detail::with_transaction_impl<T>(tr, std::forward<F>(f), [mode](detail::Transaction &t) { return t.begin(mode); });
}

} // namespace qb::pg
