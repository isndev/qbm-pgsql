/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2026 isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use it except in compliance with the License.
 * See the License for the specific terms.
 */

/**
 * @file pg_fake_tls.h
 * @brief Both ends of PostgreSQL's TLS negotiation for fake-backend tests: the SSLRequest packet, the
 *        server's one-byte `'S'`, then a TLS handshake on the same descriptor -- blocking and bounded,
 *        on non-blocking sockets driven by readiness waits.
 *
 * The backend side accepts and answers the negotiation; the client side runs it on a connected socket,
 * as a program must before it hands a transport to `tcp::ssl::database::connect(dsn, raw_io)` (Huly
 * QB-1096). Only compiled with OpenSSL (QB_HAS_SSL); certificates come from qb's ssl_fixtures.h.
 */

#ifndef QBM_PGSQL_TESTS_SHARED_PG_FAKE_TLS_H
#define QBM_PGSQL_TESTS_SHARED_PG_FAKE_TLS_H

#ifdef QB_HAS_SSL

#include <chrono>
#include <cstdint>
#include <vector>

#include <openssl/ssl.h>
#include <qb/io/tcp/listener.h>
#include <qb/io/tcp/socket.h>
#include <qb/io/tcp/ssl/socket.h>

#include "pg_fake_backend.h"

namespace qb::pg::test::fake {

/// Wait for the direction the TLS session wants next, until `deadline`.
[[nodiscard]] inline bool
wait_tls_io(qb::io::tcp::ssl::socket &socket, std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
        return false;
    const auto remaining = std::chrono::duration_cast<qb::duration>(deadline - now);
    return (SSL_want_write(socket.ssl_handle()) ? qb::io::socket::handle_write_ready(socket.native_handle(), remaining)
                                                : qb::io::socket::handle_read_ready(socket.native_handle(), remaining))
           > 0;
}

/// Drive a TLS handshake (either role, set on the SSL object) to completion within 5 s.
[[nodiscard]] inline bool
finish_tls_handshake(qb::io::tcp::ssl::socket &socket) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const int step = socket.handshake_status();
        if (step != 0)
            return step == 1;
        if (!wait_tls_io(socket, deadline))
            return false;
    }
}

/// Read exactly `size` decrypted bytes within 5 s.
[[nodiscard]] inline bool
tls_recv_exact(qb::io::tcp::ssl::socket &socket, std::uint8_t *bytes, std::size_t size) {
    const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::size_t got      = 0;
    while (got < size) {
        const int n = socket.read(bytes + got, size - got);
        if (n < 0)
            return false;
        if (n > 0)
            got += static_cast<std::size_t>(n);
        else if (!wait_tls_io(socket, deadline))
            return false;
    }
    return true;
}

/// Write every byte within 5 s.
[[nodiscard]] inline bool
tls_send_all(qb::io::tcp::ssl::socket &socket, const std::vector<std::uint8_t> &bytes) {
    const auto  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
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

/// Backend side: accept one connection, read its SSLRequest, answer `'S'`, run the server handshake.
[[nodiscard]] inline bool
accept_postgres_tls(qb::io::tcp::listener &listener, qb::io::ssl::Context &tls, qb::io::tcp::ssl::socket &out) {
    if (qb::io::socket::handle_read_ready(listener.native_handle(), std::chrono::seconds(5)) <= 0)
        return false;
    qb::io::tcp::socket plain;
    if (listener.accept(plain) != qb::io::SocketStatus::Done || plain.set_nonblocking(true) != 0)
        return false;
    std::uint8_t request[8];
    if (!recv_exact(plain, request, sizeof request) || get_i32(request) != 8u || get_i32(request + 4) != 80877103u
        || qb::io::socket::handle_write_ready(plain.native_handle(), std::chrono::seconds(5)) <= 0
        || !send_all(plain, std::vector<std::uint8_t>{'S'}))
        return false;
    SSL *ssl = SSL_new(tls.native());
    if (!ssl)
        return false;
    SSL_set_accept_state(ssl);
    out = qb::io::tcp::ssl::socket{ssl, plain};
    return out.set_nonblocking(true) == 0 && finish_tls_handshake(out);
}

/// Client side, on a connected socket: send the SSLRequest, require `'S'`, run the client handshake --
/// what a program does before it hands a transport to `tcp::ssl::database::connect(dsn, raw_io)`.
[[nodiscard]] inline bool
negotiate_postgres_tls(qb::io::tcp::socket &plain, qb::io::ssl::Context &tls, qb::io::tcp::ssl::socket &out) {
    std::vector<std::uint8_t> request;
    put_i32(request, 8u);
    put_i32(request, 80877103u);
    std::uint8_t verdict = 0;
    if (plain.set_nonblocking(true) != 0 || qb::io::socket::handle_write_ready(plain.native_handle(), std::chrono::seconds(5)) <= 0
        || !send_all(plain, request) || !recv_exact(plain, &verdict, 1) || verdict != 'S')
        return false;
    SSL *ssl = SSL_new(tls.native());
    if (!ssl)
        return false;
    SSL_set_connect_state(ssl);
    out = qb::io::tcp::ssl::socket{ssl, plain};
    return out.set_nonblocking(true) == 0 && finish_tls_handshake(out);
}

} // namespace qb::pg::test::fake

#endif // QB_HAS_SSL

#endif // QBM_PGSQL_TESTS_SHARED_PG_FAKE_TLS_H
