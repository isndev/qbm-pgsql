/**
 * @file qbm/pgsql/tests/system/connection/cancel-request-wire.cpp
 * @brief System test: the out-of-band CancelRequest on the wire, `cancel_async()` next to `cancel()`,
 *        without a daemon (Huly QB-113).
 *
 * A fake PostgreSQL backend on a background thread (the shared `pg_fake_backend.h` helpers: qb's
 * own cross-platform `qb::io::tcp` sockets, an ephemeral loopback port) speaks just enough of the
 * wire protocol to hand the client a session --
 *   Startup  ->  AuthenticationOk(0), BackendKeyData(K: pid, secret), ReadyForQuery('I')
 * -- then accepts the SECOND connection the cancel opens and reads what it carries. What is pinned:
 *   - `cancel_async()` delivers exactly the 16-byte CancelRequest (length 16, code 80877102, the
 *     pid and the secret the backend handed out) and reports true;
 *   - it does so WITHOUT blocking the loop: a callback deferred to the loop's next turn before the
 *     cancel runs before the cancel completes, which the blocking `cancel()` can never let happen;
 *   - `cancel()` sends the same bytes (the two variants share the packet) -- so a caller moving
 *     from the blocking to the non-blocking form changes nothing on the wire;
 *   - both report false, and open no socket, on a never-connected database.
 * The TLS half of QB-113 (a secure database negotiating SSLRequest -> TLS on the cancel
 * connection) needs a TLS-speaking peer and is proven against a live server in
 * `integration/connection/connection-ssl.cpp`.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include <qbm/pgsql/pgsql.h>

#include "../../shared/pg_fake_backend.h"

#ifdef QB_HAS_SSL
#include <qb/io/tcp/ssl/socket.h>
#include <qb/io/transport/stcp.h>
#include <openssl/err.h>
#include "ssl_fixtures.h"
#endif

using namespace qb::pg;
using namespace qb::pg::test::fake;
using namespace std::chrono_literals;

namespace cancel_request_wire_test {

constexpr std::uint32_t kBackendPid    = 4242;
constexpr std::uint32_t kBackendSecret = 0x0BADF00Du;

struct CancelWire {
    std::atomic<bool>         session_up{false};  ///< the fake answered the startup (Ok + K + Z)
    std::atomic<bool>         second_conn{false}; ///< the cancel connection was accepted
    std::atomic<bool>         got_16{false};      ///< 16 bytes arrived on it
    std::atomic<bool>         then_eof{false};    ///< and nothing followed (the client closed)
    std::vector<std::uint8_t> bytes;              ///< what arrived on the cancel connection
};

// Accept the session, hand out the key, then accept the cancel connection and record its bytes.
void
run_cancel_backend(qb::io::tcp::listener &listener, CancelWire *res) {
    if (qb::io::socket::handle_read_ready(listener.native_handle(), std::chrono::seconds(10)) <= 0)
        return;
    qb::io::tcp::socket session;
    if (listener.accept(session) != qb::io::SocketStatus::Done)
        return;

    std::vector<std::uint8_t> startup;
    if (!read_startup(session, startup)) {
        session.disconnect();
        return;
    }
    {
        std::vector<std::uint8_t> ok;
        put_i32(ok, 0); // AuthenticationOk
        std::vector<std::uint8_t> key;
        put_i32(key, kBackendPid);
        put_i32(key, kBackendSecret);
        std::vector<std::uint8_t> ready{static_cast<std::uint8_t>('I')};
        if (!send_all(session, backend_msg('R', ok)) || !send_all(session, backend_msg('K', key))
            || !send_all(session, backend_msg('Z', ready))) {
            session.disconnect();
            return;
        }
    }
    res->session_up = true;

    // The cancel connection: a SECOND accept on the same listener.
    if (qb::io::socket::handle_read_ready(listener.native_handle(), std::chrono::seconds(10)) <= 0) {
        session.disconnect();
        return;
    }
    qb::io::tcp::socket cancel;
    if (listener.accept(cancel) != qb::io::SocketStatus::Done) {
        session.disconnect();
        return;
    }
    res->second_conn = true;
    std::uint8_t buf[16];
    if (recv_exact(cancel, buf, sizeof buf)) {
        res->bytes.assign(buf, buf + sizeof buf);
        res->got_16   = true;
        res->then_eof = peer_stays_silent(cancel, 500ms);
    }
    cancel.disconnect();
    session.disconnect();
}

/// The session against the fake backend: connected, and the BackendKeyData landed.
[[nodiscard]] bool
open_session(qb::pg::tcp::database &db, std::uint16_t port) {
    const std::string dsn = "tcp://test:test@127.0.0.1:" + std::to_string(port) + "[test]";
    if (!static_cast<bool>(qb::io::async::run_sync(db.connect(dsn))))
        return false;
    // AuthenticationOk resumes connect(); the K that follows is processed by the next passes.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (db.backend_pid() != static_cast<int>(kBackendPid) && std::chrono::steady_clock::now() < deadline)
        qb::io::async::run(EVRUN_NOWAIT);
    return db.backend_pid() == static_cast<int>(kBackendPid);
}

void
expect_cancel_request(const std::vector<std::uint8_t> &bytes) {
    ASSERT_EQ(bytes.size(), 16u);
    EXPECT_EQ(get_i32(bytes.data() + 0), 16u) << "CancelRequest length";
    EXPECT_EQ(get_i32(bytes.data() + 4), 80877102u) << "CancelRequest code (0x04D2162E)";
    EXPECT_EQ(get_i32(bytes.data() + 8), kBackendPid) << "the backend pid the session was handed";
    EXPECT_EQ(get_i32(bytes.data() + 12), kBackendSecret) << "the secret the session was handed";
}

} // namespace cancel_request_wire_test

using namespace cancel_request_wire_test;

TEST(PgsqlCancelWire, CancelAsyncSendsTheCancelRequestOnASecondConnectionWithoutBlockingTheLoop) {
    qb::io::async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    CancelWire res;
    // A fatal assertion below returns from TestBody. Keep the worker join owned by
    // scope while listener and result storage are still alive.
    std::jthread backend(run_cancel_backend, std::ref(listener), &res);

    bool delivered = false;
    int  ticks     = 0;
    {
        qb::pg::tcp::database db;
        ASSERT_TRUE(open_session(db, port)) << "the fake backend's AuthenticationOk + BackendKeyData never made a session";
        EXPECT_TRUE(res.session_up.load());

        // Liveness witness: a deferred callback runs on the loop's next turn. A cancel that blocked
        // the loop until its socket was connected and written would finish before that turn.
        bool ticked_before_cancel_completed = false;
        qb::io::async::defer([&] { ++ticks; });
        delivered = qb::io::async::run_sync([&]() -> qb::io::async::task<bool> {
            const bool ok                  = co_await db.cancel_async();
            ticked_before_cancel_completed = ticks > 0;
            co_return ok;
        }());
        EXPECT_TRUE(ticked_before_cancel_completed) << "cancel_async() completed before the loop ran anything else: it blocked";
        db.disconnect();
    }
    backend.join();
    listener.disconnect();

    EXPECT_TRUE(delivered) << "cancel_async() must report the request delivered";
    ASSERT_TRUE(res.second_conn.load()) << "no second connection reached the backend";
    ASSERT_TRUE(res.got_16.load()) << "the cancel connection did not carry 16 bytes";
    expect_cancel_request(res.bytes);
    EXPECT_TRUE(res.then_eof.load()) << "the cancel connection carried more than the CancelRequest, or stayed open";
}

TEST(PgsqlCancelWire, BlockingCancelSendsTheSameBytes) {
    qb::io::async::init();
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const std::uint16_t port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    CancelWire   res;
    std::jthread backend(run_cancel_backend, std::ref(listener), &res);

    bool delivered = false;
    {
        qb::pg::tcp::database db;
        ASSERT_TRUE(open_session(db, port));
        delivered = db.cancel();
        db.disconnect();
    }
    backend.join();
    listener.disconnect();

    EXPECT_TRUE(delivered);
    ASSERT_TRUE(res.got_16.load());
    expect_cancel_request(res.bytes);
    EXPECT_TRUE(res.then_eof.load());
}

TEST(PgsqlCancelWire, NeverConnectedReportsFalseAndOpensNothing) {
    qb::io::async::init();
    // A listener nobody should reach: the never-connected database has no endpoint AND no key.
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);

    qb::pg::tcp::database db;
    EXPECT_FALSE(db.cancel());
    EXPECT_FALSE(qb::io::async::run_sync(db.cancel_async()));
    // Nothing knocked: the listener has no pending connection.
    EXPECT_LE(qb::io::socket::handle_read_ready(listener.native_handle(), 100ms), 0);
    listener.disconnect();
}

#ifdef QB_HAS_SSL
namespace cancel_request_wire_test {

struct CancelTlsRetryProbe {
    bool          request_renegotiation       = false;
    bool          renegotiation_armed         = false;
    bool          initial_handshake_complete  = false;
    bool          native_write_wanted_read    = false;
    bool          blocked_socket_writable     = false;
    bool          blocked_socket_not_readable = false;
    std::size_t   write_calls                 = 0;
    std::size_t   successful_write_calls      = 0;
    std::size_t   want_read_calls             = 0;
    std::size_t   want_write_calls            = 0;
    std::size_t   failed_write_calls          = 0;
    int           last_write_errno            = 0;
    unsigned long last_write_ssl_error        = 0;
    std::size_t   accepted_bytes              = 0;
    long          completed_renegotiations    = 0;
};

// The existing Database<transport> extension point owns this test socket. The
// application write itself still calls qb's native SSL_write path; no return or
// OpenSSL rwstate is fabricated. A fresh TLS 1.2 renegotiation makes that write
// require peer input while the ordinary TCP descriptor remains writable.
class CancelRetrySocket : public qb::io::tcp::ssl::socket {
    static qb::io::ssl::Context
    tls12(qb::io::ssl::Context tls) {
        tls.min_version(qb::io::ssl::TlsVersion::v1_2).max_version(qb::io::ssl::TlsVersion::v1_2);
        return tls;
    }

public:
    inline static thread_local CancelTlsRetryProbe *probe = nullptr;

    CancelRetrySocket() = default;
    explicit CancelRetrySocket(qb::io::ssl::Context tls)
        : qb::io::tcp::ssl::socket(tls12(std::move(tls))) {}
    CancelRetrySocket(CancelRetrySocket &&)            = default;
    CancelRetrySocket &operator=(CancelRetrySocket &&) = default;

    int
    write(const void *data, std::size_t size) noexcept {
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        if (!probe || size != 16 || get_i32(bytes) != 16u || get_i32(bytes + 4) != 80877102u)
            return qb::io::tcp::ssl::socket::write(data, size);
        ++probe->write_calls;
        if (probe->request_renegotiation && probe->write_calls == 1) {
            probe->initial_handshake_complete = handshake_complete();
            probe->renegotiation_armed =
                probe->initial_handshake_complete && SSL_version(ssl_handle()) == TLS1_2_VERSION && SSL_renegotiate(ssl_handle()) == 1;
            if (!probe->renegotiation_armed)
                return -1;
        }
        const int n = qb::io::tcp::ssl::socket::write(data, size);
        if (n < 0) {
            ++probe->failed_write_calls;
            probe->last_write_errno     = qb::io::socket::get_last_errno();
            probe->last_write_ssl_error = ERR_peek_last_error();
        }
        if (n == 0 && SSL_want_read(ssl_handle())) {
            ++probe->want_read_calls;
            probe->native_write_wanted_read = true;
            if (probe->write_calls == 1) {
                probe->blocked_socket_writable     = qb::io::socket::handle_write_ready(native_handle(), 0ms) > 0;
                probe->blocked_socket_not_readable = qb::io::socket::handle_read_ready(native_handle(), 0ms) == 0;
            }
        } else if (n == 0 && SSL_want_write(ssl_handle())) {
            ++probe->want_write_calls;
        } else if (n > 0) {
            ++probe->successful_write_calls;
            probe->accepted_bytes += static_cast<std::size_t>(n);
            probe->completed_renegotiations = SSL_total_renegotiations(ssl_handle());
        }
        return n;
    }
};

struct CancelTlsPeerReadProbe {
    std::size_t   read_calls            = 0;
    std::size_t   successful_read_calls = 0;
    std::size_t   want_read_calls       = 0;
    std::size_t   want_write_calls      = 0;
    std::size_t   failed_read_calls     = 0;
    std::size_t   accepted_bytes        = 0;
    int           last_read_errno       = 0;
    unsigned long last_read_ssl_error   = 0;
    bool          readiness_wait_failed = false;
};

std::string
tls_error_text(unsigned long code) {
    char text[256]{};
    ERR_error_string_n(code, text, sizeof text);
    return text;
}

class CancelRetryTransport : public qb::io::stream<CancelRetrySocket> {
public:
    static constexpr bool
    is_secure() noexcept {
        return true;
    }
};

using CancelRetryDatabase = qb::pg::detail::Database<CancelRetryTransport, void>;

bool
wait_tls_io(qb::io::tcp::ssl::socket &socket, std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
        return false;
    const auto remaining = std::chrono::duration_cast<qb::duration>(deadline - now);
    return (SSL_want_write(socket.ssl_handle()) ? qb::io::socket::handle_write_ready(socket.native_handle(), remaining)
                                                : qb::io::socket::handle_read_ready(socket.native_handle(), remaining))
           > 0;
}

bool
tls_recv_exact(qb::io::tcp::ssl::socket &socket, std::uint8_t *bytes, std::size_t size, CancelTlsPeerReadProbe *probe = nullptr) {
    const auto  deadline = std::chrono::steady_clock::now() + 5s;
    std::size_t got      = 0;
    while (got < size) {
        const int n = socket.read(bytes + got, size - got);
        if (probe) {
            ++probe->read_calls;
            if (n < 0) {
                ++probe->failed_read_calls;
                probe->last_read_errno     = qb::io::socket::get_last_errno();
                probe->last_read_ssl_error = ERR_peek_last_error();
            } else if (n > 0) {
                ++probe->successful_read_calls;
                probe->accepted_bytes += static_cast<std::size_t>(n);
            } else if (SSL_want_read(socket.ssl_handle())) {
                ++probe->want_read_calls;
            } else if (SSL_want_write(socket.ssl_handle())) {
                ++probe->want_write_calls;
            }
        }
        if (n < 0)
            return false;
        if (n > 0)
            got += static_cast<std::size_t>(n);
        else if (!wait_tls_io(socket, deadline)) {
            if (probe)
                probe->readiness_wait_failed = true;
            return false;
        }
    }
    return true;
}

bool
tls_send_all(qb::io::tcp::ssl::socket &socket, const std::vector<std::uint8_t> &bytes) {
    const auto  deadline = std::chrono::steady_clock::now() + 5s;
    std::size_t sent     = 0;
    while (sent < bytes.size()) {
        const int n = socket.write(bytes.data() + sent, bytes.size() - sent);
        if (n < 0)
            return false;
        if (n > 0)
            sent += static_cast<std::size_t>(n);
        else if (!wait_tls_io(socket, deadline))
            return false;
    }
    return true;
}

class CancelTlsPeer {
    qb::io::tcp::listener             listener_;
    qb::io::ssl::Context              tls_;
    std::thread                       worker_;
    std::mutex                        mutex_;
    std::condition_variable           condition_;
    bool                              release_response_ = false;
    qb::io::test::teardown_rendezvous teardown_;

    bool
    accept_tls(qb::io::tcp::ssl::socket &out) {
        if (qb::io::socket::handle_read_ready(listener_.native_handle(), 5s) <= 0)
            return false;
        qb::io::tcp::socket plain;
        if (listener_.accept(plain) != qb::io::SocketStatus::Done)
            return false;
        if (plain.set_nonblocking(true) != 0)
            return false;
        std::uint8_t request[8];
        if (!recv_exact(plain, request, sizeof request) || get_i32(request) != 8u || get_i32(request + 4) != 80877103u
            || qb::io::socket::handle_write_ready(plain.native_handle(), 5s) <= 0 || !send_all(plain, std::vector<std::uint8_t>{'S'}))
            return false;
        SSL *ssl = SSL_new(tls_.native());
        if (!ssl)
            return false;
        SSL_set_accept_state(ssl);
        out = qb::io::tcp::ssl::socket{ssl, plain};
        if (out.set_nonblocking(true) != 0)
            return false;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            const int step = out.handshake_status();
            if (step != 0)
                return step == 1;
            if (!wait_tls_io(out, deadline))
                return false;
        }
    }

    void
    run() {
        qb::io::tcp::ssl::socket session;
        qb::io::tcp::ssl::socket cancel;
        if (!accept_tls(session))
            return;
        std::uint8_t length[4];
        if (!tls_recv_exact(session, length, sizeof length))
            return;
        const auto startup_size = get_i32(length);
        if (startup_size < 8u || startup_size > 65536u)
            return;
        std::vector<std::uint8_t> startup(startup_size - 4u);
        if (!tls_recv_exact(session, startup.data(), startup.size()))
            return;
        std::vector<std::uint8_t> ok;
        put_i32(ok, 0);
        std::vector<std::uint8_t> key;
        put_i32(key, kBackendPid);
        put_i32(key, kBackendSecret);
        if (!tls_send_all(session, backend_msg('R', ok)) || !tls_send_all(session, backend_msg('K', key))
            || !tls_send_all(session, backend_msg('Z', std::vector<std::uint8_t>{'I'})) || !accept_tls(cancel))
            return;
        // Hold ALL application/renegotiation reads until the test has observed
        // the native read demand and inspected a fixed number of loop turns.
        {
            std::unique_lock lock(mutex_);
            if (!condition_.wait_for(lock, 5s, [&] { return release_response_; }))
                return;
        }
        std::uint8_t packet[16];
        if (tls_recv_exact(cancel, packet, sizeof packet, &cancel_reads)) {
            bytes.assign(packet, packet + sizeof packet);
            received = true;
        }
        teardown_.server_done();
        (void) teardown_.wait_client();
    }

public:
    bool                      received = false;
    std::vector<std::uint8_t> bytes;
    CancelTlsPeerReadProbe    cancel_reads;

    CancelTlsPeer()
        : tls_(qb::io::ssl::Context::server(qb::io::test::ssl_resource_path("cert.pem"), qb::io::test::ssl_resource_path("key.pem"))) {
        tls_.min_version(qb::io::ssl::TlsVersion::v1_2).max_version(qb::io::ssl::TlsVersion::v1_2);
#ifdef SSL_OP_ALLOW_CLIENT_RENEGOTIATION
        if (tls_.ok())
            SSL_CTX_set_options(tls_.native(), SSL_OP_ALLOW_CLIENT_RENEGOTIATION);
#endif
    }

    ~CancelTlsPeer() {
        release_response();
        teardown_.release_all();
        if (worker_.joinable())
            worker_.join();
        listener_.disconnect();
    }

    bool
    start() {
        if (!tls_.ok() || listener_.listen_v4(0, "127.0.0.1") != qb::io::SocketStatus::Done)
            return false;
        worker_ = std::thread([this] { run(); });
        return true;
    }

    std::uint16_t
    port() const {
        return listener_.local_endpoint().port();
    }

    void
    release_response() {
        {
            std::lock_guard lock(mutex_);
            release_response_ = true;
        }
        condition_.notify_one();
    }

    void
    finish() {
        teardown_.client_done();
        if (worker_.joinable())
            worker_.join();
    }
};

template <typename Predicate>
bool
pump_cancel_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        qb::io::async::run(EVRUN_NOWAIT);
        qb::io::async::coro_scheduler().run_ready();
        std::this_thread::yield();
    }
    return predicate();
}

qb::io::async::task<void>
record_tls_cancel(CancelRetryDatabase *db, bool *delivered, bool *done, std::shared_ptr<std::coroutine_handle<>> slot) {
    qb::io::async::detail::worker_lease::slot_guard guard(std::move(slot));
    *delivered = co_await db->cancel_async();
    *done      = true;
}

void
check_tls_cancel_write(bool renegotiate) {
    ASSERT_TRUE(qb::io::test::require_ssl_files());
    qb::io::async::listener::current.clear();
    CancelTlsRetryProbe probe;
    probe.request_renegotiation = renegotiate;
    struct ProbeScope {
        explicit ProbeScope(CancelTlsRetryProbe &value) {
            CancelRetrySocket::probe = &value;
        }
        ~ProbeScope() {
            CancelRetrySocket::probe = nullptr;
        }
    } scope(probe);
    CancelTlsPeer peer;
    ASSERT_TRUE(peer.start());
    CancelRetryDatabase db;
    auto                opts = connection_options::parse("tcp://test:test@127.0.0.1:" + std::to_string(peer.port()) + "[test]");
    opts.ssl_verify          = ssl_verify_mode::none;
    ASSERT_TRUE(qb::io::async::run_sync(db.connect(opts)));
    ASSERT_TRUE(pump_cancel_until([&] { return db.backend_pid() == static_cast<int>(kBackendPid); }));

    bool                                done      = false;
    bool                                delivered = false;
    qb::io::async::detail::worker_lease cancellation;
    cancellation.start(record_tls_cancel(&db, &delivered, &done, cancellation.slot()));
    ASSERT_TRUE(pump_cancel_until([&] { return probe.write_calls != 0 || done; }));
    if (renegotiate) {
        ASSERT_TRUE(probe.initial_handshake_complete);
        ASSERT_TRUE(probe.renegotiation_armed) << "native TLS 1.2 fixture failed to arm renegotiation";
        ASSERT_TRUE(probe.native_write_wanted_read) << "fixture did not obtain SSL_write WANT_READ; no readiness verdict is valid";
        ASSERT_TRUE(probe.blocked_socket_writable) << "fixture lacks the opposing writable-readiness control";
        ASSERT_TRUE(probe.blocked_socket_not_readable) << "peer input was not actually withheld at the first blocked write";
        const auto blocked_calls = probe.write_calls;
        SCOPED_TRACE("native WANT_READ calls=" + std::to_string(probe.want_read_calls) + "; blocked write calls="
                     + std::to_string(blocked_calls) + "; TCP writable=" + std::to_string(probe.blocked_socket_writable)
                     + "; TCP unreadable=" + std::to_string(probe.blocked_socket_not_readable));
        for (int turn = 0; turn < 8; ++turn) {
            qb::io::async::run(EVRUN_NOWAIT);
            qb::io::async::coro_scheduler().run_ready();
        }
        EXPECT_EQ(probe.write_calls, blocked_calls) << "cancel write retried without any peer read progress";
        EXPECT_FALSE(done) << "cancel ended while its real TLS write still requires the withheld peer response";
    }
    peer.release_response();
    ASSERT_TRUE(pump_cancel_until([&] { return done; }));
    SCOPED_TRACE("cancel writes=" + std::to_string(probe.write_calls) + "; successful=" + std::to_string(probe.successful_write_calls)
                 + "; WANT_READ=" + std::to_string(probe.want_read_calls) + "; WANT_WRITE=" + std::to_string(probe.want_write_calls)
                 + "; failed=" + std::to_string(probe.failed_write_calls) + "; errno=" + std::to_string(probe.last_write_errno)
                 + "; OpenSSL=" + tls_error_text(probe.last_write_ssl_error));
    EXPECT_TRUE(delivered);
    EXPECT_EQ(probe.accepted_bytes, 16u);
    if (renegotiate)
        EXPECT_EQ(probe.completed_renegotiations, 1) << "the withheld native read demand must resolve through the peer handshake";
    if (!renegotiate) {
        EXPECT_EQ(probe.write_calls, 1u);
        EXPECT_EQ(probe.want_read_calls, 0u);
    }
    peer.finish();
    SCOPED_TRACE("peer cancel reads=" + std::to_string(peer.cancel_reads.read_calls) + "; successful="
                 + std::to_string(peer.cancel_reads.successful_read_calls) + "; WANT_READ=" + std::to_string(peer.cancel_reads.want_read_calls)
                 + "; WANT_WRITE=" + std::to_string(peer.cancel_reads.want_write_calls) + "; failed="
                 + std::to_string(peer.cancel_reads.failed_read_calls) + "; bytes=" + std::to_string(peer.cancel_reads.accepted_bytes)
                 + "; wait failed=" + std::to_string(peer.cancel_reads.readiness_wait_failed) + "; errno="
                 + std::to_string(peer.cancel_reads.last_read_errno) + "; OpenSSL=" + tls_error_text(peer.cancel_reads.last_read_ssl_error));
    ASSERT_TRUE(peer.received);
    expect_cancel_request(peer.bytes);
    db.disconnect();
    qb::io::async::listener::current.clear();
}

} // namespace cancel_request_wire_test

TEST(PgsqlCancelWire, TlsCancelWriteWaitsForNativeReadDemand) {
    check_tls_cancel_write(true);
}

TEST(PgsqlCancelWire, TlsCancelWriteWithoutReadDemandDeliversExactlyOnce) {
    check_tls_cancel_write(false);
}
#endif // QB_HAS_SSL
