/**
 * @file unit/misc/module-surface.cpp
 * @brief Unit tests for small module-surface entry points that are otherwise uncovered:
 *        qb::pg::init(), the client_error exception-wrapping ctor, the SQLSTATE
 *        unknown-code sentinel, and the unconnected-database accessors.
 *
 * Pure logic, daemon-free, parallel-safe: no qb::Main, no event loop, no RESOURCE_LOCK.
 * Each case pins behavior confirmed directly in the module sources (pgsql.cpp, error.cpp,
 * sqlstates.cpp, pgsql.h).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <stdexcept>
#include <string>
#include <gtest/gtest.h>
#include <qbm/pgsql/pgsql.h>

using namespace qb::pg;

// qb::pg::init() is defined in pgsql.cpp but has no public header declaration; forward
// declare it here to exercise the symbol. It performs the one-time module setup
// (detail::initialize_field_reader()).
namespace qb::pg {
void init();
}

// ---------------------------------------------------------------------------
// qb::pg::init() (pgsql.cpp:194)
// ---------------------------------------------------------------------------

/// init() runs the one-time module setup (field-reader initialization) with no daemon and
/// must not throw. It is idempotent, so a unit test can call it directly.
TEST(ModuleSurface, InitRunsWithoutThrowing) {
    EXPECT_NO_THROW(qb::pg::init());
}

// ---------------------------------------------------------------------------
// error::client_error wrapping an existing exception (error.cpp:165)
// ---------------------------------------------------------------------------

/// The exception-wrapping ctor prefixes the wrapped message with "Client thrown exception: "
/// and carries the synthetic client SQLSTATE "00000".
TEST(ModuleSurface, ClientErrorWrapsExceptionMessage) {
    const std::runtime_error  original("boom");
    const error::client_error wrapped(original);

    const std::string what = wrapped.what();
    EXPECT_NE(what.find("Client thrown exception: "), std::string::npos);
    EXPECT_NE(what.find("boom"), std::string::npos);
    EXPECT_EQ(wrapped.code, "00000");
}

// ---------------------------------------------------------------------------
// sqlstate::code_to_state unknown sentinel (sqlstates.cpp:748)
// ---------------------------------------------------------------------------

/// An unrecognized 5-char SQLSTATE maps to the unknown_code sentinel; a real code resolves
/// to a different (non-sentinel) enumerator.
TEST(ModuleSurface, UnknownSqlstateMapsToUnknownCode) {
    EXPECT_EQ(sqlstate::code_to_state("ZZZZZ"), sqlstate::unknown_code);
    EXPECT_NE(sqlstate::code_to_state("00000"), sqlstate::unknown_code); // successful_completion
}

// ---------------------------------------------------------------------------
// Unconnected tcp::database accessors (pgsql.h) — safe, well-defined defaults.
// Mirrors PgsqlCancel.ReturnsFalseWhenNotConnected in unit/auth/scram-and-cancel.cpp.
// ---------------------------------------------------------------------------

/// A fresh, never-connected database reports a zeroed server identity and no parameters.
TEST(ModuleSurface, UnconnectedDatabaseAccessorsReturnDefaults) {
    qb::pg::tcp::database db;
    EXPECT_EQ(db.server_version(), 0);
    EXPECT_EQ(db.backend_pid(), 0);
    EXPECT_FALSE(db.parameter_status("server_version").has_value());
}

// ---------------------------------------------------------------------------
// The reconnect contract's starting point, and the notification layout (Huly QB-111)
// ---------------------------------------------------------------------------

/// A never-connected database is idle and NOT connected: is_connected() means ready. Until 3.3 it was the
/// io base's flag, true on a fresh handle that had never opened a socket.
TEST(ModuleSurface, UnconnectedDatabaseIsIdleAndNotConnected) {
    qb::pg::tcp::database db;
    EXPECT_FALSE(db.is_connected()) << "a database that never connected reported itself connected";
    EXPECT_EQ(db.state(), qb::io::connection_state::idle);
    EXPECT_FALSE(db.is_reconnecting());
    EXPECT_TRUE(db.listened_channels().empty());
}

namespace module_surface_test {

/// What qb::pg::notification is laid out as: the 3.2 fields, then `kind` and `gap` (additive growth).
struct notification_layout {
    int                       server_backend_pid;
    std::string               channel;
    std::string               payload;
    qb::pg::notification_kind kind;
    qb::io::gap_info          gap;
};

} // namespace module_surface_test

static_assert(sizeof(qb::pg::notification) == sizeof(module_surface_test::notification_layout),
              "qb::pg::notification grew beyond its 3.3 fields: record the ABI change in the CHANGELOG");

/// A notification is a NOTIFY unless it says otherwise; `gap` is meaningful only on a marker.
TEST(ModuleSurface, ANotificationIsANotifyByDefault) {
    const qb::pg::notification n;
    EXPECT_EQ(n.kind, qb::pg::notification_kind::notify);
    EXPECT_EQ(n.gap.lost, 0u);
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
