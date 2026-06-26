/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file queries-execution.cpp
 * @brief Live query-execution integration tests for the qbm-pgsql client.
 *
 * Covers SELECT / WHERE / JOIN / aggregation / subquery / CTE retrieval, the
 * `field::to()` / `row::to()` write-target contract, DECIMAL decode via both the text
 * (`as<std::string>`) and binary (`as<double>`) paths, file-driven execution, prepared
 * statement bind, and negative malformed-SQL → typed `error::query_error` / SQLSTATE.
 *
 * Absorbs the unique cases from the retired `test-operations.cpp` (the `$1::int`
 * prepared-statement bind and the missing-table ErrorHandling pair, now tightened to
 * assert SQLSTATE `42P01`). The callback↔coroutine 1:1 twins are folded into a single
 * `run_select`/`expect_three_users` helper expressed once and exercised through both
 * transports (D5). The former `QueryPerformance` wall-clock timing assert lives in the
 * benchmark tier now (§5).
 *
 * Derives from the shared skip-not-fail fixture: when no live `postgres:5432` is reachable
 * each case is `GTEST_SKIP`-ped, never hard-failed.
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "../pgsql.h"
#include "../../shared/pg_integration_fixture.hpp"
#include "../../shared/test_config.hpp"

using namespace qb::pg;
using qb::pg::detail::numeric; // exact-decimal marker type (lives in detail)

namespace {

/**
 * @brief Query fixture: seeds two temp tables (`test_users`, `test_orders`) on a live
 * connection, or skips the whole case when PostgreSQL is down.
 */
class QueryExecutionTest : public qb::pg::test::PgIntegrationTest {
protected:
    void
    SetUp() override {
        PgIntegrationTest::SetUp(); // connect-or-skip; sets db_
        if (IsSkipped())
            return;

        ASSERT_TRUE(db_->execute(R"(
            CREATE TEMP TABLE test_users (
                id SERIAL PRIMARY KEY,
                name VARCHAR(50),
                age INTEGER,
                email VARCHAR(100),
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            ))",
                                 discard_query, discard_error)
                        .await())
            << "create test_users failed";

        ASSERT_TRUE(db_->execute(R"(
            CREATE TEMP TABLE test_orders (
                id SERIAL PRIMARY KEY,
                user_id INTEGER REFERENCES test_users(id),
                amount DECIMAL(10,2),
                status VARCHAR(20),
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            ))",
                                 discard_query, discard_error)
                        .await())
            << "create test_orders failed";

        ASSERT_TRUE(db_->execute(R"(
            INSERT INTO test_users (name, age, email) VALUES
            ('John Doe', 30, 'john@example.com'),
            ('Jane Smith', 25, 'jane@example.com'),
            ('Bob Wilson', 45, 'bob@example.com'))",
                                 discard_query, discard_error)
                        .await())
            << "seed test_users failed";

        ASSERT_TRUE(db_->execute(R"(
            INSERT INTO test_orders (user_id, amount, status) VALUES
            (1, 100.50, 'completed'),
            (1, 200.75, 'pending'),
            (2, 150.25, 'completed'),
            (3, 300.00, 'cancelled'))",
                                 discard_query, discard_error)
                        .await())
            << "seed test_orders failed";
    }

    /**
     * @brief Run @p sql through both transports (callback `execute` and `co_await query`),
     * applying @p check to the `results` of each. Surfaces server error text on failure.
     *
     * This is the single transport-parameterized helper that replaces the callback/coroutine
     * twin bodies (D5): the SQL + expectations are written once, coverage of both transports
     * is preserved.
     */
    template <class Check>
    void
    run_select(std::string_view sql, Check &&check) {
        bool cb_ran = false;
        auto st     = db_->execute(
                          sql,
                          [&](transaction &, results r) {
                          check(r);
                          cb_ran = true;
                      },
                          [&](error::db_error const &e) { ADD_FAILURE() << "callback query failed: " << e.code << " " << e.what(); })
                      .await();
        ASSERT_TRUE(st) << "callback transport: " << sql;
        ASSERT_TRUE(cb_ran) << "success callback never fired: " << sql;

        bool        coro_ran = false;
        bool        coro_ok  = false;
        std::string coro_err;
        qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
            auto reply = co_await db_->query(std::string{sql});
            coro_ok    = reply.ok();
            if (!reply.ok()) {
                coro_err = reply.error().code + " " + reply.error().what();
                co_return;
            }
            check(reply.result());
            coro_ran = true;
        }());
        ASSERT_TRUE(coro_ok) << "coroutine transport: " << coro_err;
        ASSERT_TRUE(coro_ran) << "coroutine body never completed: " << sql;
    }
};

} // namespace

/** @brief SELECT * returns all three seeded users with exact column values. */
TEST_F(QueryExecutionTest, BasicSelect) {
    run_select("SELECT * FROM test_users ORDER BY id", [](results r) {
        ASSERT_EQ(r.size(), 3u);
        EXPECT_EQ(r[0][1].as<std::string>(), "John Doe");
        EXPECT_EQ(r[0][2].as<int>(), 30);
        EXPECT_EQ(r[0][3].as<std::string>(), "john@example.com");
        EXPECT_EQ(r[1][1].as<std::string>(), "Jane Smith");
        EXPECT_EQ(r[2][1].as<std::string>(), "Bob Wilson");
    });
}

/**
 * @brief `field::to(T&)` and `row::to(vals...)` must WRITE the destination.
 *
 * Regression guard: `field::to_impl()` once read the buffer but never assigned the
 * out-parameter, so `to()` returned true while leaving the target untouched. `as<T>()`
 * was unaffected, so nothing else catches it.
 */
TEST_F(QueryExecutionTest, FieldAndRowToWriteTarget) {
    bool ok = false;
    auto st = db_->execute(
                     "SELECT * FROM test_users ORDER BY id",
                     [&](transaction &, results r) {
                         ASSERT_EQ(r.size(), 3u);

                         std::string name;
                         ASSERT_TRUE(r[0][1].to(name));
                         EXPECT_EQ(name, "John Doe");
                         int age = -1;
                         ASSERT_TRUE(r[0][2].to(age));
                         EXPECT_EQ(age, 30);
                         std::string email;
                         ASSERT_TRUE(r[0][3].to(email));
                         EXPECT_EQ(email, "john@example.com");

                         int         id2{};
                         std::string name2;
                         int         age2{};
                         std::string email2;
                         r[0].to(id2, name2, age2, email2);
                         EXPECT_EQ(name2, "John Doe");
                         EXPECT_EQ(age2, 30);
                         EXPECT_EQ(email2, "john@example.com");
                         ok = true;
                     },
                     [](error::db_error const &e) { ADD_FAILURE() << e.code << " " << e.what(); })
                  .await();
    ASSERT_TRUE(st);
    ASSERT_TRUE(ok);
}

/** @brief WHERE clause filters to the single user older than 30. */
TEST_F(QueryExecutionTest, WhereClause) {
    run_select("SELECT * FROM test_users WHERE age > 30", [](results r) {
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0][1].as<std::string>(), "Bob Wilson");
        EXPECT_EQ(r[0][2].as<int>(), 45);
    });
}

/** @brief JOIN over users/orders restricted to completed orders. */
TEST_F(QueryExecutionTest, JoinQuery) {
    run_select(R"(
        SELECT u.name, o.amount, o.status
        FROM test_users u
        JOIN test_orders o ON u.id = o.user_id
        WHERE o.status = 'completed'
        ORDER BY o.amount)",
               [](results r) {
                   ASSERT_EQ(r.size(), 2u);
                   EXPECT_EQ(r[0][0].as<std::string>(), "John Doe");
                   EXPECT_EQ(r[0][1].as<std::string>(), "100.50");
                   EXPECT_EQ(r[0][2].as<std::string>(), "completed");
                   EXPECT_EQ(r[1][0].as<std::string>(), "Jane Smith");
                   EXPECT_EQ(r[1][1].as<std::string>(), "150.25");
               });
}

/** @brief COUNT / SUM aggregation with GROUP BY, highest-total user first. */
TEST_F(QueryExecutionTest, Aggregation) {
    run_select(R"(
        SELECT u.name, COUNT(o.id) AS order_count, SUM(o.amount) AS total_amount
        FROM test_users u
        LEFT JOIN test_orders o ON u.id = o.user_id
        GROUP BY u.id, u.name
        ORDER BY total_amount DESC)",
               [](results r) {
                   ASSERT_EQ(r.size(), 3u);
                   EXPECT_EQ(r[0][0].as<std::string>(), "John Doe");
                   EXPECT_EQ(r[0][1].as<int>(), 2);
                   EXPECT_EQ(r[0][2].as<std::string>(), "301.25");
               });
}

/** @brief Subquery: users with a pending order. */
TEST_F(QueryExecutionTest, Subquery) {
    run_select(R"(
        SELECT name, age FROM test_users
        WHERE id IN (SELECT user_id FROM test_orders WHERE status = 'pending'))",
               [](results r) {
                   ASSERT_EQ(r.size(), 1u);
                   EXPECT_EQ(r[0][0].as<std::string>(), "John Doe");
                   EXPECT_EQ(r[0][1].as<int>(), 30);
               });
}

/** @brief CTE + CASE classification, ordered by total spend. */
TEST_F(QueryExecutionTest, ComplexCteQuery) {
    run_select(R"(
        WITH user_stats AS (
            SELECT u.id, u.name,
                   COUNT(o.id)  AS order_count,
                   SUM(o.amount) AS total_amount,
                   AVG(o.amount) AS avg_amount
            FROM test_users u
            LEFT JOIN test_orders o ON u.id = o.user_id
            GROUP BY u.id, u.name)
        SELECT name, order_count, total_amount, avg_amount,
               CASE WHEN total_amount > 200 THEN 'High Value'
                    WHEN total_amount > 100 THEN 'Medium Value'
                    ELSE 'Low Value' END AS customer_category
        FROM user_stats
        ORDER BY total_amount DESC NULLS LAST)",
               [](results r) {
                   ASSERT_EQ(r.size(), 3u);
                   EXPECT_EQ(r[0][0].as<std::string>(), "John Doe");
                   EXPECT_EQ(r[0][1].as<int>(), 2);
                   EXPECT_EQ(r[0][2].as<std::string>(), "301.25");
                   EXPECT_NEAR(std::stod(r[0][3].as<std::string>()), 150.63, 0.01);
                   EXPECT_EQ(r[0][4].as<std::string>(), "High Value");
               });
}

/**
 * @brief NUMERIC decode through the simple-query TEXT path and the prepared BINARY path.
 *
 * The amount column is `DECIMAL(10,2)`; `SUM(amount)` over the two completed orders is
 * 100.50 + 150.25 = 250.75, and `SUM` of a numeric returns `numeric` (OID 1700).
 *
 * Text leg (simple-query `execute(SQL)`): every column comes back in TEXT format, so
 * `as<std::string>()` exercises the text NUMERIC decoder and `as<double>()` exercises the
 * genuine NUMERIC→double path (`TypeConverter<double>::from_text` → `std::stod`).
 *
 * Binary leg (PREPARED, extended-query): numeric is on the `common.h` binary whitelist
 * (`type_oid_prefers_binary_result_format`), so the field arrives in `Binary` format and
 * `as<numeric>()` exercises the binary NUMERIC digit-array codec
 * (`TypeConverter<numeric>::from_binary`) — the binary-NUMERIC coverage the previous version
 * of this test only *claimed*: that body ran on the simple-query (text) path against a
 * `::float8` cast (OID 701), so it never touched the binary NUMERIC decoder at all. This is
 * distinct from datatypes-roundtrip (which echoes a `$1::numeric` parameter / a dedicated
 * numeric column) — here the value is a server-side aggregate over a real DECIMAL column.
 *
 * NOTE: `as<double>()` is asserted only on the TEXT leg. On a BINARY numeric field,
 * `TypeConverter<double>::from_binary` reinterprets the value bytes as a raw IEEE-754 double
 * (it does NOT route through the numeric digit-array codec), so `as<double>()` on a binary
 * numeric column yields garbage — there is no binary NUMERIC→double decoder. Reading a binary
 * numeric as a double is therefore intentionally NOT exercised here.
 */
TEST_F(QueryExecutionTest, DecimalDecodeTextAndBinary) {
    // Text leg: simple-query transport, text format. as<double>() is genuine here (std::stod).
    bool text_ok = false;
    ASSERT_TRUE(db_->execute(
                       R"(SELECT SUM(amount) AS s
                          FROM test_orders WHERE status = 'completed')",
                       [&](transaction &, results r) {
                           ASSERT_EQ(r.size(), 1u);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Text);
                           EXPECT_EQ(r[0][0].as<std::string>(), "250.75");
                           EXPECT_DOUBLE_EQ(r[0][0].as<double>(), 250.75);
                           text_ok = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << e.code << " " << e.what(); })
                    .await());
    ASSERT_TRUE(text_ok);

    // Binary leg: prepared (extended-query) SELECT of a numeric aggregate. The numeric OID is
    // on the binary whitelist, so the result arrives in Binary format and is decoded by the
    // binary NUMERIC digit-array codec via as<numeric>().
    ASSERT_TRUE(db_->prepare("sum_completed",
                             R"(SELECT SUM(amount) AS s
                                FROM test_orders WHERE status = 'completed')",
                             type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    bool bin_ok = false;
    ASSERT_TRUE(db_->execute(
                       "sum_completed", params{},
                       [&](transaction &, results r) {
                           ASSERT_EQ(r.size(), 1u);
                           EXPECT_EQ(r.field(0).format_code, protocol_data_format::Binary)
                               << "numeric aggregate did not arrive in binary format";
                           EXPECT_EQ(r[0][0].as<numeric>().str(), "250.75");
                           bin_ok = true;
                       },
                       [](error::db_error const &e) { ADD_FAILURE() << e.code << " " << e.what(); })
                    .await());
    ASSERT_TRUE(bin_ok);
}

/**
 * @brief Vector/array parameter passing across the three insert shapes.
 *
 * Single param, four explicit params, and a `std::vector<std::string>` expansion. Verifies
 * every inserted value round-trips back in order (correctness only; throughput lives in the
 * benchmark tier).
 */
TEST_F(QueryExecutionTest, ParameterPassingShapes) {
    ASSERT_TRUE(db_->execute("CREATE TEMP TABLE test_perf (id SERIAL PRIMARY KEY, value TEXT)", discard_query, discard_error).await());

    ASSERT_TRUE(db_->prepare("single_ins", "INSERT INTO test_perf (value) VALUES ($1)", type_oid_sequence{}, discard_prepare, discard_error)
                    .await());
    ASSERT_TRUE(db_->prepare("quad_ins", "INSERT INTO test_perf (value) VALUES ($1),($2),($3),($4)", type_oid_sequence{}, discard_prepare,
                             discard_error)
                    .await());

    ASSERT_TRUE(db_->execute("single_ins", params{std::string("single")}, discard_query, discard_error).await());
    ASSERT_TRUE(db_->execute("quad_ins",
                             params{std::string("explicit 1"), std::string("explicit 2"), std::string("explicit 3"),
                                    std::string("explicit 4")},
                             discard_query, discard_error)
                    .await());

    std::vector<std::string> values;
    for (int i = 1; i <= 4; ++i)
        values.push_back("vector " + std::to_string(i));
    ASSERT_TRUE(db_->execute("quad_ins", params{values}, discard_query, discard_error).await());

    bool ok = false;
    auto st = db_->execute(
                     "SELECT value FROM test_perf ORDER BY id",
                     [&](transaction &, results r) {
                         ASSERT_EQ(r.size(), 9u);
                         EXPECT_EQ(r[0][0].as<std::string>(), "single");
                         for (int i = 1; i <= 4; ++i)
                             EXPECT_EQ(r[i][0].as<std::string>(), "explicit " + std::to_string(i));
                         for (int i = 1; i <= 4; ++i)
                             EXPECT_EQ(r[i + 4][0].as<std::string>(), "vector " + std::to_string(i));
                         ok = true;
                     },
                     [](error::db_error const &e) { ADD_FAILURE() << e.code << " " << e.what(); })
                  .await();
    ASSERT_TRUE(st);
    ASSERT_TRUE(ok);
}

/**
 * @brief Prepared statement with a typed `$1::int` bind (absorbed from test-operations).
 */
TEST_F(QueryExecutionTest, PreparedStatementBind) {
    ASSERT_TRUE(db_->prepare("echo_int", "SELECT $1::int", type_oid_sequence{}, discard_prepare, discard_error).await());

    bool ok = false;
    auto st = db_->execute(
                     "echo_int", params{42},
                     [&](transaction &, results r) {
                         ASSERT_EQ(r.size(), 1u);
                         EXPECT_EQ(r[0][0].as<int>(), 42);
                         ok = true;
                     },
                     [](error::db_error const &e) { ADD_FAILURE() << e.code << " " << e.what(); })
                  .await();
    ASSERT_TRUE(st);
    ASSERT_TRUE(ok);
}

/**
 * @brief Missing table → server error with SQLSTATE 42P01 (undefined_table).
 *
 * Tightened from the old test-operations ErrorHandling pair, which asserted only
 * `!error.code.empty()` / `!reply.ok()`. Both transports must surface the exact SQLSTATE.
 */
TEST_F(QueryExecutionTest, MissingTableErrorIsUndefinedTable) {
    bool err_fired = false;
    auto st        = db_->execute(
                       "SELECT * FROM nonexistent_table", [](transaction &, results) { FAIL() << "query on missing table must not succeed"; },
                       [&](error::db_error const &e) {
                    EXPECT_EQ(e.code, "42P01");
                    EXPECT_EQ(e.sqlstate, sqlstate::undefined_table);
                    err_fired = true;
                       })
                    .await();
    EXPECT_FALSE(st);
    EXPECT_TRUE(err_fired);

    bool           coro_failed = false;
    std::string    coro_code;
    sqlstate::code coro_state{};
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply  = co_await db_->query("SELECT * FROM nonexistent_table");
        coro_failed = !reply.ok();
        if (coro_failed) {
            coro_code  = reply.error().code;
            coro_state = reply.error().sqlstate;
        }
    }());
    ASSERT_TRUE(coro_failed);
    EXPECT_EQ(coro_code, "42P01");
    EXPECT_EQ(coro_state, sqlstate::undefined_table);
}

/**
 * @brief Malformed SQL → syntax error (SQLSTATE 42601), surfaced as `error::query_error`.
 */
TEST_F(QueryExecutionTest, MalformedSqlIsSyntaxError) {
    bool err_fired = false;
    auto st        = db_->execute(
                       "SELEKT 1 FROMM nowhere", [](transaction &, results) { FAIL() << "malformed SQL must not succeed"; },
                       [&](error::db_error const &e) {
                    EXPECT_EQ(e.code, "42601");
                    EXPECT_EQ(e.sqlstate, sqlstate::syntax_error);
                    err_fired = true;
                       })
                    .await();
    EXPECT_FALSE(st);
    EXPECT_TRUE(err_fired);

    bool        coro_failed = false;
    std::string coro_code;
    qb::io::async::run_sync([&]() -> qb::io::async::task<void> {
        auto reply  = co_await db_->query("SELEKT 1 FROMM nowhere");
        coro_failed = !reply.ok();
        if (coro_failed)
            coro_code = reply.error().code;
    }());
    ASSERT_TRUE(coro_failed);
    EXPECT_EQ(coro_code, "42601");
}

/**
 * @brief `run_sync(db->query(...))` bridge returns the same awaiter as `co_await`.
 */
TEST_F(QueryExecutionTest, QueryAwaiterSelectOne) {
    auto reply = qb::io::async::run_sync(db_->query("SELECT 1 AS one"));
    ASSERT_TRUE(reply.ok()) << reply.error().what();
    ASSERT_EQ(reply.result().size(), 1u);
    EXPECT_EQ(reply.result()[0][0].as<int>(), 1);
}

/**
 * @brief `execute_file()`: load + run SQL from a file, success-only and discard overloads,
 * plus the non-existent-file error path (callback fires AND `query_error` is thrown).
 */
TEST_F(QueryExecutionTest, ExecuteFromFile) {
    const std::filesystem::path temp_file = std::filesystem::temp_directory_path() / "qbm_pgsql_test_query.sql";
    {
        std::ofstream file(temp_file);
        ASSERT_TRUE(file.is_open());
        file << "SELECT name, email FROM test_users WHERE age > 25 ORDER BY name";
        ASSERT_TRUE(file.good());
    }

    bool ok = false;
    auto st = db_->execute_file(
                     temp_file,
                     [&](transaction &, results r) {
                         ASSERT_EQ(r.size(), 2u); // Bob Wilson, John Doe
                         EXPECT_EQ(r[0][0].as<std::string>(), "Bob Wilson");
                         EXPECT_EQ(r[0][1].as<std::string>(), "bob@example.com");
                         EXPECT_EQ(r[1][0].as<std::string>(), "John Doe");
                         EXPECT_EQ(r[1][1].as<std::string>(), "john@example.com");
                         ok = true;
                     },
                     [](error::db_error const &e) { ADD_FAILURE() << e.code << " - " << e.what(); })
                  .await();
    ASSERT_TRUE(st);
    ASSERT_TRUE(ok);

    // success-only overload
    ok = false;
    st = db_->execute_file(temp_file,
                           [&](transaction &, results r) {
                               EXPECT_EQ(r.size(), 2u);
                               ok = true;
                           })
             .await();
    ASSERT_TRUE(st);
    ASSERT_TRUE(ok);

    // discard overload
    ASSERT_TRUE(db_->execute_file(temp_file, discard_query, discard_error).await());

    // non-existent file: error callback fires AND query_error is thrown.
    bool error_cb     = false;
    bool exception_th = false;
    try {
        (void) db_->execute_file(
                       std::filesystem::temp_directory_path() / "qbm_pgsql_nonexistent.sql",
                       [](transaction &, results) { FAIL() << "must not succeed on non-existent file"; },
                       [&](error::db_error const &) { error_cb = true; })
            .await();
        FAIL() << "expected error::query_error for non-existent file";
    } catch (const error::query_error &) {
        exception_th = true;
    }
    EXPECT_TRUE(error_cb) << "error callback should have fired";
    EXPECT_TRUE(exception_th) << "query_error should have been thrown";

    std::filesystem::remove(temp_file);
}

int
main(int argc, char **argv) {
    qb::io::async::init();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
