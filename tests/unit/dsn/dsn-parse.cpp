/**
 * @file unit/dsn/dsn-parse.cpp
 * @brief Unit tests for the PostgreSQL DSN / connection-string parser.
 *
 * Pure logic, daemon-free, parallel-safe: exercises
 * `qb::pg::detail::connection_options::parse` (and the `_pg` literal that wraps it)
 * with zero I/O — no `qb::Main`, no event loop, no `RESOURCE_LOCK`. Split out of the
 * tier-mixing `test-connection.cpp` (whose other 9 cases are live-PG integration) and
 * co-located with the other unit parsers per `_spec-qbm-pgsql.md` §1.
 *
 * Coverage:
 *  - the full grammar: `alias=`, `schema://`, `user:password@`, `host:port`, `[database]`;
 *  - the high-bit-byte regression (`std::isspace` UB on a negative `char`);
 *  - whitespace stripping inside tokens;
 *  - schema-only / no-credentials / socket forms;
 *  - keepalive + timeout + ssl-verify defaults (so a future change can't silently drift);
 *  - malformed DSN rejection (the `schema_slash` states throw).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <chrono>
#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include "../pgsql.h"

using namespace qb::pg;

namespace {

[[nodiscard]] connection_options
parse(std::string const &dsn) {
    return connection_options::parse(dsn);
}

} // namespace

// ---------------------------------------------------------------------------
// Full-grammar happy path
// ---------------------------------------------------------------------------

/// Every grammar field is split out of a fully-specified `alias=tcp://user:pw@host:port[db]`.
TEST(DsnParse, FullTcpDsnWithAliasUserPasswordHostPortDatabase) {
    const auto opts = parse("myalias=tcp://bob:s3cret@db.example.com:6543[shop]");

    EXPECT_EQ(opts.alias, "myalias");
    EXPECT_EQ(opts.schema, "tcp");
    EXPECT_EQ(opts.user, "bob");
    EXPECT_EQ(opts.password, "s3cret");
    EXPECT_EQ(opts.uri, "db.example.com:6543");
    EXPECT_EQ(opts.database, "shop");
}

/// No alias prefix: the first token is the schema, not the alias.
TEST(DsnParse, NoAliasYieldsEmptyAliasAndSchemaFirst) {
    const auto opts = parse("tcp://test:test@localhost:5432[test]");

    EXPECT_TRUE(opts.alias.empty());
    EXPECT_EQ(opts.schema, "tcp");
    EXPECT_EQ(opts.user, "test");
    EXPECT_EQ(opts.password, "test");
    EXPECT_EQ(opts.uri, "localhost:5432");
    EXPECT_EQ(opts.database, "test");
}

/// `user@host` with no `:password` — the user is taken on the `@`, password stays empty.
TEST(DsnParse, UserWithoutPasswordLeavesPasswordEmpty) {
    const auto opts = parse("tcp://alice@localhost:5432[shop]");

    EXPECT_EQ(opts.user, "alice");
    EXPECT_TRUE(opts.password.empty());
    EXPECT_EQ(opts.uri, "localhost:5432");
    EXPECT_EQ(opts.database, "shop");
}

/// No credentials at all: `schema://host:port[db]` — the host token lands directly in `uri`.
TEST(DsnParse, NoCredentialsHostGoesToUri) {
    const auto opts = parse("tcp://localhost:5432[postgres]");

    EXPECT_TRUE(opts.user.empty());
    EXPECT_TRUE(opts.password.empty());
    EXPECT_EQ(opts.uri, "localhost:5432");
    EXPECT_EQ(opts.database, "postgres");
}

/// UNIX-socket form: the path (slashes/colons) is preserved verbatim in `uri`.
TEST(DsnParse, SocketSchemaPreservesPathInUri) {
    const auto opts = parse("socket:///var/run/postgresql:5432[appdb]");

    EXPECT_EQ(opts.schema, "socket");
    EXPECT_EQ(opts.uri, "/var/run/postgresql:5432");
    EXPECT_EQ(opts.database, "appdb");
}

/// `ssl://` is a valid schema string (verification mode itself defaults to `none`).
TEST(DsnParse, SslSchemaParsedAsSchemaString) {
    const auto opts = parse("ssl://localhost:5432[secure]");

    EXPECT_EQ(opts.schema, "ssl");
    EXPECT_EQ(opts.uri, "localhost:5432");
    EXPECT_EQ(opts.database, "secure");
    // Parser does NOT promote ssl:// to ssl_verify::full — that is a caller decision.
    EXPECT_EQ(opts.ssl_verify, ssl_verify_mode::none);
}

// ---------------------------------------------------------------------------
// Byte-fidelity regressions
// ---------------------------------------------------------------------------

/**
 * @brief DSN parsing must preserve high-bit bytes in credentials.
 *
 * The parser skipped whitespace via `std::isspace(*p)` on a plain `char`; a high-bit
 * byte (e.g. 0xC3 in a non-ASCII password) is negative, and passing a negative value to
 * `std::isspace` is UB that, depending on the libc ctype table, can misclassify the byte
 * as whitespace and silently drop it. The fix casts to `unsigned char` first; this guards
 * the byte-for-byte round trip (regression from `test-connection.cpp`).
 */
TEST(DsnParse, PreservesHighBitBytesInPassword) {
    const std::string password = std::string("p\xC3\xA9ss"); // "péss" in UTF-8
    const std::string dsn      = "tcp://user:" + password + "@localhost:5432[db]";

    const auto opts = parse(dsn);

    EXPECT_EQ(opts.user, "user");
    EXPECT_EQ(opts.password, password);
    EXPECT_EQ(opts.password.size(), 5u); // 'p' 0xC3 0xA9 's' 's' — high bit not dropped
    EXPECT_EQ(opts.database, "db");
}

/// A high-bit byte in the *database* name survives too (same UB surface, different field).
TEST(DsnParse, PreservesHighBitBytesInDatabaseName) {
    const std::string db   = std::string("caf\xC3\xA9"); // "café"
    const auto        opts = parse("tcp://u:p@localhost:5432[" + db + "]");

    EXPECT_EQ(opts.database, db);
}

/// ASCII whitespace inside an otherwise-valid token is stripped (documented behavior).
TEST(DsnParse, AsciiWhitespaceInTokensIsStripped) {
    const auto opts = parse("tcp://us er:pa ss@local host:5432[my db]");

    EXPECT_EQ(opts.user, "user");
    EXPECT_EQ(opts.password, "pass");
    EXPECT_EQ(opts.uri, "localhost:5432");
    EXPECT_EQ(opts.database, "mydb");
}

// ---------------------------------------------------------------------------
// Defaults (lock down so a future field-reorder / default change is caught)
// ---------------------------------------------------------------------------

/// Freshly-parsed options carry the documented timeout / keepalive / ssl defaults.
TEST(DsnParse, DefaultsAreStableAfterParse) {
    const auto opts = parse("tcp://test:test@localhost:5432[test]");

    EXPECT_EQ(opts.connect_timeout, std::chrono::seconds(10));
    EXPECT_EQ(opts.ssl_verify, ssl_verify_mode::none);
    EXPECT_EQ(opts.keepalive_interval, 0); // disabled
    EXPECT_EQ(opts.keepalive_probes, 3);
    EXPECT_EQ(opts.keepalive_idle, 60);
}

/// A default-constructed `connection_options` carries the same defaults (no parse path).
TEST(DsnParse, DefaultConstructedOptionsMatchDocumentedDefaults) {
    const connection_options opts{};

    EXPECT_EQ(opts.connect_timeout, std::chrono::seconds(10));
    EXPECT_EQ(opts.ssl_verify, ssl_verify_mode::none);
    EXPECT_EQ(opts.keepalive_interval, 0);
    EXPECT_EQ(opts.keepalive_probes, 3);
    EXPECT_EQ(opts.keepalive_idle, 60);
}

// ---------------------------------------------------------------------------
// `_pg` user-defined literal == parse()
// ---------------------------------------------------------------------------

/// The `_pg` literal is exactly `connection_options::parse` and yields identical fields.
TEST(DsnParse, PgLiteralEquivalentToParse) {
    using namespace qb::pg;
    const auto lit    = "tcp://bob:pw@host:5432[db]"_pg;
    const auto parsed = parse("tcp://bob:pw@host:5432[db]");

    EXPECT_EQ(lit.schema, parsed.schema);
    EXPECT_EQ(lit.user, parsed.user);
    EXPECT_EQ(lit.password, parsed.password);
    EXPECT_EQ(lit.uri, parsed.uri);
    EXPECT_EQ(lit.database, parsed.database);
}

// ---------------------------------------------------------------------------
// generate_alias()
// ---------------------------------------------------------------------------

/// `generate_alias()` synthesizes `user@uri[database]` when no explicit alias was given.
TEST(DsnParse, GenerateAliasFromUserUriDatabase) {
    auto opts = parse("tcp://bob:pw@host:5432[db]");
    EXPECT_TRUE(opts.alias.empty());

    opts.generate_alias();
    EXPECT_EQ(opts.alias, "bob@host:5432[db]");
}

// ---------------------------------------------------------------------------
// Malformed DSN (ADD — spec §2 / c03 §ADD)
// ---------------------------------------------------------------------------

/**
 * @brief A single slash after the schema is a malformed `schema:/` and must throw.
 *
 * After the schema's `:` the parser enters `schema_slash1`/`schema_slash2`; any
 * non-`/` byte there throws `std::runtime_error("invalid connection string")`. With a
 * single slash the second slash position sees `l` (of `localhost`) and rejects.
 */
TEST(DsnParse, MalformedSingleSlashAfterSchemaThrows) {
    EXPECT_THROW((void) parse("tcp:/localhost:5432[db]"), std::runtime_error);
}

/// A schema with no slashes at all (`tcp:host`) hits the slash state on a non-`/` byte.
TEST(DsnParse, MalformedNoSlashAfterSchemaThrows) {
    EXPECT_THROW((void) parse("tcp:host[db]"), std::runtime_error);
}

/// `schema:::` — the slash state sees ':' instead of '/' and rejects.
TEST(DsnParse, MalformedColonAfterSchemaThrows) {
    EXPECT_THROW((void) parse("tcp:::"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// transaction_mode / isolation_level stream operators + to_string (common.cpp).
// ---------------------------------------------------------------------------

/// Each named isolation level prints its canonical lowercase spelling.
TEST(TransactionModeFormat, IsolationLevelNamesStreamed) {
    auto render = [](isolation_level lvl) {
        std::ostringstream os;
        os << lvl;
        return os.str();
    };
    EXPECT_EQ(render(isolation_level::read_committed), "read committed");
    EXPECT_EQ(render(isolation_level::repeatable_read), "repeatable read");
    EXPECT_EQ(render(isolation_level::serializable), "serializable");
}

/// An out-of-range isolation level falls to the "Unknown ..." branch.
TEST(TransactionModeFormat, UnknownIsolationLevelFallback) {
    std::ostringstream os;
    os << static_cast<isolation_level>(99);
    EXPECT_NE(os.str().find("Unknown transaction isolation level"), std::string::npos);
}

/// A default transaction_mode (read committed, read-write, non-deferrable) is empty.
TEST(TransactionModeFormat, DefaultModeRendersEmpty) {
    EXPECT_EQ(to_string(transaction_mode{}), "");
}

/// Non-default isolation alone emits just the ISOLATION LEVEL clause.
TEST(TransactionModeFormat, IsolationOnlyClause) {
    transaction_mode m{isolation_level::serializable};
    EXPECT_EQ(to_string(m), " ISOLATION LEVEL serializable");
}

/// read_only after a non-default isolation inserts the leading comma (need_comma path).
TEST(TransactionModeFormat, IsolationAndReadOnlyCommaSeparated) {
    transaction_mode m{isolation_level::serializable, /*ro=*/true};
    EXPECT_EQ(to_string(m), " ISOLATION LEVEL serializable, READ ONLY");
}

/// read_only alone (default isolation) emits READ ONLY without a leading comma.
TEST(TransactionModeFormat, ReadOnlyOnlyNoComma) {
    transaction_mode m{isolation_level::read_committed, /*ro=*/true};
    EXPECT_EQ(to_string(m), " READ ONLY");
}

/// deferrable after read_only inserts its own comma (the deferrable need_comma branch).
TEST(TransactionModeFormat, ReadOnlyAndDeferrableCommaSeparated) {
    transaction_mode m{isolation_level::serializable, /*ro=*/true, /*def=*/true};
    EXPECT_EQ(to_string(m), " ISOLATION LEVEL serializable, READ ONLY, DEFERRABLE");
}

/// deferrable alone (no prior clause) emits DEFERRABLE with no leading comma.
TEST(TransactionModeFormat, DeferrableOnlyNoComma) {
    transaction_mode m{isolation_level::read_committed, /*ro=*/false, /*def=*/true};
    EXPECT_EQ(to_string(m), " DEFERRABLE");
}

// ---------------------------------------------------------------------------
// `_db` user-defined literal (common.cpp operator""_db).
// ---------------------------------------------------------------------------

TEST(DbAliasLiteral, ProducesDbaliasFromStringLiteral) {
    using namespace qb::pg;
    dbalias a = "primary"_db;
    EXPECT_EQ(static_cast<const std::string &>(a), "primary");
}

// ---------------------------------------------------------------------------
// DSN parser rare default branches (common.cpp:211/226/244/253).
// ---------------------------------------------------------------------------

/// A ']' encountered outside the database state is treated as a literal token byte
/// (the `]` else-branch). Here it appears inside the URI before any '['.
TEST(DsnParse, StrayCloseBracketInUriIsLiteral) {
    const auto opts = parse("tcp://localhost]5432[db]");
    // The ']' is pushed into the current (uri) token verbatim.
    EXPECT_NE(opts.uri.find(']'), std::string::npos);
    EXPECT_EQ(opts.database, "db");
}

/// A '@' after the host (state == url) is not a credential separator and is kept
/// as a literal value byte (the '@' default branch).
TEST(DsnParse, AtSignInUrlIsLiteral) {
    const auto opts = parse("tcp://host@name:5432[db]");
    // First '@' splits user/url; a later state keeps subsequent bytes literally.
    EXPECT_EQ(opts.database, "db");
}

// ---------------------------------------------------------------------------
// Delimiter push_back arms taken while past their primary split (common.cpp
// 209 / 226 / 244): a ':' at state url, and a second '@' / '[' after the first
// consumed one, are all retained as literal token bytes, not state transitions.
// ---------------------------------------------------------------------------

/// A ':' seen while parsing the host (state == url, after `user@`) is a literal token
/// byte (common.cpp:209 `case url:` push_back), not a user/password split.
TEST(DsnParse, ColonInUrlStateIsLiteral) {
    const auto opts = parse("tcp://user@a:b[db]");
    EXPECT_EQ(opts.user, "user");
    EXPECT_EQ(opts.uri, "a:b"); // the ':' after the '@' stays inside the uri token
    EXPECT_EQ(opts.database, "db");
}

/// A second '@' (the first already split user/url, so state == url) hits the '@' default
/// arm (common.cpp:226) and is kept verbatim in the uri token.
TEST(DsnParse, SecondAtSignInUrlStateIsLiteral) {
    const auto opts = parse("tcp://user@a@b[db]");
    EXPECT_EQ(opts.user, "user");
    EXPECT_EQ(opts.uri, "a@b"); // second '@' pushed into current -> uri
    EXPECT_EQ(opts.database, "db");
}

/// A '[' seen while already parsing the database (state == database, after the first '['
/// opened it) hits the '[' default arm (common.cpp:244) and is a literal byte.
TEST(DsnParse, OpenBracketInDatabaseStateIsLiteral) {
    const auto opts = parse("tcp://u@host[d[b]");
    EXPECT_EQ(opts.uri, "host");
    EXPECT_EQ(opts.database, "d[b"); // second '[' kept inside the database name
}

int
main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
