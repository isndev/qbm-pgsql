/**
 * @file pg_awaiter.h
 * @brief Coroutine awaiter for qbm-pgsql (Redis-style: callback API + schedule_resume)
 *
 * Bridges the module's callback-based operation API to C++20 coroutines. An
 * operation is handed a `complete(Reply<T>&&)` hook that it must invoke exactly
 * once; the awaiter parks the calling coroutine until that hook fires, then
 * schedules its resumption on the async coroutine scheduler.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#pragma once

#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <qb/io/async.h>

#include "./error.h"
#include "./pg_reply.h"

namespace qb::pg::detail {

/**
 * @brief Completion hook passed into coroutine operations; call exactly once with the outcome.
 */
template <typename T>
using pg_coro_complete = std::function<void(::qb::pg::Reply<T> &&)>;

/**
 * @brief Awaiter: suspends until the wrapped operation invokes the completion callback.
 *
 * @tparam T         Payload type carried by the resulting `::qb::pg::Reply<T>`.
 * @tparam Operation Callable invoked with a `complete(Reply<T>&&)` hook; it must
 *                   call the hook exactly once with the operation outcome.
 *
 * Lifetime safety: a shared `alive` flag is cleared in the destructor so that a
 * completion callback firing after the awaiter has been destroyed (e.g. on
 * cancellation) becomes a no-op instead of a use-after-free.
 */
template <typename T, typename Operation>
class pg_awaiter {
public:
    /**
     * @brief State shared between the awaiter and the completion callback.
     *
     * Held via `shared_ptr` so the callback can safely outlive the awaiter and
     * observe the `alive` flag before touching any member.
     */
    struct Shared {
        std::optional<::qb::pg::Reply<T>> out{};                               ///< Operation result, set on completion.
        std::shared_ptr<bool>             alive{std::make_shared<bool>(true)}; ///< Cleared when the awaiter is destroyed.
        std::coroutine_handle<>           h{};                                 ///< Handle to resume once `out` is set.
    };

private:
    std::shared_ptr<Shared> shared_;
    Operation               operation_;

public:
    /**
     * @brief Construct the awaiter, capturing the operation to launch on suspend.
     * @param op Callable invoked with the completion hook in `await_suspend`.
     */
    explicit pg_awaiter(Operation op)
        : shared_(std::make_shared<Shared>())
        , operation_(std::forward<Operation>(op)) {}

    /**
     * @brief Mark the shared state dead so a late completion callback is a no-op.
     */
    ~pg_awaiter() {
        if (shared_ && shared_->alive)
            *shared_->alive = false;
    }

    pg_awaiter(pg_awaiter const &)                = delete;
    pg_awaiter &operator=(pg_awaiter const &)     = delete;
    pg_awaiter(pg_awaiter &&) noexcept            = default;
    pg_awaiter &operator=(pg_awaiter &&) noexcept = default;

    /**
     * @brief Always suspends so the operation can complete asynchronously.
     * @return Always `false`.
     */
    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }

    /**
     * @brief Store the resume handle and launch the operation.
     * @param handle Coroutine handle to resume once the operation completes.
     *
     * The completion hook captures the shared state; when invoked it stores the
     * reply and schedules `handle` on the async coroutine scheduler, unless the
     * awaiter has already been destroyed.
     */
    void
    await_suspend(std::coroutine_handle<> handle) {
        shared_->h = handle;
        auto s     = shared_;
        auto done  = [s](::qb::pg::Reply<T> &&r) {
            if (!s || !*s->alive)
                return;
            s->out.emplace(std::move(r));
            if (s->h)
                qb::io::async::coro_scheduler().schedule_resume(s->h);
        };
        operation_(std::move(done));
    }

    /**
     * @brief Return the operation result to the resumed coroutine.
     * @return The completed `::qb::pg::Reply<T>`, or a failure reply if the
     *         awaiter was resumed without a result (cancelled or internal error).
     */
    [[nodiscard]] ::qb::pg::Reply<T>
    await_resume() {
        if (!shared_ || !shared_->out.has_value()) {
            return ::qb::pg::Reply<T>::failure(error::db_error{"pgsql coroutine awaiter resumed without result (cancelled or "
                                                               "internal error)"});
        }
        return std::move(*shared_->out);
    }
};

/**
 * @brief Type-erased awaiter for public Transaction / Database coroutine methods.
 */
template <typename Payload>
using pg_reply_awaiter = pg_awaiter<Payload, std::function<void(pg_coro_complete<Payload>)>>;

/**
 * @brief Factory: @p op receives `complete(Reply<T>&&)` and must call it once (like
 * redis_awaiter).
 */
template <typename T, typename Op>
[[nodiscard]] auto
make_pg_awaiter(Op &&op) {
    return pg_awaiter<T, std::remove_cvref_t<Op>>{std::forward<Op>(op)};
}

} // namespace qb::pg::detail
