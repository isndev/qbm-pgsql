/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file integration/resilience/coro-cancel-and-connection-loss.cpp
 * @brief The two things a live PostgreSQL connection can have done TO it from outside.
 *
 * Everything else in this suite drives the happy path plus client-initiated teardown. Two
 * shapes were untested against a live server, and both are shapes this codebase has been bitten
 * by before:
 *
 *   1. **A coroutine destroyed while parked on an in-flight query.** The destroy-while-parked
 *      class — ten sites in qb-io alone. The reply is already on its way when the frame goes
 *      away; the completion hook must observe that and drop it. `pg_awaiter` guards this with a
 *      `shared_ptr<bool> alive` cleared in its destructor.
 *
 *      The obvious oracle for that guard — "reading the dead frame is a use-after-free, ASan
 *      will say so" — is WRONG here, and measuring it was the whole design of this case. qb's
 *      coroutine frames come from a pooled freelist (`CoroutineFrameAllocator` /`BucketPool` in
 *      `coroutine/task.h`), so `h.destroy()` returns the block to the pool instead of to the
 *      allocator: the memory stays mapped, ASan never sees a free, and `schedule_resume`'s
 *      `handle.done()` on it reads a zeroed slot and quietly returns. Removing the guard is
 *      therefore INVISIBLE to a test that only destroys one coroutine — measured: it passes,
 *      clean, under ASan.
 *
 *      The pool is also what makes the guard load-bearing. It is LIFO, so the next spawn of the
 *      same size class gets that exact block back. An un-retracted completion then schedules a
 *      resume on an address that belongs to somebody else's coroutine, and resumes it out of
 *      turn. That is the hazard, and this case arms it deliberately: it recycles the frame
 *      under a second, innocent coroutine and requires that one to keep its own schedule.
 *      Negative control: delete `if (!s || !*s->alive) return;` from `pg_awaiter.h` and the
 *      recycled coroutine resumes ~6 s early.
 *
 *   2. **A server-side drop mid-transaction.** Every "connection loss" case in this suite is a
 *      client-initiated `disconnect()`, which takes a synchronous path (`disconnect()` fails the
 *      queue itself) that never exercises `on(disconnected)`. A backend terminated from another
 *      session is the real thing: the socket dies under an in-flight query, inside an open
 *      transaction, with no cooperation from the client at all. `pg_terminate_backend()` from a
 *      second connection is exactly that, and it is deterministic — no sleeps, no port games.
 *      Negative control: delete the `fail_all_pending(...)` line from `on(disconnected)` and
 *      `ServerSideBackendTerminationFailsTheInFlightQuery` hangs on its watchdog instead of
 *      resuming.
 *
 * Tier: integration (REQUIRES live postgres, coroutine). Skip-not-fail via the shared fixture.
 *
 * @ingroup Pgsql
 */
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>

#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/test_config.hpp"
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using qb::pg::test::dsn_tcp_string;

namespace {

/// Per-TU table name; integration binaries are serialized by the CTest resource lock.
constexpr std::string_view kTable = "qb_pgsql_resilience_t";

/**
 * @brief Pump the loop until @p pred or the watchdog expires; returns pred's final value.
 *
 * Bounded by wall clock only as a hang detector — never as a timing assumption.
 */
template <typename Pred>
bool
pump_until(Pred &&pred, std::chrono::milliseconds budget = std::chrono::milliseconds(15000)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!pred()) {
        qb::io::async::run(EVRUN_NOWAIT);
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
    }
    return true;
}

/**
 * @brief A free coroutine (not a lambda) that parks on @p sql and records what came back.
 *
 * Deliberately NOT an immediately-invoked lambda: `task`'s `initial_suspend` is
 * `suspend_always`, so a `spawn(lambda(){...}())` closure is already destroyed by the time the
 * body runs (see qb/scripts/check-spawn-dangling-closure.py). Everything this needs is a
 * by-value parameter, and the pointers all address locals in the TEST body, which outlives the
 * frame in every case here.
 */
qb::io::async::task<void>
park_on_query(qb::pg::tcp::database *db, std::string sql, bool *resumed, bool *reply_ok) {
    auto reply = co_await db->query(sql);
    // Only reached if the frame was NOT destroyed while parked.
    *reply_ok = reply.ok();
    *resumed  = true;
    co_return;
}

/// Integration fixture: connect-or-skip, then a clean work table.
class PgsqlResilienceTest : public qb::pg::test::PgIntegrationTest {
protected:
    void
    SetUp() override {
        qb::pg::test::PgIntegrationTest::SetUp(); // connect-or-skip
        if (IsSkipped())
            return;
        ASSERT_TRUE(db_->execute(std::string("DROP TABLE IF EXISTS ") + std::string(kTable), qb::pg::discard_query, qb::pg::discard_error)
                        .await());
        // NOT a TEMP table: the whole point of the connection-loss case is to read the row back
        // from a DIFFERENT session, and a TEMP table is invisible to one.
        ASSERT_TRUE(db_->execute(std::string("CREATE TABLE ") + std::string(kTable) + " (id INT PRIMARY KEY, v TEXT NOT NULL)",
                                 qb::pg::discard_query, qb::pg::discard_error)
                        .await());
    }

    void
    TearDown() override {
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
        // The table outlives the connection it was made on, so drop it from a fresh session.
        auto cleanup = std::make_unique<qb::pg::tcp::database>();
        if (qb::pg::test::pg_try_connect(*cleanup)) {
            (void) cleanup->execute(std::string("DROP TABLE IF EXISTS ") + std::string(kTable), qb::pg::discard_query,
                                    qb::pg::discard_error)
                .await();
            cleanup->disconnect();
        }
    }
};

} // namespace

// ------------------------------------------------------------------------------------------
// 1. Coroutine destroyed mid-flight
// ------------------------------------------------------------------------------------------

/**
 * @brief A destroyed coroutine's late reply must not resume whoever inherited its frame.
 *
 * Timeline, all against the live server (t = 0 is the cancel):
 *
 *   t≈0    `SELECT pg_sleep(2)` is confirmed active on the backend, its coroutine parked inside
 *          `pg_awaiter::await_suspend`; `cancel_spawned()` destroys that frame and `~pg_awaiter`
 *          retracts the shared `alive` flag. The block goes back to the coroutine frame pool.
 *   t≈0    a second coroutine is spawned — same coroutine function, therefore the same frame size
 *          class, therefore (LIFO freelist) the same block. It parks on `SELECT pg_sleep(8)`,
 *          which the server queues behind the first query.
 *   t≈2    the ORPHANED reply arrives. The completion hook must drop it. If it does not, it
 *          schedules a resume on its remembered handle — now the second coroutine — and that
 *          coroutine wakes six seconds early, with a `Reply` that was never its own.
 *   t≈4    checkpoint: the second coroutine must still be parked.
 *   t≈10   it resumes on its own reply, successfully, on a connection that is still in sync.
 *
 * The frame-identity check is an instrument check, not decoration: if the pool did not hand the
 * block back, the recycled-frame hazard is not armed and the t≈4 assertion below proves nothing.
 * It reports that rather than passing quietly.
 */
TEST_F(PgsqlResilienceTest, CoroutineDestroyedWhileParkedOnLiveQuery) {
    bool orphan_resumed  = false;
    bool orphan_reply_ok = false;

    auto orphan = qb::io::async::coro_scheduler().spawn_tracked(park_on_query(db_.get(), "SELECT pg_sleep(2)", &orphan_resumed,
                                                                              &orphan_reply_ok));
    ASSERT_TRUE(orphan) << "spawn_tracked returned an empty handle — nothing was parked";
    void *const orphan_frame = orphan.address();

    // Wait for the backend itself to report the statement active. Positive evidence that the
    // coroutine is parked on real in-flight work, rather than a sleep-and-hope.
    auto probe = std::make_unique<qb::pg::tcp::database>();
    ASSERT_TRUE(qb::pg::test::pg_try_connect(*probe));
    const int target = db_->backend_pid();
    ASSERT_GT(target, 0);
    ASSERT_TRUE(pump_until([&] {
        auto r = qb::io::async::run_sync(probe->query("SELECT count(*) FROM pg_stat_activity WHERE pid = " + std::to_string(target)
                                                      + " AND state = 'active'"));
        return r.ok() && r.result().size() == 1 && r.result()[0][0].as<std::int64_t>() == 1;
    })) << "the query never became active on the backend; the coroutine was not parked on real work";

    // Destroy the parked frame; ~pg_awaiter retracts the alive flag and the block is pooled.
    qb::io::async::coro_scheduler().cancel_spawned(orphan);

    // Recycle it under an innocent coroutine.
    bool heir_resumed  = false;
    bool heir_reply_ok = false;
    auto heir = qb::io::async::coro_scheduler().spawn_tracked(park_on_query(db_.get(), "SELECT pg_sleep(8)", &heir_resumed,
                                                                            &heir_reply_ok));
    ASSERT_TRUE(heir);
    EXPECT_EQ(heir.address(), orphan_frame)
        << "the coroutine frame pool did not hand the cancelled frame back to the next spawn, so the recycled-frame hazard is NOT "
           "armed and the early-resume check below cannot observe it";

    // Run past the orphaned reply (t≈2) and stop well before the heir's own (t≈8).
    const auto checkpoint = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < checkpoint)
        qb::io::async::run(EVRUN_NOWAIT);

    EXPECT_FALSE(orphan_resumed) << "the destroyed coroutine was resumed";
    EXPECT_FALSE(heir_resumed) << "a reply belonging to a coroutine that no longer exists resumed the coroutine that inherited its "
                                  "frame — six seconds before its own query could possibly have answered";

    // The heir must still get its own, correct completion, on a connection still in sync.
    ASSERT_TRUE(pump_until([&] { return heir_resumed; }, std::chrono::milliseconds(20000)))
        << "the surviving coroutine never resumed: swallowing the orphaned reply desynced the protocol stream";
    EXPECT_TRUE(heir_reply_ok) << "the surviving coroutine resumed with a failure";
    EXPECT_FALSE(orphan_resumed);

    EXPECT_TRUE(db_->is_connected()) << "swallowing the orphaned reply took the connection down with it";
    EXPECT_EQ(db_->backend_pid(), target) << "the client silently reconnected instead of keeping the session";
    auto after = qb::io::async::run_sync(db_->query("SELECT 42 AS answer"));
    ASSERT_TRUE(after.ok()) << "connection unusable after the orphaned reply: " << after.error().what();
    ASSERT_EQ(after.result().size(), 1u);
    EXPECT_EQ(after.result()[0][0].as<int>(), 42) << "protocol stream desynced: the orphaned reply was not consumed cleanly";

    probe->disconnect();
}

// ------------------------------------------------------------------------------------------
// 2. Server-side drop mid-transaction
// ------------------------------------------------------------------------------------------

/**
 * @brief A backend killed from another session must fail the in-flight query and roll the
 *        transaction back.
 *
 * Sequence, all on a real server: BEGIN, INSERT (uncommitted), then park a coroutine on
 * `pg_sleep(30)` — long enough that nothing can complete by accident. From a second connection,
 * `pg_terminate_backend()` the first one. The socket dies under the in-flight query with no
 * client cooperation whatsoever, which is the path `on(disconnected)` exists for and which
 * `disconnect()` (synchronous, client-initiated) never takes.
 *
 * What the consumer must observe:
 *   - the parked `co_await` RESUMES, with a failed reply — a hang here is the actual hazard,
 *     since the caller has no other way to learn the socket is gone;
 *   - `is_connected()` goes false, so the next `query()` fails fast instead of enqueuing work
 *     that can never be sent;
 *   - the uncommitted INSERT is not visible from any other session: the server rolled the
 *     transaction back when it killed the backend. Asserted from a third connection, because a
 *     dirty read of one's own aborted transaction would prove nothing.
 */
TEST_F(PgsqlResilienceTest, ServerSideBackendTerminationFailsTheInFlightQuery) {
    const int target = db_->backend_pid();
    ASSERT_GT(target, 0);

    // Open a transaction and write a row that must NEVER become visible.
    ASSERT_TRUE(qb::io::async::run_sync(db_->query("BEGIN")).ok());
    auto ins = qb::io::async::run_sync(db_->query(std::string("INSERT INTO ") + std::string(kTable) + " (id, v) VALUES (1, 'doomed')"));
    ASSERT_TRUE(ins.ok()) << ins.error().what();

    // Park on a query that cannot finish on its own.
    bool resumed  = false;
    bool reply_ok = true;
    auto handle   = qb::io::async::coro_scheduler().spawn_tracked(park_on_query(db_.get(), "SELECT pg_sleep(30)", &resumed, &reply_ok));
    ASSERT_TRUE(handle);

    auto killer = std::make_unique<qb::pg::tcp::database>();
    ASSERT_TRUE(qb::pg::test::pg_try_connect(*killer));
    ASSERT_NE(killer->backend_pid(), target) << "the killer must be a different backend";

    // Wait until the sleep is genuinely running on the target backend, then kill it.
    ASSERT_TRUE(pump_until([&] {
        auto r = qb::io::async::run_sync(
            killer->query("SELECT count(*) FROM pg_stat_activity WHERE pid = " + std::to_string(target) + " AND state = 'active'"));
        return r.ok() && r.result().size() == 1 && r.result()[0][0].as<std::int64_t>() == 1;
    })) << "pg_sleep never became active on the target backend";

    auto killed = qb::io::async::run_sync(killer->query("SELECT pg_terminate_backend(" + std::to_string(target) + ")"));
    ASSERT_TRUE(killed.ok()) << killed.error().what();

    // The awaiting coroutine must be woken by the disconnect, not left parked forever.
    ASSERT_TRUE(pump_until([&] { return resumed; })) << "the coroutine parked on the killed connection never resumed: a caller has no "
                                                        "other way to learn the socket is gone, so this is an unbounded hang";
    EXPECT_FALSE(reply_ok) << "the query on a terminated backend reported success";

    // `pg_terminate_backend` sends an ErrorResponse (57P01) and THEN closes the socket, so the
    // failed reply above arrives one or more loop turns BEFORE the FIN. Pump for the close
    // rather than assuming it landed with the error, and bound it: never observing it is the
    // defect (a handle that still reports connected accepts queries onto a dead socket, whose
    // awaiters can then never be resolved).
    ASSERT_TRUE(pump_until([&] { return !db_->is_connected(); }))
        << "the handle still reports connected after the server dropped it; queries submitted now are enqueued on a dead socket and "
           "their awaiters hang forever";

    // Fail fast, not hang, on the next use of the dead handle.
    auto after = qb::io::async::run_sync(db_->query("SELECT 1"));
    EXPECT_FALSE(after.ok()) << "a query on a handle whose backend was terminated reported success";

    // The uncommitted row must be gone, seen from a session that never touched it.
    auto witness = std::make_unique<qb::pg::tcp::database>();
    ASSERT_TRUE(qb::pg::test::pg_try_connect(*witness));
    auto seen = qb::io::async::run_sync(witness->query(std::string("SELECT count(*) FROM ") + std::string(kTable) + " WHERE id = 1"));
    ASSERT_TRUE(seen.ok()) << seen.error().what();
    ASSERT_EQ(seen.result().size(), 1u);
    EXPECT_EQ(seen.result()[0][0].as<std::int64_t>(), std::int64_t{0}) << "the transaction open on the terminated backend left a visible row";

    witness->disconnect();
    killer->disconnect();
}
