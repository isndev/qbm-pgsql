/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

/** @file savepoint-command-order.cpp
 * @brief Exact SQL and callback order for callback savepoint cleanup, without a server.
 */

#include <gtest/gtest.h>
#include <functional>
#include <string>
#include <utility>
#include <qbm/pgsql/pgsql.h>

namespace savepoint_command_order_test {

using qb::pg::detail::EndSavePoint;
using qb::pg::detail::ISqlQuery;
using qb::pg::detail::PreparedStorage;
using qb::pg::detail::Transaction;

class Root final : public Transaction {
public:
    explicit Root(PreparedStorage &storage)
        : Transaction(storage) {}
};

std::string
sql(ISqlQuery const &query) {
    auto        message = query.get();
    auto        range   = message.buffer();
    std::string wire(range.first, range.second);
    EXPECT_EQ(wire.front(), 'Q');
    return std::string(wire.c_str() + 5);
}

TEST(SavepointCommandOrder, FailedBodyRollsBackThenReleasesBeforeTerminalCallback) {
    PreparedStorage                                      storage;
    Root                                                 root(storage);
    int                                                  terminal_errors = 0;
    std::function<void(qb::pg::error::db_error const &)> on_error        = [&](qb::pg::error::db_error const &) {
        ++terminal_errors;
    };
    EndSavePoint<decltype(on_error)> end(&root, std::string("same_name"), std::move(on_error));
    end.result(false);
    end.on_end_savepoint();

    auto rollback = end.pop_query();
    ASSERT_NE(rollback, nullptr);
    EXPECT_EQ(sql(*rollback), "rollback to savepoint \"same_name\"");
    EXPECT_EQ(terminal_errors, 0);
    rollback->on_success();

    auto release = end.pop_query();
    ASSERT_NE(release, nullptr);
    EXPECT_EQ(sql(*release), "release savepoint \"same_name\"");
    EXPECT_EQ(terminal_errors, 0);
    release->on_success();
    EXPECT_TRUE(end.result());
    EXPECT_EQ(terminal_errors, 1);
    EXPECT_EQ(end.next_query(), nullptr);
}

TEST(SavepointCommandOrder, FailedRollbackReportsOnceAndNeverReleases) {
    PreparedStorage                                      storage;
    Root                                                 root(storage);
    int                                                  terminal_errors = 0;
    std::function<void(qb::pg::error::db_error const &)> on_error        = [&](qb::pg::error::db_error const &) {
        ++terminal_errors;
    };
    EndSavePoint<decltype(on_error)> end(&root, std::string("same_name"), std::move(on_error));
    end.result(false);
    end.on_end_savepoint();

    auto rollback = end.pop_query();
    rollback->on_error(qb::pg::error::query_error{"savepoint missing"});
    EXPECT_FALSE(end.result());
    EXPECT_EQ(terminal_errors, 1);
    EXPECT_EQ(end.next_query(), nullptr);
}

} // namespace savepoint_command_order_test
