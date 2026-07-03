/**
 * @file scram-mitm-refuse.cpp
 * @brief System test: the SCRAM mutual-auth gate refuses a server that never proves the password.
 *
 * Regression guard for the P1 fix (pgsql.h `on_authentication`, case OK): an impersonating server /
 * active MITM that carries the SCRAM exchange far enough to elicit the client's proof but then sends
 * `AuthenticationOk` WITHOUT a verified `AuthenticationSASLFinal` (SCRAM ServerSignature) must be
 * refused — the client's own proof leaks nothing, so without this gate the client would trust a peer
 * that never demonstrated knowledge of the password.
 *
 * This runs entirely in-process: a tiny fake PostgreSQL backend on a background thread (qb's own
 * cross-platform `qb::io::tcp` sockets, ephemeral loopback port) speaks just enough of the wire
 * protocol —
 *   Startup  ->  AuthenticationSASL(10, "SCRAM-SHA-256")
 *            <-  SASLInitialResponse (client-first: n,,n=user,r=<clientNonce>)
 *   AuthenticationSASLContinue(11, r=<clientNonce+serverNonce>,s=<salt>,i=4096)
 *            <-  SASLResponse       (client-final with client proof)
 *   AuthenticationOk(0)   <-- the malicious step: SASLFinal(12) is SKIPPED
 * — and the test asserts `connect()` returns false. No live daemon; portable (qb::io::tcp — the
 * timeout guard uses handle_read_ready() rather than the POSIX-only SO_RCVTIMEO/timeval).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>

#include "../pgsql.h"

using namespace qb::pg;

namespace {

// --- little-helpers: big-endian int32 + exact send/recv --------------------------------------

void
put_i32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}

uint32_t
get_i32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8)
           | static_cast<uint32_t>(p[3]);
}

using namespace std::chrono_literals;

// Bounded exact recv over a (blocking) qb socket: gate every read on handle_read_ready() so a
// stuck peer can never hang the server thread. Cross-platform replacement for the SO_RCVTIMEO the
// original set — handle_read_ready() is qb's portable select() wrapper (works on Windows too).
bool
recv_exact(qb::io::tcp::socket &s, uint8_t *buf, size_t n, qb::duration timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    size_t     got      = 0;
    while (got < n) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        if (qb::io::socket::handle_read_ready(s.native_handle(), std::chrono::duration_cast<qb::duration>(deadline - now)) <= 0)
            return false; // timeout or error
        const int r = s.read(buf + got, n - got);
        if (r <= 0)
            return false; // EOF or error
        got += static_cast<size_t>(r);
    }
    return true;
}

bool
send_all(qb::io::tcp::socket &s, const std::vector<uint8_t> &m) {
    size_t sent = 0;
    while (sent < m.size()) {
        const int r = s.write(m.data() + sent, m.size() - sent);
        if (r <= 0)
            return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

// A backend message: [type][int32 length incl. length field][payload].
std::vector<uint8_t>
backend_msg(char type, const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> m;
    m.push_back(static_cast<uint8_t>(type));
    put_i32(m, static_cast<uint32_t>(4 + payload.size()));
    m.insert(m.end(), payload.begin(), payload.end());
    return m;
}

// Read one frontend message that has a type byte ('p' for SASL responses): returns its body.
bool
read_typed(qb::io::tcp::socket &s, char expected_type, std::vector<uint8_t> &body_out) {
    uint8_t hdr[5];
    if (!recv_exact(s, hdr, 5))
        return false;
    if (static_cast<char>(hdr[0]) != expected_type)
        return false;
    const uint32_t mlen = get_i32(hdr + 1);
    if (mlen < 4 || mlen > 65536)
        return false;
    body_out.assign(mlen - 4, 0);
    return recv_exact(s, body_out.data(), body_out.size());
}

struct FakeResult {
    std::atomic<bool> got_startup{false};
    std::atomic<bool> got_client_first{false};
    std::atomic<bool> got_client_final{false};
    std::atomic<bool> sent_ok{false};
};

// The malicious/broken backend. Accepts one connection and runs the truncated SCRAM dance above.
void
run_fake_server(qb::io::tcp::listener &listener, FakeResult *res) {
    // Bound the accept so a failed client connect can't hang the join(): handle_read_ready() on a
    // listening socket reports a pending connection — the portable stand-in for the old SO_RCVTIMEO.
    if (qb::io::socket::handle_read_ready(listener.native_handle(), std::chrono::seconds(10)) <= 0)
        return;
    qb::io::tcp::socket client;
    if (listener.accept(client) != qb::io::SocketStatus::Done)
        return;

    // 1. Startup message: [int32 length][body]. Read length, then body; discard.
    uint8_t lenbuf[4];
    if (!recv_exact(client, lenbuf, 4)) {
        client.disconnect();
        return;
    }
    const uint32_t slen = get_i32(lenbuf);
    if (slen < 8 || slen > 65536) {
        client.disconnect();
        return;
    }
    std::vector<uint8_t> sbody(slen - 4);
    if (!recv_exact(client, sbody.data(), sbody.size())) {
        client.disconnect();
        return;
    }
    res->got_startup = true;

    // 2. AuthenticationSASL(10): int32(10) + "SCRAM-SHA-256\0" + "\0" (end of mechanism list).
    {
        std::vector<uint8_t> p;
        put_i32(p, 10);
        static const char kMech[] = "SCRAM-SHA-256";
        p.insert(p.end(), kMech, kMech + sizeof(kMech)); // includes the trailing NUL
        p.push_back(0);                                  // empty string terminates the list
        if (!send_all(client, backend_msg('R', p))) {
            client.disconnect();
            return;
        }
    }

    // 3. SASLInitialResponse ('p'): mechname\0 + int32(len) + client-first "n,,n=user,r=<nonce>".
    //    Extract the client nonce so the server-first can faithfully extend it (the client rejects a
    //    server nonce that does not begin with the exact bytes it sent).
    std::string client_nonce;
    {
        std::vector<uint8_t> body;
        if (!read_typed(client, 'p', body)) {
            client.disconnect();
            return;
        }
        const std::string s(reinterpret_cast<char *>(body.data()), body.size());
        const auto        rpos = s.find("r="); // first lowercase r= is the client nonce (SCRAM-SHA-256/n=user have none)
        if (rpos == std::string::npos) {
            client.disconnect();
            return;
        }
        for (size_t i = rpos + 2; i < s.size(); ++i) {
            const char c = s[i];
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
                client_nonce.push_back(c); // client nonce is hex-lower
            else
                break;
        }
        if (client_nonce.empty()) {
            client.disconnect();
            return;
        }
        res->got_client_first = true;
    }

    // 4. AuthenticationSASLContinue(11): server-first "r=<clientNonce+ext>,s=<base64 salt>,i=4096".
    {
        const std::string server_nonce = client_nonce + "0123456789abcdef"; // extends the client nonce
        const std::string server_first = "r=" + server_nonce + ",s=MDEyMzQ1,i=4096"; // s= is valid base64 ("012345")
        std::vector<uint8_t> p;
        put_i32(p, 11);
        p.insert(p.end(), server_first.begin(), server_first.end());
        if (!send_all(client, backend_msg('R', p))) {
            client.disconnect();
            return;
        }
    }

    // 5. SASLResponse ('p'): the client-final message with its proof. Read and discard — a real server
    //    would verify it, but the whole point is that THIS server does not, and skips SASLFinal.
    {
        std::vector<uint8_t> body;
        if (!read_typed(client, 'p', body)) {
            client.disconnect();
            return;
        }
        res->got_client_final = true;
    }

    // 6. AuthenticationOk(0) WITHOUT AuthenticationSASLFinal(12). The bypass attempt.
    {
        std::vector<uint8_t> p;
        put_i32(p, 0);
        send_all(client, backend_msg('R', p));
        res->sent_ok = true;
    }

    // 7. Block until the client (which must refuse) tears the connection down, bounded by the
    //    handle_read_ready() timeout so a misbehaving client can never hang the join().
    if (qb::io::socket::handle_read_ready(client.native_handle(), std::chrono::seconds(5)) > 0) {
        uint8_t drain[64];
        (void) client.read(drain, sizeof(drain));
    }
    client.disconnect();
}

} // namespace

TEST(PgsqlScramMitm, ClientRefusesAuthenticationOkWithoutVerifiedServerSignature) {
    // Ephemeral loopback listener (OS-assigned port -> no clashes, no hard-coded port).
    qb::io::tcp::listener listener;
    ASSERT_EQ(listener.listen_v4(0, "127.0.0.1"), qb::io::SocketStatus::Done);
    const uint16_t port = listener.local_endpoint().port();
    ASSERT_NE(port, 0);

    FakeResult  res;
    std::thread server(run_fake_server, std::ref(listener), &res);

    const std::string dsn = "tcp://test:test@127.0.0.1:" + std::to_string(port) + "[test]";
    bool              connected = true; // must be flipped to false by the gate
    {
        qb::pg::tcp::database db{};
        connected = static_cast<bool>(qb::io::async::run_sync(db.connect(dsn)));
        db.disconnect();
    }

    server.join();
    listener.disconnect();

    // The server must have driven the handshake all the way to the bogus AuthenticationOk — otherwise
    // the refusal below would be vacuous (e.g. a TCP-level failure would also yield connected==false).
    EXPECT_TRUE(res.got_startup.load()) << "client never sent the startup message";
    EXPECT_TRUE(res.got_client_first.load()) << "client never sent the SCRAM client-first";
    EXPECT_TRUE(res.got_client_final.load()) << "client never sent the SCRAM client-final proof";
    EXPECT_TRUE(res.sent_ok.load()) << "fake server never reached the AuthenticationOk step";

    // THE GUARD: AuthenticationOk arrived without a verified SASLFinal -> the P1 gate must refuse.
    EXPECT_FALSE(connected) << "client accepted AuthenticationOk without a verified SCRAM server signature (P1 mutual-auth gate regression)";
}
