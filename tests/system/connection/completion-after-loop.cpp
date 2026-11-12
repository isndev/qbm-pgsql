/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file completion-after-loop.cpp
 * @brief System: a completion of the module delivered after its thread's event loop is destroyed is dropped
 *        (Huly QB-1246).
 *
 * Databases with static storage are mid-handshake when a coroutine calls `exit()`. The thread's `listener::current`
 * goes first, from inside that coroutine, and its scheduler destroys nothing (one of its coroutines is executing): the
 * handshake frames stay parked on their startup slots. Then the statics go, and each `~Database` settles its slot: a
 * wake delivered to a thread whose loop is gone. The module wakes through `qb::io::async::resume_via_current()`, which
 * drops it; until 3.4 it went through `coro_scheduler().schedule_resume()`, which asks for the thread's scheduler --
 * what now stops the process on one named line, and what before built a scheduler on the destroyed listener. The case
 * runs in a child process (the abandoned coroutines are a leak by design) whose judge is an `atexit` handler. A
 * loopback listener that never accepts holds each connection mid-handshake. No daemon.
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/tcp/listener.h>

#include <qbm/pgsql/pgsql.h>

#include "../../shared/pg_pump.hpp"

using namespace std::chrono_literals;

namespace completion_after_loop_test {

namespace async = qb::io::async;

constexpr int kDatabases = 2;
/// What the thread's scheduler owns when the process exits: per database, the coroutine that awaits connect() and
/// the connection flow the module spawns for it; and the coroutine that calls exit().
constexpr std::size_t kOwnedCoroutines = 2 * kDatabases + 1;

std::size_t g_tally_before          = 0;
int         g_databases_destroyed   = 0;
bool        g_last_static_destroyed = false;

/// Built before every other static of the child, destroyed after them.
struct last_static {
    ~last_static() {
        g_last_static_destroyed = true;
    }
};

/// Counts a database once its destructor has returned: declared before it, it is destroyed after it.
struct database_destroyed {
    ~database_destroyed() {
        ++g_databases_destroyed;
    }
};

struct counted_database {
    database_destroyed    counter;
    qb::pg::tcp::database db;
};

/// The judge: an atexit handler registered before the statics are built, so it runs after their destructors, which
/// run after the thread's listener. It ends the child with its verdict and without the exit-time leak check: the
/// abandoned coroutines are the point. (Not a static's destructor: a destructor that never returns is MSVC's C4722.)
void
judge_at_exit() {
    const std::size_t abandoned = async::abandoned_coroutine_frames_total() - g_tally_before;
    const bool        rebound   = async::CoroutineScheduler::current_ptr() != nullptr;
    std::fprintf(stderr,
                 "judge: abandoned %zu (expected %zu), databases destroyed %d of %d, last static destroyed %d, a "
                 "scheduler bound after the loop %d\n",
                 abandoned, kOwnedCoroutines, g_databases_destroyed, kDatabases, g_last_static_destroyed ? 1 : 0, rebound ? 1 : 0);
    std::fflush(stderr);
    const bool verdict = abandoned == kOwnedCoroutines && g_databases_destroyed == kDatabases && g_last_static_destroyed && !rebound;
    std::_Exit(verdict ? 0 : 1);
}

async::task<void>
connect_and_wait(qb::pg::tcp::database &db, std::string dsn) {
    (void) co_await db.connect(dsn);
}

/// Called from a coroutine body, as a program does: resumed by the scheduler's drain after an await (`sleep()` of
/// zero is a cooperative yield and arms no timer), it ends the process from inside one of its scheduler's coroutines.
async::task<void>
exit_from_a_coroutine() {
    co_await async::sleep(qb::duration::zero());
    std::exit(3); // the thread-locals, then the statics, then the judge, which decides the exit code
}

[[noreturn]] void
exit_while_mid_handshake() {
    g_tally_before = async::abandoned_coroutine_frames_total();
    if (std::atexit(judge_at_exit) != 0) {
        std::fprintf(stderr, "the judge could not be registered\n");
        std::_Exit(4);
    }
    static last_static                                    last;
    static std::vector<std::unique_ptr<counted_database>> databases;
    (void) last;

    // A server that completes the TCP handshake and never speaks: it listens and never accepts.
    qb::io::tcp::listener server;
    if (server.listen_v4(0, "127.0.0.1") != qb::io::SocketStatus::Done || server.local_endpoint().port() == 0) {
        std::fprintf(stderr, "no loopback listener\n");
        std::_Exit(4);
    }
    const std::string dsn = "tcp://u:p@127.0.0.1:" + std::to_string(server.local_endpoint().port()) + "[d]";
    for (int i = 0; i < kDatabases; ++i) {
        databases.push_back(std::make_unique<counted_database>());
        async::coro_scheduler().spawn(connect_and_wait(databases.back()->db, dsn));
    }
    // `handshaking` is set where the link is installed, and the handshake parks on its slot in the same resume.
    const bool parked = qb::pg::test::pump_until(
        [] {
            return std::all_of(databases.begin(), databases.end(),
                               [](auto const &counted) { return counted->db.state() == qb::io::connection_state::handshaking; });
        },
        30s);
    if (!parked) {
        std::fprintf(stderr, "the connections never reached the handshake: the case would test nothing\n");
        std::_Exit(4);
    }
    async::coro_scheduler().spawn(exit_from_a_coroutine());
    (void) qb::pg::test::pump_until([] { return false; }, 30s);
    std::fprintf(stderr, "NOT REACHED: the coroutine never called exit()\n");
    std::_Exit(5);
}

} // namespace completion_after_loop_test

/**
 * @test `exit()` from a coroutine while two databases with static storage are mid-handshake. The thread's scheduler
 *       goes from inside one of its coroutines and abandons the five it owns, reporting the order; each `~Database`
 *       then settles its slot, whose handle still names a parked frame, and that wake is dropped: no scheduler is bound
 *       after the loop, both databases are destroyed and every static after them. On the base, `~listener`'s
 *       scheduler destroyed every frame it owned, the executing one included: the g++ 14 sanitize lane stopped there
 *       ("member call on address ... which does not point to an object of type 'timer_awaiter'"), before any static;
 *       the clang 19 lane ran on to the statics and reported a heap-use-after-free in `CoroutineScheduler::
 *       schedule_resume()` from `startup_slot::settle()` in `~Database` (the handshake frame, freed); a g++ 14 Release
 *       child reached the judge with no coroutine reported and a scheduler bound after the loop (`coro_scheduler()`
 *       built one on the destroyed listener). With the wake back on `coro_scheduler()` (the lot's mutant MODN4,
 *       pgsql.h) the child stops on the named line and the last static is never destroyed.
 */
TEST(PgsqlCompletionAfterLoopDeathTest, AWakeDeliveredAfterTheThreadsLoopIsDropped) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const std::string abandon_line =
        "destroyed FROM INSIDE one of its own coroutines with " + std::to_string(completion_after_loop_test::kOwnedCoroutines) + " coroutine";
    EXPECT_EXIT(completion_after_loop_test::exit_while_mid_handshake(), ::testing::ExitedWithCode(0), abandon_line);
}
