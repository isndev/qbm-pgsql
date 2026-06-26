/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file pg_pump.hpp
 * @brief Bounded, deadline-driven io-loop pump for qbm-pgsql integration tests.
 *
 * Several pgsql integration scenarios (LISTEN/NOTIFY delivery, in-flight CancelRequest,
 * disconnect-fails-queued-queries) need to drive the qb-io event loop until a server-side
 * event lands — *without* spinning forever when the daemon misbehaves. The legacy tests
 * hand-rolled this as `for (i < 2000 && !done) run(EVRUN_NOWAIT)` (iteration-bounded, not
 * time-bounded, and machine-speed dependent) or as a fixed `sleep(200ms)` race window.
 *
 * `pump_until` replaces both with a single wall-clock-bounded helper: it pumps the current
 * thread's `listener` with `EVRUN_NOWAIT` until @p predicate() is true or @p deadline
 * elapses, then returns *whether the predicate was met*. Callers assert that boolean
 * (`EXPECT_TRUE(pump_until(...)) << "notification never arrived"`) so a timeout fails with a
 * diagnostic message instead of an opaque `hits == 0`.
 *
 * It never blocks the loop (always `EVRUN_NOWAIT`) and yields the CPU briefly between empty
 * passes so a busy-wait does not peg a core while waiting on network round-trips.
 */

#ifndef QBM_PGSQL_TESTS_SHARED_PG_PUMP_HPP
#define QBM_PGSQL_TESTS_SHARED_PG_PUMP_HPP

#include <chrono>
#include <thread>
#include <qb/io/async.h>
#include <qb/system/time.h>

namespace qb::pg::test {

/**
 * @brief Pump the current thread's io loop until @p predicate holds or @p deadline elapses.
 *
 * @tparam Predicate  callable returning something contextually convertible to bool.
 * @param predicate   re-evaluated after every non-blocking loop pass.
 * @param deadline    maximum wall-clock time to keep pumping (default 5s).
 * @return true if @p predicate became true within the deadline; false on timeout.
 *
 * Drives `qb::io::async::run(EVRUN_NOWAIT)` (never blocking) so queued completions,
 * notifications and timers fire. Between empty passes it sleeps ~200µs to avoid a hot spin.
 * The predicate is checked once up-front so an already-satisfied condition returns
 * immediately.
 */
template <typename Predicate>
[[nodiscard]] inline bool
pump_until(Predicate &&predicate,
           qb::duration deadline = std::chrono::seconds(5)) {
    const auto start = std::chrono::steady_clock::now();
    const auto limit = std::chrono::nanoseconds(deadline.count());
    while (true) {
        if (predicate())
            return true;
        if (std::chrono::steady_clock::now() - start >= limit)
            return static_cast<bool>(predicate());
        qb::io::async::run(EVRUN_NOWAIT);
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

/**
 * @brief Pump the loop for a bounded window regardless of any predicate (drain helper).
 *
 * Useful to give a fire-and-forget timer or a deadline timer a chance to come due so a
 * use-after-free would be observable under ASan. Always returns after @p window elapses.
 */
inline void
pump_for(qb::duration window) {
    const auto start = std::chrono::steady_clock::now();
    const auto limit = std::chrono::nanoseconds(window.count());
    while (std::chrono::steady_clock::now() - start < limit) {
        qb::io::async::run(EVRUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

} // namespace qb::pg::test

#endif // QBM_PGSQL_TESTS_SHARED_PG_PUMP_HPP
