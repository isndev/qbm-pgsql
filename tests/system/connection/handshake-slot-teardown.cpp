/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file handshake-slot-teardown.cpp
 * @brief System: a handshake frame destroyed while it waits on its startup slot takes its handle back out of the
 *        slot, so the settle `~Database` makes wakes nothing (Huly QB-1246).
 *
 * The protocol handshake of a connect() parks its frame on the connection's startup slot (`slot_awaiter`). The slot
 * is shared -- the connection settles it on a message, on the loss of the link and in its destructor -- so it
 * outlives the frame. Until 3.4 the awaiter had no destructor: a frame destroyed while parked left its handle in the
 * slot, `~Database` settled the slot and queued that handle on the thread's scheduler, and the next loop pass resumed
 * freed memory. `listener::reset_coro_scheduler()`, the documented teardown, destroys every frame its scheduler owns,
 * which is how the frames go here. A loopback listener that never accepts holds each connection mid-handshake: the
 * TCP connection completes in its backlog and the StartupMessage is never read. No daemon.
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
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

namespace handshake_slot_teardown_test {

namespace async = qb::io::async;

/// The coroutine frames this thread has allocated and not freed.
long
live_frames() noexcept {
    return async::detail::CoroutineFrameAllocator::live_frames;
}

async::task<void>
connect_and_wait(qb::pg::tcp::database &db, std::string dsn) {
    (void) co_await db.connect(dsn);
}

/// Each case leaves the thread's loop as it found it, the scheduler included, whatever assertion stopped it.
class HandshakeSlotTeardown : public ::testing::TestWithParam<int> {
protected:
    void
    TearDown() override {
        if (async::listener::current.has_coro_scheduler())
            async::listener::current.reset_coro_scheduler();
        async::listener::current.clear();
    }
};

} // namespace handshake_slot_teardown_test

using handshake_slot_teardown_test::HandshakeSlotTeardown;

/**
 * @test N databases mid-handshake, each connect()'s protocol handshake parked on its startup slot; the thread's
 *       scheduler is reset, which destroys every frame it owns; then the databases are destroyed, each settling its
 *       slot. Nothing may be woken then: a queued handle names a destroyed frame, and the next loop pass resumes it.
 *       Read BEFORE the loop runs, so the defect is a failed assertion and not a crash; the loop then runs and every
 *       frame stays freed. The wake is checked as well as the queue: the reset left the thread's listener with no
 *       scheduler, so only a wake builds one again, and `schedule_resume()` skips a handle whose freed frame reads as
 *       finished (`done()` reads the frame pool's free-list link, null in the first frame freed into an empty
 *       bucket). On the base (no destructor on `slot_awaiter`, qbm-pgsql c25c35d1) every case fails, in the g++ 14 and
 *       clang 19 sanitize lanes, g++ 14 Release and MSVC Release alike: one handle queued per database, one fewer when
 *       the case freed a handshake frame into an empty bucket -- 0, 1 and 3 for 1, 2 and 4 databases each run alone
 *       (g++ 14), the 1-database case failing on the wake alone. The scratch witness that ran the loop next died in
 *       `CoroutineScheduler::run_ready()` (SIGSEGV).
 */
TEST_P(HandshakeSlotTeardown, AFrameDestroyedOnItsSlotLeavesNoHandleForTheDatabaseToWake) {
    namespace async = handshake_slot_teardown_test::async;
    using handshake_slot_teardown_test::live_frames;
    const int databases = GetParam();

    // A server that completes the TCP handshake and never speaks: it listens and never accepts.
    qb::io::tcp::listener server;
    ASSERT_EQ(server.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = server.local_endpoint().port();
    ASSERT_NE(port, 0);
    const std::string dsn = "tcp://u:p@127.0.0.1:" + std::to_string(port) + "[d]";

    const long                                          live_before = live_frames();
    std::vector<std::unique_ptr<qb::pg::tcp::database>> dbs;
    for (int i = 0; i < databases; ++i) {
        dbs.push_back(std::make_unique<qb::pg::tcp::database>());
        async::coro_scheduler().spawn(handshake_slot_teardown_test::connect_and_wait(*dbs.back(), dsn));
    }
    // `handshaking` is set where the link is installed, and the handshake parks on its slot in the same resume.
    const auto all_handshaking = [&dbs] {
        return std::all_of(dbs.begin(), dbs.end(), [](auto const &db) { return db->state() == qb::io::connection_state::handshaking; });
    };
    ASSERT_TRUE(qb::pg::test::pump_until(all_handshaking, 30s)) << "the connections never reached the handshake: the case would test nothing";
    ASSERT_GT(live_frames(), live_before);

    async::listener::current.reset_coro_scheduler();
    ASSERT_EQ(live_frames(), live_before) << "reset_coro_scheduler() destroys every frame its scheduler owns";

    dbs.clear(); // each ~Database settles the startup slot its handshake frame was parked on
    const bool        woke   = async::listener::current.has_coro_scheduler();
    const std::size_t queued = woke ? async::coro_scheduler().pending_count() : std::size_t{0};
    if (queued != 0)
        async::listener::current.reset_coro_scheduler(); // drop them unresumed: each one names a destroyed frame
    ASSERT_EQ(queued, 0u) << "~Database queued " << queued << " handle(s) of handshake frames already destroyed";
    EXPECT_FALSE(woke) << "~Database woke a handshake frame already destroyed";

    async::run(EVRUN_NOWAIT);
    EXPECT_EQ(live_frames(), live_before) << "no frame came back to life";
    server.disconnect();
}

INSTANTIATE_TEST_SUITE_P(Databases, HandshakeSlotTeardown, ::testing::Values(1, 2, 4));
