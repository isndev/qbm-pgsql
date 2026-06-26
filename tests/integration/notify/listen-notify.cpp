/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file listen-notify.cpp
 * @brief Integration: LISTEN / NOTIFY across the plain database, the callback consumer
 *        (`notify_cb_consumer`) and the coroutine consumer (`notify_co_consumer`).
 *
 * Asynchronous NotificationResponse delivery is driven by a bounded `pump_until` (see
 * shared/pg_pump.hpp) rather than the legacy fixed `for (i<2000)` spins: every wait
 * asserts a diagnostic boolean so a missed notification fails with a message instead of
 * a silent `hits == 0`. Skips (never fails) when the daemon is unreachable.
 *
 * Migrated from test-notify.cpp; absorbs test-protocol-integration.cpp's
 * `ListenNotify_SequentialQueries_NoHang` and the free
 * `PgNotifyFromPeerDoesNotBreakListener` (D6). Added: multi-payload ordering for the
 * callback consumer, reconnect + re-LISTEN, and a capacity-N (not just 1) drop boundary.
 *
 * NOTE: `pump_until` calls `qb::io::async::run(EVRUN_NOWAIT)`, which throws if invoked
 * from inside a `run_sync` coroutine. All pumping therefore happens at test-body scope,
 * never inside a `co_await` body.
 */

#include <atomic>
#include <chrono>
#include <string>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../pgsql.h"
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/pg_pump.hpp"
#include "../../shared/test_config.hpp"

using namespace qb::pg;
using qb::pg::test::dsn_tcp_string;
using qb::pg::test::pump_until;

namespace {

constexpr std::string_view kChan  = "qb_pgsql_notify_test_ch";
constexpr std::string_view kChan2 = "qb_pgsql_notify_test_ch2";
constexpr std::string_view kOther = "qb_pgsql_notify_other_ch";
constexpr auto             kDeadline = std::chrono::seconds(5);

/// Publisher fixture: a connected db used to emit NOTIFYs; skips when daemon is down.
class ListenNotify : public ::testing::Test {
protected:
    std::unique_ptr<qb::pg::tcp::database> pub_;

    void
    SetUp() override {
        pub_ = std::make_unique<qb::pg::tcp::database>();
        if (!qb::io::async::run_sync(pub_->connect(dsn_tcp_string())))
            GTEST_SKIP() << qb::pg::test::kDaemonUnreachableSentinel
                         << " (postgres at " << dsn_tcp_string() << " not reachable)";
    }

    void
    TearDown() override {
        if (pub_) {
            pub_->disconnect();
            pub_.reset();
        }
    }
};

} // namespace

// ---------------------------------------------------------------------------
// notify() / listen() basic awaitable + callback surfaces
// ---------------------------------------------------------------------------

TEST_F(ListenNotify, Notify_Callback_WithPayload) {
    ASSERT_TRUE(pub_->notify(std::string(kChan), "hello-payload", discard_query, discard_error)
                    .await());
}

TEST_F(ListenNotify, Notify_Coro_WithAndWithoutPayload) {
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        if (!(co_await pub_->notify(std::string(kChan), "coro-payload")).ok())
            co_return false;
        co_return (co_await pub_->notify(std::string(kChan))).ok();
    }()));
}

TEST_F(ListenNotify, Notify_Coro_ViaRunSyncOnAwaiter) {
    ASSERT_TRUE(qb::io::async::run_sync(pub_->notify(std::string(kChan), "direct-awaiter")));
}

TEST_F(ListenNotify, Listen_Unlisten_Coro_ViaRunSync) {
    ASSERT_TRUE(qb::io::async::run_sync(pub_->listen(std::string(kChan))));
    ASSERT_TRUE(pub_->unlisten(std::string(kChan), discard_query, discard_error).await());
}

// ---------------------------------------------------------------------------
// notify_cb_consumer — async delivery driven by pump_until
// ---------------------------------------------------------------------------

TEST_F(ListenNotify, CbConsumer_ReceivesPayload) {
    int                             hits{};
    std::string                     last_payload;
    qb::pg::tcp::notify_cb_consumer sub{dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan)) {
            last_payload = std::move(n.payload);
            ++hits;
        }
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "cb-ok", discard_query, discard_error).await());

    EXPECT_TRUE(pump_until([&] { return hits >= 1; }, kDeadline))
        << "notification for " << kChan << " never delivered";
    EXPECT_EQ(hits, 1);
    EXPECT_EQ(last_payload, "cb-ok");
    EXPECT_TRUE(sub.is_connected());
    sub.disconnect();
}

TEST_F(ListenNotify, CbConsumer_MultiPayload_Ordered) {
    std::vector<std::string>        received;
    qb::pg::tcp::notify_cb_consumer sub{dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan))
            received.push_back(std::move(n.payload));
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());

    // PostgreSQL delivers notifications from a single backend in emission order.
    for (const char *p : {"p1", "p2", "p3"})
        ASSERT_TRUE(pub_->notify(std::string(kChan), p, discard_query, discard_error).await());

    EXPECT_TRUE(pump_until([&] { return received.size() >= 3; }, kDeadline))
        << "expected 3 notifications, got " << received.size();
    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[0], "p1");
    EXPECT_EQ(received[1], "p2");
    EXPECT_EQ(received[2], "p3");
    sub.disconnect();
}

TEST_F(ListenNotify, CbConsumer_AfterUnlisten_NoDelivery) {
    std::atomic<int>                hits{0};
    qb::pg::tcp::notify_cb_consumer sub{dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan))
            hits.fetch_add(1, std::memory_order_relaxed);
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(sub.unlisten(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "after-unlisten", discard_query, discard_error)
                    .await());

    // Give any (wrongly) delivered notification a bounded window to show up, then assert
    // none did.
    qb::pg::test::pump_for(std::chrono::milliseconds(300));
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
    sub.disconnect();
}

TEST_F(ListenNotify, CbConsumer_OtherChannel_NoCallback) {
    std::atomic<int>                hits{0};
    qb::pg::tcp::notify_cb_consumer sub{dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan))
            hits.fetch_add(1, std::memory_order_relaxed);
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kOther), "noise", discard_query, discard_error).await());

    qb::pg::test::pump_for(std::chrono::milliseconds(300));
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
    sub.disconnect();
}

TEST_F(ListenNotify, CbConsumer_ReconnectAndReListen_StillDelivers) {
    int                             hits{};
    qb::pg::tcp::notify_cb_consumer sub{dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan))
            ++hits;
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    sub.disconnect();
    EXPECT_FALSE(sub.is_connected());

    // Reconnect and re-LISTEN — the subscription does not survive a reconnect, so it must
    // be re-established explicitly.
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "after-reconnect", discard_query, discard_error)
                    .await());

    EXPECT_TRUE(pump_until([&] { return hits >= 1; }, kDeadline))
        << "notification not delivered after reconnect + re-LISTEN";
    EXPECT_EQ(hits, 1);
    sub.disconnect();
}

// ---------------------------------------------------------------------------
// notify_co_consumer — coroutine receive()
// ---------------------------------------------------------------------------

TEST_F(ListenNotify, CoConsumer_Receive) {
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        qb::pg::tcp::notify_co_consumer sub(dsn_tcp_string());
        if (!co_await sub.connect(dsn_tcp_string()))
            co_return false;
        if (!(co_await sub.listen(std::string(kChan))).ok())
            co_return false;
        if (!(co_await pub_->notify(std::string(kChan), "co-recv")).ok())
            co_return false;
        auto n = co_await sub.receive();
        co_return n.has_value() && n->channel == std::string(kChan) && n->payload == "co-recv";
    }()));
}

TEST_F(ListenNotify, CoConsumer_TwoSequentialReceives_Ordered) {
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        qb::pg::tcp::notify_co_consumer sub(dsn_tcp_string());
        if (!co_await sub.connect(dsn_tcp_string()))
            co_return false;
        if (!(co_await sub.listen(std::string(kChan))).ok())
            co_return false;
        if (!(co_await pub_->notify(std::string(kChan), "first")).ok())
            co_return false;
        if (!(co_await pub_->notify(std::string(kChan), "second")).ok())
            co_return false;
        auto a = co_await sub.receive();
        auto b = co_await sub.receive();
        co_return a.has_value() && b.has_value() && a->payload == "first"
            && b->payload == "second";
    }()));
}

TEST_F(ListenNotify, CoConsumer_CallbackAndReceive) {
    std::atomic<int> cb_hits{0};
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        qb::pg::tcp::notify_co_consumer sub(dsn_tcp_string());
        sub.on_notify([&](qb::pg::notification &&n) {
            if (n.channel == std::string(kChan) && n.payload == "both")
                cb_hits.fetch_add(1, std::memory_order_relaxed);
        });
        if (!co_await sub.connect(dsn_tcp_string()))
            co_return false;
        if (!(co_await sub.listen(std::string(kChan))).ok())
            co_return false;
        if (!(co_await pub_->notify(std::string(kChan), "both")).ok())
            co_return false;
        auto n = co_await sub.receive();
        co_return n.has_value() && n->payload == "both"
            && cb_hits.load(std::memory_order_relaxed) == 1;
    }()));
}

TEST_F(ListenNotify, CoConsumer_ServerBackendPidPositive) {
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        qb::pg::tcp::notify_co_consumer sub(dsn_tcp_string());
        if (!co_await sub.connect(dsn_tcp_string()))
            co_return false;
        if (!(co_await sub.listen(std::string(kChan))).ok())
            co_return false;
        if (!(co_await pub_->notify(std::string(kChan), "pid")).ok())
            co_return false;
        auto n = co_await sub.receive();
        co_return n.has_value() && n->server_backend_pid > 0;
    }()));
}

TEST_F(ListenNotify, CoConsumer_ReceiveNulloptAfterDisconnect) {
    qb::pg::tcp::notify_co_consumer sub(dsn_tcp_string());
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        if (!co_await sub.connect(dsn_tcp_string()))
            co_return false;
        co_return (co_await sub.listen(std::string(kChan))).ok();
    }()));

    sub.disconnect();

    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto n = co_await sub.receive();
        co_return !n.has_value();
    }()));
}

// Capacity-bounded queue: with capacity N, the (N+1)-th queued notification is routed to
// the dropped handler. Tested at N=2 (not just the legacy N=1) to exercise the boundary.
TEST_F(ListenNotify, CoConsumer_CapacityTwo_DropBoundary) {
    std::atomic<int>                dropped{0};
    qb::pg::tcp::notify_co_consumer sub(dsn_tcp_string(), /*capacity*/ 2);
    sub.on_notify_dropped([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan))
            dropped.fetch_add(1, std::memory_order_relaxed);
    });

    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        if (!co_await sub.connect(dsn_tcp_string()))
            co_return false;
        if (!(co_await sub.listen(std::string(kChan))).ok())
            co_return false;
        // Three notifications, capacity 2 -> exactly one is dropped.
        for (const char *p : {"one", "two", "three"})
            if (!(co_await pub_->notify(std::string(kChan), p)).ok())
                co_return false;
        co_return true;
    }()));

    // Drain the subscriber socket so all three NotificationResponses reach the queue
    // before the first receive() pulls one and frees a slot.
    qb::pg::test::pump_for(std::chrono::milliseconds(300));

    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto first = co_await sub.receive();
        co_return first.has_value() && first->payload == "one"
            && dropped.load(std::memory_order_relaxed) == 1;
    }()));
    sub.disconnect();
}

// ---------------------------------------------------------------------------
// Plain database: on_incoming_notify hook
// ---------------------------------------------------------------------------

TEST_F(ListenNotify, PlainDatabase_OnIncomingNotify) {
    int hits = 0;
    pub_->on_incoming_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan))
            ++hits;
    });
    ASSERT_TRUE(pub_->listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "plain", discard_query, discard_error).await());

    EXPECT_TRUE(pump_until([&] { return hits >= 1; }, kDeadline))
        << "plain database did not surface its own notification";
    EXPECT_EQ(hits, 1);
    pub_->on_incoming_notify({});
    ASSERT_TRUE(pub_->unlisten_all(discard_query, discard_error).await());
}

TEST_F(ListenNotify, PlainDatabase_TwoChannels_TwoHits) {
    int hits = 0;
    pub_->on_incoming_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan) || n.channel == std::string(kChan2))
            ++hits;
    });
    ASSERT_TRUE(pub_->listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->listen(std::string(kChan2), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "a", discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan2), "b", discard_query, discard_error).await());

    EXPECT_TRUE(pump_until([&] { return hits >= 2; }, kDeadline))
        << "expected 2 notifications across channels, got " << hits;
    EXPECT_EQ(hits, 2);
    pub_->on_incoming_notify({});
    ASSERT_TRUE(pub_->unlisten_all(discard_query, discard_error).await());
}

TEST_F(ListenNotify, PlainDatabase_EmptyPayload) {
    int         hits = 0;
    std::string last;
    pub_->on_incoming_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan)) {
            last = n.payload;
            ++hits;
        }
    });
    ASSERT_TRUE(pub_->listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "", discard_query, discard_error).await());

    EXPECT_TRUE(pump_until([&] { return hits >= 1; }, kDeadline))
        << "empty-payload notification not delivered";
    EXPECT_EQ(hits, 1);
    EXPECT_TRUE(last.empty());
    pub_->on_incoming_notify({});
    ASSERT_TRUE(pub_->unlisten(std::string(kChan), discard_query, discard_error).await());
}

// ---------------------------------------------------------------------------
// Absorbed from test-protocol-integration.cpp (D6)
// ---------------------------------------------------------------------------

TEST_F(ListenNotify, SequentialQueries_AfterListenNotify_NoHang) {
    ASSERT_TRUE(pub_->execute("LISTEN qb_proto_chan", discard_query, discard_error).await());
    ASSERT_TRUE(pub_->execute("NOTIFY qb_proto_chan, 'ping'", discard_query, discard_error)
                    .await());
    EXPECT_TRUE(pub_->execute("SELECT 1", discard_query, discard_error).await());
}

// A NOTIFY emitted by a *peer* backend must not break the listener's connection: it can
// still run ordinary queries afterwards.
TEST(ListenNotifyTwoConnections, PeerNotifyDoesNotBreakListener) {
    auto a = std::make_unique<qb::pg::tcp::database>();
    auto b = std::make_unique<qb::pg::tcp::database>();
    if (!qb::io::async::run_sync(a->connect(dsn_tcp_string()))
        || !qb::io::async::run_sync(b->connect(dsn_tcp_string()))) {
        GTEST_SKIP() << qb::pg::test::kDaemonUnreachableSentinel;
    }

    ASSERT_TRUE(a->execute("LISTEN qb_proto_peer", discard_query, discard_error).await());
    ASSERT_TRUE(b->execute("SELECT pg_notify('qb_proto_peer', 'from_b')", discard_query,
                           discard_error)
                    .await());
    EXPECT_TRUE(a->execute("SELECT 1", discard_query, discard_error).await());

    a->disconnect();
    b->disconnect();
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
