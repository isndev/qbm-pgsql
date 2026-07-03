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
 * This runs entirely in-process: a tiny fake PostgreSQL backend on a background thread (raw POSIX
 * sockets, ephemeral loopback port) speaks just enough of the wire protocol —
 *   Startup  ->  AuthenticationSASL(10, "SCRAM-SHA-256")
 *            <-  SASLInitialResponse (client-first: n,,n=user,r=<clientNonce>)
 *   AuthenticationSASLContinue(11, r=<clientNonce+serverNonce>,s=<salt>,i=4096)
 *            <-  SASLResponse       (client-final with client proof)
 *   AuthenticationOk(0)   <-- the malicious step: SASLFinal(12) is SKIPPED
 * — and the test asserts `connect()` returns false. No live daemon; POSIX sockets (macOS/Linux).
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <qb/io/async.h>

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

bool
recv_exact(int fd, uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r <= 0)
            return false; // EOF, timeout (SO_RCVTIMEO), or error
        got += static_cast<size_t>(r);
    }
    return true;
}

bool
send_all(int fd, const std::vector<uint8_t> &m) {
    size_t sent = 0;
    while (sent < m.size()) {
        ssize_t r = ::send(fd, m.data() + sent, m.size() - sent, 0);
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

void
set_rcv_timeout(int fd, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Read one frontend message that has a type byte ('p' for SASL responses): returns its body.
bool
read_typed(int fd, char expected_type, std::vector<uint8_t> &body_out) {
    uint8_t hdr[5];
    if (!recv_exact(fd, hdr, 5))
        return false;
    if (static_cast<char>(hdr[0]) != expected_type)
        return false;
    const uint32_t mlen = get_i32(hdr + 1);
    if (mlen < 4 || mlen > 65536)
        return false;
    body_out.assign(mlen - 4, 0);
    return recv_exact(fd, body_out.data(), body_out.size());
}

struct FakeResult {
    std::atomic<bool> got_startup{false};
    std::atomic<bool> got_client_first{false};
    std::atomic<bool> got_client_final{false};
    std::atomic<bool> sent_ok{false};
};

// The malicious/broken backend. Accepts one connection and runs the truncated SCRAM dance above.
void
run_fake_server(int listen_fd, FakeResult *res) {
    int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0)
        return;
    set_rcv_timeout(fd, 5); // never let a stuck client hang the join()

    // 1. Startup message: [int32 length][body]. Read length, then body; discard.
    uint8_t lenbuf[4];
    if (!recv_exact(fd, lenbuf, 4)) {
        ::close(fd);
        return;
    }
    const uint32_t slen = get_i32(lenbuf);
    if (slen < 8 || slen > 65536) {
        ::close(fd);
        return;
    }
    std::vector<uint8_t> sbody(slen - 4);
    if (!recv_exact(fd, sbody.data(), sbody.size())) {
        ::close(fd);
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
        if (!send_all(fd, backend_msg('R', p))) {
            ::close(fd);
            return;
        }
    }

    // 3. SASLInitialResponse ('p'): mechname\0 + int32(len) + client-first "n,,n=user,r=<nonce>".
    //    Extract the client nonce so the server-first can faithfully extend it (the client rejects a
    //    server nonce that does not begin with the exact bytes it sent).
    std::string client_nonce;
    {
        std::vector<uint8_t> body;
        if (!read_typed(fd, 'p', body)) {
            ::close(fd);
            return;
        }
        const std::string s(reinterpret_cast<char *>(body.data()), body.size());
        const auto        rpos = s.find("r="); // first lowercase r= is the client nonce (SCRAM-SHA-256/n=user have none)
        if (rpos == std::string::npos) {
            ::close(fd);
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
            ::close(fd);
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
        if (!send_all(fd, backend_msg('R', p))) {
            ::close(fd);
            return;
        }
    }

    // 5. SASLResponse ('p'): the client-final message with its proof. Read and discard — a real server
    //    would verify it, but the whole point is that THIS server does not, and skips SASLFinal.
    {
        std::vector<uint8_t> body;
        if (!read_typed(fd, 'p', body)) {
            ::close(fd);
            return;
        }
        res->got_client_final = true;
    }

    // 6. AuthenticationOk(0) WITHOUT AuthenticationSASLFinal(12). The bypass attempt.
    {
        std::vector<uint8_t> p;
        put_i32(p, 0);
        send_all(fd, backend_msg('R', p));
        res->sent_ok = true;
    }

    // 7. Block until the client (which must refuse) tears the connection down, bounded by SO_RCVTIMEO.
    uint8_t drain[64];
    (void) ::recv(fd, drain, sizeof(drain), 0);
    ::close(fd);
}

} // namespace

TEST(PgsqlScramMitm, ClientRefusesAuthenticationOkWithoutVerifiedServerSignature) {
    // Ephemeral loopback listener (OS-assigned port -> no clashes, no hard-coded port).
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::listen(listen_fd, 1), 0);
    socklen_t alen = sizeof(addr);
    ASSERT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr *>(&addr), &alen), 0);
    const uint16_t port = ntohs(addr.sin_port);
    set_rcv_timeout(listen_fd, 10); // bound accept() so a failed client connect can't hang the thread

    FakeResult  res;
    std::thread server(run_fake_server, listen_fd, &res);

    const std::string dsn = "tcp://test:test@127.0.0.1:" + std::to_string(port) + "[test]";
    bool              connected = true; // must be flipped to false by the gate
    {
        qb::pg::tcp::database db{};
        connected = static_cast<bool>(qb::io::async::run_sync(db.connect(dsn)));
        db.disconnect();
    }

    server.join();
    ::close(listen_fd);

    // The server must have driven the handshake all the way to the bogus AuthenticationOk — otherwise
    // the refusal below would be vacuous (e.g. a TCP-level failure would also yield connected==false).
    EXPECT_TRUE(res.got_startup.load()) << "client never sent the startup message";
    EXPECT_TRUE(res.got_client_first.load()) << "client never sent the SCRAM client-first";
    EXPECT_TRUE(res.got_client_final.load()) << "client never sent the SCRAM client-final proof";
    EXPECT_TRUE(res.sent_ok.load()) << "fake server never reached the AuthenticationOk step";

    // THE GUARD: AuthenticationOk arrived without a verified SASLFinal -> the P1 gate must refuse.
    EXPECT_FALSE(connected) << "client accepted AuthenticationOk without a verified SCRAM server signature (P1 mutual-auth gate regression)";
}
