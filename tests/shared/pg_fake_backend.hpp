/**
 * @file qbm/pgsql/tests/shared/pg_fake_backend.hpp
 * @brief The wire-level helpers a fake PostgreSQL backend needs on a test thread: big-endian int32,
 *        bounded exact recv, exact send, backend-message framing, typed frontend-message reads.
 *
 * Shared by the daemon-less system tests that ARE the server for one connection — the SCRAM
 * mutual-auth refusal (`system/auth/scram-mitm-refuse.cpp`) and the out-of-band CancelRequest
 * (`system/connection/cancel-request-wire.cpp`, Huly QB-113) — so the two cannot drift apart on
 * how a message is framed or how a stuck peer is bounded. Everything is qb's own cross-platform
 * `qb::io::tcp` sockets; the read timeout is `handle_read_ready()`, qb's portable select() wrapper,
 * rather than the POSIX-only SO_RCVTIMEO, so this works on Windows too.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License").
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <qb/io/tcp/socket.h>
#include <qb/system/time.h>

namespace qb::pg::test::fake {

/// Append a big-endian int32.
inline void
put_i32(std::vector<std::uint8_t> &b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

/// Read a big-endian int32.
[[nodiscard]] inline std::uint32_t
get_i32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8)
           | static_cast<std::uint32_t>(p[3]);
}

/**
 * @brief Bounded exact recv over a (blocking) qb socket: every read is gated on `handle_read_ready()`
 *        so a stuck peer can never hang the server thread.
 * @return true when exactly `n` bytes landed; false on the deadline, EOF or an error.
 */
[[nodiscard]] inline bool
recv_exact(qb::io::tcp::socket &s, std::uint8_t *buf, std::size_t n, qb::duration timeout = std::chrono::seconds(5)) {
    const auto  deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t got      = 0;
    while (got < n) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        if (qb::io::socket::handle_read_ready(s.native_handle(), std::chrono::duration_cast<qb::duration>(deadline - now)) <= 0)
            return false; // timeout or error
        const int r = s.read(buf + got, n - got);
        if (r <= 0)
            return false; // EOF or error
        got += static_cast<std::size_t>(r);
    }
    return true;
}

/// True when the peer sends NOTHING within `timeout` (EOF or silence) -- the negative of recv_exact.
[[nodiscard]] inline bool
peer_stays_silent(qb::io::tcp::socket &s, qb::duration timeout) {
    std::uint8_t one = 0;
    return !recv_exact(s, &one, 1, timeout);
}

/// Send every byte of `m` (blocking socket).
[[nodiscard]] inline bool
send_all(qb::io::tcp::socket &s, const std::vector<std::uint8_t> &m) {
    std::size_t sent = 0;
    while (sent < m.size()) {
        const int r = s.write(m.data() + sent, m.size() - sent);
        if (r <= 0)
            return false;
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

/// A backend message: [type][int32 length incl. the length field][payload].
[[nodiscard]] inline std::vector<std::uint8_t>
backend_msg(char type, const std::vector<std::uint8_t> &payload) {
    std::vector<std::uint8_t> m;
    m.push_back(static_cast<std::uint8_t>(type));
    put_i32(m, static_cast<std::uint32_t>(4 + payload.size()));
    m.insert(m.end(), payload.begin(), payload.end());
    return m;
}

/// Read one frontend message that has a type byte ('p' for SASL responses, 'Q' for a query): its body.
[[nodiscard]] inline bool
read_typed(qb::io::tcp::socket &s, char expected_type, std::vector<std::uint8_t> &body_out) {
    std::uint8_t hdr[5];
    if (!recv_exact(s, hdr, 5))
        return false;
    if (static_cast<char>(hdr[0]) != expected_type)
        return false;
    const std::uint32_t mlen = get_i32(hdr + 1);
    if (mlen < 4 || mlen > 65536)
        return false;
    body_out.assign(mlen - 4, 0);
    return recv_exact(s, body_out.data(), body_out.size());
}

/**
 * @brief Read the untyped startup packet ([int32 length][body]) the client opens with.
 * @return true and the body on success; false on a malformed length, the deadline or EOF.
 */
[[nodiscard]] inline bool
read_startup(qb::io::tcp::socket &s, std::vector<std::uint8_t> &body_out) {
    std::uint8_t lenbuf[4];
    if (!recv_exact(s, lenbuf, 4))
        return false;
    const std::uint32_t slen = get_i32(lenbuf);
    if (slen < 8 || slen > 65536)
        return false;
    body_out.assign(slen - 4, 0);
    return recv_exact(s, body_out.data(), body_out.size());
}

} // namespace qb::pg::test::fake
