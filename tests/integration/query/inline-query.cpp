/**
 * @file inline-query.cpp
 * @brief Live PostgreSQL coverage for sequential inline queries and their failure paths (QB-106).
 */

#include <string>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qbm/pgsql/pgsql.h>

#include "../../shared/pg_integration_fixture.hpp"

namespace inline_query_test {

class InlineQueryTest : public qb::pg::test::PgIntegrationTest {};

TEST_F(InlineQueryTest, SequentialQueriesKeepTheirOwnSQLAndColumnTypes) {
    int         first  = -1;
    int         second = -1;
    std::string text;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto a = co_await db_->query("SELECT $1::int + 1", 10);
        auto b = co_await db_->query("SELECT $1::int + 100", 20);
        auto c = co_await db_->query("SELECT $1::text || '!'", std::string("hello"));
        if (a.ok() && a.result().size() == 1)
            first = a.result()[0][0].as<int>();
        if (b.ok() && b.result().size() == 1)
            second = b.result()[0][0].as<int>();
        if (c.ok() && c.result().size() == 1)
            text = c.result()[0][0].as<std::string>();
    }());
    EXPECT_EQ(first, 11);
    EXPECT_EQ(second, 120);
    EXPECT_EQ(text, "hello!");
    db_->disconnect();
}

TEST_F(InlineQueryTest, ParseBindAndExecuteErrorsLeaveTheQueueUsable) {
    std::string parse_code;
    std::string bind_code;
    std::string execute_code;
    int         after_parse   = -1;
    int         after_bind    = -1;
    int         after_execute = -1;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto bad_parse = co_await db_->query("SELECT $1::int +", 10);
        if (!bad_parse.ok())
            parse_code = bad_parse.error().code;
        auto a = co_await db_->query("SELECT $1::int + 1", 10);
        if (a.ok() && a.result().size() == 1)
            after_parse = a.result()[0][0].as<int>();

        // Parse accepts $2 with an inferred int OID; Bind then supplies only one value.
        auto bad_bind = co_await db_->query("SELECT $1::int + $2::int", 10);
        if (!bad_bind.ok())
            bind_code = bad_bind.error().code;
        auto b = co_await db_->query("SELECT $1::int + 1", 10);
        if (b.ok() && b.result().size() == 1)
            after_bind = b.result()[0][0].as<int>();

        auto bad_execute = co_await db_->query("SELECT $1::int", std::string("not an integer"));
        if (!bad_execute.ok())
            execute_code = bad_execute.error().code;
        auto c = co_await db_->query("SELECT $1::int + 1", 10);
        if (c.ok() && c.result().size() == 1)
            after_execute = c.result()[0][0].as<int>();
    }());
    EXPECT_EQ(parse_code, "42601");
    EXPECT_EQ(bind_code, "08P01");
    EXPECT_EQ(execute_code, "22P02");
    EXPECT_EQ(after_parse, 11);
    EXPECT_EQ(after_bind, 11);
    EXPECT_EQ(after_execute, 11);
    db_->disconnect();
}

TEST_F(InlineQueryTest, CancelDuringBindExecuteCompletesOnceAndLeavesQueueUsable) {
    auto sentinel = std::make_unique<qb::pg::tcp::database>();
    ASSERT_TRUE(qb::io::async::run_sync(sentinel->connect(qb::pg::test::dsn_tcp_string())));
    const int target_pid = db_->backend_pid();
    ASSERT_GT(target_pid, 0);

    bool cancel_issued = false;
    bool canceled      = false;
    int  completions   = 0;
    int  next_value    = -1;

    auto trigger = [&]() -> qb::io::async::task<void> {
        const std::string probe = "SELECT count(*)::int FROM pg_stat_activity WHERE pid = " + std::to_string(target_pid)
                                  + " AND query LIKE 'SELECT pg_sleep(5), $1%' AND state = 'active'";
        for (int attempt = 0; attempt < 250; ++attempt) {
            auto active = co_await sentinel->query(probe);
            if (active.ok() && active.result().size() == 1 && active.result()[0][0].as<int>() > 0) {
                cancel_issued = co_await db_->cancel_async();
                co_return;
            }
            (void) co_await sentinel->query("SELECT pg_sleep(0.02)");
        }
    };

    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        qb::io::async::coro_scheduler().spawn(trigger());
        auto slow = co_await db_->query("SELECT pg_sleep(5), $1::int", 7);
        ++completions;
        canceled  = !slow.ok() && slow.error().sqlstate == qb::pg::sqlstate::query_canceled;
        auto next = co_await db_->query("SELECT $1::int + 1", 10);
        if (next.ok() && next.result().size() == 1)
            next_value = next.result()[0][0].as<int>();
    }());

    EXPECT_TRUE(cancel_issued);
    EXPECT_TRUE(canceled);
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(next_value, 11);
    sentinel->disconnect();
    db_->disconnect();
}

} // namespace inline_query_test
