/**
 * @file unit/auth/scram-and-cancel.cpp
 * @brief Unit tests for the daemon-free SCRAM helpers + the no-network cancel guard.
 *
 * The three free `TEST()`s extracted from `test-pgsql-coro-api.cpp` need neither a live
 * PostgreSQL nor an event loop:
 *  - `scram_server_nonce_extends_client` — RFC 5802 §5.1 server-nonce validation;
 *  - `scram_escape_saslname` — RFC 5802 saslname escaping (`=` → `=3D`, `,` → `=2C`);
 *  - `database::cancel()` on an un-handshaked connection (no BackendKeyData → false,
 *    no network touched).
 *
 * Pure logic, parallel-safe, no `RESOURCE_LOCK`.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <string>
#include <gtest/gtest.h>
#include "../pgsql.h"

using namespace qb::pg;
using namespace qb::pg::detail;

// ---------------------------------------------------------------------------
// cancel() without a handshake
// ---------------------------------------------------------------------------

/**
 * @brief `cancel()` on a connection that never completed a handshake reports false.
 *
 * Cancellation needs the BackendKeyData (PID + secret key) the server sends after a
 * successful startup. A fresh, never-connected `tcp::database` has none, so `cancel()`
 * must fail fast (return false) without opening a second socket. This is a pure
 * local-state check — no daemon, no event loop required.
 */
TEST(PgsqlCancel, ReturnsFalseWhenNotConnected) {
    qb::pg::tcp::database db;
    EXPECT_FALSE(db.cancel());
}

// ---------------------------------------------------------------------------
// SCRAM-SHA-256 server-nonce validation (RFC 5802 §5.1)
// ---------------------------------------------------------------------------

/**
 * @brief The server-first nonce must echo the client nonce verbatim AND extend it.
 *
 * A server — or MITM — that contributes no entropy, truncates, or tampers with the
 * echoed prefix is rejected before any proof is derived.
 */
TEST(ScramNonce, ServerMustExtendClientNonce) {
    // Accept: server echoes the client nonce and appends its own.
    EXPECT_TRUE(scram_server_nonce_extends_client("clientNONCE", "clientNONCEserverPART"));
    const std::string c = "0123456789abcdef0123456789abcdef"; // realistic 32-char client nonce
    EXPECT_TRUE(scram_server_nonce_extends_client(c, c + "SRV-side-nonce"));

    // Reject: server contributed nothing (no strict-longer).
    EXPECT_FALSE(scram_server_nonce_extends_client(c, c));
    // Reject: server nonce shorter than the client nonce.
    EXPECT_FALSE(scram_server_nonce_extends_client(c, "0123456789abcdef"));
    // Reject: client nonce is not a prefix of the combined nonce.
    EXPECT_FALSE(scram_server_nonce_extends_client(c, "X" + c + "more"));
    // Reject: empty client nonce.
    EXPECT_FALSE(scram_server_nonce_extends_client("", "anything"));
    // Reject: empty server nonce.
    EXPECT_FALSE(scram_server_nonce_extends_client(c, ""));

    // Reject: a single tampered character inside the echoed prefix.
    std::string tampered = c + "tail";
    tampered[5]          = (tampered[5] == 'a') ? 'b' : 'a';
    EXPECT_FALSE(scram_server_nonce_extends_client(c, tampered));
}

// ---------------------------------------------------------------------------
// SCRAM saslname escaping (RFC 5802)
// ---------------------------------------------------------------------------

/**
 * @brief `=` → `=3D` (applied first), `,` → `=2C`; the inserted `=3D` is NOT re-escaped.
 */
TEST(ScramEscape, EscapesCommaAndEquals) {
    EXPECT_EQ(scram_escape_saslname("alice"), "alice");          // no special chars
    EXPECT_EQ(scram_escape_saslname("a,b"), "a=2Cb");            // comma
    EXPECT_EQ(scram_escape_saslname("a=b"), "a=3Db");            // equals
    EXPECT_EQ(scram_escape_saslname("a=,b"), "a=3D=2Cb");        // equals before comma, no double-escape
    EXPECT_EQ(scram_escape_saslname(""), "");                    // empty
    // Order matters: a comma followed by an equals must each map to its own code.
    EXPECT_EQ(scram_escape_saslname(",="), "=2C=3D");
}

// ---------------------------------------------------------------------------
// SCRAM iteration-count bound (DoS guard)
// ---------------------------------------------------------------------------

/**
 * @brief `scram_validate_iteration_count` accepts only a sane [1, kMaxScramIterations].
 *
 * The `i=` count comes from the (untrusted) SCRAM server-first message and feeds
 * PBKDF2 synchronously on the I/O event-loop thread, so an unbounded value is a
 * denial of service (i=2147483647 => minutes of inline HMAC stalling the core).
 * This pins the accept/reject contract without a live handshake.
 */
TEST(ScramIteration, AcceptsSaneRangeRejectsHostileAndMalformed) {
    // Accepted: PostgreSQL's server default, the minimum, and the exact ceiling.
    EXPECT_EQ(scram_validate_iteration_count("4096"), 4096);
    EXPECT_EQ(scram_validate_iteration_count("1"), 1);
    EXPECT_EQ(scram_validate_iteration_count(std::to_string(kMaxScramIterations)),
              kMaxScramIterations);

    // Rejected — the DoS guard: one past the ceiling and the INT_MAX worst case.
    EXPECT_THROW(scram_validate_iteration_count(std::to_string(kMaxScramIterations + 1)),
                 error::connection_error);
    EXPECT_THROW(scram_validate_iteration_count("2147483647"), error::connection_error);

    // Rejected — malformed / non-positive / overflow / trailing junk (to_number strict).
    EXPECT_THROW(scram_validate_iteration_count("0"), error::connection_error);
    EXPECT_THROW(scram_validate_iteration_count("-5"), error::connection_error);
    EXPECT_THROW(scram_validate_iteration_count(""), error::connection_error);
    EXPECT_THROW(scram_validate_iteration_count("abc"), error::connection_error);
    EXPECT_THROW(scram_validate_iteration_count("4096x"), error::connection_error);
    EXPECT_THROW(scram_validate_iteration_count("99999999999999999999"), error::connection_error);
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
