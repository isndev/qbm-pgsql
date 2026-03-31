/**
 * @file test-notify.cpp
 * @brief LISTEN/NOTIFY: `notify()` / `listen()`, consumers, and `qb::io::async::run_sync` coverage.
 */
#include <atomic>
#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include "../pgsql.h"
#include "test_config.hpp"

using namespace qb::pg;

namespace {

constexpr std::string_view kChan  = "qb_pgsql_notify_test_ch";
constexpr std::string_view kChan2 = "qb_pgsql_notify_test_ch2";
constexpr std::string_view kOther = "qb_pgsql_notify_other_ch";

inline void
io_pump(int iterations = 4000) noexcept {
    for (int i = 0; i < iterations; ++i)
        qb::io::async::run(EVRUN_NOWAIT);
}

} // namespace

class PgNotifyTest : public ::testing::Test {
protected:
    std::unique_ptr<qb::pg::tcp::database> pub_;

    void
    SetUp() override {
        pub_ = std::make_unique<qb::pg::tcp::database>();
        if (!qb::io::async::run_sync(pub_->connect(qb::pg::test::dsn_tcp_string()))) {
            GTEST_SKIP() << "PostgreSQL not reachable";
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

TEST_F(PgNotifyTest, Notify_Callback_WithPayload) {
    ASSERT_TRUE(
        pub_->notify(std::string(kChan), "hello-payload", discard_query, discard_error).await());
}

TEST_F(PgNotifyTest, Notify_Coro_WithPayload) {
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto r = co_await pub_->notify(std::string(kChan), "coro-payload");
        co_return         r.ok();
    }()));
}

TEST_F(PgNotifyTest, Notify_Coro_NoPayloadOverload) {
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        auto r = co_await pub_->notify(std::string(kChan));
        co_return         r.ok();
    }()));
}

TEST_F(PgNotifyTest, Notify_Coro_ViaRunSyncOnAwaiter) {
    ASSERT_TRUE(qb::io::async::run_sync(pub_->notify(std::string(kChan), "direct-awaiter")));
}

TEST_F(PgNotifyTest, Listen_Coro_ViaRunSync) {
    ASSERT_TRUE(qb::io::async::run_sync(pub_->listen(std::string(kChan))));
    ASSERT_TRUE(pub_->unlisten(std::string(kChan), discard_query, discard_error).await());
}

TEST_F(PgNotifyTest, NotifyCbConsumer_ReceivesPayload) {
    int                             hits{};
    std::string                     last_payload;
    qb::pg::tcp::notify_cb_consumer sub{qb::pg::test::dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan)) {
            last_payload = std::move(n.payload);
            ++hits;
        }
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(qb::pg::test::dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "cb-ok", discard_query, discard_error).await());
    for (int i = 0; i < 2000 && hits == 0; ++i)
        qb::io::async::run(EVRUN_NOWAIT);
    EXPECT_EQ(hits, 1);
    EXPECT_EQ(last_payload, "cb-ok");
    EXPECT_TRUE(sub.is_connected());
    sub.disconnect();
}

TEST_F(PgNotifyTest, NotifyCbConsumer_ListenUnlisten_NoDelivery) {
    std::atomic<int>                hits{0};
    qb::pg::tcp::notify_cb_consumer sub{qb::pg::test::dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan))
            hits.fetch_add(1, std::memory_order_relaxed);
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(qb::pg::test::dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(sub.unlisten(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(
        pub_->notify(std::string(kChan), "after-unlisten", discard_query, discard_error).await());
    io_pump(2000);
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
    sub.disconnect();
}

TEST_F(PgNotifyTest, NotifyCbConsumer_ListenUnlisten_Coro_RunSync) {
    std::atomic<int>                hits{0};
    qb::pg::tcp::notify_cb_consumer sub{qb::pg::test::dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan))
            hits.fetch_add(1, std::memory_order_relaxed);
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(qb::pg::test::dsn_tcp_string())));
    ASSERT_TRUE(qb::io::async::run_sync(sub.listen(std::string(kChan))));
    ASSERT_TRUE(qb::io::async::run_sync(sub.unlisten(std::string(kChan))));
    ASSERT_TRUE(pub_->notify(std::string(kChan), "x", discard_query, discard_error).await());
    io_pump(2000);
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
    sub.disconnect();
}

TEST_F(PgNotifyTest, NotifyCoConsumer_Receive) {
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        qb::pg::tcp::notify_co_consumer sub(qb::pg::test::dsn_tcp_string());
        if (!co_await sub.connect(qb::pg::test::dsn_tcp_string()))
            co_return false;
        auto lr = co_await sub.listen(std::string(kChan));
        if (!lr.ok())
            co_return false;
        if (!pub_->notify(std::string(kChan), "co-recv", discard_query, discard_error).await())
            co_return false;
        auto n = co_await sub.receive();
        sub.disconnect();
        co_return n.has_value() && n->channel == std::string(kChan) && n->payload == "co-recv";
    }()));
}

TEST_F(PgNotifyTest, NotifyCoConsumer_TwoSequentialReceives_Ordered) {
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        qb::pg::tcp::notify_co_consumer sub(qb::pg::test::dsn_tcp_string());
        if (!co_await sub.connect(qb::pg::test::dsn_tcp_string()))
            co_return false;
        if (!(co_await sub.listen(std::string(kChan))).ok())
            co_return false;
        if (!pub_->notify(std::string(kChan), "first", discard_query, discard_error).await())
            co_return false;
        if (!pub_->notify(std::string(kChan), "second", discard_query, discard_error).await())
            co_return false;
        auto a = co_await sub.receive();
        auto b = co_await sub.receive();
        sub.disconnect();
        co_return a.has_value() && b.has_value() && a->payload == "first" && b->payload == "second";
    }()));
}

TEST_F(PgNotifyTest, NotifyCoConsumer_CallbackAndReceive) {
    std::atomic<int> cb_hits{0};
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        qb::pg::tcp::notify_co_consumer sub(qb::pg::test::dsn_tcp_string());
        sub.on_notify([&](qb::pg::notification &&n) {
            if (n.channel == std::string(kChan) && n.payload == "both")
                cb_hits.fetch_add(1, std::memory_order_relaxed);
        });
        if (!co_await sub.connect(qb::pg::test::dsn_tcp_string()))
            co_return false;
        if (!(co_await sub.listen(std::string(kChan))).ok())
            co_return false;
        if (!pub_->notify(std::string(kChan), "both", discard_query, discard_error).await())
            co_return false;
        auto n = co_await sub.receive();
        sub.disconnect();
        co_return n.has_value() && n->payload == "both" &&
            cb_hits.load(std::memory_order_relaxed) == 1;
    }()));
}

TEST_F(PgNotifyTest, NotifyCbConsumer_ListenOneChannel_NotifyOther_NoCallback) {
    std::atomic<int>                hits{0};
    qb::pg::tcp::notify_cb_consumer sub{qb::pg::test::dsn_tcp_string()};
    sub.on_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan))
            hits.fetch_add(1, std::memory_order_relaxed);
    });
    ASSERT_TRUE(qb::io::async::run_sync(sub.connect(qb::pg::test::dsn_tcp_string())));
    ASSERT_TRUE(sub.listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kOther), "noise", discard_query, discard_error).await());
    io_pump(2000);
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
    sub.disconnect();
}

TEST_F(PgNotifyTest, NotifyCoConsumer_ServerBackendPidPositive) {
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        qb::pg::tcp::notify_co_consumer sub(qb::pg::test::dsn_tcp_string());
        if (!co_await sub.connect(qb::pg::test::dsn_tcp_string()))
            co_return false;
        if (!(co_await sub.listen(std::string(kChan))).ok())
            co_return false;
        if (!pub_->notify(std::string(kChan), "pid", discard_query, discard_error).await())
            co_return false;
        auto n = co_await sub.receive();
        sub.disconnect();
        co_return n.has_value() && n->server_backend_pid > 0;
    }()));
}

TEST_F(PgNotifyTest, NotifyCoConsumer_ReceiveNulloptAfterDisconnect) {
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        qb::pg::tcp::notify_co_consumer sub(qb::pg::test::dsn_tcp_string());
        if (!co_await sub.connect(qb::pg::test::dsn_tcp_string()))
            co_return false;
        if (!(co_await sub.listen(std::string(kChan))).ok())
            co_return false;
        sub.disconnect();
        auto n = co_await sub.receive();
        co_return !n.has_value();
    }()));
}

TEST_F(PgNotifyTest, NotifyCoConsumer_ChannelFull_DroppedHandler) {
    std::atomic<int> dropped{0};
    ASSERT_TRUE(qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
        qb::pg::tcp::notify_co_consumer sub(qb::pg::test::dsn_tcp_string(), /*capacity*/ 1);
        sub.on_notify_dropped([&](qb::pg::notification &&n) {
            if (n.channel == std::string(kChan))
                dropped.fetch_add(1, std::memory_order_relaxed);
        });
        if (!co_await sub.connect(qb::pg::test::dsn_tcp_string()))
            co_return false;
        if (!(co_await sub.listen(std::string(kChan))).ok())
            co_return false;
        if (!pub_->notify(std::string(kChan), "one", discard_query, discard_error).await())
            co_return false;
        if (!pub_->notify(std::string(kChan), "two", discard_query, discard_error).await())
            co_return false;
        // Drain the subscriber socket so the second NotificationResponse reaches deliver_pg_notify
        // before receive(); otherwise recv() can complete synchronously from the buffer and skip
        // running the loop, leaving the second notify undelivered (dropped stays 0).
        io_pump();
        auto first = co_await sub.receive();
        sub.disconnect();
        co_return first.has_value() && first->payload == "one" &&
            dropped.load(std::memory_order_relaxed) == 1;
    }()));
}

TEST_F(PgNotifyTest, PlainDatabase_OnIncomingNotify) {
    int hits = 0;
    pub_->on_incoming_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan))
            ++hits;
    });
    ASSERT_TRUE(pub_->listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "plain", discard_query, discard_error).await());

    for (int i = 0; i < 2000 && hits == 0; ++i)
        qb::io::async::run(EVRUN_NOWAIT);

    EXPECT_EQ(hits, 1);
    pub_->on_incoming_notify({});
    ASSERT_TRUE(pub_->unlisten_all(discard_query, discard_error).await());
}

TEST_F(PgNotifyTest, PlainDatabase_TwoChannels_TwoHits) {
    int hits = 0;
    pub_->on_incoming_notify([&](qb::pg::notification &&n) {
        if (n.channel == std::string(kChan) || n.channel == std::string(kChan2))
            ++hits;
    });
    ASSERT_TRUE(pub_->listen(std::string(kChan), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->listen(std::string(kChan2), discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan), "a", discard_query, discard_error).await());
    ASSERT_TRUE(pub_->notify(std::string(kChan2), "b", discard_query, discard_error).await());
    io_pump(2000);
    EXPECT_EQ(hits, 2);
    pub_->on_incoming_notify({});
    ASSERT_TRUE(pub_->unlisten_all(discard_query, discard_error).await());
}

TEST_F(PgNotifyTest, PlainDatabase_EmptyPayload) {
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
    io_pump(2000);
    EXPECT_EQ(hits, 1);
    EXPECT_TRUE(last.empty());
    pub_->on_incoming_notify({});
    ASSERT_TRUE(pub_->unlisten(std::string(kChan), discard_query, discard_error).await());
}
