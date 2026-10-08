/**
 * @file inline-query-order.cpp
 * @brief Deterministic unnamed-statement interleaving test (QB-106).
 *
 * A fake backend holds Parse A's reply until both coroutine queries have started.
 * It models PostgreSQL's unnamed statement: each Parse replaces the previous SQL.
 * A and B have compatible parameters and columns, so a reordered Bind can return
 * a plausible but wrong value rather than a protocol error.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include <qbm/pgsql/pgsql.h>

#include "../../shared/pg_fake_backend.h"

using namespace std::chrono_literals;
using namespace qb::pg::test::fake;

namespace inline_query_order_test {

struct WireResult {
    bool                     ok{true};
    std::vector<std::string> order;
};

void
put_i16(std::vector<std::uint8_t> &b, std::uint16_t n) {
    b.push_back(static_cast<std::uint8_t>(n >> 8));
    b.push_back(static_cast<std::uint8_t>(n));
}

std::uint16_t
get_i16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

bool
read_frame(qb::io::tcp::socket &s, char &tag, std::vector<std::uint8_t> &body) {
    std::uint8_t header[5];
    if (!recv_exact(s, header, sizeof header))
        return false;
    const auto length = get_i32(header + 1);
    if (length < 4 || length > 65536)
        return false;
    tag = static_cast<char>(header[0]);
    body.resize(length - 4);
    return recv_exact(s, body.data(), body.size());
}

std::vector<std::uint8_t>
row_description() {
    std::vector<std::uint8_t> b;
    put_i16(b, 1);
    for (char c : std::string("answer"))
        b.push_back(static_cast<std::uint8_t>(c));
    b.push_back(0);
    put_i32(b, 0);           // table OID
    put_i16(b, 0);           // attribute number
    put_i32(b, 23);          // int4
    put_i16(b, 4);           // int4 width
    put_i32(b, 0xFFFFFFFFu); // typmod
    put_i16(b, 0);           // described in text format
    return b;
}

bool
send_ready(qb::io::tcp::socket &s) {
    return send_all(s, backend_msg('Z', {'I'}));
}

int
bind_value(const std::vector<std::uint8_t> &b) {
    std::size_t pos = 0;
    while (pos < b.size() && b[pos] != 0)
        ++pos;
    ++pos; // portal
    while (pos < b.size() && b[pos] != 0)
        ++pos;
    ++pos; // statement
    if (pos + 2 > b.size())
        return -1;
    const auto format_count = get_i16(b.data() + pos);
    pos += 2 + 2 * format_count;
    if (pos + 10 > b.size() || get_i16(b.data() + pos) != 1)
        return -1;
    pos += 2;
    if (get_i32(b.data() + pos) != 4)
        return -1;
    return static_cast<int>(get_i32(b.data() + pos + 4));
}

void
backend(qb::io::tcp::listener *listener, std::atomic<int> *started, WireResult *result) {
    if (qb::io::socket::handle_read_ready(listener->native_handle(), 5s) <= 0) {
        result->ok = false;
        return;
    }
    qb::io::tcp::socket session;
    if (listener->accept(session) != qb::io::SocketStatus::Done) {
        result->ok = false;
        return;
    }
    std::vector<std::uint8_t> startup;
    std::vector<std::uint8_t> auth;
    put_i32(auth, 0);
    if (!read_startup(session, startup) || !send_all(session, backend_msg('R', auth)) || !send_ready(session)) {
        result->ok = false;
        return;
    }

    std::string unnamed_sql;
    for (int phase = 0; phase != 4; ++phase) {
        char                      tag = 0;
        std::vector<std::uint8_t> body;
        if (!read_frame(session, tag, body)) {
            result->ok = false;
            return;
        }
        if (tag == 'P') {
            std::size_t pos = 0;
            while (pos < body.size() && body[pos] != 0)
                ++pos;
            ++pos;
            const auto begin = pos;
            while (pos < body.size() && body[pos] != 0)
                ++pos;
            if (pos >= body.size()) {
                result->ok = false;
                return;
            }
            unnamed_sql.assign(reinterpret_cast<const char *>(body.data() + begin), pos - begin);
            result->order.push_back(unnamed_sql.find("+ 100") != std::string::npos ? "Parse B" : "Parse A");
            char next = 0;
            if (!read_frame(session, next, body) || next != 'D' || !read_frame(session, next, body) || next != 'S') {
                result->ok = false;
                return;
            }
            if (phase == 0) {
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                while (started->load() != 2 && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(1ms);
                if (started->load() != 2) {
                    result->ok = false;
                    return;
                }
            }
            if (!send_all(session, backend_msg('1', {})) || !send_all(session, backend_msg('T', row_description())) || !send_ready(session)) {
                result->ok = false;
                return;
            }
        } else if (tag == 'B') {
            const int value = bind_value(body);
            if (value < 0) {
                result->ok = false;
                return;
            }
            result->order.push_back(value == 10 ? "Bind A" : "Bind B");
            char next = 0;
            if (!read_frame(session, next, body) || next != 'E' || !read_frame(session, next, body) || next != 'S') {
                result->ok = false;
                return;
            }
            std::vector<std::uint8_t> row;
            put_i16(row, 1);
            put_i32(row, 4);
            put_i32(row, static_cast<std::uint32_t>(value + (unnamed_sql.find("+ 100") != std::string::npos ? 100 : 1)));
            std::vector<std::uint8_t> complete{'S', 'E', 'L', 'E', 'C', 'T', ' ', '1', 0};
            if (!send_all(session, backend_msg('2', {})) || !send_all(session, backend_msg('D', row))
                || !send_all(session, backend_msg('C', complete)) || !send_ready(session)) {
                result->ok = false;
                return;
            }
        } else {
            result->ok = false;
            return;
        }
    }
    session.disconnect();
}

std::vector<std::uint8_t>
parse_canceled_error() {
    std::vector<std::uint8_t> body;
    for (const auto &field : {std::string("SERROR"), std::string("C57014"), std::string("Mparse canceled")}) {
        body.insert(body.end(), field.begin(), field.end());
        body.push_back(0);
    }
    body.push_back(0);
    return backend_msg('E', body);
}

void
backend_parse_cancel(qb::io::tcp::listener *listener, std::atomic<int> *started, WireResult *result) {
    if (qb::io::socket::handle_read_ready(listener->native_handle(), 5s) <= 0) {
        result->ok = false;
        return;
    }
    qb::io::tcp::socket session;
    if (listener->accept(session) != qb::io::SocketStatus::Done) {
        result->ok = false;
        return;
    }
    std::vector<std::uint8_t> body;
    std::vector<std::uint8_t> auth;
    put_i32(auth, 0);
    if (!read_startup(session, body) || !send_all(session, backend_msg('R', auth)) || !send_ready(session)) {
        result->ok = false;
        return;
    }

    char tag = 0;
    if (!read_frame(session, tag, body) || tag != 'P' || !read_frame(session, tag, body) || tag != 'D' || !read_frame(session, tag, body)
        || tag != 'S') {
        result->ok = false;
        return;
    }
    result->order.push_back("Parse A");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (started->load() != 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    if (started->load() != 2 || !send_all(session, parse_canceled_error()) || !send_ready(session)) {
        result->ok = false;
        return;
    }

    // The next frontend frame must be B's Parse. A's Bind after ErrorResponse is a failure.
    if (!read_frame(session, tag, body) || tag != 'P') {
        result->ok = false;
        return;
    }
    result->order.push_back("Parse B");
    if (!read_frame(session, tag, body) || tag != 'D' || !read_frame(session, tag, body) || tag != 'S'
        || !send_all(session, backend_msg('1', {})) || !send_all(session, backend_msg('T', row_description())) || !send_ready(session)) {
        result->ok = false;
        return;
    }

    if (!read_frame(session, tag, body) || tag != 'B' || bind_value(body) != 20) {
        result->ok = false;
        return;
    }
    result->order.push_back("Bind B");
    if (!read_frame(session, tag, body) || tag != 'E' || !read_frame(session, tag, body) || tag != 'S') {
        result->ok = false;
        return;
    }
    std::vector<std::uint8_t> row;
    put_i16(row, 1);
    put_i32(row, 4);
    put_i32(row, 120);
    const std::vector<std::uint8_t> complete{'S', 'E', 'L', 'E', 'C', 'T', ' ', '1', 0};
    result->ok = send_all(session, backend_msg('2', {})) && send_all(session, backend_msg('D', row))
                 && send_all(session, backend_msg('C', complete)) && send_ready(session);
    session.disconnect();
}

qb::io::async::task<void>
run_query(qb::pg::tcp::database *db, std::string sql, int value, std::atomic<int> *started, int *answer, bool *done) {
    started->fetch_add(1);
    auto reply = co_await db->query(std::move(sql), value);
    if (reply.ok() && reply.result().size() == 1)
        *answer = reply.result()[0][0].as<int>();
    *done = true;
}

qb::io::async::task<void>
run_canceled_query(qb::pg::tcp::database *db, std::atomic<int> *started, std::string *code, bool *done) {
    started->fetch_add(1);
    auto reply = co_await db->query("SELECT $1::int + 1", 10);
    if (!reply.ok())
        *code = reply.error().code;
    *done = true;
}

} // namespace inline_query_order_test

TEST(PgsqlInlineQueryOrder, TwoOverlappingQueriesKeepTheirOwnUnnamedStatement) {
    using namespace inline_query_order_test;
    qb::io::async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<int>      started{0};
    WireResult            wire;
    std::thread           server(backend, &listener, &started, &wire);
    qb::pg::tcp::database db;
    const std::string     dsn       = "tcp://test:test@127.0.0.1:" + std::to_string(port) + "[test]";
    const bool            connected = static_cast<bool>(qb::io::async::run_sync(db.connect(dsn)));
    int                   a = -1, b = -1;
    bool                  a_done = false, b_done = false;
    if (connected) {
        auto &scheduler = qb::io::async::coro_scheduler();
        scheduler.spawn(run_query(&db, "SELECT $1::int + 1", 10, &started, &a, &a_done));
        scheduler.spawn(run_query(&db, "SELECT $1::int + 100", 20, &started, &b, &b_done));
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while ((!a_done || !b_done) && std::chrono::steady_clock::now() < deadline) {
            qb::io::async::run(EVRUN_NOWAIT);
            std::this_thread::sleep_for(1ms);
        }
    }
    db.disconnect();
    server.join();
    listener.disconnect();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(wire.ok);
    EXPECT_TRUE(a_done);
    EXPECT_TRUE(b_done);
    EXPECT_EQ(a, 11);
    EXPECT_EQ(b, 120);
    EXPECT_EQ(wire.order, (std::vector<std::string>{"Parse A", "Bind A", "Parse B", "Bind B"}));
}

TEST(PgsqlInlineQueryOrder, ParseErrorSendsNoBindAndAllowsQueuedQuery) {
    using namespace inline_query_order_test;
    qb::io::async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const auto port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    std::atomic<int>      started{0};
    WireResult            wire;
    std::thread           server(backend_parse_cancel, &listener, &started, &wire);
    qb::pg::tcp::database db;
    const std::string     dsn       = "tcp://test:test@127.0.0.1:" + std::to_string(port) + "[test]";
    const bool            connected = static_cast<bool>(qb::io::async::run_sync(db.connect(dsn)));
    std::string           a_code;
    int                   b      = -1;
    bool                  a_done = false, b_done = false;
    if (connected) {
        auto &scheduler = qb::io::async::coro_scheduler();
        scheduler.spawn(run_canceled_query(&db, &started, &a_code, &a_done));
        scheduler.spawn(run_query(&db, "SELECT $1::int + 100", 20, &started, &b, &b_done));
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while ((!a_done || !b_done) && std::chrono::steady_clock::now() < deadline) {
            qb::io::async::run(EVRUN_NOWAIT);
            std::this_thread::sleep_for(1ms);
        }
    }
    db.disconnect();
    server.join();
    listener.disconnect();

    ASSERT_TRUE(connected);
    ASSERT_TRUE(wire.ok);
    EXPECT_TRUE(a_done);
    EXPECT_TRUE(b_done);
    EXPECT_EQ(a_code, "57014");
    EXPECT_EQ(b, 120);
    EXPECT_EQ(wire.order, (std::vector<std::string>{"Parse A", "Parse B", "Bind B"}));
}
