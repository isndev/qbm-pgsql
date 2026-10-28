/**
 * @file prepared-replan-wire.cpp
 * @brief A statement whose cached plan went stale is re-prepared, and its execution retried once
 *        outside a transaction block (Huly QB-114) -- every edge, on the wire.
 *
 * A fake backend scripts each case byte for byte: the 0A000 that `RevalidateCachedQuery` raises
 * when a DDL changed a statement's result columns, and what the client must send after it --
 * `Close('S')` + `Parse` + `Describe('S')` + `Sync`, then the `Bind` again, with the result
 * formats of the NEW columns, and the execution after it as a Bind alone. It also scripts what no
 * live server produces on demand: commands queued behind the refused execution, a second stale
 * plan right after the retry (no second retry), a 0A000 from another routine (no retry), a
 * re-preparation that fails, a malformed RowDescription, an error inside a block, an execution
 * queued in the failed block it left (no re-preparation), a stale plan after DataRows (no
 * replay), and the unnamed statement of an inline query. The live counterpart, with real DDL, is in
 * `integration/prepared/prepared-statements.cpp`.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include <qbm/pgsql/pgsql.h>

#include "../../shared/pg_fake_backend.h"

using namespace std::chrono_literals;
using namespace qb::pg::test::fake;

namespace prepared_replan_wire_test {

using bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t int4_oid = 23;
constexpr std::uint32_t text_oid = 25;

void
put_i16(bytes &b, std::uint16_t n) {
    b.push_back(static_cast<std::uint8_t>(n >> 8));
    b.push_back(static_cast<std::uint8_t>(n));
}

[[nodiscard]] std::uint16_t
get_i16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

void
put_cstr(bytes &b, std::string const &s) {
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
}

struct column {
    std::string   name;
    std::uint32_t oid;
};

/// RowDescription of @p columns, every one described in text format (a statement Describe's answer).
[[nodiscard]] bytes
row_description(std::vector<column> const &columns) {
    bytes b;
    put_i16(b, static_cast<std::uint16_t>(columns.size()));
    for (auto const &c : columns) {
        put_cstr(b, c.name);
        put_i32(b, 0);           // table OID
        put_i16(b, 0);           // attribute number
        put_i32(b, c.oid);       // type
        put_i16(b, 4);           // type size (unused by the client)
        put_i32(b, 0xFFFFFFFFu); // typmod
        put_i16(b, 0);           // format: a statement is described in text
    }
    return backend_msg('T', b);
}

/// One DataRow of a single text column: what a simple Query returns.
[[nodiscard]] bytes
text_row(std::string const &value) {
    bytes b;
    put_i16(b, 1);
    put_i32(b, static_cast<std::uint32_t>(value.size()));
    b.insert(b.end(), value.begin(), value.end());
    return backend_msg('D', b);
}

[[nodiscard]] bytes
malformed_row_description() {
    bytes b;
    put_i16(b, 0xFFFF); // a negative column count
    return backend_msg('T', b);
}

[[nodiscard]] bytes
ready(char status) {
    return backend_msg('Z', {static_cast<std::uint8_t>(status)});
}

[[nodiscard]] bytes
no_parameters() {
    bytes b;
    put_i16(b, 0);
    return backend_msg('t', b);
}

[[nodiscard]] bytes
command_complete(std::string const &tag) {
    bytes b;
    put_cstr(b, tag);
    return backend_msg('C', b);
}

/// ErrorResponse with severity, SQLSTATE, message and, when given, the routine field.
[[nodiscard]] bytes
error_response(std::string const &sqlstate, std::string const &message, std::optional<std::string> const &routine) {
    bytes b;
    for (auto const &[type, value] :
         std::vector<std::pair<char, std::string>>{{'S', "ERROR"}, {'V', "ERROR"}, {'C', sqlstate}, {'M', message}}) {
        b.push_back(static_cast<std::uint8_t>(type));
        put_cstr(b, value);
    }
    if (routine) {
        b.push_back(static_cast<std::uint8_t>('R'));
        put_cstr(b, *routine);
    }
    b.push_back(0);
    return backend_msg('E', b);
}

/// The 0A000 PostgreSQL raises when a DDL changed a cached statement's result columns.
[[nodiscard]] bytes
stale_plan(std::string const &routine = "RevalidateCachedQuery") {
    return error_response("0A000", "cached plan must not change result type", routine);
}

/**
 * @brief The fake backend of one connection: reads the client's extended-query cycles frame by
 *        frame, summarizes each frame ("P s1", "B s1 r=1,0", "C S s1", ...), and answers.
 *
 * Every read is bounded (5 s); a mismatch records why in `failure` and makes every later step
 * a no-op, so the client is never left waiting on a server that gave up silently.
 */
class backend {
public:
    bool                ok = true;
    std::string         failure;
    std::vector<int>    result_formats; ///< The result format codes of the last Bind
    qb::io::tcp::socket session;

    bool
    accept(qb::io::tcp::listener &listener) {
        if (qb::io::socket::handle_read_ready(listener.native_handle(), 5s) <= 0 || listener.accept(session) != qb::io::SocketStatus::Done)
            return fail("no connection");
        bytes startup;
        bytes auth;
        put_i32(auth, 0);
        if (!read_startup(session, startup) || !send_all(session, backend_msg('R', auth)) || !send_all(session, ready('I')))
            return fail("startup");
        return true;
    }

    /// Read one cycle -- up to its Sync, or one simple Query -- and compare it with @p expected.
    bool
    expect(std::vector<std::string> const &expected) {
        if (!ok)
            return false;
        std::vector<std::string> got;
        for (;;) {
            char  tag = 0;
            bytes body;
            if (!read_frame(tag, body))
                return fail("cycle cut short after [" + join(got) + "], expected [" + join(expected) + "]");
            got.push_back(summarize(tag, body));
            if (tag == 'S' || tag == 'Q')
                break;
        }
        if (got != expected)
            return fail("got [" + join(got) + "], expected [" + join(expected) + "]");
        return true;
    }

    bool
    reply(std::vector<bytes> const &messages) {
        if (!ok)
            return false;
        bytes all;
        for (auto const &m : messages)
            all.insert(all.end(), m.begin(), m.end());
        return send_all(session, all) || fail("send");
    }

    /// One DataRow of an int4 and, when given, a text column, encoded in the formats the last Bind asked.
    [[nodiscard]] bytes
    data_row(std::int32_t id, std::optional<std::string> const &text = std::nullopt) const {
        bytes b;
        put_i16(b, static_cast<std::uint16_t>(text ? 2 : 1));
        if (!result_formats.empty() && result_formats[0] == 1) {
            put_i32(b, 4);
            put_i32(b, static_cast<std::uint32_t>(id));
        } else {
            const auto s = std::to_string(id);
            put_i32(b, static_cast<std::uint32_t>(s.size()));
            b.insert(b.end(), s.begin(), s.end());
        }
        if (text) {
            put_i32(b, static_cast<std::uint32_t>(text->size()));
            b.insert(b.end(), text->begin(), text->end());
        }
        return backend_msg('D', b);
    }

    /// After the script: nothing but the client's Terminate may arrive before it closes.
    void
    finish() {
        for (;;) {
            char  tag = 0;
            bytes body;
            if (!read_frame(tag, body))
                break;
            if (tag != 'X') {
                fail("unexpected frame after the script: " + summarize(tag, body));
                break;
            }
        }
        session.disconnect();
    }

private:
    bool
    fail(std::string why) {
        if (ok) {
            ok      = false;
            failure = std::move(why);
        }
        return false;
    }

    bool
    read_frame(char &tag, bytes &body) {
        std::uint8_t header[5];
        if (!recv_exact(session, header, sizeof header))
            return false;
        const auto length = get_i32(header + 1);
        if (length < 4 || length > 65536)
            return false;
        tag = static_cast<char>(header[0]);
        body.resize(length - 4);
        return recv_exact(session, body.data(), body.size());
    }

    static std::string
    cstr(bytes const &b, std::size_t &pos) {
        std::string s;
        while (pos < b.size() && b[pos] != 0)
            s.push_back(static_cast<char>(b[pos++]));
        ++pos;
        return s;
    }

    std::string
    summarize(char tag, bytes const &body) {
        std::size_t pos = 0;
        switch (tag) {
            case 'P':
                return "P " + cstr(body, pos);
            case 'D':
            case 'C': {
                const char kind = body.empty() ? '?' : static_cast<char>(body[0]);
                pos             = 1;
                return std::string(1, tag) + ' ' + kind + ' ' + cstr(body, pos);
            }
            case 'B': {
                (void) cstr(body, pos); // portal
                const auto statement = cstr(body, pos);
                if (pos + 2 > body.size())
                    return "B malformed";
                pos += 2 + 2 * get_i16(body.data() + pos); // parameter format codes
                if (pos + 2 > body.size())
                    return "B malformed";
                const auto values = get_i16(body.data() + pos);
                pos += 2;
                for (unsigned i = 0; i != values && pos + 4 <= body.size(); ++i) {
                    const auto length = get_i32(body.data() + pos);
                    pos += 4 + (length == 0xFFFFFFFFu ? 0 : length);
                }
                if (pos + 2 > body.size())
                    return "B malformed";
                const auto formats = get_i16(body.data() + pos);
                pos += 2;
                result_formats.clear();
                std::string summary = "B " + statement + " r=";
                for (unsigned i = 0; i != formats && pos + 2 <= body.size(); ++i, pos += 2) {
                    result_formats.push_back(get_i16(body.data() + pos));
                    summary += (i ? "," : "") + std::to_string(result_formats.back());
                }
                return summary;
            }
            case 'Q':
                return "Q " + cstr(body, pos);
            default:
                return std::string(1, tag);
        }
    }

    static std::string
    join(std::vector<std::string> const &v) {
        std::string s;
        for (auto const &e : v)
            s += (s.empty() ? "" : " | ") + e;
        return s;
    }
};

/// One client run against a scripted backend: listener, server thread, connected database.
struct wire {
    qb::io::tcp::listener listener;
    backend               server;
    std::thread           thread;
    qb::pg::tcp::database db;

    template <typename Script>
    bool
    start(Script script) {
        qb::io::async::init();
        if (listener.listen_v4(0, "127.0.0.1") != qb::io::SocketStatus::Done)
            return false;
        const auto port       = listener.local_endpoint().port();
        thread                = std::thread([this, script] {
            if (server.accept(listener))
                script(server);
            server.finish();
        });
        const std::string dsn = "tcp://test:test@127.0.0.1:" + std::to_string(port) + "[test]";
        return static_cast<bool>(qb::io::async::run_sync(db.connect(dsn)));
    }

    void
    stop() {
        db.disconnect();
        if (thread.joinable())
            thread.join();
        listener.disconnect();
        qb::io::async::listener::current.clear();
    }

    /// `prepare(name, sql)` with no declared parameter types; true on success.
    bool
    prepare(std::string const &name, std::string const &sql) {
        return static_cast<bool>(db.prepare(name, sql, qb::pg::type_oid_sequence{}, qb::pg::discard_prepare, qb::pg::discard_error).await());
    }

    struct outcome {
        bool                       ok = false;
        qb::pg::sqlstate::code     sqlstate{qb::pg::sqlstate::unknown_code};
        std::string                what;
        std::size_t                columns = 0;
        std::int32_t               id      = -1;
        std::optional<std::string> text;
        std::vector<std::int32_t>  ids;       ///< Every row's int4, in order
        int                        calls = 0; ///< Callbacks run, success or error
        int                        seq   = 0; ///< Rank of its (last) callback among the run's completions
    };

    int completions = 0; ///< Callbacks run so far, every query of the run

    /// Queue `execute(name)` with no parameters: @p o receives the reply, its first row read as
    /// (int4[, text]).
    void
    queue_execute(std::string const &name, outcome &o) {
        (void) db.execute(
            name, qb::pg::params{},
            [this, &o](qb::pg::detail::Transaction &, qb::pg::results r) {
                ++o.calls;
                o.seq     = ++completions;
                o.ok      = true;
                o.columns = static_cast<std::size_t>(r.columns_size());
                for (std::size_t i = 0; i < static_cast<std::size_t>(r.size()); ++i)
                    o.ids.push_back(r[i][0].as<std::int32_t>());
                if (r.size() == 1U) {
                    o.id = r[0][0].as<std::int32_t>();
                    if (o.columns > 1)
                        o.text = r[0][1].as<std::string>();
                }
            },
            [this, &o](qb::pg::error::db_error const &e) {
                ++o.calls;
                o.seq      = ++completions;
                o.sqlstate = e.sqlstate;
                o.what     = e.what();
            });
    }

    /// `execute(name)` with no parameters, awaited.
    outcome
    execute(std::string const &name) {
        outcome o;
        queue_execute(name, o);
        (void) db.await();
        return o;
    }
};

} // namespace prepared_replan_wire_test

using namespace prepared_replan_wire_test;

/// Outside a block: Close + Parse + Describe of the statement, then the Bind again with the result
/// formats of its NEW columns -- binary for the int4, text for the column the DDL added. The
/// re-preparation cleared the mark: the next execution is a Bind alone, with the same formats.
TEST(PgsqlPreparedReplan, AStalePlanOutsideABlockIsRePreparedAndTheExecutionRetried) {
    wire w;
    ASSERT_TRUE(w.start([](backend &b) {
        b.expect({"P s1", "D S s1", "S"}) && b.reply({backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('I')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({stale_plan(), ready('I')});
        b.expect({"C S s1", "P s1", "D S s1", "S"})
            && b.reply(
                {backend_msg('3', {}), backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}, {"v", text_oid}}),
                 ready('I')});
        b.expect({"B s1 r=1,0", "E", "S"}) && b.reply({backend_msg('2', {}), b.data_row(7, "x"), command_complete("SELECT 1"), ready('I')});
        b.expect({"B s1 r=1,0", "E", "S"}) && b.reply({backend_msg('2', {}), b.data_row(8, "y"), command_complete("SELECT 1"), ready('I')});
    }));
    const auto start = w.db.stats();
    ASSERT_TRUE(w.prepare("s1", "SELECT * FROM t"));
    const auto before = w.db.stats();
    const auto r      = w.execute("s1");
    const auto after  = w.db.stats();
    const auto next   = w.execute("s1");
    const auto end    = w.db.stats();
    w.stop();

    EXPECT_TRUE(w.server.ok) << w.server.failure;
    ASSERT_TRUE(r.ok) << r.what;
    EXPECT_EQ(r.columns, 2U);
    EXPECT_EQ(r.id, 7);
    EXPECT_EQ(r.text, std::optional<std::string>{"x"});
    EXPECT_EQ(after.prepared_replans - before.prepared_replans, 1U);
    EXPECT_EQ(after.queries - before.queries, 1U) << "the retry is the same query, counted once";
    EXPECT_EQ(after.errors - before.errors, 0U) << "the kept 0A000 is not an error of the caller's";
    ASSERT_TRUE(next.ok) << next.what;
    EXPECT_EQ(next.columns, 2U);
    EXPECT_EQ(next.id, 8);
    EXPECT_EQ(next.text, std::optional<std::string>{"y"});
    EXPECT_EQ(end.prepared_replans - start.prepared_replans, 1U) << "the re-preparation cleared the statement's mark";
}

/// Commands queued behind the refused execution wait for its retry: the script sees the
/// re-preparation and the retried Bind, then the second Bind, then the query -- one cycle at a time,
/// nothing interleaved -- and each callback runs once, with its own result.
TEST(PgsqlPreparedReplan, CommandsQueuedBehindTheRefusedExecutionRunAfterItsRetry) {
    wire w;
    ASSERT_TRUE(w.start([](backend &b) {
        b.expect({"P s1", "D S s1", "S"}) && b.reply({backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('I')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({stale_plan(), ready('I')});
        b.expect({"C S s1", "P s1", "D S s1", "S"})
            && b.reply(
                {backend_msg('3', {}), backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}, {"v", text_oid}}),
                 ready('I')});
        b.expect({"B s1 r=1,0", "E", "S"}) && b.reply({backend_msg('2', {}), b.data_row(7, "x"), command_complete("SELECT 1"), ready('I')});
        b.expect({"B s1 r=1,0", "E", "S"}) && b.reply({backend_msg('2', {}), b.data_row(8, "y"), command_complete("SELECT 1"), ready('I')});
        b.expect({"Q SELECT 'q'"}) && b.reply({row_description({{"q", text_oid}}), text_row("q"), command_complete("SELECT 1"), ready('I')});
    }));
    ASSERT_TRUE(w.prepare("s1", "SELECT * FROM t"));
    const auto    before = w.db.stats();
    wire::outcome first;
    wire::outcome second;
    int           query_calls = 0;
    int           query_seq   = 0;
    std::string   query_value;
    w.queue_execute("s1", first);
    w.queue_execute("s1", second);
    (void) w.db.execute(
        "SELECT 'q'",
        [&](qb::pg::detail::Transaction &, qb::pg::results r) {
            ++query_calls;
            query_seq = ++w.completions;
            if (r.size() == 1U)
                query_value = r[0][0].as<std::string>();
        },
        [&](qb::pg::error::db_error const &e) {
            ++query_calls;
            query_seq   = ++w.completions;
            query_value = e.what();
        });
    // Nothing has completed yet: the first Bind is in flight, the rest waits.
    EXPECT_EQ(first.calls + second.calls + query_calls, 0);
    (void) w.db.await();
    const auto after = w.db.stats();
    w.stop();

    EXPECT_TRUE(w.server.ok) << w.server.failure;
    EXPECT_EQ(first.calls, 1);
    EXPECT_EQ(second.calls, 1);
    EXPECT_EQ(query_calls, 1);
    EXPECT_EQ(first.seq, 1);
    EXPECT_EQ(second.seq, 2);
    EXPECT_EQ(query_seq, 3);
    ASSERT_TRUE(first.ok) << first.what;
    EXPECT_EQ(first.ids, (std::vector<std::int32_t>{7}));
    EXPECT_EQ(first.text, std::optional<std::string>{"x"});
    ASSERT_TRUE(second.ok) << second.what;
    EXPECT_EQ(second.ids, (std::vector<std::int32_t>{8}));
    EXPECT_EQ(second.text, std::optional<std::string>{"y"});
    EXPECT_EQ(query_value, "q");
    EXPECT_EQ(after.prepared_replans - before.prepared_replans, 1U);
    EXPECT_EQ(after.queries - before.queries, 3U);
    EXPECT_EQ(after.errors - before.errors, 0U);
}

/// The retry happens once: a second stale plan on the retried Bind reaches the caller, and the
/// statement stays marked, so the NEXT execution re-prepares it before binding.
TEST(PgsqlPreparedReplan, TheRetryIsNotRepeatedAndTheNextExecutionRePrepares) {
    wire w;
    ASSERT_TRUE(w.start([](backend &b) {
        b.expect({"P s1", "D S s1", "S"}) && b.reply({backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('I')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({stale_plan(), ready('I')});
        b.expect({"C S s1", "P s1", "D S s1", "S"})
            && b.reply(
                {backend_msg('3', {}), backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}, {"v", text_oid}}),
                 ready('I')});
        b.expect({"B s1 r=1,0", "E", "S"}) && b.reply({stale_plan(), ready('I')});
        b.expect({"C S s1", "P s1", "D S s1", "S"})
            && b.reply(
                {backend_msg('3', {}), backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}, {"v", text_oid}}),
                 ready('I')});
        b.expect({"B s1 r=1,0", "E", "S"}) && b.reply({backend_msg('2', {}), b.data_row(8, "y"), command_complete("SELECT 1"), ready('I')});
    }));
    ASSERT_TRUE(w.prepare("s1", "SELECT * FROM t"));
    const auto before = w.db.stats();
    const auto first  = w.execute("s1");
    const auto second = w.execute("s1");
    const auto after  = w.db.stats();
    w.stop();

    EXPECT_TRUE(w.server.ok) << w.server.failure;
    EXPECT_FALSE(first.ok);
    EXPECT_EQ(first.sqlstate, qb::pg::sqlstate::feature_not_supported) << first.what;
    ASSERT_TRUE(second.ok) << second.what;
    EXPECT_EQ(second.id, 8);
    EXPECT_EQ(after.prepared_replans - before.prepared_replans, 2U);
    EXPECT_EQ(after.errors - before.errors, 1U);
}

/// In a block the error is the caller's -- the block is aborted, a retry would fail with 25P02 --
/// and the statement is marked: after the ROLLBACK its next execution re-prepares it first.
TEST(PgsqlPreparedReplan, InsideABlockTheErrorReachesTheCallerAndTheNextExecutionRePrepares) {
    wire w;
    ASSERT_TRUE(w.start([](backend &b) {
        // The Prepare's ReadyForQuery says 'T': a block the caller opened is in progress.
        b.expect({"P s1", "D S s1", "S"}) && b.reply({backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('T')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({stale_plan(), ready('E')});
        b.expect({"Q ROLLBACK"}) && b.reply({command_complete("ROLLBACK"), ready('I')});
        b.expect({"C S s1", "P s1", "D S s1", "S"})
            && b.reply(
                {backend_msg('3', {}), backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}, {"v", text_oid}}),
                 ready('I')});
        b.expect({"B s1 r=1,0", "E", "S"}) && b.reply({backend_msg('2', {}), b.data_row(9, "z"), command_complete("SELECT 1"), ready('I')});
    }));
    ASSERT_TRUE(w.prepare("s1", "SELECT * FROM t"));
    const auto before   = w.db.stats();
    const auto in_block = w.execute("s1");
    const bool rolled   = static_cast<bool>(w.db.execute("ROLLBACK", qb::pg::discard_query, qb::pg::discard_error).await());
    const auto after_rb = w.execute("s1");
    const auto after    = w.db.stats();
    w.stop();

    EXPECT_TRUE(w.server.ok) << w.server.failure;
    EXPECT_FALSE(in_block.ok);
    EXPECT_EQ(in_block.sqlstate, qb::pg::sqlstate::feature_not_supported) << in_block.what;
    EXPECT_TRUE(rolled);
    ASSERT_TRUE(after_rb.ok) << after_rb.what;
    EXPECT_EQ(after_rb.columns, 2U);
    EXPECT_EQ(after_rb.text, std::optional<std::string>{"z"});
    EXPECT_EQ(after.prepared_replans - before.prepared_replans, 1U);
}

/// In a failed block ('E') a marked statement is not re-prepared: the server would refuse the Parse
/// with 25P02 as it refuses the Bind. The execution queued behind the 0A000 goes as a Bind, its 25P02
/// reaches the caller, nothing is counted, and the first execution after the ROLLBACK re-prepares.
TEST(PgsqlPreparedReplan, InAFailedBlockAMarkedStatementIsBoundWithoutRePreparation) {
    wire w;
    ASSERT_TRUE(w.start([](backend &b) {
        b.expect({"P s1", "D S s1", "S"}) && b.reply({backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('T')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({stale_plan(), ready('E')});
        b.expect({"B s1 r=1", "E", "S"})
            && b.reply(
                {error_response("25P02", "current transaction is aborted, commands ignored until end of transaction block",
                                "exec_bind_message"),
                 ready('E')});
        b.expect({"Q ROLLBACK"}) && b.reply({command_complete("ROLLBACK"), ready('I')});
        b.expect({"C S s1", "P s1", "D S s1", "S"})
            && b.reply(
                {backend_msg('3', {}), backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}, {"v", text_oid}}),
                 ready('I')});
        b.expect({"B s1 r=1,0", "E", "S"}) && b.reply({backend_msg('2', {}), b.data_row(9, "z"), command_complete("SELECT 1"), ready('I')});
    }));
    ASSERT_TRUE(w.prepare("s1", "SELECT * FROM t"));
    const auto    before = w.db.stats();
    wire::outcome stale;
    wire::outcome aborted;
    w.queue_execute("s1", stale);
    w.queue_execute("s1", aborted);
    (void) w.db.await();
    const auto in_block = w.db.stats();
    const bool rolled   = static_cast<bool>(w.db.execute("ROLLBACK", qb::pg::discard_query, qb::pg::discard_error).await());
    const auto after_rb = w.execute("s1");
    const auto after    = w.db.stats();
    w.stop();

    EXPECT_TRUE(w.server.ok) << w.server.failure;
    EXPECT_FALSE(stale.ok);
    EXPECT_EQ(stale.sqlstate, qb::pg::sqlstate::feature_not_supported) << stale.what;
    EXPECT_FALSE(aborted.ok);
    EXPECT_EQ(aborted.sqlstate, qb::pg::sqlstate::in_failed_sql_transaction) << aborted.what;
    EXPECT_EQ(in_block.prepared_replans - before.prepared_replans, 0U) << "nothing is re-prepared in a failed block";
    EXPECT_TRUE(rolled);
    ASSERT_TRUE(after_rb.ok) << after_rb.what;
    EXPECT_EQ(after_rb.columns, 2U);
    EXPECT_EQ(after_rb.text, std::optional<std::string>{"z"});
    EXPECT_EQ(after.prepared_replans - before.prepared_replans, 1U);
}

/// A stale plan raised after DataRows -- a function that EXECUTEs a SQL-level prepared statement fails
/// mid-stream -- is the caller's: a replay would deliver those rows twice. Nothing is re-prepared for
/// it, and the statement is marked: the next execution re-prepares first and gets its own rows, once.
/// The rows are the execution's own: a stale plan raised before any row of a later one is retried.
TEST(PgsqlPreparedReplan, AStalePlanAfterRowsReachesTheCallerAndIsNotReplayed) {
    wire w;
    ASSERT_TRUE(w.start([](backend &b) {
        b.expect({"P s1", "D S s1", "S"}) && b.reply({backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('I')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({backend_msg('2', {}), b.data_row(1), b.data_row(2), stale_plan(), ready('I')});
        b.expect({"C S s1", "P s1", "D S s1", "S"})
            && b.reply({backend_msg('3', {}), backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('I')});
        b.expect({"B s1 r=1", "E", "S"})
            && b.reply({backend_msg('2', {}), b.data_row(1), b.data_row(2), command_complete("SELECT 2"), ready('I')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({stale_plan(), ready('I')});
        b.expect({"C S s1", "P s1", "D S s1", "S"})
            && b.reply({backend_msg('3', {}), backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('I')});
        b.expect({"B s1 r=1", "E", "S"})
            && b.reply({backend_msg('2', {}), b.data_row(1), b.data_row(2), command_complete("SELECT 2"), ready('I')});
    }));
    ASSERT_TRUE(w.prepare("s1", "SELECT * FROM f()"));
    const auto before   = w.db.stats();
    const auto streamed = w.execute("s1");
    const auto between  = w.db.stats();
    const auto next     = w.execute("s1");
    const auto after    = w.db.stats();
    const auto retried  = w.execute("s1");
    const auto end      = w.db.stats();
    w.stop();

    EXPECT_TRUE(w.server.ok) << w.server.failure;
    EXPECT_FALSE(streamed.ok) << "replayed: the caller got the rows " << ::testing::PrintToString(streamed.ids);
    EXPECT_EQ(streamed.sqlstate, qb::pg::sqlstate::feature_not_supported) << streamed.what;
    EXPECT_EQ(streamed.calls, 1);
    EXPECT_EQ(between.prepared_replans - before.prepared_replans, 0U) << "no Parse for an execution that streamed rows";
    ASSERT_TRUE(next.ok) << next.what;
    EXPECT_EQ(next.calls, 1);
    EXPECT_EQ(next.ids, (std::vector<std::int32_t>{1, 2}));
    EXPECT_EQ(after.prepared_replans - before.prepared_replans, 1U) << "the statement was marked: its next execution re-prepared it";
    ASSERT_TRUE(retried.ok) << retried.what;
    EXPECT_EQ(retried.ids, (std::vector<std::int32_t>{1, 2}));
    EXPECT_EQ(end.prepared_replans - after.prepared_replans, 1U);
    EXPECT_EQ(end.errors - after.errors, 0U) << "the earlier execution's rows do not stop this one's retry";
}

/// 0A000 names any feature the server does not support: only the plan cache's routine is a stale
/// plan. Another one, or none at all, reaches the caller and re-prepares nothing.
TEST(PgsqlPreparedReplan, ZeroA000FromAnotherRoutineIsNotAStalePlan) {
    wire w;
    ASSERT_TRUE(w.start([](backend &b) {
        b.expect({"P s1", "D S s1", "S"}) && b.reply({backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('I')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({stale_plan("exec_bind_message"), ready('I')});
        b.expect({"B s1 r=1", "E", "S"})
            && b.reply({error_response("0A000", "cached plan must not change result type", std::nullopt), ready('I')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({backend_msg('2', {}), b.data_row(5), command_complete("SELECT 1"), ready('I')});
    }));
    ASSERT_TRUE(w.prepare("s1", "SELECT * FROM t"));
    const auto before   = w.db.stats();
    const auto other    = w.execute("s1");
    const auto no_field = w.execute("s1");
    const auto then     = w.execute("s1");
    const auto after    = w.db.stats();
    w.stop();

    EXPECT_TRUE(w.server.ok) << w.server.failure;
    EXPECT_FALSE(other.ok);
    EXPECT_EQ(other.sqlstate, qb::pg::sqlstate::feature_not_supported);
    EXPECT_FALSE(no_field.ok);
    ASSERT_TRUE(then.ok) << then.what;
    EXPECT_EQ(then.id, 5);
    EXPECT_EQ(after.prepared_replans - before.prepared_replans, 0U);
}

/// The re-preparation can fail (the table was dropped): its error is the execution's, and the
/// statement stays marked -- the next execution tries to re-prepare it again.
TEST(PgsqlPreparedReplan, AFailedRePreparationFailsTheExecutionWithItsOwnError) {
    wire w;
    ASSERT_TRUE(w.start([](backend &b) {
        b.expect({"P s1", "D S s1", "S"}) && b.reply({backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('I')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({stale_plan(), ready('I')});
        b.expect({"C S s1", "P s1", "D S s1", "S"})
            && b.reply({backend_msg('3', {}), error_response("42P01", "relation \"t\" does not exist", "parserOpenTable"), ready('I')});
        b.expect({"C S s1", "P s1", "D S s1", "S"})
            && b.reply({backend_msg('3', {}), backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('I')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({backend_msg('2', {}), b.data_row(6), command_complete("SELECT 1"), ready('I')});
    }));
    ASSERT_TRUE(w.prepare("s1", "SELECT * FROM t"));
    const auto failed = w.execute("s1");
    const auto then   = w.execute("s1");
    w.stop();

    EXPECT_TRUE(w.server.ok) << w.server.failure;
    EXPECT_FALSE(failed.ok);
    EXPECT_EQ(failed.sqlstate, qb::pg::sqlstate::undefined_table) << failed.what;
    ASSERT_TRUE(then.ok) << then.what;
    EXPECT_EQ(then.id, 6);
    EXPECT_EQ(w.db.stats().prepared_replans, 2U);
}

/// A malformed RowDescription in the re-preparation fails the execution (the Bind could not take its
/// result formats from it) instead of binding with columns the client could not read.
TEST(PgsqlPreparedReplan, AMalformedRowDescriptionFailsTheExecution) {
    wire w;
    ASSERT_TRUE(w.start([](backend &b) {
        b.expect({"P s1", "D S s1", "S"}) && b.reply({backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('I')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({stale_plan(), ready('I')});
        b.expect({"C S s1", "P s1", "D S s1", "S"})
            && b.reply({backend_msg('3', {}), backend_msg('1', {}), no_parameters(), malformed_row_description(), ready('I')});
        b.expect({"C S s1", "P s1", "D S s1", "S"})
            && b.reply({backend_msg('3', {}), backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('I')});
        b.expect({"B s1 r=1", "E", "S"}) && b.reply({backend_msg('2', {}), b.data_row(4), command_complete("SELECT 1"), ready('I')});
    }));
    ASSERT_TRUE(w.prepare("s1", "SELECT * FROM t"));
    const auto failed = w.execute("s1");
    const auto then   = w.execute("s1");
    w.stop();

    EXPECT_TRUE(w.server.ok) << w.server.failure;
    EXPECT_FALSE(failed.ok);
    EXPECT_NE(failed.what.find("malformed RowDescription"), std::string::npos) << failed.what;
    ASSERT_TRUE(then.ok) << then.what;
    EXPECT_EQ(then.id, 4);
}

/// The inline `query(sql, args...)` parses its unnamed statement on every call: a stale plan there
/// is the caller's, and nothing is re-prepared.
TEST(PgsqlPreparedReplan, TheUnnamedStatementOfAnInlineQueryIsNotRePrepared) {
    wire w;
    ASSERT_TRUE(w.start([](backend &b) {
        b.expect({"P ", "D S ", "S"}) && b.reply({backend_msg('1', {}), no_parameters(), row_description({{"id", int4_oid}}), ready('I')});
        b.expect({"B  r=1", "E", "S"}) && b.reply({stale_plan(), ready('I')});
    }));
    std::optional<qb::pg::sqlstate::code> code;
    auto                                  reply = qb::io::async::run_sync(w.db.query("SELECT $1::int", 1));
    if (!reply.ok())
        code = reply.error().sqlstate;
    const auto replans = w.db.stats().prepared_replans;
    w.stop();

    EXPECT_TRUE(w.server.ok) << w.server.failure;
    EXPECT_EQ(code, std::optional<qb::pg::sqlstate::code>{qb::pg::sqlstate::feature_not_supported});
    EXPECT_EQ(replans, 0U);
}
