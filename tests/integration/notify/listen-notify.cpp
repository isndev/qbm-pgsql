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
#include <cstdint>
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/pg_pump.hpp"
#include "../../shared/test_config.hpp"
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;
using qb::pg::test::dsn_tcp_string;
using qb::pg::test::pump_until;

namespace listen_notify_test {

constexpr std::string_view kChan     = "qb_pgsql_notify_test_ch";
constexpr std::string_view kChan2    = "qb_pgsql_notify_test_ch2";
constexpr std::string_view kOther    = "qb_pgsql_notify_other_ch";
constexpr auto             kDeadline = std::chrono::seconds(5);

/// Publisher fixture: a connected db used to emit NOTIFYs; skips when daemon is down.
class ListenNotify : public ::testing::Test {
protected:
    std::unique_ptr<qb::pg::tcp::database> pub_;

    void
    SetUp() override {
        pub_ = std::make_unique<qb::pg::tcp::database>();
        if (!qb::io::async::run_sync(pub_->connect(dsn_tcp_string())))
            GTEST_SKIP() << qb::pg::test::kDaemonUnreachableSentinel << " (postgres at " << dsn_tcp_string() << " not reachable)";
    }

    void
    TearDown() override {
        if (pub_) {
            pub_->disconnect();
            pub_.reset();
        }
    }
};

std::string
notify_boundary_payload(bool multibyte) {
    if (!multibyte)
        return std::string(7999, 'a');
    std::string payload;
    payload.reserve(7999);
    for (int i = 0; i < 3999; ++i)
        payload.append("\xc3\xa9"); // UTF-8: two bytes per code point.
    payload.push_back('a');
    return payload;
}

/// `receive()` bounded by a deadline: a regression that stops the stream fails the test instead of hanging it.
qb::io::async::task<std::optional<qb::pg::notification>>
receive_within(qb::pg::tcp::notify_co_consumer &sub, std::chrono::seconds limit = std::chrono::seconds(5)) {
    try {
        co_return co_await qb::io::async::with_deadline(sub.receive(), std::chrono::steady_clock::now() + limit);
    } catch (qb::io::async::timeout_error const &) {
        ADD_FAILURE() << "receive() did not resolve within " << limit.count() << " s";
        co_return std::nullopt;
    }
}

class NotifyCallbackGuard {
    qb::pg::tcp::database &db_;

public:
    explicit NotifyCallbackGuard(qb::pg::tcp::database &db)
        : db_(db) {}
    ~NotifyCallbackGuard() {
        db_.on_incoming_notify({});
    }
};

} // namespace listen_notify_test

using namespace listen_notify_test;

// ---------------------------------------------------------------------------
// notify() / listen() basic awaitable + callback surfaces
// ---------------------------------------------------------------------------

TEST_F(ListenNotify, Notify_Callback_WithPayload) {
    ASSERT_TRUE(pub_->notify(std::string(kChan), "hello-payload", discard_query, discard_error).await());
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

TEST_F(ListenNotify, NotifyPayloadBytesSurviveBothStringSettingsAndPublicForms) {
    const std::string              channel = "qb_pgsql_notify_\"quoted";
    const std::vector<std::string> payloads{
        "literal\\nsequence", "apostrophe's", "trailing\\", "actual\nnewline", "$qb_notify$inside$qb_notify_1$", ""
    };
    std::vector<std::string> received;
    pub_->on_incoming_notify([&](qb::pg::notification &&n) {
        if (n.channel == channel)
            received.push_back(std::move(n.payload));
    });
    ASSERT_TRUE(pub_->listen(channel, discard_query, discard_error).await());

    for (bool standard_strings : {false, true}) {
        ASSERT_TRUE(pub_->execute(standard_strings ? "SET standard_conforming_strings = on" : "SET standard_conforming_strings = off",
                                  discard_query, discard_error)
                        .await());
        for (const std::string &payload : payloads) {
            for (bool coroutine_form : {false, true}) {
                const std::size_t next = received.size() + 1;
                if (coroutine_form)
                    ASSERT_TRUE(qb::io::async::run_sync(pub_->notify(channel, payload)));
                else
                    ASSERT_TRUE(pub_->notify(channel, payload, discard_query, discard_error).await());
                ASSERT_TRUE(pump_until([&] { return received.size() >= next; }, kDeadline))
                    << "missing NOTIFY with standard_conforming_strings=" << standard_strings;
                ASSERT_EQ(received.size(), next);
                EXPECT_EQ(received.back(), payload)
                    << "payload changed under standard_conforming_strings=" << standard_strings << " (coroutine=" << coroutine_form << ')';
            }
        }
    }

    pub_->on_incoming_notify({});
    ASSERT_TRUE(pub_->unlisten(channel, discard_query, discard_error).await());
}

TEST_F(ListenNotify, NotifyDelimiterBoundaryOverlapKeepsPayloadLiteral) {
    const std::string              channel = "qb_pgsql_notify_overlap";
    const std::vector<std::string> payloads{"abc$qb_notify", "$qb_notify$abc$qb_notify_1"};
    std::vector<std::string>       received;
    pub_->on_incoming_notify([&](qb::pg::notification &&n) {
        if (n.channel == channel)
            received.push_back(std::move(n.payload));
    });
    ASSERT_TRUE(pub_->listen(channel, discard_query, discard_error).await());

    for (bool standard_strings : {false, true}) {
        ASSERT_TRUE(pub_->execute(standard_strings ? "SET standard_conforming_strings = on" : "SET standard_conforming_strings = off",
                                  discard_query, discard_error)
                        .await());
        for (const std::string &payload : payloads) {
            for (bool coroutine_form : {false, true}) {
                const std::size_t next = received.size() + 1;
                const bool sent = coroutine_form ? static_cast<bool>(qb::io::async::run_sync(pub_->notify(channel, payload)))
                                                 : static_cast<bool>(pub_->notify(channel, payload, discard_query, discard_error).await());
                EXPECT_TRUE(sent) << "delimiter-boundary NOTIFY failed with standard_conforming_strings=" << standard_strings
                                  << " (coroutine=" << coroutine_form << ')';
                if (!sent)
                    continue;
                ASSERT_TRUE(pump_until([&] { return received.size() >= next; }, kDeadline));
                ASSERT_EQ(received.size(), next);
                EXPECT_EQ(received.back(), payload)
                    << "delimiter-boundary payload changed under standard_conforming_strings=" << standard_strings
                    << " (coroutine=" << coroutine_form << ')';
            }
        }
    }

    pub_->on_incoming_notify({});
    ASSERT_TRUE(pub_->unlisten(channel, discard_query, discard_error).await());
}

TEST_F(ListenNotify, NotifyPreservesSjisMultibyteTrailingBackslash) {
    const std::string        channel = "qb_pgsql_notify_sjis";
    const std::string        payload{static_cast<char>(0x95), static_cast<char>(0x5c)}; // 表 in SJIS
    std::vector<std::string> received;
    pub_->on_incoming_notify([&](qb::pg::notification &&n) {
        if (n.channel == channel)
            received.push_back(std::move(n.payload));
    });
    ASSERT_TRUE(pub_->execute("SET client_encoding = SJIS", discard_query, discard_error).await());
    ASSERT_TRUE(pub_->listen(channel, discard_query, discard_error).await());

    for (bool standard_strings : {false, true}) {
        ASSERT_TRUE(pub_->execute(standard_strings ? "SET standard_conforming_strings = on" : "SET standard_conforming_strings = off",
                                  discard_query, discard_error)
                        .await());
        for (bool coroutine_form : {false, true}) {
            const std::size_t next = received.size() + 1;
            const bool        sent = coroutine_form ? static_cast<bool>(qb::io::async::run_sync(pub_->notify(channel, payload)))
                                                    : static_cast<bool>(pub_->notify(channel, payload, discard_query, discard_error).await());
            EXPECT_TRUE(sent) << "SJIS NOTIFY failed under standard_conforming_strings=" << standard_strings << " (coroutine=" << coroutine_form
                              << ')';
            if (!sent)
                continue;
            ASSERT_TRUE(pump_until([&] { return received.size() >= next; }, kDeadline));
            ASSERT_EQ(received.size(), next);
            EXPECT_EQ(received.back(), payload) << "SJIS payload changed under standard_conforming_strings=" << standard_strings
                                                << " (coroutine=" << coroutine_form << ')';
        }
    }

    pub_->on_incoming_notify({});
    ASSERT_TRUE(pub_->unlisten(channel, discard_query, discard_error).await());
}

TEST_F(ListenNotify, Listen_Unlisten_Coro_ViaRunSync) {
    ASSERT_TRUE(qb::io::async::run_sync(pub_->listen(std::string(kChan))));
    ASSERT_TRUE(pub_->unlisten(std::string(kChan), discard_query, discard_error).await());
}

// unlisten_all() has a coroutine overload distinct from the callback overload the
// other tests use; co_await it after LISTENing on two channels (issues UNLISTEN *).
TEST_F(ListenNotify, UnlistenAll_Coro_ViaRunSync) {
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        if (!(co_await pub_->listen(std::string(kChan))).ok())
            co_return false;
        if (!(co_await pub_->listen(std::string(kChan2))).ok())
            co_return false;
        co_return (co_await pub_->unlisten_all()).ok();
    }()));
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

    EXPECT_TRUE(pump_until([&] { return hits >= 1; }, kDeadline)) << "notification for " << kChan << " never delivered";
    EXPECT_EQ(hits, 1);
    EXPECT_EQ(last_payload, "cb-ok");
    EXPECT_TRUE(sub.is_connected());
    sub.disconnect();
}

// A callback-only consumer retains nothing, so it drops nothing (Huly QB-1092). notify_cb_consumer was an
// alias of the queueing consumer: every notification also went into receive()'s bounded queue (8192 by
// default), which a callback-only user never reads -- it kept up to 8192 payloads alive, then reported
// every later NOTIFY as dropped although the callback had handled it. 8192 + 8 notifications, published
// in one transaction (pg_notify over generate_series), all reach the callback and none counts as dropped.
TEST_F(ListenNotify, CbConsumer_RetainsNothingAndDropsNothing) {
    constexpr int                   kNotifications = 8192 + 8;
    int                             hits{};
    qb::pg::tcp::notify_cb_consumer sub{dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan))
            ++hits;
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->execute("SELECT pg_notify('" + std::string(kChan) + "', g::text) FROM generate_series(1, "
                                  + std::to_string(kNotifications) + ") AS g",
                              discard_query, discard_error)
                    .await());

    ASSERT_TRUE(pump_until([&] { return hits >= kNotifications; }, kDeadline)) << "only " << hits << " notifications reached the callback";
    EXPECT_EQ(hits, kNotifications);
    EXPECT_EQ(sub.stats().notifications_received, static_cast<std::uint64_t>(kNotifications));
    EXPECT_EQ(sub.stats().notifications_dropped, 0u) << "a callback-only consumer queued its notifications and then dropped them";
    sub.disconnect();
}

// The callback-only consumer contains a throwing callback as well, and goes on delivering.
TEST_F(ListenNotify, CbConsumer_OnNotifyCallbackThrows_StillDelivers) {
    int                             calls{};
    qb::pg::tcp::notify_cb_consumer sub{dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel != std::string(kChan))
            return;
        if (++calls == 1)
            throw std::runtime_error("intentional on_notify throw");
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "throws", discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "delivered", discard_query, discard_error).await());
    EXPECT_TRUE(pump_until([&] { return calls >= 2; }, kDeadline)) << "the consumer stopped delivering after a throwing callback";
    EXPECT_EQ(calls, 2);
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

    EXPECT_TRUE(pump_until([&] { return received.size() >= 3; }, kDeadline)) << "expected 3 notifications, got " << received.size();
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
    ASSERT_TRUE(pub_->notify(std::string(kChan), "after-unlisten", discard_query, discard_error).await());

    // Give any (wrongly) delivered notification a bounded window to show up, then assert
    // none did.
    qb::pg::test::pump_for(std::chrono::milliseconds(300));
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
    sub.disconnect();
}

TEST_F(ListenNotify, CbConsumer_OtherChannel_NoCallback) {
    std::atomic<int>                hits{0};
    std::string                     unexpected_channel;
    qb::pg::tcp::notify_cb_consumer sub{dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        unexpected_channel = std::move(n.channel);
        hits.fetch_add(1, std::memory_order_relaxed);
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kOther), "noise", discard_query, discard_error).await());

    qb::pg::test::pump_for(std::chrono::milliseconds(300));
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0) << "unexpected callback for channel " << unexpected_channel;
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
    ASSERT_TRUE(pub_->notify(std::string(kChan), "after-reconnect", discard_query, discard_error).await());

    EXPECT_TRUE(pump_until([&] { return hits >= 1; }, kDeadline)) << "notification not delivered after reconnect + re-LISTEN";
    EXPECT_EQ(hits, 1);
    sub.disconnect();
}

// The coroutine side of the same reconnect (Huly QB-252). A disconnect closes receive()'s queue, and a
// channel closes for good: until the fix the reconnected consumer dropped every NOTIFY ("buffer full")
// and receive() yielded nullopt forever. What was received before the disconnect and never read is
// carried over, ahead of the new connection's notifications. The callback only counts arrivals.
TEST_F(ListenNotify, CoConsumer_ReconnectAndReListen_ReceiveServesTheNewConnection) {
    int                             arrived{};
    qb::pg::tcp::notify_co_consumer sub{dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&) { ++arrived; });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "kept", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return arrived == 1; }, kDeadline)) << "the first NOTIFY never arrived";
    sub.disconnect(); // "kept" was received and never read

    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "after", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return arrived == 2; }, kDeadline)) << "the NOTIFY after the reconnect never arrived";

    auto first  = qb::io::async::run_sync(sub.receive());
    auto second = qb::io::async::run_sync(sub.receive());
    ASSERT_TRUE(first.has_value()) << "what was received before the disconnect is still read";
    EXPECT_EQ(first->payload, "kept");
    ASSERT_TRUE(second.has_value()) << "receive() must serve the new connection";
    EXPECT_EQ(second->payload, "after");
    sub.disconnect();
}

// A throwing on_notify callback must be swallowed (logged) by deliver_pg_notify and must
// NOT prevent the notification from being queued for receive(): the consumer keeps working.
// Exercises the catch(std::exception&) arm of the on_notify_callback_ path. The queueing consumer is
// notify_co_consumer: notify_cb_consumer has no queue since Huly QB-1092.
TEST_F(ListenNotify, CoConsumer_OnNotifyCallbackThrows_StillQueuesAndSurvives) {
    std::atomic<int>                cb_calls{0};
    qb::pg::tcp::notify_co_consumer sub{dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan)) {
            cb_calls.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("intentional on_notify throw");
        }
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "throw-but-queue", discard_query, discard_error).await());

    // Drain the subscriber socket so the NotificationResponse reaches deliver_pg_notify,
    // where the throwing callback fires and is swallowed, then the message is still queued.
    EXPECT_TRUE(pump_until([&] { return cb_calls.load(std::memory_order_relaxed) >= 1; }, kDeadline))
        << "throwing on_notify callback was never invoked";
    EXPECT_EQ(cb_calls.load(std::memory_order_relaxed), 1);

    // The throw did not poison the queue: receive() still yields the notification.
    bool got = false;
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto n = co_await sub.receive();
        got    = n.has_value() && n->payload == "throw-but-queue";
        co_return got;
    }()));
    EXPECT_TRUE(got);
    EXPECT_TRUE(sub.is_connected());
    sub.disconnect();
}

// A throwing on_notify_dropped callback (capacity overflow path) must also be swallowed:
// with capacity 1 and 2 queued notifications, the 2nd routes to on_notify_dropped, whose
// throw is caught. The consumer remains usable.
TEST_F(ListenNotify, CoConsumer_OnDroppedCallbackThrows_Swallowed) {
    std::atomic<int>                dropped_calls{0};
    qb::pg::tcp::notify_co_consumer sub(dsn_tcp_string(), /*capacity*/ 1);
    sub.on_notify_dropped([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan)) {
            dropped_calls.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("intentional on_notify_dropped throw");
        }
    });

    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        if (!co_await sub.connect(dsn_tcp_string()))
            co_return false;
        if (!(co_await sub.listen(std::string(kChan))).ok())
            co_return false;
        // Capacity 1, two notifications -> the second overflows to on_notify_dropped.
        for (const char *p : {"keep", "drop"})
            if (!(co_await pub_->notify(std::string(kChan), p)).ok())
                co_return false;
        co_return true;
    }()));

    // Drain so both NotificationResponses reach the queue; the second triggers the
    // (throwing) dropped handler before the first receive() frees a slot.
    EXPECT_TRUE(pump_until([&] { return dropped_calls.load(std::memory_order_relaxed) >= 1; }, kDeadline))
        << "overflow notification never routed to on_notify_dropped";
    EXPECT_EQ(dropped_calls.load(std::memory_order_relaxed), 1);

    // Queue still holds the first (kept) notification despite the dropped-handler throw.
    bool got_keep = false;
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto n   = co_await sub.receive();
        got_keep = n.has_value() && n->payload == "keep";
        co_return got_keep;
    }()));
    EXPECT_TRUE(got_keep);
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
        co_return a.has_value() && b.has_value() && a->payload == "first" && b->payload == "second";
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
        co_return n.has_value() && n->payload == "both" && cb_hits.load(std::memory_order_relaxed) == 1;
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
    std::string                     dropped_payload;
    qb::pg::tcp::notify_co_consumer sub(dsn_tcp_string(), /*capacity*/ 2);
    sub.on_notify_dropped([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan)) {
            dropped_payload = std::move(n.payload);
            dropped.fetch_add(1, std::memory_order_relaxed);
        }
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

    // The drop callback is the causal barrier: do not free a queue slot until the
    // third notification has actually reached the subscriber and overflowed it.
    ASSERT_TRUE(pump_until([&] { return dropped.load(std::memory_order_relaxed) >= 1; }, kDeadline))
        << "the third notification never overflowed the capacity-two queue";
    EXPECT_EQ(dropped.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(dropped_payload, "three");
    // The connection counts them too (Huly QB-115): three received, one of them dropped.
    EXPECT_EQ(sub.stats().notifications_received, 3u);
    EXPECT_EQ(sub.stats().notifications_dropped, 1u);

    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto first  = co_await sub.receive();
        auto second = co_await sub.receive();
        co_return first.has_value() && second.has_value() && first->payload == "one" && second->payload == "two";
    }()));
    sub.disconnect();
}

// ---------------------------------------------------------------------------
// The reconnect contract on the notify consumers (Huly QB-111)
// ---------------------------------------------------------------------------

// A run that recovers a lost connection replays the listened set -- what listen() / unlisten() left,
// not what was ever listened -- and the stream says where it has a hole: one gap marker
// (reason reconnect) before the first notification of the new session. The stream never ended.
TEST_F(ListenNotify, CoConsumer_ARunReplaysTheListenedSetAndMarksTheGap) {
    qb::pg::tcp::notify_co_consumer sub{dsn_tcp_string()};
    int                             connected = 0;
    sub.on_connected([&] { ++connected; });
    sub.enable_auto_reconnect(RetryPolicy{}.with_initial_delay(std::chrono::milliseconds(20)).with_jitter(false));
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(sub.listen(std::string(kChan2), discard_query, discard_error).await());
    ASSERT_TRUE(sub.unlisten(std::string(kChan2), discard_query, discard_error).await());
    EXPECT_EQ(sub.listened_channels(), (std::set<std::string, std::less<>>{std::string(kChan)}));
    ASSERT_TRUE(pub_->notify(std::string(kChan), "before", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return sub.stats().notifications_received == 1; }, kDeadline));
    EXPECT_EQ(sub.stats().gaps, 0u) << "the first ready session of a lifecycle follows no hole";

    ASSERT_TRUE(pub_->execute("SELECT pg_terminate_backend(" + std::to_string(sub.backend_pid()) + ")", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return connected == 2; }, kDeadline)) << "the run never reconnected";
    // The replay is server state, not a guess: the new backend listens on kChan, and only on it.
    std::string listening;
    ASSERT_TRUE(sub.execute(
                       "SELECT coalesce(string_agg(c, ','), '') FROM pg_listening_channels() AS c",
                       [&](transaction &, results r) { listening = r[0][0].as<std::string>(); }, discard_error)
                    .await());
    EXPECT_EQ(listening, std::string(kChan));
    ASSERT_TRUE(pub_->notify(std::string(kChan), "after", discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan2), "unlistened", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return sub.stats().notifications_received == 2; }, kDeadline));

    std::vector<qb::pg::notification> stream;
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        for (int i = 0; i < 3; ++i) {
            auto n = co_await receive_within(sub);
            if (!n)
                co_return false;
            stream.push_back(std::move(*n));
        }
        co_return true;
    }()))
        << "the stream ended across a recovered loss";
    ASSERT_EQ(stream.size(), 3u);
    EXPECT_EQ(stream[0].kind, notification_kind::notify);
    EXPECT_EQ(stream[0].payload, "before");
    EXPECT_EQ(stream[1].kind, notification_kind::gap) << "no gap marker before the first notification of the new session";
    EXPECT_EQ(stream[1].gap.reason, qb::io::gap_reason::reconnect);
    EXPECT_EQ(stream[1].gap.lost, 0u);
    EXPECT_EQ(stream[2].kind, notification_kind::notify);
    EXPECT_EQ(stream[2].payload, "after");
    EXPECT_EQ(sub.stats().gaps, 1u);
    EXPECT_EQ(sub.stats().reconnects, 1u);
    sub.disconnect();
}

// The callback consumer is told of the same hole by on_gap, before the next on_notify.
TEST_F(ListenNotify, CbConsumer_OnGapComesBeforeTheNextNotify) {
    std::vector<std::string>        events;
    qb::pg::tcp::notify_cb_consumer sub{dsn_tcp_string()};
    int                             connected = 0;
    bool                            ready_in_gap{}, resynced{};
    sub.on_connected([&] {
        events.push_back("connected");
        ++connected;
    });
    sub.on_notify([&](qb::pg::notification &&n) { events.push_back("notify:" + n.payload); });
    sub.on_gap([&](qb::io::gap_info gap) {
        events.push_back(std::string("gap:") + std::string(qb::io::gap_reason_name(gap.reason)));
        // The resync a gap calls for, on this very connection: it is ready when on_gap runs.
        ready_in_gap = sub.is_connected();
        sub.execute("SELECT 1", [&](transaction &, results) { resynced = true; }, discard_error);
    });
    sub.enable_auto_reconnect(RetryPolicy{}.with_initial_delay(std::chrono::milliseconds(20)).with_jitter(false));
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "1", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return events.size() == 2; }, kDeadline)); // "connected", "notify:1"

    ASSERT_TRUE(pub_->execute("SELECT pg_terminate_backend(" + std::to_string(sub.backend_pid()) + ")", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return connected == 2; }, kDeadline)) << "the run never reconnected";
    ASSERT_TRUE(pub_->notify(std::string(kChan), "2", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return events.size() == 5 && resynced; }, kDeadline));
    EXPECT_EQ(events, (std::vector<std::string>{"connected", "notify:1", "gap:reconnect", "connected", "notify:2"}));
    EXPECT_TRUE(ready_in_gap) << "on_gap ran before the connection was ready";
    EXPECT_TRUE(resynced) << "the resync query issued from on_gap was refused";
    EXPECT_EQ(sub.stats().gaps, 1u);
    sub.disconnect();
}

// The new session's LISTEN is live while the hook still runs: a NOTIFY that arrives then is held, and
// released at ready behind the gap marker -- the reader that wakes on the marker can resync on the
// same connection at once, and every notification of the new session reaches it on a ready connection.
TEST_F(ListenNotify, CoConsumer_ANotifyDuringTheHookIsHeldBehindTheMarker) {
    qb::pg::tcp::notify_co_consumer sub{dsn_tcp_string()};
    int                             connected = 0, hook_runs = 0;
    std::vector<bool>               ready_at_delivery;
    sub.on_connected([&] { ++connected; });
    sub.on_notify([&](qb::pg::notification &&) { ready_at_delivery.push_back(sub.is_connected()); });
    sub.set_handshake([&](handshake_session &hs) -> qb::io::async::task<bool> {
        if (++hook_runs == 1)
            co_return true;
        // Behind the replay, so the new session listens; then publish, and let the NOTIFY arrive while
        // the handshake still runs.
        if (!(co_await hs.execute("SELECT 1")).ok())
            co_return false;
        if (!(co_await pub_->notify(std::string(kChan), "during-hook")).ok())
            co_return false;
        co_return (co_await hs.execute("SELECT pg_sleep(0.2)")).ok();
    });
    sub.enable_auto_reconnect(RetryPolicy{}.with_initial_delay(std::chrono::milliseconds(20)).with_jitter(false));
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->execute("SELECT pg_terminate_backend(" + std::to_string(sub.backend_pid()) + ")", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return connected == 2; }, kDeadline)) << "the run never reconnected";
    ASSERT_EQ(hook_runs, 2);
    ASSERT_TRUE(pub_->notify(std::string(kChan), "after", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return sub.stats().notifications_received == 2; }, kDeadline));

    std::vector<qb::pg::notification> stream;
    bool                              resynced = false;
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        for (int i = 0; i < 3; ++i) {
            auto n = co_await receive_within(sub);
            if (!n)
                co_return false;
            if (n->kind == notification_kind::gap)
                resynced = (co_await sub.query("SELECT 1")).ok(); // the resync, on the same connection
            stream.push_back(std::move(*n));
        }
        co_return true;
    }()));
    ASSERT_EQ(stream.size(), 3u);
    EXPECT_EQ(stream[0].kind, notification_kind::gap);
    EXPECT_EQ(stream[1].payload, "during-hook");
    EXPECT_EQ(stream[2].payload, "after");
    EXPECT_TRUE(resynced) << "the resync query issued on the marker was refused";
    EXPECT_EQ(ready_at_delivery, (std::vector<bool>{true, true})) << "a notification was delivered before the session was ready";
    sub.disconnect();
}

// A disconnect() from on_gap ends the session: the release stops -- the notifications held for the
// session are not delivered -- and on_connected does not fire for it.
TEST_F(ListenNotify, CbConsumer_ADisconnectFromOnGapStopsTheRelease) {
    qb::pg::tcp::notify_cb_consumer sub{dsn_tcp_string()};
    int                             connected = 0, hook_runs = 0, gaps = 0;
    std::vector<std::string>        delivered;
    sub.on_connected([&] { ++connected; });
    sub.on_notify([&](qb::pg::notification &&n) { delivered.push_back(n.payload); });
    sub.on_gap([&](qb::io::gap_info) {
        ++gaps;
        sub.disconnect();
    });
    sub.set_handshake([&](handshake_session &hs) -> qb::io::async::task<bool> {
        if (++hook_runs == 1)
            co_return true;
        if (!(co_await hs.execute("SELECT 1")).ok())
            co_return false;
        if (!(co_await pub_->notify(std::string(kChan), "held")).ok())
            co_return false;
        co_return (co_await hs.execute("SELECT pg_sleep(0.2)")).ok();
    });
    sub.enable_auto_reconnect(RetryPolicy{}.with_initial_delay(std::chrono::milliseconds(20)).with_jitter(false));
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->execute("SELECT pg_terminate_backend(" + std::to_string(sub.backend_pid()) + ")", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return gaps == 1 && sub.state() == qb::io::connection_state::down; }, kDeadline))
        << "on_gap never ran, or its disconnect() did not end the session";
    qb::pg::test::pump_for(std::chrono::milliseconds(200));
    EXPECT_EQ(sub.down_reason(), qb::io::down_reason::user);
    EXPECT_EQ(connected, 1) << "on_connected fired for a session on_gap had ended";
    EXPECT_TRUE(delivered.empty()) << "a notification held for the ended session was delivered";
    EXPECT_EQ(hook_runs, 2);
}

// The stream ends only at a terminal state: a recovered loss keeps it open, an explicit disconnect()
// ends it -- what it held drains, then nullopt -- and down_reason() says why.
TEST_F(ListenNotify, CoConsumer_TheStreamEndsOnlyAtDown) {
    qb::pg::tcp::notify_co_consumer sub{dsn_tcp_string()};
    int                             connected = 0;
    sub.on_connected([&] { ++connected; });
    sub.enable_auto_reconnect(RetryPolicy{}.with_initial_delay(std::chrono::milliseconds(20)).with_jitter(false));
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->execute("SELECT pg_terminate_backend(" + std::to_string(sub.backend_pid()) + ")", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return connected == 2; }, kDeadline)) << "the run never reconnected";
    ASSERT_TRUE(pub_->notify(std::string(kChan), "kept", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return sub.stats().notifications_received == 1; }, kDeadline));
    sub.disconnect();
    EXPECT_EQ(sub.down_reason(), qb::io::down_reason::user);

    std::vector<qb::pg::notification> stream;
    bool                              ended = false;
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        for (int i = 0; i < 3; ++i) {
            auto n = co_await receive_within(sub);
            if (!n) {
                ended = true;
                co_return true;
            }
            stream.push_back(std::move(*n));
        }
        co_return false;
    }()))
        << "the stream did not end at down";
    EXPECT_TRUE(ended);
    ASSERT_EQ(stream.size(), 2u) << "the stream held a marker and a notification";
    EXPECT_EQ(stream[0].kind, notification_kind::gap);
    EXPECT_EQ(stream[1].payload, "kept");
}

// A full stream is a hole too: every NOTIFY that does not fit still reaches on_notify_dropped and the
// counters, and ONE marker (reason overflow, lost = every drop) ends what the reader drained.
TEST_F(ListenNotify, CoConsumer_AnOverflowIsOneCoalescedGapMarker) {
    constexpr int                   kCapacity = 4, kPublished = 10;
    int                             dropped_calls{};
    qb::pg::tcp::notify_co_consumer sub(dsn_tcp_string(), kCapacity);
    sub.on_notify_dropped([&](qb::pg::notification &&) { ++dropped_calls; });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->execute("SELECT pg_notify('" + std::string(kChan) + "', g::text) FROM generate_series(1, " + std::to_string(kPublished)
                                  + ") AS g",
                              discard_query, discard_error)
                    .await());
    ASSERT_TRUE(pump_until([&] { return sub.stats().notifications_received == kPublished; }, kDeadline));
    EXPECT_EQ(dropped_calls, kPublished - kCapacity);
    EXPECT_EQ(sub.stats().notifications_dropped, static_cast<std::uint64_t>(kPublished - kCapacity));

    std::vector<qb::pg::notification> stream;
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        for (int i = 0; i < kCapacity + 1; ++i) {
            auto n = co_await receive_within(sub);
            if (!n)
                co_return false;
            stream.push_back(std::move(*n));
        }
        co_return true;
    }()));
    ASSERT_EQ(stream.size(), static_cast<std::size_t>(kCapacity + 1));
    for (int i = 0; i < kCapacity; ++i) {
        EXPECT_EQ(stream[i].kind, notification_kind::notify);
        EXPECT_EQ(stream[i].payload, std::to_string(i + 1));
    }
    EXPECT_EQ(stream[kCapacity].kind, notification_kind::gap) << "the drops left no marker in the stream";
    EXPECT_EQ(stream[kCapacity].gap.reason, qb::io::gap_reason::overflow);
    EXPECT_EQ(stream[kCapacity].gap.lost, static_cast<std::uint64_t>(kPublished - kCapacity));
    EXPECT_EQ(sub.stats().gaps, 1u);

    // After the hole, the stream takes notifications again -- the marker said where they went missing.
    ASSERT_TRUE(pub_->notify(std::string(kChan), "next", discard_query, discard_error).await());
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto n = co_await receive_within(sub);
        co_return n && n->kind == notification_kind::notify && n->payload == "next";
    }()));
    sub.disconnect();
}

// An explicit disconnect() ends the session (Huly QB-111, the contract's R1): the listened set goes with
// it, and a later connect() is a fresh session -- nothing replayed, no gap marker.
TEST_F(ListenNotify, AnExplicitDisconnectEndsTheListeningSession) {
    qb::pg::tcp::notify_cb_consumer sub{dsn_tcp_string()};
    int                             gaps = 0;
    sub.on_gap([&](qb::io::gap_info) { ++gaps; });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    sub.disconnect();
    EXPECT_TRUE(sub.listened_channels().empty()) << "disconnect() kept the listened set";
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    std::string listening = "unset";
    ASSERT_TRUE(sub.execute(
                       "SELECT coalesce(string_agg(c, ','), '') FROM pg_listening_channels() AS c",
                       [&](transaction &, results r) { listening = r[0][0].as<std::string>(); }, discard_error)
                    .await());
    EXPECT_EQ(listening, "") << "a LISTEN of the ended session was replayed";
    EXPECT_EQ(gaps, 0) << "a fresh session was reported as a gap";
    EXPECT_EQ(sub.stats().gaps, 0u);
    EXPECT_EQ(sub.stats().reconnects, 0u);
    sub.disconnect();
}

// A LISTEN written as SQL is not in the listened set, so a run does not replay it: only listen() is
// tracked (the connection logs this once, at DEBUG).
TEST_F(ListenNotify, ARawListenIsNotReplayed) {
    qb::pg::tcp::notify_cb_consumer sub{dsn_tcp_string()};
    int                             connected = 0;
    sub.on_connected([&] { ++connected; });
    sub.enable_auto_reconnect(RetryPolicy{}.with_initial_delay(std::chrono::milliseconds(20)).with_jitter(false));
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_tcp_string())));
    ASSERT_TRUE(sub.execute("LISTEN " + std::string(kChan), discard_query, discard_error).await());
    EXPECT_TRUE(sub.listened_channels().empty());
    ASSERT_TRUE(pub_->execute("SELECT pg_terminate_backend(" + std::to_string(sub.backend_pid()) + ")", discard_query, discard_error).await());
    ASSERT_TRUE(pump_until([&] { return connected == 2; }, kDeadline)) << "the run never reconnected";
    std::string listening = "unset";
    ASSERT_TRUE(sub.execute(
                       "SELECT coalesce(string_agg(c, ','), '') FROM pg_listening_channels() AS c",
                       [&](transaction &, results r) { listening = r[0][0].as<std::string>(); }, discard_error)
                    .await());
    EXPECT_EQ(listening, "");
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

    EXPECT_TRUE(pump_until([&] { return hits >= 1; }, kDeadline)) << "plain database did not surface its own notification";
    EXPECT_EQ(hits, 1);
    EXPECT_EQ(pub_->stats().notifications_received, 1u);
    EXPECT_EQ(pub_->stats().notifications_dropped, 0u) << "a plain database queues nothing, so drops nothing";
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

    EXPECT_TRUE(pump_until([&] { return hits >= 2; }, kDeadline)) << "expected 2 notifications across channels, got " << hits;
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

    EXPECT_TRUE(pump_until([&] { return hits >= 1; }, kDeadline)) << "empty-payload notification not delivered";
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
    ASSERT_TRUE(pub_->execute("NOTIFY qb_proto_chan, 'ping'", discard_query, discard_error).await());
    EXPECT_TRUE(pub_->execute("SELECT 1", discard_query, discard_error).await());
}

// A NOTIFY emitted by a *peer* backend must not break the listener's connection: it can
// still run ordinary queries afterwards.
TEST(ListenNotifyTwoConnections, PeerNotifyDoesNotBreakListener) {
    auto a = std::make_unique<qb::pg::tcp::database>();
    auto b = std::make_unique<qb::pg::tcp::database>();
    if (!qb::io::async::run_sync(a->connect(dsn_tcp_string())) || !qb::io::async::run_sync(b->connect(dsn_tcp_string()))) {
        GTEST_SKIP() << qb::pg::test::kDaemonUnreachableSentinel;
    }

    ASSERT_TRUE(a->execute("LISTEN qb_proto_peer", discard_query, discard_error).await());
    ASSERT_TRUE(b->execute("SELECT pg_notify('qb_proto_peer', 'from_b')", discard_query, discard_error).await());
    EXPECT_TRUE(a->execute("SELECT 1", discard_query, discard_error).await());

    a->disconnect();
    b->disconnect();
}

// ---------------------------------------------------------------------------
// Empty-channel validation: build_{notify,listen,unlisten}_sql reject an empty
// channel name. Callback forms invoke on_error AND rethrow; coroutine forms
// surface it as a failed Reply (the build-throws -> pg_fail_void path).
// ---------------------------------------------------------------------------

TEST_F(ListenNotify, NotifyPayloadByteBoundary_CallbackRejects8000Locally) {
    ASSERT_TRUE(pub_->execute("SET client_encoding = UTF8", discard_query, discard_error).await());
    std::string server_encoding;
    ASSERT_TRUE(pub_->execute(
                        "SHOW server_encoding", [&](qb::pg::transaction &, resultset rows) { server_encoding = rows[0][0].as<std::string>(); },
                        discard_error)
                    .await());
    ASSERT_EQ(server_encoding, "UTF8") << "the boundary fixture requires equal client/server byte encodings";

    const std::string        channel = "qb_pgsql_notify_byte_cb";
    std::vector<std::string> received;
    NotifyCallbackGuard      callback_guard(*pub_);
    pub_->on_incoming_notify([&](qb::pg::notification &&n) {
        if (n.channel == channel)
            received.push_back(std::move(n.payload));
    });
    ASSERT_TRUE(pub_->listen(channel, discard_query, discard_error).await());

    for (bool multibyte : {false, true}) {
        SCOPED_TRACE(multibyte ? "UTF8 multibyte bytes" : "ASCII bytes");
        const std::string accepted = notify_boundary_payload(multibyte);
        ASSERT_EQ(accepted.size(), 7999u);
        const std::size_t next = received.size() + 1;
        ASSERT_TRUE(pub_->notify(channel, accepted, discard_query, discard_error).await());
        ASSERT_TRUE(pump_until([&] { return received.size() >= next; }, kDeadline));
        ASSERT_EQ(received.size(), next);
        EXPECT_EQ(received.back(), accepted) << "all 7999 payload bytes must arrive intact";

        const std::string rejected = accepted + 'a';
        ASSERT_EQ(rejected.size(), 8000u);
        int                  error_calls        = 0;
        int                  success_calls      = 0;
        bool                 threw_client_error = false;
        std::string          error_code;
        qb::pg::transaction *pending = nullptr;
        try {
            pending = &pub_->notify(
                channel, rejected, [&](qb::pg::transaction &, resultset) { ++success_calls; },
                [&](error::db_error const &e) {
                    ++error_calls;
                    error_code = e.code;
                });
        } catch (error::client_error const &) {
            threw_client_error = true;
        }
        // Assert BEFORE driving I/O: a later server rejection is not local validation.
        // client_error uses the existing local sentinel code "00000", not an empty code.
        EXPECT_TRUE(threw_client_error);
        EXPECT_EQ(error_calls, 1);
        EXPECT_EQ(error_code, "00000");
        EXPECT_EQ(success_calls, 0);
        if (pending)
            (void) pending->await(); // Drain the deliberately accepted base request before reusing the connection.
        EXPECT_TRUE(pub_->execute("SELECT 1", discard_query, discard_error).await());
        EXPECT_EQ(received.size(), next) << "the rejected payload must not generate a notification";
    }
    ASSERT_TRUE(pub_->unlisten(channel, discard_query, discard_error).await());
}

TEST_F(ListenNotify, NotifyPayloadByteBoundary_CoroutineRejects8000Locally) {
    ASSERT_TRUE(pub_->execute("SET client_encoding = UTF8", discard_query, discard_error).await());
    std::string server_encoding;
    ASSERT_TRUE(pub_->execute(
                        "SHOW server_encoding", [&](qb::pg::transaction &, resultset rows) { server_encoding = rows[0][0].as<std::string>(); },
                        discard_error)
                    .await());
    ASSERT_EQ(server_encoding, "UTF8") << "the boundary fixture requires equal client/server byte encodings";

    const std::string        channel = "qb_pgsql_notify_byte_coro";
    std::vector<std::string> received;
    NotifyCallbackGuard      callback_guard(*pub_);
    pub_->on_incoming_notify([&](qb::pg::notification &&n) {
        if (n.channel == channel)
            received.push_back(std::move(n.payload));
    });
    ASSERT_TRUE(qb::io::async::run_sync(pub_->listen(channel)));

    for (bool multibyte : {false, true}) {
        SCOPED_TRACE(multibyte ? "UTF8 multibyte bytes" : "ASCII bytes");
        const std::string accepted = notify_boundary_payload(multibyte);
        ASSERT_EQ(accepted.size(), 7999u);
        const std::size_t next = received.size() + 1;
        ASSERT_TRUE(qb::io::async::run_sync(pub_->notify(channel, accepted)));
        ASSERT_TRUE(pump_until([&] { return received.size() >= next; }, kDeadline));
        ASSERT_EQ(received.size(), next);
        EXPECT_EQ(received.back(), accepted);

        const std::string rejected = accepted + 'a';
        ASSERT_EQ(rejected.size(), 8000u);
        auto reply = qb::io::async::run_sync(pub_->notify(channel, rejected));
        EXPECT_FALSE(reply.ok());
        if (!reply.ok()) {
            EXPECT_EQ(reply.error().code, "00000") << "server SQLSTATE " << reply.error().code << " is not local payload validation";
            EXPECT_EQ(std::string(reply.error().what()), "NOTIFY payload exceeds maximum length");
        }
        EXPECT_TRUE(pub_->execute("SELECT 1", discard_query, discard_error).await());
        EXPECT_EQ(received.size(), next) << "the rejected payload must not generate a notification";
    }
    ASSERT_TRUE(qb::io::async::run_sync(pub_->unlisten(channel)));
}

TEST_F(ListenNotify, EmptyChannel_CallbackForms_InvokeErrorAndThrow) {
    bool notify_err = false, listen_err = false, unlisten_err = false;
    bool notify_threw = false, listen_threw = false, unlisten_threw = false;

    try {
        pub_->notify("", "payload", discard_query, [&](error::db_error const &) { notify_err = true; });
    } catch (error::db_error const &) {
        notify_threw = true;
    }
    try {
        pub_->listen("", discard_query, [&](error::db_error const &) { listen_err = true; });
    } catch (error::db_error const &) {
        listen_threw = true;
    }
    try {
        pub_->unlisten("", discard_query, [&](error::db_error const &) { unlisten_err = true; });
    } catch (error::db_error const &) {
        unlisten_threw = true;
    }

    EXPECT_TRUE(notify_err);
    EXPECT_TRUE(notify_threw);
    EXPECT_TRUE(listen_err);
    EXPECT_TRUE(listen_threw);
    EXPECT_TRUE(unlisten_err);
    EXPECT_TRUE(unlisten_threw);
    EXPECT_TRUE(pub_->execute("SELECT 1", discard_query, discard_error).await())
        << "connection must remain usable after rejected empty-channel calls";
}

TEST_F(ListenNotify, EmptyChannel_CoroForms_FailGracefully) {
    bool notify_failed = false, listen_failed = false, unlisten_failed = false, survived = false;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto n          = co_await pub_->notify("", "payload");
        notify_failed   = !n.ok();
        auto l          = co_await pub_->listen("");
        listen_failed   = !l.ok();
        auto u          = co_await pub_->unlisten("");
        unlisten_failed = !u.ok();
        auto ok         = co_await pub_->query("SELECT 1");
        survived        = ok.ok();
        co_return;
    }());
    EXPECT_TRUE(notify_failed);
    EXPECT_TRUE(listen_failed);
    EXPECT_TRUE(unlisten_failed);
    EXPECT_TRUE(survived);
}

// ---------------------------------------------------------------------------
// SSL (stcp) consumer — drives the `qb::pg::tcp::ssl::notify_co_consumer` /
// `notify_cb_consumer` instantiation and, through it, the entire
// `Database<stcp, void>` protocol path for NOTIFY: on_notification_response,
// on_command_complete (LISTEN/NOTIFY/UNLISTEN), on_ready_for_query, consume/
// deliver_pg_notify over TLS. The publisher must live on the SAME TLS daemon
// (NOTIFY does not cross independent PostgreSQL servers), so both sides use the
// SSL DSN. Skips (never fails) when the TLS daemon is unreachable.
// ---------------------------------------------------------------------------
#ifdef QB_HAS_SSL

using qb::pg::test::dsn_ssl_string;

namespace {

[[nodiscard]] bool
ssl_dsn_pinned() noexcept {
    const char *v = std::getenv("QB_PG_SSL_DSN");
    return v != nullptr && v[0] != '\0';
}

/// Publisher fixture on the TLS daemon: a connected SSL db used to emit NOTIFYs.
class SslListenNotify : public ::testing::Test {
protected:
    std::unique_ptr<qb::pg::tcp::ssl::database> pub_;

    void
    SetUp() override {
        pub_ = std::make_unique<qb::pg::tcp::ssl::database>();
        if (!qb::io::async::run_sync(pub_->connect(dsn_ssl_string()))) {
            if (!ssl_dsn_pinned())
                GTEST_SKIP() << qb::pg::test::kDaemonUnreachableSentinel << " (TLS postgres at " << dsn_ssl_string()
                             << " not reachable and QB_PG_SSL_DSN unset)";
            else
                FAIL() << "QB_PG_SSL_DSN is set but TLS connect failed: " << dsn_ssl_string();
        }
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

// Callback consumer over TLS: connect -> LISTEN -> receive a NOTIFY emitted by a second
// TLS backend. Covers Database<stcp>::on_notification_response + consume/deliver_pg_notify
// + the stcp on_command_complete/on_ready_for_query path for LISTEN.
TEST_F(SslListenNotify, SslCbConsumer_ReceivesPayload) {
    int                                  hits{};
    std::string                          last_payload;
    qb::pg::tcp::ssl::notify_cb_consumer sub{dsn_ssl_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan)) {
            last_payload = std::move(n.payload);
            ++hits;
        }
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(dsn_ssl_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "ssl-cb-ok", discard_query, discard_error).await());

    EXPECT_TRUE(pump_until([&] { return hits >= 1; }, kDeadline)) << "TLS notification for " << kChan << " never delivered";
    EXPECT_EQ(hits, 1);
    EXPECT_EQ(last_payload, "ssl-cb-ok");
    EXPECT_TRUE(sub.is_connected());
    sub.disconnect();
}

// Coroutine receive() over TLS, plus multi-payload ordering and UNLISTEN. Covers the
// stcp on_command_complete path for both NOTIFY and UNLISTEN commands and exercises the
// stcp consumer's channel receive() across more than one message.
TEST_F(SslListenNotify, SslCoConsumer_ReceiveOrderedAndUnlisten) {
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        qb::pg::tcp::ssl::notify_co_consumer sub(dsn_ssl_string());
        if (!co_await sub.connect(dsn_ssl_string()))
            co_return false;
        if (!(co_await sub.listen(std::string(kChan))).ok())
            co_return false;
        if (!(co_await pub_->notify(std::string(kChan), "ssl-1")).ok())
            co_return false;
        if (!(co_await pub_->notify(std::string(kChan), "ssl-2")).ok())
            co_return false;
        auto a = co_await sub.receive();
        auto b = co_await sub.receive();
        if (!(a.has_value() && b.has_value() && a->payload == "ssl-1" && b->payload == "ssl-2"))
            co_return false;
        if (a->server_backend_pid <= 0)
            co_return false;
        // UNLISTEN over TLS exercises the stcp command-complete path for UNLISTEN.
        co_return (co_await sub.unlisten(std::string(kChan))).ok();
    }()));
}

// SSL plain database surfaces its own NOTIFY through on_incoming_notify — drives the
// Database<stcp>::on_incoming_notify hook (distinct from the consumer subclasses) and the
// stcp on_notification_response routing on a non-consumer database.
TEST_F(SslListenNotify, SslPlainDatabase_OnIncomingNotify) {
    int hits = 0;
    pub_->on_incoming_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan))
            ++hits;
    });
    ASSERT_TRUE(pub_->listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "ssl-plain", discard_query, discard_error).await());

    EXPECT_TRUE(pump_until([&] { return hits >= 1; }, kDeadline)) << "SSL plain database did not surface its own notification";
    EXPECT_EQ(hits, 1);
    pub_->on_incoming_notify({});
    ASSERT_TRUE(pub_->unlisten_all(discard_query, discard_error).await());
}

#endif // QB_HAS_SSL

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
