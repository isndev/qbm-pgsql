/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

/**
 * @file integration/api/copy-owner.cpp
 * @brief A COPY callback belongs to its admitted command on one connection.
 *
 * Tier: integration (REQUIRES live postgres). Both tasks start before either
 * server response can be processed; the second COPY must resolve explicitly
 * without replacing the first command's sink or source.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <cstddef>
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../../shared/pg_integration_fixture.hpp"

namespace copy_owner_test {

using qb::pg::Reply;
using qb::pg::resultset;
using database = qb::pg::tcp::database;

qb::io::async::task<Reply<resultset>>
copy_out_once(database *db, std::string sql, std::string *sink, int *completed) {
    auto reply = co_await db->copy_out(std::move(sql), [sink](std::string_view bytes) { sink->append(bytes); });
    ++*completed;
    co_return reply;
}

qb::io::async::task<Reply<resultset>>
copy_in_once(database *db, std::string sql, std::string payload, int *source_calls, int *completed) {
    auto reply = co_await db->copy_in(std::move(sql),
                                      [payload = std::move(payload), source_calls, sent = false]() mutable -> std::optional<std::string> {
                                          ++*source_calls;
                                          if (sent)
                                              return std::nullopt;
                                          sent = true;
                                          return std::move(payload);
                                      });
    ++*completed;
    co_return reply;
}

qb::io::async::task<void>
copy_out_detached(database *db, std::string *sink, bool *resumed) {
    (void) co_await db->copy_out("COPY (SELECT 'orphan') TO STDOUT", [sink](std::string_view bytes) { sink->append(bytes); });
    *resumed = true;
    co_return;
}

qb::io::async::task<void>
observe_copy_out(database *db, std::string *sink, bool *done, bool *ok) {
    auto reply = co_await db->copy_out("COPY (SELECT 'other') TO STDOUT", [sink](std::string_view bytes) { sink->append(bytes); });
    *ok        = reply.ok();
    *done      = true;
    co_return;
}

qb::io::async::task<void>
copy_in_detached(database *db, int *source_calls, bool *resumed) {
    (void) co_await db->copy_in("COPY qb_copy_owner_cancelled FROM STDIN",
                                [source_calls, sent = false]() mutable -> std::optional<std::string> {
                                    ++*source_calls;
                                    if (sent)
                                        return std::nullopt;
                                    sent = true;
                                    return "orphan\n";
                                });
    *resumed = true;
    co_return;
}

qb::io::async::task<void>
observe_copy_in(database *db, int *source_calls, bool *done, bool *ok) {
    auto reply = co_await db->copy_in("COPY qb_copy_owner_cancelled FROM STDIN", [source_calls]() -> std::optional<std::string> {
        ++*source_calls;
        return std::nullopt;
    });
    *ok        = reply.ok();
    *done      = true;
    co_return;
}

qb::io::async::task<void>
copy_in_source_cancels_waiter(database *db, std::coroutine_handle<> *handle, int *source_calls, bool *resumed) {
    (void) co_await db->copy_in("COPY qb_copy_owner_self_cancel FROM STDIN", [handle, source_calls]() -> std::optional<std::string> {
        ++*source_calls;
        if (*source_calls == 1) {
            qb::io::async::coro_scheduler().cancel_spawned(*handle);
            return "orphan\n";
        }
        return std::nullopt;
    });
    *resumed = true;
    co_return;
}

qb::io::async::task<void>
copy_out_sink_cancels_waiter(database *db, std::coroutine_handle<> *handle, std::shared_ptr<int> lifetime, int *sink_calls,
                             bool *capture_retained, bool *resumed, bool disconnect) {
    qb::io::async::pin_frame_copy(lifetime);
    (void) co_await db->copy_out("COPY (SELECT 'orphan' FROM generate_series(1, 3)) TO STDOUT",
                                 [db, handle, sink_calls, capture_retained, lifetime = std::move(lifetime), disconnect](std::string_view) {
                                     ++*sink_calls;
                                     qb::io::async::coro_scheduler().cancel_spawned(*handle);
                                     if (disconnect)
                                         db->disconnect();
                                     // Both the waiting frame and, on disconnect, the admitted
                                     // command have gone. This invocation still owns its captures.
                                     *capture_retained = *lifetime == 629;
                                 });
    *resumed = true;
    co_return;
}

qb::io::async::task<void>
copy_out_with_sink(database *db, std::function<void(std::string_view)> sink, bool *resumed, bool *ok) {
    auto reply = co_await db->copy_out("COPY (SELECT 'owner' FROM generate_series(1, 3)) TO STDOUT", std::move(sink));
    *ok        = reply.ok();
    *resumed   = true;
    co_return;
}

class CopyOwner : public qb::pg::test::PgIntegrationTest {};

TEST_F(CopyOwner, OverlappingCopyOutKeepsFirstSinkAndRejectsSecond) {
    std::string first_bytes, second_bytes;
    int         first_completed = 0, second_completed = 0;
    auto        replies = qb::io::async::run_sync(qb::io::async::when_all(
        copy_out_once(db_.get(), "COPY (SELECT 'first' FROM generate_series(1, 3)) TO STDOUT", &first_bytes, &first_completed),
        copy_out_once(db_.get(), "COPY (SELECT 'second' FROM generate_series(1, 2)) TO STDOUT", &second_bytes, &second_completed)));

    EXPECT_TRUE(std::get<0>(replies).ok());
    EXPECT_FALSE(std::get<1>(replies).ok());
    EXPECT_EQ(first_bytes, "first\nfirst\nfirst\n");
    EXPECT_TRUE(second_bytes.empty());
    EXPECT_EQ(first_completed, 1);
    EXPECT_EQ(second_completed, 1);

    auto after = qb::io::async::run_sync(
        db_->copy_out("COPY (SELECT 'second') TO STDOUT", [&second_bytes](std::string_view bytes) { second_bytes.append(bytes); }));
    EXPECT_TRUE(after.ok());
    EXPECT_EQ(second_bytes, "second\n");
}

TEST_F(CopyOwner, OverlappingCopyInKeepsFirstSourceAndRejectsSecond) {
    ASSERT_TRUE(qb::io::async::run_sync(db_->query("CREATE TEMP TABLE qb_copy_owner_first (v text)")).ok());
    ASSERT_TRUE(qb::io::async::run_sync(db_->query("CREATE TEMP TABLE qb_copy_owner_second (v text)")).ok());

    int  first_calls = 0, second_calls = 0, first_completed = 0, second_completed = 0;
    auto replies = qb::io::async::run_sync(
        qb::io::async::when_all(copy_in_once(db_.get(), "COPY qb_copy_owner_first FROM STDIN", "first\n", &first_calls, &first_completed),
                                copy_in_once(db_.get(), "COPY qb_copy_owner_second FROM STDIN", "second\n", &second_calls, &second_completed)));

    EXPECT_TRUE(std::get<0>(replies).ok());
    EXPECT_FALSE(std::get<1>(replies).ok());
    EXPECT_EQ(first_calls, 2);
    EXPECT_EQ(second_calls, 0);
    EXPECT_EQ(first_completed, 1);
    EXPECT_EQ(second_completed, 1);

    std::string first_bytes, second_bytes;
    auto        first = qb::io::async::run_sync(
        db_->copy_out("COPY qb_copy_owner_first TO STDOUT", [&first_bytes](std::string_view bytes) { first_bytes.append(bytes); }));
    auto second = qb::io::async::run_sync(
        db_->copy_out("COPY qb_copy_owner_second TO STDOUT", [&second_bytes](std::string_view bytes) { second_bytes.append(bytes); }));
    EXPECT_TRUE(first.ok());
    EXPECT_TRUE(second.ok());
    EXPECT_EQ(first_bytes, "first\n");
    EXPECT_TRUE(second_bytes.empty());

    auto after = qb::io::async::run_sync(db_->copy_in("COPY qb_copy_owner_second FROM STDIN", std::string{"second\n"}));
    EXPECT_TRUE(after.ok());
    second_bytes.clear();
    auto check = qb::io::async::run_sync(
        db_->copy_out("COPY qb_copy_owner_second TO STDOUT", [&second_bytes](std::string_view bytes) { second_bytes.append(bytes); }));
    EXPECT_TRUE(check.ok());
    EXPECT_EQ(second_bytes, "second\n");
}

TEST_F(CopyOwner, QueuedCopySourceDoesNotFeedAnEarlierPlainCopyQuery) {
    ASSERT_TRUE(qb::io::async::run_sync(db_->query("CREATE TEMP TABLE qb_copy_owner_bare (v text)")).ok());
    ASSERT_TRUE(qb::io::async::run_sync(db_->query("CREATE TEMP TABLE qb_copy_owner_target (v text)")).ok());

    bool bare_succeeded = false, bare_failed = false;
    db_->execute(
        "COPY qb_copy_owner_bare FROM STDIN", [&bare_succeeded](qb::pg::transaction &, qb::pg::results) { bare_succeeded = true; },
        [&bare_failed](qb::pg::error::db_error const &) { bare_failed = true; });
    auto loaded = qb::io::async::run_sync(db_->copy_in("COPY qb_copy_owner_target FROM STDIN", std::string{"target\n"}));

    EXPECT_FALSE(bare_succeeded);
    EXPECT_TRUE(bare_failed);
    EXPECT_TRUE(loaded.ok());
    std::string bare_bytes, target_bytes;
    auto        bare = qb::io::async::run_sync(
        db_->copy_out("COPY qb_copy_owner_bare TO STDOUT", [&bare_bytes](std::string_view bytes) { bare_bytes.append(bytes); }));
    auto target = qb::io::async::run_sync(
        db_->copy_out("COPY qb_copy_owner_target TO STDOUT", [&target_bytes](std::string_view bytes) { target_bytes.append(bytes); }));
    EXPECT_TRUE(bare.ok());
    EXPECT_TRUE(target.ok());
    EXPECT_TRUE(bare_bytes.empty());
    EXPECT_EQ(target_bytes, "target\n");
}

TEST_F(CopyOwner, DestroyedAwaiterKeepsCopyOwnerUntilCommandCompletes) {
    // A second session holds an advisory lock so the first connection cannot
    // finish its preceding command until both COPY tasks have been started.
    auto blocker = std::make_unique<database>();
    ASSERT_TRUE(qb::pg::test::pg_try_connect(*blocker));
    ASSERT_TRUE(qb::io::async::run_sync(blocker->query("SELECT pg_advisory_lock(6290001)")).ok());
    db_->execute("SELECT pg_advisory_lock(6290001)", qb::pg::discard_query, qb::pg::discard_error);

    std::string orphan_bytes, other_bytes;
    bool        orphan_resumed = false, other_done = false, other_ok = false;
    auto       &scheduler = qb::io::async::coro_scheduler();
    auto        orphan    = scheduler.spawn_tracked(copy_out_detached(db_.get(), &orphan_bytes, &orphan_resumed));
    scheduler.spawn(observe_copy_out(db_.get(), &other_bytes, &other_done, &other_ok));

    // Driving an unrelated query on the blocker runs both queued coroutine
    // starts, while the database command remains stopped on the held lock.
    EXPECT_TRUE(qb::io::async::run_sync(blocker->query("SELECT 1")).ok());
    if (orphan)
        scheduler.cancel_spawned(orphan);
    else
        ADD_FAILURE() << "the first COPY task was not spawned";

    auto unlocked = qb::io::async::run_sync(blocker->query("SELECT pg_advisory_unlock(6290001)"));
    EXPECT_TRUE(unlocked.ok());
    auto drained = qb::io::async::run_sync(db_->query("SELECT 1"));
    EXPECT_TRUE(drained.ok());
    EXPECT_FALSE(orphan_resumed);
    EXPECT_TRUE(orphan_bytes.empty()) << "a destroyed coroutine must not receive later COPY chunks";
    EXPECT_TRUE(other_done);
    EXPECT_FALSE(other_ok);
    EXPECT_TRUE(other_bytes.empty());
    blocker->disconnect();
}

TEST_F(CopyOwner, DestroyedCopyInAwaiterDetachesSourceAndDrainsCopyFail) {
    ASSERT_TRUE(qb::io::async::run_sync(db_->query("CREATE TEMP TABLE qb_copy_owner_cancelled (v text)")).ok());
    auto blocker = std::make_unique<database>();
    ASSERT_TRUE(qb::pg::test::pg_try_connect(*blocker));
    ASSERT_TRUE(qb::io::async::run_sync(blocker->query("SELECT pg_advisory_lock(6290002)")).ok());
    db_->execute("SELECT pg_advisory_lock(6290002)", qb::pg::discard_query, qb::pg::discard_error);

    int   orphan_source_calls = 0, other_source_calls = 0;
    bool  orphan_resumed = false, other_done = false, other_ok = false;
    auto &scheduler = qb::io::async::coro_scheduler();
    auto  orphan    = scheduler.spawn_tracked(copy_in_detached(db_.get(), &orphan_source_calls, &orphan_resumed));
    scheduler.spawn(observe_copy_in(db_.get(), &other_source_calls, &other_done, &other_ok));
    EXPECT_TRUE(qb::io::async::run_sync(blocker->query("SELECT 1")).ok());
    if (orphan)
        scheduler.cancel_spawned(orphan);
    else
        ADD_FAILURE() << "the first COPY IN task was not spawned";

    EXPECT_TRUE(qb::io::async::run_sync(blocker->query("SELECT pg_advisory_unlock(6290002)")).ok());
    EXPECT_TRUE(qb::io::async::run_sync(db_->query("SELECT 1")).ok());
    EXPECT_FALSE(orphan_resumed);
    EXPECT_EQ(orphan_source_calls, 0);
    EXPECT_TRUE(other_done);
    EXPECT_FALSE(other_ok);
    EXPECT_EQ(other_source_calls, 0);

    std::string bytes;
    auto        empty = qb::io::async::run_sync(
        db_->copy_out("COPY qb_copy_owner_cancelled TO STDOUT", [&bytes](std::string_view chunk) { bytes.append(chunk); }));
    EXPECT_TRUE(empty.ok());
    EXPECT_TRUE(bytes.empty());
    blocker->disconnect();
}

TEST_F(CopyOwner, SourceCancelsItsOwnWaiterWithoutAnotherCallback) {
    ASSERT_TRUE(qb::io::async::run_sync(db_->query("CREATE TEMP TABLE qb_copy_owner_self_cancel (v text)")).ok());
    int                     source_calls = 0;
    bool                    resumed      = false;
    std::coroutine_handle<> handle;
    handle = qb::io::async::coro_scheduler().spawn_tracked(copy_in_source_cancels_waiter(db_.get(), &handle, &source_calls, &resumed));
    ASSERT_TRUE(handle);

    // This SELECT queues behind COPY, so its completion proves CopyFail was
    // answered and the connection advanced exactly past the cancelled command.
    auto after = qb::io::async::run_sync(db_->query("SELECT 1"));
    EXPECT_TRUE(after.ok());
    EXPECT_EQ(source_calls, 1) << "the local source handle must not permit a second call after its coroutine is destroyed";
    EXPECT_FALSE(resumed);

    std::string bytes;
    auto        read = qb::io::async::run_sync(
        db_->copy_out("COPY qb_copy_owner_self_cancel TO STDOUT", [&bytes](std::string_view chunk) { bytes.append(chunk); }));
    EXPECT_TRUE(read.ok());
    EXPECT_TRUE(bytes.empty()) << "the cancelled source chunk must not be sent before CopyFail";
}

TEST_F(CopyOwner, SinkCancelsItsOwnWaiterWithoutAnotherCallback) {
    int                     sink_calls       = 0;
    bool                    capture_retained = false, resumed = false;
    auto                    token = std::make_shared<int>(629);
    std::weak_ptr<int>      lifetime{token};
    std::coroutine_handle<> handle;
    handle = qb::io::async::coro_scheduler().spawn_tracked(
        copy_out_sink_cancels_waiter(db_.get(), &handle, std::move(token), &sink_calls, &capture_retained, &resumed, false));
    ASSERT_TRUE(handle);

    // The SELECT queues behind COPY: its completion proves the remaining chunks
    // and ReadyForQuery were drained after the sink detached itself.
    auto after = qb::io::async::run_sync(db_->query("SELECT 1"));
    EXPECT_TRUE(after.ok());
    EXPECT_EQ(sink_calls, 1);
    EXPECT_TRUE(capture_retained) << "destroying the waiter must not destroy the sink while it is running";
    EXPECT_TRUE(lifetime.expired()) << "the sink's captures must be released after the last invocation returns";
    EXPECT_FALSE(resumed);

    std::string bytes;
    auto        read =
        qb::io::async::run_sync(db_->copy_out("COPY (SELECT 'after') TO STDOUT", [&bytes](std::string_view chunk) { bytes.append(chunk); }));
    EXPECT_TRUE(read.ok());
    EXPECT_EQ(bytes, "after\n");
}

TEST_F(CopyOwner, SinkCancellationAndDisconnectKeepTheRunningCallbackAlive) {
    int                     sink_calls       = 0;
    bool                    capture_retained = false, resumed = false;
    auto                    token = std::make_shared<int>(629);
    std::weak_ptr<int>      lifetime{token};
    std::coroutine_handle<> handle;
    handle = qb::io::async::coro_scheduler().spawn_tracked(
        copy_out_sink_cancels_waiter(db_.get(), &handle, std::move(token), &sink_calls, &capture_retained, &resumed, true));
    ASSERT_TRUE(handle);

    auto queued = qb::io::async::run_sync(db_->query("SELECT 1"));
    EXPECT_FALSE(queued.ok()) << "the sink's disconnect must fail the query queued behind COPY";
    EXPECT_FALSE(db_->is_connection_usable());
    EXPECT_EQ(sink_calls, 1);
    EXPECT_TRUE(capture_retained) << "releasing the admitted command must not destroy its running sink";
    EXPECT_TRUE(lifetime.expired());
    EXPECT_FALSE(resumed);

    ASSERT_TRUE(qb::pg::test::pg_try_connect(*db_));
    std::string bytes;
    auto        read = qb::io::async::run_sync(
        db_->copy_out("COPY (SELECT 'reconnected') TO STDOUT", [&bytes](std::string_view chunk) { bytes.append(chunk); }));
    EXPECT_TRUE(read.ok());
    EXPECT_EQ(bytes, "reconnected\n");
}

TEST_F(CopyOwner, NestedSinkCancellationKeepsOuterCaptureAndReleasesItBeforeCommandCompletion) {
    int                                                  sink_calls = 0, other_calls = 0;
    bool                                                 capture_retained = false;
    auto                                                 token            = std::make_shared<int>(629);
    std::weak_ptr<int>                                   lifetime{token};
    std::optional<qb::io::async::task<Reply<resultset>>> pending;
    pending.emplace(
        db_->copy_out("COPY (SELECT 'owner' FROM generate_series(1, 3)) TO STDOUT",
                      [db = db_.get(), &pending, &sink_calls, &other_calls, &capture_retained, token = std::move(token)](std::string_view) {
                          ++sink_calls;
                          if (sink_calls == 1) {
                              qb::pg::detail::message nested(qb::pg::detail::copy_data_tag);
                              nested.write_sv("nested\n");
                              nested.reset_read();
                              db->on_copy_data(nested);
                              capture_retained = *token == 629;
                          } else {
                              pending.reset();
                              // Detachment must not release the command's reservation.
                              auto other = db->copy_out("COPY (SELECT 'other') TO STDOUT", [&other_calls](std::string_view) { ++other_calls; });
                              other.handle().resume();
                              EXPECT_TRUE(other.done());
                              if (other.done()) {
                                  EXPECT_FALSE(other.await_resume().ok());
                              }
                          }
                      }));
    // Start the owned task without a listener pass, then dispatch two local chunks.
    // The real server response has not been read when the lifetime is checked.
    pending->handle().resume();
    qb::pg::detail::message outer(qb::pg::detail::copy_data_tag);
    outer.write_sv("outer\n");
    outer.reset_read();
    db_->on_copy_data(outer);

    EXPECT_FALSE(pending.has_value());
    EXPECT_EQ(sink_calls, 2);
    EXPECT_EQ(other_calls, 0);
    EXPECT_TRUE(capture_retained) << "inner cancellation must not destroy the outer callable's captures";
    EXPECT_TRUE(lifetime.expired()) << "detached captures must be released before the server command completes";
    EXPECT_TRUE(qb::io::async::run_sync(db_->query("SELECT 1")).ok());
    EXPECT_EQ(sink_calls, 2) << "the real remaining CopyData messages must not reach the detached sink";
}

TEST_F(CopyOwner, SinkDisconnectWithoutCancellingWaiterKeepsCapturesAndSeparatesAnotherOperation) {
    int                                                  sink_calls = 0, other_calls = 0;
    bool                                                 capture_retained = false, resumed = false, ok = true;
    auto                                                 token = std::make_shared<int>(629);
    std::weak_ptr<int>                                   lifetime{token};
    std::optional<qb::io::async::task<Reply<resultset>>> other;
    auto                                                 handle = qb::io::async::coro_scheduler().spawn_tracked(copy_out_with_sink(
        db_.get(),
        [db = db_.get(), &sink_calls, &other_calls, &capture_retained, &other, token = std::move(token)](std::string_view) {
            ++sink_calls;
            db->disconnect();
            // finish_copy has released the old operation. The new closed-handle
            // operation completes independently while the old sink still executes.
            other.emplace(db->copy_out("COPY (SELECT 'other') TO STDOUT", [&other_calls](std::string_view) { ++other_calls; }));
            other->handle().resume();
            capture_retained = *token == 629;
        },
        &resumed, &ok));
    auto frame_guard = qb::scope_guard([handle] { qb::io::async::coro_scheduler().cancel_spawned(handle); });
    ASSERT_TRUE(handle);

    EXPECT_FALSE(qb::io::async::run_sync(db_->query("SELECT 1")).ok());
    // Another closed-handle query drives any already-scheduled completion without
    // opening a connection or issuing more SQL to the server.
    EXPECT_FALSE(qb::io::async::run_sync(db_->query("SELECT 1")).ok());
    EXPECT_FALSE(db_->is_connection_usable());
    EXPECT_EQ(sink_calls, 1);
    EXPECT_EQ(other_calls, 0);
    EXPECT_TRUE(capture_retained);
    EXPECT_TRUE(lifetime.expired());
    EXPECT_TRUE(resumed);
    EXPECT_FALSE(ok);
    ASSERT_TRUE(other.has_value());
    ASSERT_TRUE(other->done());
    EXPECT_FALSE(other->await_resume().ok());

    ASSERT_TRUE(qb::pg::test::pg_try_connect(*db_));
    std::string bytes;
    auto        read =
        qb::io::async::run_sync(db_->copy_out("COPY (SELECT 'after') TO STDOUT", [&bytes](std::string_view chunk) { bytes.append(chunk); }));
    EXPECT_TRUE(read.ok());
    EXPECT_EQ(bytes, "after\n");
}

TEST_F(CopyOwner, SinkRejectsReentrantCopyInAndCopyOutBeforeAnotherListenerPass) {
    int         sink_calls = 0, other_sink_calls = 0, other_source_calls = 0;
    std::string bytes;
    auto        reply = qb::io::async::run_sync(
        db_->copy_out("COPY (SELECT 'first' FROM generate_series(1, 3)) TO STDOUT", [db = db_.get(), &sink_calls, &other_sink_calls,
                                                                                     &other_source_calls, &bytes](std::string_view chunk) {
            bytes.append(chunk);
            if (++sink_calls != 1)
                return;
            auto out = db->copy_out("COPY (SELECT 'other') TO STDOUT", [&other_sink_calls](std::string_view) { ++other_sink_calls; });
            out.handle().resume();
            EXPECT_TRUE(out.done());
            if (out.done()) {
                EXPECT_FALSE(out.await_resume().ok());
            }
            auto in = db->copy_in("COPY qb_copy_owner_unused FROM STDIN", [&other_source_calls]() -> std::optional<std::string> {
                ++other_source_calls;
                return std::nullopt;
            });
            in.handle().resume();
            EXPECT_TRUE(in.done());
            if (in.done()) {
                EXPECT_FALSE(in.await_resume().ok());
            }
        }));
    EXPECT_TRUE(reply.ok());
    EXPECT_EQ(sink_calls, 3);
    EXPECT_EQ(bytes, "first\nfirst\nfirst\n");
    EXPECT_EQ(other_sink_calls, 0);
    EXPECT_EQ(other_source_calls, 0);
    EXPECT_TRUE(qb::io::async::run_sync(db_->query("SELECT 1")).ok());
}

TEST_F(CopyOwner, ThrowingSinkUnwindsPinsAndFailsPendingQueriesWithOrWithoutCancellation) {
    for (int mode = 0; mode < 4; ++mode) {
        SCOPED_TRACE(mode);
        const bool              cancel           = (mode & 1) != 0;
        const bool              unknown          = (mode & 2) != 0;
        int                     sink_calls       = 0;
        bool                    capture_retained = false, resumed = false, ok = true;
        auto                    token = std::make_shared<int>(629);
        std::weak_ptr<int>      lifetime{token};
        std::coroutine_handle<> handle;
        handle           = qb::io::async::coro_scheduler().spawn_tracked(copy_out_with_sink(
            db_.get(),
            [&handle, &sink_calls, &capture_retained, token = std::move(token), cancel, unknown](std::string_view) {
                ++sink_calls;
                if (cancel)
                    qb::io::async::coro_scheduler().cancel_spawned(handle);
                capture_retained = *token == 629;
                if (unknown)
                    throw 629;
                throw std::runtime_error("sink boom");
            },
            &resumed, &ok));
        auto frame_guard = qb::scope_guard([handle] { qb::io::async::coro_scheduler().cancel_spawned(handle); });
        ASSERT_TRUE(handle);

        EXPECT_FALSE(qb::io::async::run_sync(db_->query("SELECT 1")).ok());
        EXPECT_FALSE(db_->is_connection_usable());
        EXPECT_EQ(sink_calls, 1);
        EXPECT_TRUE(capture_retained);
        EXPECT_TRUE(lifetime.expired());
        EXPECT_EQ(resumed, !cancel);
        if (resumed) {
            EXPECT_FALSE(ok);
        }
        ASSERT_TRUE(qb::pg::test::pg_try_connect(*db_));
        std::string bytes;
        auto        read = qb::io::async::run_sync(
            db_->copy_out("COPY (SELECT 'after') TO STDOUT", [&bytes](std::string_view chunk) { bytes.append(chunk); }));
        EXPECT_TRUE(read.ok());
        EXPECT_EQ(bytes, "after\n");
    }
}

qb::io::async::task<Reply<resultset>>
copy_in_chunked(database *db, std::string payload, std::size_t *calls) {
    qb::io::async::pin_frame_copy(payload); // Keep the by-value frame convention before constructing its callback.
    co_return co_await db->copy_in("COPY qb_copy_owner_chunks (v) FROM STDIN",
                                   [data = std::move(payload), calls, offset = std::size_t{0}]() mutable -> std::optional<std::string> {
                                       ++*calls;
                                       if (offset == data.size())
                                           return std::nullopt;
                                       const auto take  = std::min(std::size_t{1024}, data.size() - offset);
                                       auto       chunk = data.substr(offset, take);
                                       offset += take;
                                       return chunk;
                                   });
}

TEST_F(CopyOwner, SequentialChunkedCopiesKeepEveryByteAndLeaveTheConnectionReusable) {
    std::string payload;
    for (int row = 0; row < 1024; ++row) {
        const auto digits = std::to_string(row);
        payload.append(4 - digits.size(), '0');
        payload.append(digits);
        payload.append(59, static_cast<char>('a' + row % 26));
        payload.push_back('\n');
    }
    ASSERT_EQ(payload.size(), 64u * 1024u);
    ASSERT_TRUE(qb::io::async::run_sync(db_->query("CREATE TEMP TABLE qb_copy_owner_chunks (v text)")).ok());
    std::size_t total_calls = 0;
    std::size_t total_bytes = 0;
    for (int round = 0; round < 2; ++round) {
        ASSERT_TRUE(qb::io::async::run_sync(db_->query("TRUNCATE qb_copy_owner_chunks")).ok());
        std::size_t calls = 0;
        auto        added = qb::io::async::run_sync(copy_in_chunked(db_.get(), payload, &calls));
        ASSERT_TRUE(added.ok());
        EXPECT_EQ(added.result().rows_affected(), 1024);
        EXPECT_EQ(calls, 65u);
        std::string bytes;
        auto        read = qb::io::async::run_sync(db_->copy_out("COPY (SELECT v FROM qb_copy_owner_chunks ORDER BY left(v, 4)::int) TO STDOUT",
                                                                 [&bytes](std::string_view chunk) { bytes.append(chunk); }));
        ASSERT_TRUE(read.ok());
        EXPECT_EQ(read.result().rows_affected(), 1024);
        EXPECT_EQ(bytes, payload);
        total_calls += calls;
        total_bytes += bytes.size();
        auto after = qb::io::async::run_sync(db_->query("SELECT $1::int + 1", 10));
        ASSERT_TRUE(after.ok());
        ASSERT_EQ(after.result().size(), 1u);
        EXPECT_EQ(after.result()[0][0].as<int>(), 11);
    }
    EXPECT_EQ(total_calls, 130u);
    EXPECT_EQ(total_bytes, 2u * payload.size());
}

} // namespace copy_owner_test
