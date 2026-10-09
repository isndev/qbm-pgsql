/**
 * @file tls-warning-race.cpp
 * @brief TSan regression for the once-only unverified PostgreSQL TLS warning.
 *
 * This is a separate binary because CTest runs GoogleTest cases with --gtest_shuffle:
 * another TLS case could otherwise consume the warning before the race case runs.
 * Multiple clients use independent qb-io loops, as actors on separate cores do. A bad
 * CA path reaches the warning decision and fails before any network I/O.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <barrier>
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <qb/io/async.h>
#include <qb/io/async/coroutine/utils.h>
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;

TEST(TlsWarning, ConcurrentFirstUnverifiedConnectsAreRaceFree) {
    auto opts          = connection_options::parse("tcp://test:test@192.0.2.1:5432[test]");
    opts.ssl_root_cert = "qb-nonexistent-private-ca.pem";

    // Neither control enters the warning branch.
    qb::pg::tcp::ssl::database verified;
    auto                       full = opts;
    full.ssl_verify                 = ssl_verify_mode::full;
    EXPECT_FALSE(qb::io::async::run_sync(verified.connect(full)));
    qb::pg::tcp::database plain;
    EXPECT_FALSE(qb::io::async::run_sync(plain.connect("tcp://test:test@127.0.0.1:1[test]")));

    // One process gets only one first-use race window. A modest cohort makes
    // TSan's normal CTest run catch the old bool without a lucky two-thread schedule.
    constexpr std::size_t      kClients = 16;
    std::barrier<>             ready(kClients + 1);
    std::array<bool, kClients> failed{};
    std::vector<std::thread>   threads;
    auto                       connect_unverified = [&](std::size_t index) {
        qb::io::async::init();
        qb::pg::tcp::ssl::database client;
        auto                       none = opts;
        none.ssl_verify                 = ssl_verify_mode::none;
        ready.arrive_and_wait();
        failed[index] = !qb::io::async::run_sync(client.connect(none));
    };
    threads.reserve(kClients);
    for (std::size_t i = 0; i < kClients; ++i)
        threads.emplace_back(connect_unverified, i);
    ready.arrive_and_wait();
    for (auto &thread : threads)
        thread.join();
    for (bool result : failed)
        EXPECT_TRUE(result);
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
