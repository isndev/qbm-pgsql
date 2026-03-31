/**
 * @file pg_awaiter.h
 * @brief Coroutine awaiter for qbm-pgsql (Redis-style: callback API + schedule_resume)
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
 */
template <typename T, typename Operation>
class pg_awaiter {
public:
    struct Shared {
        std::optional<::qb::pg::Reply<T>> out{};
        std::shared_ptr<bool>               alive{std::make_shared<bool>(true)};
        std::coroutine_handle<>             h{};
    };

private:
    std::shared_ptr<Shared> shared_;
    Operation               operation_;

public:
    explicit pg_awaiter(Operation op)
        : shared_(std::make_shared<Shared>())
        , operation_(std::forward<Operation>(op)) {}

    ~pg_awaiter() {
        if (shared_ && shared_->alive)
            *shared_->alive = false;
    }

    pg_awaiter(pg_awaiter const &)                = delete;
    pg_awaiter &operator=(pg_awaiter const &)     = delete;
    pg_awaiter(pg_awaiter &&) noexcept            = default;
    pg_awaiter &operator=(pg_awaiter &&) noexcept = default;

    [[nodiscard]] bool
    await_ready() const noexcept {
        return false;
    }

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

    [[nodiscard]] ::qb::pg::Reply<T>
    await_resume() {
        if (!shared_ || !shared_->out.has_value()) {
            return ::qb::pg::Reply<T>::failure(
                error::db_error{"pgsql coroutine awaiter resumed without result (cancelled or "
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
