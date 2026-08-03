/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file pg_integration_fixture.hpp
 * @brief Shared skip-not-fail base for qbm-pgsql integration (`REQUIRES live`) tests.
 *
 * Integration tests need a live `postgres:5432`. The DSN is already env-overridable via
 * `test_config.hpp` (`QB_PG_DSN`); this header adds the missing half of the conventions
 * contract (docs/tests-audit/_CONVENTIONS.md §4.5): if the daemon is unreachable the test is
 * `GTEST_SKIP`-ped (never `ASSERT_TRUE(false)` / thrown), printing the exact sentinel
 * `QBM_INTEGRATION_SKIP_DAEMON_UNREACHABLE` that the CMake helper wires into CTest's
 * `SKIP_REGULAR_EXPRESSION` (via `REQUIRES live`), so a daemon-down run reports
 * **Skipped, not Failed** and `ctest -LE live` stays green.
 *
 * Replaces the legacy mix (some fixtures `ASSERT_TRUE(connect)` hard-fail, some skip).
 */

#ifndef QBM_PGSQL_TESTS_SHARED_PG_INTEGRATION_FIXTURE_HPP
#define QBM_PGSQL_TESTS_SHARED_PG_INTEGRATION_FIXTURE_HPP

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <qb/io/async.h>
#include <qbm/pgsql/pgsql.h>
#include "test_config.hpp"

namespace qb::pg::test {

/// Exact phrase CTest's SKIP_REGULAR_EXPRESSION matches (set by `REQUIRES live`) to mark a
/// daemon-down binary as Skipped. Keep in sync with qb/cmake/qbFunctions.cmake.
inline constexpr const char *kDaemonUnreachableSentinel = "QBM_INTEGRATION_SKIP_DAEMON_UNREACHABLE";

/**
 * @brief Attempt to connect a database to @p dsn (default `QB_PG_DSN`). Returns true on
 * success; never throws — the caller decides what a false return means.
 */
[[nodiscard]] inline bool
pg_try_connect(qb::pg::tcp::database &db, const std::string &dsn = dsn_tcp_string()) {
    qb::io::async::init();
    return static_cast<bool>(qb::io::async::run_sync(db.connect(dsn)));
}

/**
 * @brief Base fixture for pgsql integration tests. Derive and use `db_` in the test body.
 *
 * `SetUp()` connects-or-skips against `QB_PG_DSN`. The `GTEST_SKIP` runs in `SetUp`'s scope
 * (required for the skip to take effect); fixtures that need a different DSN call
 * `pg_try_connect(*db_, dsn)` themselves and skip with @ref kDaemonUnreachableSentinel.
 */
class PgIntegrationTest : public ::testing::Test {
protected:
    std::unique_ptr<qb::pg::tcp::database> db_;

    void
    SetUp() override {
        db_ = std::make_unique<qb::pg::tcp::database>();
        if (!pg_try_connect(*db_))
            GTEST_SKIP() << kDaemonUnreachableSentinel << " (postgres at " << dsn_tcp_string() << " not reachable)";
    }
};

} // namespace qb::pg::test

#endif // QBM_PGSQL_TESTS_SHARED_PG_INTEGRATION_FIXTURE_HPP
