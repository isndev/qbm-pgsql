/**
 * @file pgsql.h
 * @brief PostgreSQL client for the QB Actor Framework
 *
 * This file implements an asynchronous PostgreSQL client integrated with the QB Actor
 * Framework. It provides a non-blocking interface for database operations such as:
 *
 * - Connection management to PostgreSQL databases
 * - Transaction management (begin, commit, rollback)
 * - Support for savepoints within transactions
 * - Simple and prepared statement execution with parameter binding
 * - Efficient query result retrieval and processing
 * - Support for multiple authentication methods (MD5, SCRAM-SHA-256, etc.)
 *
 * The implementation is designed to work with the actor model, allowing
 * database operations to be performed without blocking actor threads. The client
 * fully implements the PostgreSQL wire protocol for efficient communication.
 *
 * Connection / query API (single-threaded qb-io: one event loop + coroutine scheduler
 * per thread; never block the loop inside a callback or coroutine except via
 * explicit suspension).
 *
 * Two orthogonal styles — same method names, different completion model:
 *
 * - **Coroutine completion:** overloads **without** user callbacks return
 *   `pg_reply_awaiter<T>`. Use **only** inside a coroutine: `auto r = co_await db.query("…");`
 *   or `co_await db.execute("…")`, `co_await db.prepare(…)`, `co_await db.begin()`, etc.
 *   From synchronous code, drive the coroutine with `qb::io::async::run_sync` (see
 *   `qb/io/async/coroutine/utils.h`) or spawn a `task` on `coro_scheduler()`. **Do not** call a
 *   blocking wait on the awaiter itself — there is no
 *   `.await()` on `pg_reply_awaiter`.
 *
 * - **Callback + synchronous drain:** pass success/error callbacks; overloads return
 *   `Transaction&` for fluent chaining. To run queued work to completion on the current
 *   thread, call **`Transaction::await()`** (or `qb::pg::await(db)`). For SQL/prepared
 *   ops when you have no real handler, use the constexpr discards:
 *   `execute(sql, discard_query, discard_error)`,
 *   `prepare(name, sql, types, discard_prepare, discard_error)`,
 *   `execute(name, params, discard_query, discard_error)`.
 *
 * - **Connection:** `co_await db.connect()` or `run_sync(db.connect())` — see
 *   `qb/io/async/coroutine/utils.h`.
 *
 * - **Coroutine transaction scope:** `co_await with_transaction(db, [](Transaction &tr) -> task<int>
 * { ... })` runs `BEGIN`, awaits your `task` body (use `tr` / `db` for `co_await tr.execute` /
 * `query`), then `COMMIT` on success or `ROLLBACK` on `transaction_abort`, `commit` failure, or a
 * C++ exception. Overload `with_transaction(db, transaction_mode{...}, f)` sets isolation /
 * read-only / deferrable. When `!reply.ok()` after an operation, throw
 * `transaction_abort{reply.error()}` so the helper rolls back and returns `Reply::failure` instead
 * of calling `COMMIT` on an aborted transaction.
 *
 * **Large-project conventions**
 *
 * - **One style per call stack:** In a `begin` success callback, use only callback overloads
 *   (`execute(..., cb, err)` or discards) and `Transaction::await()` — do not mix with discarded
 *   `execute("…")` coroutine awaiters (they are not driven there). In coroutines, use only
 *   `co_await` overloads and `with_transaction` / manual `begin` / `commit` / `rollback`.
 * - **`Transaction&` vs `database&`:** `tcp::database` *is-a* `Transaction`; `with_transaction` and
 *   `co_await tr.query` use the same connection. Prefer passing `Transaction&` in helpers so code
 * works with any concrete client type.
 * - **Avoid nesting `with_transaction`:** It issues a second `BEGIN` on the same connection;
 * behavior depends on the server (some configurations reject it and abort the block — use
 *   `transaction_abort{inner.error()}` and never `COMMIT` an aborted transaction; others may accept
 *   the pattern). Prefer a single scope plus `savepoint` / `release_savepoint` /
 * `rollback_savepoint` for nested units of work.
 * - **READ ONLY:** PostgreSQL still allows writes to **temporary** tables in a read-only
 *   transaction; only non-temporary relations are restricted.
 * - **Errors:** For coroutine bodies, treat `Reply::ok()` as mandatory; on failure either throw
 *   `transaction_abort` (handled scope) or let exceptions propagate (rollback + rethrow).
 *
 * Key features:
 * - Asynchronous I/O using the QB Actor Framework
 * - Support for both plain TCP and SSL/TLS connections
 * - Comprehensive transaction management
 * - Prepared statement caching for performance
 * - Detailed error reporting and handling
 *
 * @see qb::pg::detail::Database
 * @see qb::pg::detail::Transaction
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
 */
#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <qb/io/async.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/io/crypto.h>
#ifdef QB_HAS_SSL
#include <qb/io/tcp/ssl/socket.h>
#endif
#include <qb/system/allocator/pipe.h>
#include <qb/system/cpu.h>    // qb::scope_guard
#include <qb/system/parse.h>  // qb::to_number (locale-free, non-throwing)

// P1-1: Socket includes for keepalive support
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include "./src/commands.h"
#include "./src/pg_reply.h"
#include "./src/transaction.h"

/**
 * @brief Maximum length for attribute names in PostgreSQL protocol
 *
 * Defines the maximum length in bytes for attribute names when parsing
 * PostgreSQL protocol messages. This limit helps prevent buffer overflow
 * attacks and ensures efficient memory usage.
 */
constexpr const uint32_t ATTRIBUTE_NAME_MAX = 1024; // 1 KB

/**
 * @brief Maximum length for attribute values in PostgreSQL protocol
 *
 * Defines the maximum length in bytes for attribute values when parsing
 * PostgreSQL protocol messages. This larger limit accommodates typical
 * PostgreSQL data values while preventing excessively large allocations.
 */
constexpr const uint32_t ATTRIBUTE_VALUE_MAX = 1024 * 1024; // 1 MB

/**
 * @brief Checks if a character is a control character
 *
 * Used during attribute parsing to validate input and ensure security.
 * Control characters are generally not allowed in attribute names or values
 * as they may indicate malformed or malicious input.
 *
 * @param c Character to check
 * @return true if the character is a control character (ASCII 0-31 or 127)
 */
inline bool
is_control(int c) {
    return ((c >= 0 && c <= 31) || c == 127);
}

/**
 * @brief Parses header attributes from a PostgreSQL protocol message
 *
 * Parses a buffer of header attributes into a case-insensitive map.
 * This function is primarily used during SCRAM authentication to process
 * challenge-response data between client and server.
 *
 * Features:
 * - Supports both quoted and unquoted attribute values
 * - Handles attribute separators (comma and semicolon)
 * - Enforces size limits to prevent buffer overflows
 * - Validates input to reject control characters
 * - Properly handles whitespace according to the PostgreSQL protocol
 *
 * @param ptr Pointer to the buffer containing attributes
 * @param len Length of the buffer
 * @return Case-insensitive map of attribute names to values
 * @throws std::runtime_error If parsing fails due to control characters or exceeding
 * size limits
 */
qb::icase_unordered_map<std::string> parse_header_attributes(const char *ptr, const size_t len);

namespace qb::protocol {

/**
 * @brief PostgreSQL protocol implementation for the QB actor framework
 *
 * Handles the message framing and parsing according to the PostgreSQL
 * wire protocol specification. This class is responsible for:
 *
 * - Extracting complete messages from the input stream
 * - Managing protocol state between messages
 * - Forwarding complete messages to the appropriate handlers
 * - Implementing the PostgreSQL message format requirements
 *
 * The protocol handler processes the incoming byte stream and constructs
 * well-formed PostgreSQL protocol messages. It maintains internal state
 * to handle partial messages that arrive in multiple network packets.
 *
 * @tparam IO_ I/O handler type that provides input/output stream access
 */
template <typename IO_>
class pgsql final : public qb::io::async::AProtocol<IO_> {
public:
    /**
     * @brief PostgreSQL protocol message type
     *
     * Represents a complete PostgreSQL protocol message including
     * message type, length, and payload data.
     */
    using message = std::unique_ptr<pg::detail::message>;

private:
    message     message_;    ///< Current message being processed
    std::size_t offset_ = 0; ///< Current offset in the input buffer

public:
    pgsql() = delete;

    /**
     * @brief Constructs a PostgreSQL protocol handler
     *
     * Initializes the protocol handler with a reference to the I/O
     * subsystem that provides access to input and output streams.
     *
     * @param io Reference to the I/O handler
     */
    explicit pgsql(IO_ &io) noexcept
        : qb::io::async::AProtocol<IO_>(io) {}

    /**
     * @brief Copy data from input iterator to output iterator
     *
     * Helper method to copy data between iterators with a maximum limit.
     * Used internally for buffer management and message construction.
     *
     * @tparam InputIter Input iterator type
     * @tparam OutputIter Output iterator type
     * @param in Start of input range
     * @param end End of input range
     * @param max Maximum number of items to copy
     * @param out Output iterator
     * @return InputIter Iterator after the last copied element
     */
    template <typename InputIter, typename OutputIter>
    InputIter
    copy(InputIter in, InputIter end, size_t max, OutputIter out) {
        for (size_t i = 0; i < max && in != end; ++i) {
            *out++ = *in++;
        }
        return in;
    }

    /**
     * @brief Calculate the size of a complete PostgreSQL message
     *
     * Inspects the input buffer to determine if a complete message is available.
     * This method implements the PostgreSQL message framing protocol by:
     *
     * 1. Reading the message type byte and length field
     * 2. Creating a new message object if needed
     * 3. Reading message payload data up to the expected length
     * 4. Determining if the message is complete
     *
     * If a message is complete, returns its size in bytes. If incomplete,
     * returns 0 to indicate more data is needed from the network.
     *
     * @return std::size_t Size of the complete message, or 0 if incomplete
     */
    std::size_t
    getMessageSize() noexcept final {
        constexpr const size_t header_size = sizeof(qb::pg::integer) + sizeof(qb::pg::byte);

        const auto &in = this->_io.in();
        if (in.size() < offset_ + header_size)
            return 0; // read more

        auto max_bytes = in.size() - offset_;

        if (!message_) {
            message_ = std::make_unique<pg::detail::message>();

            // OPTIMIZED: Use std::copy_n for batch copy instead of byte-by-byte
            // This provides ~10x performance improvement for large messages
            auto header_begin = in.begin();
            auto out          = message_->output();
            std::copy_n(header_begin, header_size, out);
            offset_ += header_size;
            max_bytes -= header_size;

            const qb::pg::uinteger wire_len = static_cast<qb::pg::uinteger>(message_->length());
            if (wire_len < 4u || wire_len > qb::pg::PG_PROTOCOL_MAX_MESSAGE_BYTES) {
                LOG_CRIT("[pgsql] Invalid wire message length " << wire_len << " (must be 4.." << qb::pg::PG_PROTOCOL_MAX_MESSAGE_BYTES
                                                                << "); dropping connection");
                message_.reset();
                offset_ = 0;
                // Mark the protocol invalid (the documented contract for a malformed size
                // field): the I/O layer then disposes and fires event::disconnected, whose
                // handler fails every pending query and resumes any pending connect awaiter.
                // The previous prepare_reconnect() tore the transport down synchronously from
                // inside the read handler WITHOUT firing on(disconnected) — so queued query
                // awaiters (and a pending co_await connect(), whose handle it cleared without
                // resuming) hung forever, and it left the read watcher on a closed fd.
                this->not_ok();
                return 0;
            }
        }

        if (message_->length() > message_->size()) {
            // Read the message body
            auto              out     = message_->output();
            const std::size_t to_copy = std::min(message_->length() - message_->size(), max_bytes);

            // OPTIMIZED: Use std::copy_n for batch copy instead of byte-by-byte
            auto data_begin = in.begin() + offset_;
            std::copy_n(data_begin, to_copy, out);

            offset_ += to_copy;
        }

        if (message_->length() == message_->size()) {
            return message_->buffer_size();
        }

        return 0;
    }

    /**
     * @brief Handle a complete PostgreSQL message
     *
     * Called by the protocol framework when a complete message has been
     * received and parsed according to the PostgreSQL protocol rules.
     *
     * This method:
     * 1. Validates the connection state
     * 2. Resets the message read pointer
     * 3. Forwards the complete message to the I/O handler for processing
     * 4. Resets the protocol state to prepare for the next message
     *
     * @param size Size of the message (unused in this implementation)
     */
    void
    onMessage(std::size_t) noexcept final {
        if (!this->ok())
            return;

        message_->reset_read();
        // This is a noexcept boundary: a message handler that throws would call
        // std::terminate. A hostile or misconfigured server can make on_authentication
        // throw before the connection is established (an unsupported auth method falls
        // into its `default:` throw; a malformed SCRAM server message makes
        // parse_header_attributes / the iteration-count parse / PBKDF2 throw). Contain any
        // handler exception and mark the protocol invalid so the I/O layer disposes and
        // fires event::disconnected, whose handler fails pending queries and resumes a
        // pending connect awaiter with an error instead of crashing the process.
        try {
            this->_io.on(std::move(message_));
        } catch (std::exception const &e) {
            LOG_CRIT("[pgsql] exception in message handler, dropping connection: " << e.what());
            this->not_ok();
        } catch (...) {
            LOG_CRIT("[pgsql] unknown exception in message handler, dropping connection");
            this->not_ok();
        }
        reset();
    }

    /**
     * @brief Reset the protocol state
     *
     * Prepares the protocol handler for the next message by resetting
     * internal state variables. This ensures that each new message is
     * processed from a clean initial state.
     */
    void
    reset() noexcept final {
        offset_ = 0;
    }
};

} // namespace qb::protocol

namespace qb::pg {

/**
 * @brief Asynchronous payload from PostgreSQL NOTIFY (after LISTEN on the same or another session).
 *
 * Delivered on the I/O thread when a `NotificationResponse` is received; see
 * `tcp::notify_cb_consumer` / `tcp::notify_co_consumer`, or `database::on_incoming_notify`.
 */
struct notification {
    int         server_backend_pid{};
    std::string channel;
    std::string payload;
};

namespace detail {
using namespace qb::io;
using namespace qb::pg;

/**
 * @brief Validate the SCRAM server nonce against the client nonce (RFC 5802 §5.1).
 *
 * In SCRAM-SHA-256 the server echoes the client's nonce and appends its own, so the
 * combined nonce in the server-first message MUST begin with the exact nonce the
 * client sent AND be strictly longer (the server has to contribute entropy). A
 * server — or a man-in-the-middle — that fails this is not replaying our own first
 * message faithfully, so the exchange must be aborted before deriving any proof.
 *
 * @param client_nonce The nonce this client generated and sent (`r=` in client-first).
 * @param server_nonce The combined nonce returned by the server (`r=` in server-first).
 * @return true iff server_nonce starts with client_nonce and is longer.
 */
[[nodiscard]] inline bool
scram_server_nonce_extends_client(std::string_view client_nonce, std::string_view server_nonce) noexcept {
    return !client_nonce.empty() && server_nonce.size() > client_nonce.size() && server_nonce.starts_with(client_nonce);
}

/// Hard upper bound on the SCRAM-SHA-256 iteration count this client will honour.
/// PostgreSQL's server default is 4096; this is ~244x that. The count is
/// server-controlled and feeds PBKDF2 SYNCHRONOUSLY on the I/O event-loop thread,
/// so an unbounded value (e.g. i=2147483647 => ~minutes of HMAC) would stall every
/// actor on the core — a trivial denial of service from a hostile or MITM'd server.
inline constexpr int kMaxScramIterations = 1'000'000;

/**
 * @brief Parse and bound-check a SCRAM server-first `i=` iteration count.
 *
 * Accepts only a canonical base-10 integer in [1, ::qb::pg::kMaxScramIterations].
 * Rejects a missing/empty, non-numeric, overflowing, non-positive, or absurdly
 * large value — the last being the denial-of-service guard documented on
 * ::qb::pg::kMaxScramIterations.
 *
 * @param text The raw `i=` attribute value from the SCRAM server-first message.
 * @return The validated iteration count.
 * @throws error::connection_error if @p text is malformed or out of range.
 */
[[nodiscard]] inline int
scram_validate_iteration_count(std::string_view text) {
    const auto parsed = qb::to_number<int>(text);
    if (!parsed || *parsed < 1 || *parsed > kMaxScramIterations) {
        throw error::connection_error("SCRAM iteration count missing, malformed, or out of range");
    }
    return *parsed;
}

/**
 * @brief Escape a SCRAM `saslname` (RFC 5802): `=` -> `=3D`, `,` -> `=2C`.
 *
 * The `n=<username>` field in the SCRAM client-first message uses the `saslname`
 * production, where `,` and `=` must be percent-style escaped (and `=3D` must be
 * applied before `=2C` so the inserted `=` is not re-escaped). PostgreSQL ignores
 * the SCRAM `n=` (it authenticates the startup-packet user), but a role name
 * containing a comma/equals would otherwise emit a malformed client-first message
 * that a strict RFC-5802 parser (proxy/pooler/non-PG server) rejects.
 */
[[nodiscard]] std::string scram_escape_saslname(std::string_view name);

/**
 * @brief Opportunistic-TLS (STARTTLS) negotiator for the PostgreSQL protocol.
 *
 * Plugs into `qb::io::async::tcp::starttls_connect`. After the cleartext TCP connect,
 * it sends the 8-byte **SSLRequest** packet (int32 length = 8, int32 request code =
 * 80877103 / `0x04D2162F`, big-endian) and reads the single-byte server reply:
 *   - `'S'` → upgrade: the connector then performs the TLS handshake asynchronously.
 *   - anything else (`'N'`, EOF, error) → fail: a secure database **requires** TLS
 *     (use the plain `tcp::database` for cleartext). This drops the old, broken `'N'`
 *     fallback that produced an unusable handle-less `ssl::socket`.
 *
 * All I/O is non-blocking; the connector drives the readiness events.
 */
struct postgres_ssl_negotiator {
    static constexpr bool enabled = true;

    /**
     * @brief Build the 8-byte PostgreSQL SSLRequest packet (big-endian).
     *
     * Layout: int32 length = 8, int32 request code = 80877103 (`0x04D2162F`).
     *
     * @return The serialized SSLRequest bytes ready to write on the cleartext socket.
     */
    static std::array<std::uint8_t, 8> make_request() noexcept;

    std::array<std::uint8_t, 8> request_{make_request()};
    std::size_t                 written_{0};
    std::uint8_t                verdict_{0};
    bool                        got_verdict_{false};

    /**
     * @brief Drive one step of the STARTTLS negotiation on a ready socket.
     *
     * Non-blocking state machine called by the connector on each readiness event:
     *   1. write the remaining SSLRequest bytes,
     *   2. read the single-byte server verdict,
     *   3. decide: `'S'` -> upgrade to TLS, anything else / EOF / error -> fail
     *      (a secure database requires TLS).
     *
     * @param sock The cleartext socket connected to the server (not yet upgraded).
     * @param revents Readiness flags from the event loop (unused; the phase is tracked
     *        internally).
     * @return The next `starttls_action`: `want_write` / `want_read` to be polled again,
     *         `upgrade` to start the TLS handshake, or `fail` to abort the connect.
     */
    qb::io::async::tcp::starttls_action advance(qb::io::tcp::socket &sock, int revents) noexcept;
};

/**
 * @brief PostgreSQL database client implementation
 *
 * Core implementation of the PostgreSQL client that handles connection
 * establishment, authentication, and query execution. This class provides
 * the foundation for asynchronous database operations with PostgreSQL.
 *
 * Key features:
 * - Asynchronous TCP/IP connection management
 * - Multiple authentication methods support (Cleartext, MD5, SCRAM-SHA-256)
 * - Transaction management (inherited from Transaction class)
 * - Query execution and result processing
 * - Prepared statement caching and execution
 * - Event-driven message handling
 *
 * The Database class inherits from both the TCP client base class for network
 * connectivity and the Transaction class for query and transaction management.
 *
 * @tparam QB_IO_ I/O handler type that provides networking capabilities
 * @tparam NotifyDerived CRTP notify consumer type (`void` for plain `database`); receives NOTIFY via
 *         `consume_pg_notify` / `deliver_pg_notify`. See `notify_consumer` / `notify_co_consumer`
 *         (`notify_cb_consumer` is an alias for the same class).
 */
template <typename QB_IO_, typename NotifyDerived = void>
class Database
    : public qb::io::async::tcp::client<Database<QB_IO_, NotifyDerived>, QB_IO_, void>
    , public Transaction {
public:
    /**
     * @brief PostgreSQL protocol handler type
     *
     * Type alias for the protocol handler used by this database client.
     */
    using pg_protocol = qb::protocol::pgsql<Database<QB_IO_, NotifyDerived>>;

private:
    connection_options   conn_opts_;            ///< Database connection options
    client_options_type  client_opts_;          ///< Client-supplied startup options (sent in the StartupMessage)
    client_options_type  server_params_;        ///< Server-reported ParameterStatus cache (NOT echoed back at connect)
    integer              serverPid_{};          ///< Server process ID
    integer              serverSecret_{};       ///< Server secret for protocol operations
    PreparedQueryStorage storage_;              ///< Storage for prepared statements
    bool                 is_connected_ = false; ///< Flag indicating if the connection is established

    /// Outstanding `co_await connect()` handshake (coroutine resume + validity token)
    bool                    connect_coroutine_pending_{false};
    bool                    connect_handshake_failed_{false};
    std::coroutine_handle<> connect_suspend_handle_{};
    std::shared_ptr<bool>   connect_suspend_valid_{};
    /// Bumps on each new handshake / reconnect prep so stale `callback(timeout)` ignores
    std::uint64_t connect_timer_generation_{0};
    /// Owned handshake-deadline timer. MUST be a ScopedTimeout (cancelled on destruction),
    /// not a fire-and-forget async::callback: the latter is a self-deleting heap Timeout that
    /// outlives this Database and would dereference a freed `this` if the connection is dropped
    /// within the timeout window (the common case — handshake finishes in ms, the deadline is
    /// seconds). Destroying this member with the Database stops the watcher.
    std::unique_ptr<qb::io::async::ScopedTimeout<std::function<void()>>> connect_deadline_timer_{};

    /// When `NotifyDerived` is `void`, optional handler for `NotificationResponse` (plain
    /// `database`).
    std::function<void(::qb::pg::notification &&)> inbound_notify_handler_{};

    void
    try_resume_connect_wait() {
        if (!connect_coroutine_pending_)
            return;
        const bool terminal = is_connected_ || connect_handshake_failed_ || (has_error() && !is_connected_);
        if (!terminal)
            return;
        auto h                     = connect_suspend_handle_;
        auto v                     = connect_suspend_valid_;
        connect_coroutine_pending_ = false;
        connect_suspend_handle_    = {};
        connect_suspend_valid_.reset();
        if (v && !*v)
            return;
        if (h)
            qb::io::async::coro_scheduler().schedule_resume(h);
    }

    /**
     * @brief Starts outbound TCP using the async framework (`qb::io::async::tcp::connect`).
     *
     * Uses the same connector path as Redis: non-blocking `n_connect`, `EV_WRITE` completion,
     * and an optional deadline (`connect_timeout` or default 10s). The coroutine awaiter is
     * resumed from `try_resume_connect_wait()` after TCP + PostgreSQL pre-startup steps or on
     * failure.
     *
     * @param h Coroutine handle to resume when the handshake attempt finishes (success or failure)
     * @param valid Shared flag cleared when the awaiter is destroyed (ignore stale callbacks)
     * @param timeout_override If positive, overrides `conn_opts_.connect_timeout` for this attempt
     */
    void
    start_connect_from_awaiter(std::coroutine_handle<> h, std::shared_ptr<bool> valid, qb::duration timeout_override) {
        ++connect_timer_generation_;
        const std::uint64_t timer_gen = connect_timer_generation_;

        connect_suspend_handle_    = h;
        connect_suspend_valid_     = std::move(valid);
        connect_coroutine_pending_ = true;
        connect_handshake_failed_  = false;
        _error                     = error::db_error{"unknown error"};

        if (is_connected_) {
            try_resume_connect_wait();
            return;
        }

        const double t_out =
            timeout_override > qb::duration::zero()
                ? qb::detail::to_ev_seconds(timeout_override)
                : (conn_opts_.connect_timeout > qb::duration::zero() ? qb::detail::to_ev_seconds(conn_opts_.connect_timeout) : 10.0);

        const qb::io::uri connect_uri{conn_opts_.schema + "://" + conn_opts_.uri};
        auto              awaiter_valid = connect_suspend_valid_;

        using transport_sock = std::remove_cvref_t<typename QB_IO_::transport_io_type>;
        auto cb              = [this, timer_gen, t_out, awaiter_valid](transport_sock &&sock) {
            if (awaiter_valid && !*awaiter_valid)
                return;
            on_transport_ready(std::move(sock), timer_gen, t_out);
        };

        if constexpr (transport_sock::is_secure()) {
#ifdef QB_HAS_SSL
            // PostgreSQL negotiates TLS in-band (the cleartext SSLRequest packet) BEFORE
            // the handshake. Drive the whole connect → SSLRequest → TLS handshake through
            // the connector's STARTTLS path so it runs fully asynchronously on the event
            // loop (no blocking send/recv/handshake). A server that declines SSL fails the
            // connect — a secure database requires TLS; use the plain tcp::database for
            // cleartext.
            // ssl_verify_mode::full -> verify the chain + host (the connector passes the
            // remote host to ssl::socket::init_client); ::none -> encrypt only (set_insecure).
            const bool verify = (conn_opts_.ssl_verify == qb::pg::ssl_verify_mode::full);
            qb::io::async::tcp::starttls_connect<transport_sock, postgres_ssl_negotiator>(connect_uri, std::move(cb),
                                                                                          qb::detail::from_ev_seconds(t_out), verify);
#else
            connect_handshake_failed_ = true;
            _error                    = error::connection_error{"ssl transport requires QB_HAS_SSL"};
            try_resume_connect_wait();
#endif
        } else {
            qb::io::async::tcp::connect<transport_sock>(connect_uri, std::move(cb), qb::detail::from_ev_seconds(t_out));
        }
    }

    /**
     * @brief Switches to the PostgreSQL protocol, starts read/write watchers, sends startup.
     *
     * Schedules the existing application-level handshake timeout (authentication / ReadyForQuery),
     * distinct from the TCP connector deadline in `start_connect_from_awaiter`.
     *
     * @param timer_gen Generation counter; stale timers ignore the callback after reconnect
     * @param t_out Handshake timeout in seconds passed to `async::callback`
     */
    void
    attach_pg_protocol_and_handshake_timer(std::uint64_t timer_gen, double t_out) {
        this->template switch_protocol<pg_protocol>(*this);
        this->start();
        send_startup_message();

        // Owned, cancellable deadline (see connect_deadline_timer_): reassigning here cancels
        // any prior pending timer, and ~Database cancels this one — so the callback can never
        // fire into a freed `this`. The timer_gen guard still covers an in-place reconnect.
        connect_deadline_timer_ = qb::io::async::scoped_callback(std::function<void()>([this, t_out, timer_gen]() {
                                                                     if (timer_gen != connect_timer_generation_)
                                                                         return;
                                                                     if (!connect_coroutine_pending_ || is_connected_)
                                                                         return;
                                                                     LOG_WARN("[pgsql] Connection timed out after " << t_out << "s");
                                                                     _error                    = error::db_error{"connection timeout"};
                                                                     connect_handshake_failed_ = true;
                                                                     try_resume_connect_wait();
                                                                 }),
                                                                 qb::detail::from_ev_seconds(t_out));

        try_resume_connect_wait();
    }

    /**
     * @brief Installs the ready transport and starts the PostgreSQL session.
     *
     * The connector delivers a fully-established socket: for a **secure** database it
     * has already run the cleartext SSLRequest negotiation AND the TLS handshake
     * asynchronously (via `starttls_connect` + `postgres_ssl_negotiator`); for a
     * **plain** database it is the connected cleartext TCP socket. Either way this
     * switches to the PostgreSQL protocol and sends the startup message — there is no
     * blocking send/recv/handshake on the event loop anymore.
     *
     * @param sock Ready transport socket; empty/closed if the connect or TLS handshake failed.
     * @param timer_gen Generation counter passed through to the handshake timer.
     * @param t_out Authentication / ReadyForQuery timeout in seconds.
     */
    template <typename Sock_>
    void
    on_transport_ready(Sock_ &&sock, std::uint64_t timer_gen, double t_out) {
        if (!sock.is_open()) {
            connect_handshake_failed_ = true;
            _error                    = error::connection_error{"connection / TLS handshake failed"};
            try_resume_connect_wait();
            return;
        }
        this->clear_protocols(); // idempotent: drops any prior protocol, resets to the NoProtocol sentinel
        this->transport() = std::forward<Sock_>(sock);
        attach_pg_protocol_and_handshake_timer(timer_gen, t_out);
    }

    /**
     * @brief Creates a startup message for PostgreSQL connection
     *
     * Builds the startup message according to the PostgreSQL protocol specification.
     * The message includes:
     * - Protocol version
     * - User authentication information
     * - Target database name
     * - Client parameters and options
     *
     * @param m Message object to populate with startup information
     */
    void
    create_startup_message(message &m) {
        m.write(PROTOCOL_VERSION);
        // Startup packet: null-terminated name=value pairs (PostgreSQL wire protocol).
        // write(std::string) appends the required '\0' terminator; write_sv does not.
        m.write(std::string(options::USER));
        m.write(conn_opts_.user);
        m.write(std::string(options::DATABASE));
        m.write(conn_opts_.database);

        for (auto &opt : client_opts_) {
            m.write(opt.first);
            m.write(opt.second);
        }
        // trailing terminator
        m.write('\0');
    }

    /**
     * @brief Sends the startup message to the PostgreSQL server
     */
    void
    send_startup_message() {
        message m(empty_tag);
        create_startup_message(m);
        *this << m;
    }

    /**
     * @brief Handles new command events in the transaction
     */
    void
    on_new_command() final {
        process_if_query_ready();
    }

    /**
     * @brief Handles sub-command status updates
     *
     * Propagates leaf `ResultQuery` / nested command outcomes to the root `Transaction`
     * so `await()` and `status::operator bool` reflect failures (not only child `_result`).
     */
    void
    on_sub_command_status(bool status) final {
        Transaction::on_sub_command_status(status);
    }

    /// Root `Transaction` for this connection (never null while `Database` lives).
    Transaction *
    root_transaction() noexcept {
        return static_cast<Transaction *>(static_cast<Database<QB_IO_, NotifyDerived> *>(this));
    }

    Transaction *_current_command = this;    ///< Current transaction being processed
    ISqlQuery   *_current_query   = nullptr; ///< Current query being executed
    bool         _ready_for_query = false;   ///< Flag indicating if ready for next query

    /**
     * @brief Finds the next transaction to execute
     *
     * Recursively traverses the transaction tree to find the
     * deepest (leaf) transaction that should be executed next.
     *
     * @param cmd Current transaction
     * @return Transaction* Next transaction to execute
     */
    static Transaction *
    next_transaction(Transaction *cmd) {
        if (!cmd)
            return nullptr;

        auto sub = cmd->next_transaction();
        if (!sub)
            return cmd;
        else
            return next_transaction(sub);
    }

    /**
     * @brief Processes a query in the transaction
     *
     * Fetches and executes the next query from the given transaction.
     * If no more queries are in the current transaction, moves to parent.
     *
     * @param cmd Transaction containing the query
     * @return bool true if a query was processed, false if no queries remain
     */
    bool
    process_query(Transaction *cmd) {
        _ready_for_query = false;
        if (!cmd)
            cmd = root_transaction();
        _current_command = next_transaction(cmd);
        if (!_current_command)
            _current_command = root_transaction();
        _current_query = _current_command->next_query();

        if (_current_query) {
            if (qb::likely(_current_query->is_valid())) {
                *this << _current_query->get();
                return true;
            } else {
                LOG_DEBUG("[pgsql] error processing query not valid");
                _error = error::client_error{"query couldn't be processed check logs for more infos"};
                on_error_query(error());
                return process_query(_current_command) || (_ready_for_query = true);
            }
        } else if (_current_command->parent()) {
            auto next_cmd = _current_command->parent();
            do {
                next_cmd->pop_transaction();
            } while (!next_cmd->result() && (next_cmd = next_cmd->parent()));

            return process_query(next_cmd);
        }
        return false;
    }

    /**
     * @brief Processes queries if the client is ready
     */
    void
    process_if_query_ready() {
        if (_ready_for_query) {
            process_query(_current_command);
        }
    }

    /**
     * @brief Handles successful query completion
     */
    void
    on_success_query() {
        if (!_current_query)
            return;
        if (!_current_command) {
            _current_query = nullptr;
            return;
        }
        auto query = _current_command->pop_query();
        query->on_success();
        _current_query = nullptr;
    }

    /**
     * @brief Handles query error
     *
     * @param err Error information
     */
    void
    on_error_query(error::db_error const &err) {
        _error = err;
        if (!_current_query)
            return;
        if (!_current_command) {
            _current_query = nullptr;
            return;
        }
        _current_command->result(false);
        auto query = _current_command->pop_query();
        query->on_error(err);
        _current_query = nullptr;
    }

private:
    std::string                _nonce;           ///< Client nonce for SCRAM authentication
    std::vector<uint8_t>       _password_salt;   ///< Salted password for SCRAM authentication
    std::string                _auth_message;    ///< Authentication message for SCRAM protocol
    std::string                _gs2_header;      ///< SCRAM gs2-header chosen at SASL init (`n,,` / `y,,` / `p=tls-server-end-point,,`)
    std::vector<unsigned char> _channel_binding; ///< SCRAM-SHA-256-PLUS channel-binding data (tls-server-end-point); empty when unbound
    std::function<void(std::string_view)>
        _copy_out_sink; ///< Active `COPY … TO STDOUT` chunk sink (set for the duration of copy_out); empty otherwise
    std::function<std::optional<std::string>()>
         _copy_in_source;  ///< Active `COPY … FROM STDIN` chunk source (returns next chunk, nullopt = done); empty otherwise
    char _txn_status{'I'}; ///< Last ReadyForQuery transaction status: 'I' idle, 'T' in a block, 'E' failed block

public:
    /**
     * @brief Handles authentication messages from the server
     *
     * Processes various authentication methods requested by the PostgreSQL server
     * and responds appropriately. Supports multiple authentication mechanisms:
     *
     * - OK: Authentication already successful
     * - Cleartext: Simple plaintext password authentication
     * - MD5: MD5 hash-based password authentication
     * - SCRAM-SHA-256: Modern challenge-response authentication
     *
     * For each authentication type, this method constructs and sends the
     * appropriate response message according to the PostgreSQL protocol and
     * authentication specifications.
     *
     * @param msg Authentication message from the server
     */
    void
    on_authentication(message &msg) {
        integer auth_state(-1);
        msg.read(auth_state);

        LOG_DEBUG("[pgsql] Handle auth_event");
        switch (auth_state) {
            case OK: {
                LOG_INFO("[pgsql] Authenticated with server");
                is_connected_ = true;
                // Apply keepalive settings if configured (P1-1)
                apply_keepalive_settings();
                try_resume_connect_wait();
            } break;
            case Cleartext: {
                LOG_INFO("[pgsql] Clear text authentication requested");
                message pm(password_message_tag);
                pm.write(conn_opts_.password);

                *this << pm;
            } break;
            case MD5Password: {
                LOG_INFO("[pgsql] MD5 authentication requested");
                // Read salt
                std::string salt;
                msg.read(salt, 4);
                // Calculate hash
                std::string pwdhash =
                    qb::crypto::to_hex_string(qb::crypto::md5(conn_opts_.password + conn_opts_.user), qb::crypto::range_hex_lower);
                std::string md5digest =
                    std::string("md5") + qb::crypto::to_hex_string(qb::crypto::md5(pwdhash + salt), qb::crypto::range_hex_lower);
                // Construct and send message
                message pm(password_message_tag);
                pm.write(md5digest);

                *this << pm;
            } break;
            case SCRAM_SHA256: {
                LOG_INFO("[pgsql] SCRAM-SHA-256 authentication requested");

                // AuthenticationSASL body: the NUL-terminated mechanism names the
                // server offers, ended by an empty string.
                bool        offers_plus = false;
                std::string mech;
                while (msg.read(mech) && !mech.empty()) {
                    if (mech == "SCRAM-SHA-256-PLUS")
                        offers_plus = true;
                }

                // Negotiate channel binding (RFC 5802 §6 gs2-cbind-flag / RFC 5929):
                //   - cleartext link            -> "n,," (binding not applicable)
                //   - TLS + server offers -PLUS -> "p=tls-server-end-point,," + bind data
                //   - TLS + server lacks -PLUS  -> "y,," (we support it; the server then
                //                                  detects a MITM that stripped -PLUS)
                using transport_sock           = std::remove_cvref_t<typename QB_IO_::transport_io_type>;
                std::string selected_mechanism = "SCRAM-SHA-256";
                _channel_binding.clear();
                if constexpr (transport_sock::is_secure()) {
                    if (offers_plus) {
                        _channel_binding = this->transport().tls_server_end_point();
                    }
                    if (!_channel_binding.empty()) {
                        selected_mechanism = "SCRAM-SHA-256-PLUS";
                        _gs2_header        = "p=tls-server-end-point,,";
                    } else {
                        _gs2_header = "y,,";
                    }
                } else {
                    _gs2_header = "n,,";
                }

                message pm(password_message_tag);
                // SECURITY: the SCRAM client nonce is an anti-replay/freshness value and
                // MUST come from a CSPRNG (OpenSSL RAND_bytes), not the non-cryptographic
                // mt19937 behind generate_random_string.
                _nonce          = qb::crypto::generate_secure_random_string(32, qb::crypto::range_hex_lower);
                const auto data = _gs2_header + "n=" + scram_escape_saslname(conn_opts_.user) + ",r=" + _nonce;
                pm.write(selected_mechanism);
                pm.write(static_cast<qb::pg::integer>(data.size()));
                pm.write_sv(data);
                *this << pm;
            } break;
            case SCRAM_SHA256_CLIENT_PROOF: {
                LOG_INFO("[pgsql] SCRAM-SHA-256 authentication client proof check");
                std::string data;
                msg.read(data);
                auto params = parse_header_attributes(data.c_str(), data.size());

                // SCRAM inputs
                const std::string clientNonce = _nonce; // Nonce generated by client
                const std::string username    = conn_opts_.user;
                const std::string password    = conn_opts_.password;
                const std::string serverNonce = std::move(params["r"]); // Combined nonce (client + server)
                const std::string salt_base64 = std::move(params["s"]); // Salt (base64)

                // SECURITY (RFC 5802 §5.1): the server's combined nonce must begin with
                // the exact client nonce we sent and extend it. Reject otherwise — a
                // mismatched nonce means the peer is not faithfully continuing OUR
                // exchange (MITM / replay / buggy server). Check before deriving any proof.
                if (!scram_server_nonce_extends_client(clientNonce, serverNonce)) {
                    throw error::connection_error("SCRAM: server nonce does not extend the client nonce");
                }

                // Parse + bound-check the iteration count. SECURITY: the count is
                // server-controlled and feeds PBKDF2 SYNCHRONOUSLY on the I/O event-loop
                // thread, so an unbounded value is a DoS (see scram_validate_iteration_count
                // / kMaxScramIterations).
                const auto it = params.find("i");
                if (it == params.end()) {
                    throw error::connection_error("Missing iteration count in SCRAM response");
                }
                const int iteration = scram_validate_iteration_count(it->second);

                // Client-first-message-bare — MUST match the bytes sent in the SASL
                // client-first (same saslname escaping of the username).
                std::string client_first_message_bare = "n=" + scram_escape_saslname(username) + ",r=" + clientNonce;
                std::string server_first_message      = "r=" + serverNonce + ",s=" + salt_base64 + ",i=" + std::to_string(iteration);
                // c = base64( gs2-header-bytes || channel-binding-data ). Unbound -> the
                // data is empty so this is base64("n,,")="biws" or base64("y,,"); with
                // tls-server-end-point the server-certificate hash is appended (binds the
                // SCRAM proof to this exact TLS channel).
                std::string cbind_input = _gs2_header;
                cbind_input.append(reinterpret_cast<const char *>(_channel_binding.data()), _channel_binding.size());
                const std::string channel_binding_b64 =
                    qb::crypto::base64_encode(reinterpret_cast<const unsigned char *>(cbind_input.data()), cbind_input.size());
                std::string client_final_message_without_proof = "c=" + channel_binding_b64 + ",r=" + serverNonce;
                _auth_message = client_first_message_bare + "," + server_first_message + "," + client_final_message_without_proof;
                // Compute SaltedPassword using PBKDF2-HMAC-SHA256
                std::vector<unsigned char> salt = qb::crypto::base64_decode(salt_base64);
                std::vector<unsigned char> saltedPassword(32); // 32 bytes for SHA256
                if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()), salt.data(), static_cast<int>(salt.size()),
                                      iteration, EVP_sha256(), 32, saltedPassword.data())
                    != 1) {
                    throw std::runtime_error("error during PBKDF2 computing");
                }
                // Compute clientKey: HMAC(saltedPassword, "Client Key")
                std::vector<unsigned char> clientKey = qb::crypto::hmac_sha256(saltedPassword, "Client Key");
                // Compute storedKey: SHA256(clientKey)
                std::vector<unsigned char> storedKey = qb::crypto::sha256(clientKey);
                // Compute clientSignature: HMAC(storedKey, authMessage)
                std::vector<unsigned char> clientSignature = qb::crypto::hmac_sha256(storedKey, _auth_message);
                // Compute clientProof: XOR(clientKey, clientSignature)
                std::vector<unsigned char> clientProof = qb::crypto::xor_bytes(clientKey, clientSignature);
                // Encode clientProof in base64
                std::string clientProofBase64 = qb::crypto::base64_encode(clientProof.data(), clientProof.size());
                // Construction of final message to send
                std::string client_final_message = client_final_message_without_proof + ",p=" + clientProofBase64;

                message pm(password_message_tag);
                pm.write_sv(client_final_message);
                *this << pm;
                _password_salt = std::move(saltedPassword);
            } break;
            case SCRAM_SHA256_SERVER_CHECK: {
                try {
                    std::string serverFinalMessage;

                    msg.read(serverFinalMessage);
                    // Extract the server signature from the final message
                    const std::string prefix = "v=";
                    size_t            pos    = serverFinalMessage.find(prefix);
                    if (pos == std::string::npos) {
                        throw std::runtime_error("server final message does not contain a signature");
                    }
                    std::string receivedServerSignatureBase64 = serverFinalMessage.substr(pos + prefix.size());
                    // Compute the ServerKey: HMAC(saltedPassword, "Server Key")
                    std::vector<unsigned char> serverKey = qb::crypto::hmac_sha256(_password_salt, "Server Key");
                    // Compute the ServerSignature: HMAC(serverKey, authMessage)
                    std::vector<unsigned char> computedServerSignature = qb::crypto::hmac_sha256(serverKey, _auth_message);
                    // Encode the computed server signature in Base64
                    std::string computedServerSignatureBase64 =
                        qb::crypto::base64_encode(computedServerSignature.data(), computedServerSignature.size());
                    // Compare the computed server signature with the received one
                    if (computedServerSignatureBase64 != receivedServerSignatureBase64) {
                        throw std::runtime_error("server signature does not match. Authentication failed");
                    }
                    LOG_INFO("[pgsql] SCRAM-SHA-256 Authentication successful: server "
                             "signature verified");
                    break;
                } catch (std::exception &ex) {
                    LOG_CRIT("[pgsql] SCRAM-SHA-256 Failed verifying server signature: " << ex.what());
                    connect_handshake_failed_ = true;
                    is_connected_             = false;
                    try_resume_connect_wait();
                }
            } break;
            default: {
                LOG_CRIT("[pgsql] Unsupported authentication scheme " << auth_state << "requested by server");
                throw std::runtime_error("[pgsql] fatal error: check logs");
            }
        }
    }

    // Bring the base semantic hook Transaction::on_command_complete(const std::string&)
    // into scope so this protocol-level overload (message&) does not hide it
    // (-Woverloaded-virtual). The two are intentionally distinct: this one parses the
    // wire message and forwards the parsed tag to the Transaction hook via _current_command.
    using Transaction::on_command_complete;

    /**
     * @brief Handles command complete messages
     *
     * @param msg Command complete message
     */
    void
    on_command_complete(message &msg) {
        command_complete cmpl;
        msg.read(cmpl.command_tag);
        LOG_DEBUG("[pgsql] Command complete (" << cmpl.command_tag << ")");
        if (_current_command)
            _current_command->on_command_complete(cmpl.command_tag);
    }

    /**
     * @brief Handles backend key data messages
     *
     * @param msg Backend key data message
     */
    void
    on_backend_key_data(message &msg) {
        msg.read(serverPid_);
        msg.read(serverSecret_);
        LOG_DEBUG("[pgsql] Received backend key data");
    }

    /**
     * @brief Handles error response messages
     *
     * @param msg Error response message
     */
    void
    on_error_response(message &msg) {
        notice_message notice;
        msg.read(notice);

        LOG_WARN("[pgsql] Error " << notice);
        error::query_error err(notice.message, notice.severity, notice.sqlstate, notice.detail);

        on_error_query(err);
        if (connect_coroutine_pending_ && !is_connected_)
            try_resume_connect_wait();
    }

    /**
     * @brief Handles parameter status messages
     *
     * @param msg Parameter status message
     */
    void
    on_parameter_status(message &msg) {
        std::string key;
        std::string value;

        msg.read(key);
        msg.read(value);

        LOG_DEBUG("[pgsql] Received parameter " << key << "=" << value);

        // Server-reported ParameterStatus values are cached for the accessors
        // (parameter_status() / server_version()) but kept SEPARATE from the
        // client-supplied startup options: many of them (notably "server_version")
        // are read-only GUC_REPORT parameters that PostgreSQL rejects with SQLSTATE
        // 55P02 ("parameter ... cannot be changed") if sent back in a StartupMessage.
        // Echoing them on reconnect (see create_startup_message) used to abort the
        // re-handshake right after authentication.
        server_params_[key] = value;
    }

    /**
     * @brief Handles notice response messages
     *
     * @param msg Notice response message
     */
    void
    on_notice_response(message &msg) {
        notice_message notice;
        msg.read(notice);

        LOG_INFO("[pgsql] Received notice" << notice);
    }

    /**
     * @brief Handles ready for query messages
     *
     * @param msg Ready for query message
     */
    void
    on_ready_for_query(message &msg) {
        on_success_query();
        char stat(0);
        msg.read(stat);
        // I = idle, T = in transaction block, E = failed transaction (must ROLLBACK)
        if (stat == 'I' || stat == 'T' || stat == 'E')
            _txn_status = stat;
        if (stat == 'E') {
            LOG_WARN("[pgsql] ReadyForQuery: backend session is in failed transaction "
                     "(SQLSTATE implicit); issue ROLLBACK before new commands");
        }

        if (!process_query(_current_command)) {
            _ready_for_query = true;
            LOG_DEBUG("[pgsql] Database " << conn_opts_.uri << "[" << conn_opts_.database << "]"
                                          << " is ready for query (" << stat << ")");
        }
    }

    /**
     * @brief Handles row description messages
     *
     * @param msg Row description message
     */
    void
    on_row_description(message &msg) {
        if (!_current_command)
            return;
        row_description_type fields;
        // message::read leaves its target untouched when the payload is too short, so
        // col_cnt must be initialized and the read result checked: a malformed/truncated
        // RowDescription (length the server controls) would otherwise leave col_cnt
        // indeterminate and feed garbage — possibly a negative value widening to a huge
        // size_t — into reserve()/the loop bound.
        smallint col_cnt = 0;
        if (!msg.read(col_cnt) || col_cnt < 0) {
            LOG_WARN("[pgsql] RowDescription with missing or negative column count");
            _current_command->result(false);
            _current_command->on_new_row_description({});
            return;
        }
        fields.reserve(static_cast<std::size_t>(col_cnt));
        for (int i = 0; i < col_cnt; ++i) {
            field_description fd;
            if (msg.read(fd)) {
                fields.push_back(fd);
            } else {
                LOG_WARN("[pgsql] Failed to read field description " << i);
                _current_command->result(false);
                break;
            }
        }
        _current_command->on_new_row_description(std::move(fields));
    }

    /**
     * @brief Handles data row messages
     *
     * @param msg Data row message
     */
    void
    on_data_row(message &msg) {
        if (!_current_command)
            return;
        row_data row;
        if (msg.read(row))
            _current_command->on_new_data_row(std::move(row));
        else {
            LOG_WARN("[pgsql] Failed to read data row");
            _current_command->result(false);
        }
    }

    /**
     * @brief Handles parse complete messages
     *
     * @param msg Parse complete message
     */
    void
    on_parse_complete(message &) {
        LOG_DEBUG("[pgsql] Parse complete");
    }

    /**
     * @brief Handles parameter description messages
     *
     * @param msg Parameter description message
     */
    void
    on_parameter_description(message &) {
        LOG_DEBUG("[pgsql] Parameter descriptions");
    }

    /**
     * @brief Handles bind complete messages
     *
     * @param msg Bind complete message
     */
    void
    on_bind_complete(message &) {
        LOG_DEBUG("[pgsql] Bind complete");
    }

    /**
     * @brief Handles no data messages
     *
     * @param msg No data message
     */
    void
    on_no_data(message &) {
        LOG_DEBUG("[pgsql] No data");
    }

    /**
     * @brief Handles portal suspended messages
     *
     * @param msg Portal suspended message
     */
    void
    on_portal_suspended(message &) {
        LOG_DEBUG("[pgsql] Portal suspended");
    }

    /**
     * @brief Handles empty query response messages
     *
     * Sent by the server when an empty query string is received.
     * Treated as a successful no-op — the server will follow with ReadyForQuery.
     *
     * @param msg Empty query response message
     */
    void
    on_empty_query_response(message &) {
        LOG_DEBUG("[pgsql] Empty query response");
    }

    /**
     * @todo COPY protocol — not a product feature yet; only enough handling to keep the session
     *       healthy. Future work: stream **COPY FROM STDIN** (client → server bulk load) and
     *       surface **COPY TO STDOUT** / CopyData payloads to the application (today `on_copy_data`
     *       discards chunks). See PostgreSQL docs: COPY, CopyIn/CopyOut/CopyData messages.
     */

    /**
     * @brief Abort a COPY FROM STDIN (client → server) with a CopyFail.
     *
     * COPY is issued over the SIMPLE query protocol (a 'Q' message), which has NO Sync
     * framing: the server replies to a CopyFail with ErrorResponse + a single
     * ReadyForQuery. Send ONLY CopyFail — appending a Sync (extended-protocol recovery)
     * would make the backend emit a SECOND ReadyForQuery and desynchronize the command
     * queue (premature completion / double-dispatch of any queued command).
     */
    void
    send_copy_fail() {
        message fail(copy_fail_tag);
        fail.write(std::string("qbm-pgsql: COPY FROM STDIN aborted by client"));
        *this << fail;
    }

    /**
     * @brief NotificationResponse (LISTEN/NOTIFY)
     *
     * @see https://www.postgresql.org/docs/current/protocol-message-formats.html
     */
    void
    on_notification_response(message &msg) {
        integer     pid{};
        std::string channel;
        std::string payload;
        if (!msg.read(pid) || !msg.read(channel) || !msg.read(payload)) {
            LOG_WARN("[pgsql] Malformed NotificationResponse");
            return;
        }
        ::qb::pg::notification n;
        n.server_backend_pid = static_cast<int>(pid);
        n.channel            = std::move(channel);
        n.payload            = std::move(payload);
        if constexpr (!std::is_same_v<NotifyDerived, void>) {
            static_cast<NotifyDerived *>(this)->consume_pg_notify(std::move(n));
        } else if (inbound_notify_handler_) {
            inbound_notify_handler_(std::move(n));
        } else {
            LOG_INFO("[pgsql] NOTIFY pid=" << n.server_backend_pid << " channel=" << n.channel << " payload=" << n.payload);
        }
    }

    /**
     * @brief CopyInResponse — server expects COPY data from client
     */
    void
    on_copy_in_response(message &msg) {
        char     overall{};
        smallint ncols{};
        if (!msg.read(overall) || !msg.read(ncols)) {
            LOG_WARN("[pgsql] Malformed CopyInResponse");
            send_copy_fail();
            return;
        }
        if (ncols < 0 || ncols > 4096) {
            LOG_WARN("[pgsql] CopyInResponse invalid column count " << ncols);
            on_error_query(error::client_error{"Malformed CopyInResponse from server"});
            send_copy_fail();
            return;
        }
        for (int i = 0; i < ncols; ++i) {
            smallint fmt{};
            if (!msg.read(fmt)) {
                send_copy_fail();
                return;
            }
        }
        if (!_copy_in_source) {
            LOG_WARN("[pgsql] CopyInResponse with no copy_in() source registered; failing the COPY");
            on_error_query(error::client_error{"COPY FROM STDIN requires copy_in() with a data source"});
            send_copy_fail();
            return;
        }
        // Stream the client-provided data: each source() chunk becomes a CopyData
        // message (raw bytes), then CopyDone. The server replies CommandComplete +
        // ReadyForQuery, which resolves the copy_in() awaiter. A throwing source aborts
        // the COPY cleanly with CopyFail rather than corrupting the protocol stream.
        try {
            // CopyData may split at ANY byte boundary, so cap each message body well
            // under the int32 wire length field: a single >2 GiB chunk would otherwise
            // wrap message::length() and desynchronize the stream.
            static constexpr std::size_t kMaxCopyDataBody = 1u << 30; // 1 GiB
            while (std::optional<std::string> chunk = _copy_in_source()) {
                std::string_view rest{*chunk};
                while (!rest.empty()) { // empty chunk -> skipped; large chunk -> split
                    const std::size_t take = std::min(rest.size(), kMaxCopyDataBody);
                    message           d(copy_data_tag);
                    d.write_sv(rest.substr(0, take));
                    *this << d;
                    rest.remove_prefix(take);
                }
            }
        } catch (...) {
            LOG_WARN("[pgsql] copy_in source threw; aborting the COPY with CopyFail");
            send_copy_fail();
            return;
        }
        message done(copy_done_tag);
        *this << done;
    }

    /**
     * @brief CopyOutResponse — server will send COPY data (COPY ... TO STDOUT)
     */
    void
    on_copy_out_response(message &msg) {
        char     overall{};
        smallint ncols{};
        if (!msg.read(overall) || !msg.read(ncols)) {
            LOG_WARN("[pgsql] Malformed CopyOutResponse");
            msg.discard_remaining();
            return;
        }
        if (ncols < 0 || ncols > 4096) {
            LOG_WARN("[pgsql] CopyOutResponse invalid column count " << ncols);
            msg.discard_remaining();
            return;
        }
        for (int i = 0; i < ncols; ++i) {
            smallint fmt{};
            if (!msg.read(fmt)) {
                msg.discard_remaining();
                return;
            }
        }
        LOG_DEBUG("[pgsql] CopyOutResponse received (data will pass as CopyData messages)");
    }

    /**
     * @brief CopyBothResponse — bidirectional COPY (replication / rare)
     */
    void
    on_copy_both_response(message &msg) {
        on_copy_out_response(msg);
        LOG_WARN("[pgsql] CopyBothResponse: bidirectional COPY not fully supported");
    }

    /**
     * @brief CopyData from server during COPY OUT / BOTH (payload is opaque row bytes)
     */
    void
    on_copy_data(message &msg) {
        // During a COPY ... TO STDOUT, hand the opaque payload (one row, or a chunk in
        // binary format) to the active sink without copying. No sink -> drop the chunk.
        if (_copy_out_sink) {
            const std::string_view chunk = msg.remaining();
            if (!chunk.empty())
                _copy_out_sink(chunk);
        }
        msg.discard_remaining();
    }

    /**
     * @brief CopyDone from server (end of COPY OUT data stream)
     */
    void
    on_copy_done(message &msg) {
        LOG_DEBUG("[pgsql] CopyDone (server)");
        msg.discard_remaining();
    }

    /**
     * @brief CloseComplete — acknowledgment of Close (frontend); usually empty body
     */
    void
    on_close_complete(message &msg) {
        LOG_DEBUG("[pgsql] CloseComplete");
        msg.discard_remaining();
    }

    /**
     * @brief FunctionCallResponse — legacy fast-path function protocol (rare)
     */
    void
    on_function_call_response(message &msg) {
        LOG_WARN("[pgsql] FunctionCallResponse (V): fast-path function protocol not "
                 "implemented; message ignored");
        msg.discard_remaining();
    }

    /**
     * @brief Handles unrecognized messages
     *
     * @param msg Unhandled message
     */
    void
    on_unhandled_message(message &msg) {
        LOG_WARN("[pgsql] Unhandled backend message tag " << (char) msg.tag() << " (length " << msg.length() << ") — check protocol coverage");
    }

    /**
     * @brief Message routing table
     *
     * Maps PostgreSQL protocol message tags to their handler methods.
     */
    inline static const qb::unordered_flat_map<int, void (Database<QB_IO_, NotifyDerived>::*)(message &)> routes_ = {
        {authentication_tag, &Database::on_authentication},
        {command_complete_tag, &Database::on_command_complete},
        {backend_key_data_tag, &Database::on_backend_key_data},
        {error_response_tag, &Database::on_error_response},
        {parameter_status_tag, &Database::on_parameter_status},
        {notice_response_tag, &Database::on_notice_response},
        {notification_resp_tag, &Database::on_notification_response},
        {ready_for_query_tag, &Database::on_ready_for_query},
        {row_description_tag, &Database::on_row_description},
        {data_row_tag, &Database::on_data_row},
        {parse_complete_tag, &Database::on_parse_complete},
        {parameter_description_tag, &Database::on_parameter_description},
        {bind_complete_tag, &Database::on_bind_complete},
        {no_data_tag, &Database::on_no_data},
        {portal_suspended_tag, &Database::on_portal_suspended},
        {empty_query_response_tag, &Database::on_empty_query_response},
        {copy_in_response_tag, &Database::on_copy_in_response},
        {copy_out_response_tag, &Database::on_copy_out_response},
        {copy_both_response_tag, &Database::on_copy_both_response},
        {copy_data_tag, &Database::on_copy_data},
        {copy_done_tag, &Database::on_copy_done},
        {close_complete_tag, &Database::on_close_complete},
        {function_call_resp_tag, &Database::on_function_call_response}
    };

public:
    /**
     * @brief Default constructor
     *
     * Creates a database client without connection information.
     * The client is initialized but not connected to any database.
     * Use the connect() method with connection options to establish a connection.
     */
    Database()
        : Transaction(storage_) {}

    /**
     * @brief Constructs a database client with connection options
     *
     * Initializes the client with the given connection options but does not
     * establish a connection immediately. Call connect() to initiate the connection.
     *
     * @param opts Connection string in the format
     * "postgresql://user:password@host:port/database"
     */
    explicit Database(std::string const &opts)
        : Transaction(storage_)
        , conn_opts_(connection_options::parse(opts)) {}

    /**
     * @brief Destructor
     *
     * Ensures the connected flag is reset. The actual connection
     * cleanup is handled by the TCP client base class.
     */
    ~Database() {
        is_connected_ = false;
    }

    /**
     * @struct connect_awaiter
     * @brief Coroutine awaiter for `co_await db.connect()` (see also `run_sync` in tests).
     */
    struct connect_awaiter {
        Database<QB_IO_, NotifyDerived> &db;
        qb::duration                     timeout{};
        std::shared_ptr<bool>            valid{std::make_shared<bool>(true)};

        explicit connect_awaiter(Database<QB_IO_, NotifyDerived> &d, qb::duration t = qb::duration::zero()) noexcept
            : db(d)
            , timeout(t) {}

        ~connect_awaiter() {
            if (valid)
                *valid = false;
        }

        connect_awaiter(connect_awaiter const &)            = delete;
        connect_awaiter &operator=(connect_awaiter const &) = delete;
        connect_awaiter(connect_awaiter &&)                 = default;
        connect_awaiter &operator=(connect_awaiter &&)      = default;

        [[nodiscard]] bool
        await_ready() const noexcept {
            return db.is_connected_;
        }

        void
        await_suspend(std::coroutine_handle<> h) {
            db.start_connect_from_awaiter(h, valid, timeout);
        }

        [[nodiscard]] bool
        await_resume() const noexcept {
            return db.is_connected_;
        }
    };

    /**
     * @brief Start async connection (`co_await` or `run_sync(db.connect())`).
     */
    [[nodiscard]] connect_awaiter
    connect() {
        return connect_awaiter{*this, qb::duration::zero()};
    }

    /** @brief Same as connect() with an explicit timeout override. */
    [[nodiscard]] connect_awaiter
    connect(qb::duration timeout) {
        return connect_awaiter{*this, timeout};
    }

    /** @brief Parse connection string then connect. */
    [[nodiscard]] connect_awaiter
    connect(std::string const &conn_opts) {
        conn_opts_ = connection_options::parse(conn_opts);
        return connect();
    }

    /**
     * @brief Connect with a fully-specified options struct.
     *
     * The connection string carries only `user`/`password`/`host:port`/`database`; use
     * this overload to also set the fields that are NOT expressible in the string —
     * notably `ssl_verify` (TLS verification level), `connect_timeout`, and the keepalive
     * settings. Typical: `auto o = connection_options::parse(dsn); o.ssl_verify =
     * ssl_verify_mode::full; co_await db.connect(o);`.
     */
    [[nodiscard]] connect_awaiter
    connect(connection_options opts) {
        conn_opts_ = std::move(opts);
        return connect();
    }

    /** @brief Use an existing transport channel (e.g. pool) then handshake. */
    [[nodiscard]] connect_awaiter
    connect(std::string const &conn_opts, typename QB_IO_::transport_io_type &&raw_io) {
        conn_opts_        = connection_options::parse(conn_opts);
        this->transport() = std::move(raw_io);
        return connect();
    }

    /**
     * @brief Set a client-supplied startup option sent in the StartupMessage (and re-sent on
     *        reconnect), e.g. `application_name`, `search_path`, `client_encoding`, `datestyle`.
     *
     * Must be called BEFORE `connect()` — the option set is serialized at handshake time. Only
     * GUCs settable as startup parameters are valid (server-reported read-only parameters such as
     * `server_version` are NOT echoed back; see @ref on_parameter_status). Returns `*this` for
     * chaining. The value is sent verbatim; the caller is responsible for a valid GUC value.
     */
    Database &
    set_startup_option(std::string key, std::string value) {
        client_opts_[std::move(key)] = std::move(value);
        return *this;
    }

    /** @brief Convenience for `set_startup_option("application_name", name)` (shown in
     *  `pg_stat_activity.application_name`). Call before `connect()`. */
    Database &
    application_name(std::string name) {
        return set_startup_option(std::string(options::APPLICATION_NAME), std::move(name));
    }

    /** @brief The client-supplied startup options registered so far (read-only). */
    [[nodiscard]] const client_options_type &
    startup_options() const noexcept {
        return client_opts_;
    }

    /**
     * @brief Register a handler for asynchronous NOTIFY (plain `database` only).
     *
     * Ignored for `notify_*_consumer` types (they use `on_notify` / `receive()`). Replaces any
     * previous handler. Without a handler, NOTIFY is only logged.
     */
    Database<QB_IO_, NotifyDerived> &
    on_incoming_notify(std::function<void(::qb::pg::notification &&)> fn) {
        if constexpr (std::is_same_v<NotifyDerived, void>)
            inbound_notify_handler_ = std::move(fn);
        return *this;
    }

    /**
     * @brief Enable TCP keepalive for the connection (P1-1)
     *
     * Configures TCP keepalive parameters to detect dead connections.
     * Must be called after connect() for the settings to take effect.
     *
     * @param interval Seconds between keepalive probes (0 = disable)
     * @param idle Seconds of idle time before starting probes
     * @param probes Number of unanswered probes before considering dead
     */
    void
    enable_keepalive(int interval, int idle = 60, int probes = 3) {
        conn_opts_.keepalive_interval = interval;
        conn_opts_.keepalive_idle     = idle;
        conn_opts_.keepalive_probes   = probes;

        if (is_connected_) {
            apply_keepalive_settings();
        }
    }

    /**
     * @brief A new query may only be enqueued while the handle is connected.
     *
     * Overrides Transaction::is_connection_usable() so the coroutine query/execute entry
     * points fail fast on a disconnected handle instead of enqueuing a command that can
     * never be sent (which would hang the caller's awaiter). `is_connected_` is cleared
     * synchronously by disconnect() and by on(disconnected).
     */
    [[nodiscard]] bool
    is_connection_usable() const noexcept override {
        return is_connected_;
    }

    /**
     * @brief Check if the connection is alive (P1-1)
     *
     * Performs a lightweight check to determine if the connection
     * is still active. Uses socket error state if available.
     *
     * @return true if connection appears healthy, false otherwise
     */
    bool
    is_connection_alive() const {
        if (!is_connected_) {
            return false;
        }

        // Check socket error state
        int       error_code = 0;
        socklen_t len        = sizeof(error_code);
        auto      sock_fd    = this->transport().native_handle();

        if (getsockopt(sock_fd, SOL_SOCKET, SO_ERROR,
#ifdef _WIN32
                       reinterpret_cast<char *>(&error_code),
#else
                       &error_code,
#endif
                       &len)
            < 0) {
            return false; // getsockopt failed
        }

        return error_code == 0;
    }

    /**
     * @brief Whether the SCRAM authentication was bound to the TLS channel.
     * @return true if the handshake negotiated **SCRAM-SHA-256-PLUS** with
     *         `tls-server-end-point` channel binding (only possible over TLS, when the
     *         server offers the `-PLUS` mechanism); false for plain SCRAM-SHA-256,
     *         cleartext/MD5 auth, or a `trust` connection.
     * @details Channel binding ties the SCRAM proof to this specific TLS certificate,
     *          so even an active man-in-the-middle holding valid credentials cannot
     *          relay the authentication onto a different channel.
     */
    [[nodiscard]] bool
    used_channel_binding() const noexcept {
        return _gs2_header.rfind("p=", 0) == 0;
    }

    /**
     * @brief Whether the backend session is currently inside a transaction block.
     * @return true if the last ReadyForQuery reported `T` (in a block) or `E` (failed
     *         block); false when idle (`I`). Reflects real server state.
     */
    [[nodiscard]] bool
    in_transaction() const noexcept {
        return _txn_status == 'T' || _txn_status == 'E';
    }

    /**
     * @brief Value of a server `ParameterStatus` report (libpq `PQparameterStatus`).
     * @param key e.g. "server_version", "server_encoding", "client_encoding", "TimeZone",
     *        "integer_datetimes", "standard_conforming_strings", "application_name".
     * @return The reported value, or std::nullopt if the server never sent that key.
     *         The view is valid while this connection is alive.
     */
    [[nodiscard]] std::optional<std::string_view>
    parameter_status(std::string_view key) const {
        const auto it = server_params_.find(std::string(key));
        if (it == server_params_.end())
            return std::nullopt;
        return std::string_view{it->second};
    }

    /**
     * @brief Backend process id captured at connect (BackendKeyData); libpq `PQbackendPID`.
     * @return The server-side backend PID, or 0 if not connected.
     */
    [[nodiscard]] int
    backend_pid() const noexcept {
        return static_cast<int>(serverPid_);
    }

    /**
     * @brief Server version as a libpq-style integer (`PQserverVersion`): 16.2 -> 160002,
     *        9.6.24 -> 90624. Returns 0 if the `server_version` parameter is unknown.
     */
    [[nodiscard]] int
    server_version() const {
        const auto it = server_params_.find("server_version");
        if (it == server_params_.end())
            return 0;
        // Faithful, locale-free replacement for sscanf("%d.%d.%d"): n is the count of
        // dot-separated integer fields actually parsed (major required, minor/patch
        // optional, scan stops at the first missing field), exactly like sscanf's return.
        int                    major = 0, minor = 0, patch = 0, n = 0;
        const std::string_view sv  = it->second;
        std::size_t            pos = 0, used = 0;
        if (const auto a = qb::to_number_prefix<int>(sv.substr(pos), &used)) {
            major = *a;
            pos += used;
            n = 1;
            if (pos < sv.size() && sv[pos] == '.') {
                ++pos;
                if (const auto b = qb::to_number_prefix<int>(sv.substr(pos), &used)) {
                    minor = *b;
                    pos += used;
                    n = 2;
                    if (pos < sv.size() && sv[pos] == '.') {
                        ++pos;
                        if (const auto c = qb::to_number_prefix<int>(sv.substr(pos), &used)) {
                            patch = *c;
                            n     = 3;
                        }
                    }
                }
            }
        }
        if (n >= 2 && major >= 10)
            return major * 10000 + minor; // modern scheme: major.minor
        if (n >= 3)
            return major * 10000 + minor * 100 + patch; // legacy (< 10): major.minor.patch
        if (n >= 1)
            return major * 10000;
        return 0;
    }

    /**
     * @brief Stream the result of a `COPY … TO STDOUT` to a sink (coroutine).
     *
     * Runs @p sql (which must be a `COPY <table-or-query> TO STDOUT [...]`) and delivers
     * each `CopyData` payload to @p sink **as it arrives** — the rows are never buffered
     * in a result set, so arbitrarily large exports stream in constant memory. The
     * payload bytes are the COPY wire bytes in the requested format (text/CSV: one row
     * per chunk ending in `\n`; binary: opaque framed chunks). The `string_view` is valid
     * only for the duration of the call; copy what you need.
     *
     * @param sql  A `COPY … TO STDOUT` statement.
     * @param sink Invoked once per `CopyData` chunk.
     * @return `Reply<resultset>` — `ok()` on success (the result set is empty; the data
     *         went to @p sink), or the server error.
     *
     * @code
     * std::string out;
     * co_await db.copy_out("COPY users TO STDOUT (FORMAT csv)",
     *                      [&](std::string_view chunk){ out.append(chunk); });
     * @endcode
     */
    [[nodiscard]] qb::io::async::task<qb::pg::Reply<resultset>>
    copy_out(std::string sql, std::function<void(std::string_view)> sink) {
        _copy_out_sink = std::move(sink);
        // RAII clear: also runs if the awaiting coroutine frame is destroyed mid-await
        // (cancellation / task drop), so the connection never keeps a stale sink whose
        // captures point at the torn-down frame.
        auto guard = qb::scope_guard([this] { _copy_out_sink = nullptr; });
        co_return co_await this->execute(std::string_view{sql});
    }

    /**
     * @brief Bulk-load via `COPY … FROM STDIN` from a streaming source (coroutine).
     *
     * Runs @p sql (a `COPY <table> FROM STDIN [...]`) and feeds the server the bytes
     * produced by @p source: it is called repeatedly and each returned chunk is sent as
     * a `CopyData` message until it returns `std::nullopt`, then `CopyDone` ends the load.
     * The chunk bytes must be in the COPY wire format the statement selects (text/CSV:
     * complete rows ending in `\n`; binary: the framed binary stream). A chunk does not
     * have to align to row boundaries for text/CSV — the server reassembles the stream.
     * If @p source throws, the COPY is aborted with `CopyFail` and the reply is an error.
     *
     * @return `Reply<resultset>` — `ok()` on success (`COPY n` rows loaded), else the error.
     *
     * @code
     * co_await db.copy_in("COPY t (id, v) FROM STDIN",
     *     [&]() -> std::optional<std::string> { return next_line(); });   // nullopt to finish
     * @endcode
     */
    [[nodiscard]] qb::io::async::task<qb::pg::Reply<resultset>>
    copy_in(std::string sql, std::function<std::optional<std::string>()> source) {
        _copy_in_source = std::move(source);
        auto guard      = qb::scope_guard([this] { _copy_in_source = nullptr; }); // see copy_out
        co_return co_await this->execute(std::string_view{sql});
    }

    /** @brief `COPY … FROM STDIN` convenience: send the whole payload in one shot. */
    [[nodiscard]] qb::io::async::task<qb::pg::Reply<resultset>>
    copy_in(std::string sql, std::string data) {
        co_return co_await copy_in(std::move(sql), [d = std::move(data), sent = false]() mutable -> std::optional<std::string> {
            if (sent)
                return std::nullopt;
            sent = true;
            return std::move(d);
        });
    }

    /**
     * @brief Stream a large query result row-by-row via a server-side cursor (coroutine).
     *
     * Runs @p sql through a server-side `CURSOR`, fetching @p batch_size rows per round
     * trip and invoking @p on_row for each row **as the batches arrive** — only one batch
     * is ever held in memory, so an arbitrarily large result set is processed in constant
     * memory (a plain `query()` buffers the whole result set).
     *
     * Cursors require a transaction: when the connection is idle this opens its own
     * (`BEGIN` … `COMMIT`, or `ROLLBACK` on failure); when it is already inside a
     * transaction the cursor is declared there and only the cursor is closed. If @p on_row
     * throws, the cursor is closed (and a self-opened transaction rolled back) and the
     * exception is rethrown.
     *
     * @param sql        The query to stream (a `SELECT`, typically).
     * @param batch_size Rows per `FETCH` (clamped to ≥ 1).
     * @param on_row     Invoked per row with a `row` view valid only during the call.
     * @return `Reply<void>` — `ok()` once the whole result streamed, else the server error.
     *
     * @code
     * std::uint64_t n = 0;
     * co_await db.query_stream("SELECT * FROM huge", 1000, [&](auto row){ ++n; });
     * @endcode
     */
    template <typename RowFn>
    [[nodiscard]] qb::io::async::task<qb::pg::Reply<void>>
    query_stream(std::string sql, std::size_t batch_size, RowFn on_row) {
        if (batch_size == 0)
            batch_size = 1;
        const bool        owns_txn    = !in_transaction();
        const std::string declare_sql = "DECLARE qb_stream_cursor CURSOR FOR " + sql;
        const std::string fetch_sql   = "FETCH " + std::to_string(batch_size) + " FROM qb_stream_cursor";
        const std::string close_sql   = "CLOSE qb_stream_cursor";

        if (owns_txn) {
            auto b = co_await this->begin();
            if (!b.ok())
                co_return qb::pg::Reply<void>::failure(b.error());
        }
        auto declared = co_await this->execute(std::string_view{declare_sql});
        if (!declared.ok()) {
            if (owns_txn)
                (void) co_await this->rollback();
            co_return qb::pg::Reply<void>::failure(declared.error());
        }

        bool                    failed = false;
        qb::pg::error::db_error err{"unknown error"};
        std::exception_ptr      user_exc;
        for (;;) {
            auto batch = co_await this->execute(std::string_view{fetch_sql});
            if (!batch.ok()) {
                err    = batch.error();
                failed = true;
                break;
            }
            const auto       &rs = batch.result();
            const std::size_t n  = rs.size();
            try {
                for (const auto &r : rs)
                    on_row(r);
            } catch (...) {
                user_exc = std::current_exception();
                failed   = true;
            }
            if (user_exc || n < batch_size)
                break; // user threw, or short batch -> cursor exhausted
        }

        (void) co_await this->execute(std::string_view{close_sql}); // best-effort
        if (owns_txn) {
            if (failed) {
                (void) co_await this->rollback();
            } else {
                auto c = co_await this->commit();
                if (!c.ok()) {
                    failed = true;
                    err    = c.error();
                }
            }
        }

        if (user_exc)
            std::rethrow_exception(user_exc);
        if (failed)
            co_return qb::pg::Reply<void>::failure(err);
        co_return qb::pg::Reply<void>::success();
    }

    /**
     * @brief Request cancellation of the query currently running on this connection.
     *
     * Sends a PostgreSQL CancelRequest out-of-band on a short-lived SEPARATE
     * connection, using the backend process id + secret key captured at connect time
     * (BackendKeyData). The server aborts the in-flight query on the main connection,
     * which surfaces to the awaiting caller as an error with SQLSTATE 57014
     * (`sqlstate::query_canceled`). Mirrors libpq's `PQcancel`: a synchronous,
     * best-effort control op (the request packet is 16 bytes, no reply).
     *
     * @return true if the CancelRequest was delivered; false if no backend key is
     *         known yet (never connected) or the out-of-band socket failed. A false
     *         return only means the request could not be sent — not that the query
     *         survived.
     *
     * @note The cancel connection is plaintext even when the main connection is SSL
     *       (the request carries no secret beyond the per-connection cancel key). A
     *       server that mandates SSL on every connection will reject it; SSL-tunneled
     *       cancellation is a future enhancement (tracked with sslmode/verify-full).
     */
    bool
    cancel() {
        if (serverPid_ == 0)
            return false; // never received BackendKeyData -> nothing to cancel

        // CancelRequest, 16 bytes, all big-endian:
        //   int32 length = 16
        //   int32 request code = 80877102 (0x04D2162E)
        //   int32 backend process id
        //   int32 backend secret key
        std::array<std::uint8_t, 16> pkt{};
        const std::uint32_t          len  = htonl(16u);
        const std::uint32_t          code = htonl(80877102u);
        const std::uint32_t          pid  = htonl(static_cast<std::uint32_t>(serverPid_));
        const std::uint32_t          key  = htonl(static_cast<std::uint32_t>(serverSecret_));
        std::memcpy(pkt.data() + 0, &len, 4);
        std::memcpy(pkt.data() + 4, &code, 4);
        std::memcpy(pkt.data() + 8, &pid, 4);
        std::memcpy(pkt.data() + 12, &key, 4);

        // cancel() is synchronous (like libpq's PQcancel) and is typically fired from a
        // timer ON the event loop, so the blocking connect+send must NOT stall the loop
        // for the full connect_timeout. Cap it tightly (≤ 2s): the cancel targets the
        // SAME already-reachable endpoint as the live connection, so the handshake is
        // normally sub-millisecond; the cap only bounds the pathological unreachable case.
        const qb::duration cfg   = conn_opts_.connect_timeout > qb::duration::zero()
                                       ? conn_opts_.connect_timeout
                                       : std::chrono::duration_cast<qb::duration>(std::chrono::seconds(10));
        const qb::duration t_out = std::min(cfg, std::chrono::duration_cast<qb::duration>(std::chrono::seconds(2)));

        // Plain TCP / unix socket to the same endpoint (scheme resolved by tcp::socket;
        // an ssl:// endpoint connects plaintext here — the TLS layer is skipped).
        qb::io::tcp::socket sock;
        if (sock.connect(qb::io::uri{conn_opts_.schema + "://" + conn_opts_.uri}, t_out) != 0)
            return false;
        const int n = qb::io::socket::send_n(sock.native_handle(), pkt.data(), static_cast<int>(pkt.size()), t_out);
        sock.disconnect();
        return n == static_cast<int>(pkt.size());
    }

private:
    /**
     * @brief Apply keepalive settings to the socket (P1-1)
     */
    void
    apply_keepalive_settings() {
        if (conn_opts_.keepalive_interval <= 0) {
            return; // Keepalive disabled
        }

        auto sock_fd = this->transport().native_handle();
#ifdef _WIN32
        if (sock_fd == INVALID_SOCKET) {
            return;
        }
#else
        if (sock_fd < 0) {
            return;
        }
#endif

        // Enable TCP keepalive (Winsock: option value is const char*)
        int optval = 1;
        if (setsockopt(sock_fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&optval), static_cast<int>(sizeof(optval))) < 0) {
            LOG_WARN("[pgsql] Failed to enable TCP keepalive");
            return;
        }

#ifdef TCP_KEEPIDLE
        // Seconds idle before probing (Linux)
        optval = conn_opts_.keepalive_idle;
        setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPIDLE, reinterpret_cast<const char *>(&optval), static_cast<int>(sizeof(optval)));
#endif

#ifdef TCP_KEEPINTVL
        // Seconds between probes (Linux)
        optval = conn_opts_.keepalive_interval;
        setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPINTVL, reinterpret_cast<const char *>(&optval), static_cast<int>(sizeof(optval)));
#endif

#ifdef TCP_KEEPCNT
        // Number of probes (Linux)
        optval = conn_opts_.keepalive_probes;
        setsockopt(sock_fd, IPPROTO_TCP, TCP_KEEPCNT, reinterpret_cast<const char *>(&optval), static_cast<int>(sizeof(optval)));
#endif

        LOG_INFO("[pgsql] TCP keepalive enabled: idle=" << conn_opts_.keepalive_idle << "s, interval=" << conn_opts_.keepalive_interval
                                                        << "s, probes=" << conn_opts_.keepalive_probes);
    }

public:
    /**
     * @brief Message handler callback
     *
     * Called by the protocol handler when a complete message is received.
     * Routes the message to the appropriate handler method based on its tag.
     * This is a key part of the event-driven architecture of the client.
     *
     * @param msg Protocol message to be processed
     */
    void
    on(typename pg_protocol::message msg) {
        const auto it = routes_.find(msg->tag());
        if (qb::likely(it != routes_.end()))
            (this->*(it->second))(*msg);
        else
            on_unhandled_message(*msg);
    }

    /**
     * @brief Disconnection handler
     *
     * Called when the connection to the database server is lost.
     * Updates the connection state and raises an error for any pending queries.
     *
     * @param ev Disconnection event
     */
    void
    on(qb::io::async::event::disconnected const &ev) {
        if (connect_coroutine_pending_) {
            connect_handshake_failed_ = true;
            _error                    = error::client_error("database disconnected");
            try_resume_connect_wait();
        }
        if (is_connected_) {
            is_connected_ = false;
            on_error_query(error::client_error("database disconnected"));
        }
        // The backend cancel key is per-connection; drop it so a post-disconnect
        // cancel() can't address a recycled PID on the server.
        serverPid_    = 0;
        serverSecret_ = 0;
        // on_error_query() only fails the single in-flight query. Fail every
        // query still queued behind it (pipelined / multi-statement / pending
        // sub-transactions) so their callers' coroutine awaiters resume with the
        // failure instead of hanging forever.
        root_transaction()->fail_all_pending(error::client_error("database disconnected"));
        _current_command = root_transaction();
        _current_query   = nullptr;
        _ready_for_query = false;
        if constexpr (!std::is_same_v<NotifyDerived, void>) {
            if constexpr (requires {
                              std::declval<NotifyDerived &>().on_pg_notify_consumer_disconnected(
                                  std::declval<qb::io::async::event::disconnected const &>());
                          }) {
                static_cast<NotifyDerived *>(this)->on_pg_notify_consumer_disconnected(ev);
            }
        }
    }

    /**
     * @brief Reset async I/O state after disconnect() so this client can connect() again
     *
     * `disconnect()` marks the underlying `qb::io::async::io` layer disposed; a new TCP/TLS
     * handshake must not start until `reset_io_state()` runs. Call `prepare_reconnect()`,
     * then `co_await connect()` or `run_sync(connect(...))` as usual.
     *
     * @pre No pending queries on this connection (finish or drain the transaction queue first).
     */
    void
    prepare_reconnect() noexcept {
        ++connect_timer_generation_;
        // Transport is a private base of `tcp::client`; use public `in`/`out`/`transport`.
        this->in().reset();
        this->out().reset();
        // `tcp::socket::disconnect()` is shutdown-only; the fd stays open. The next
        // `n_connect()` path requires a closed socket so `init()` opens a new fd.
        this->transport().close();
        this->reset_io_state();
        is_connected_              = false;
        connect_handshake_failed_  = false;
        connect_coroutine_pending_ = false;
        connect_suspend_handle_    = {};
        connect_suspend_valid_.reset();
        serverPid_       = 0;
        serverSecret_    = 0;
        server_params_.clear(); // server ParameterStatus is per-backend; drop the stale cache
        _error           = error::db_error{"unknown error"};
        _current_command = root_transaction();
        _current_query   = nullptr;
        _ready_for_query = false;
    }

    /**
     * @brief Explicitly disconnects from the database
     *
     * Closes the connection to the PostgreSQL server and runs the event loop once
     * to process any pending disconnection events. This ensures a clean shutdown
     * of the database connection.
     */
    void
    disconnect() {
        // Mark the handle down and fail any in-flight / queued query SYNCHRONOUSLY rather
        // than relying on an async on(disconnected) event — a *local* disconnect may not
        // deliver one, leaving is_connected_ true and those queries' coroutine awaiters
        // unresolved (hanging) forever. Clearing is_connected_ here also makes the
        // is_connection_usable() guard reject any query submitted after disconnect().
        // on(disconnected), if it fires later, is a no-op (its `if (is_connected_)` guard
        // is already false and fail_all_pending drains an empty queue).
        if (is_connected_) {
            is_connected_ = false;
            on_error_query(error::connection_error("database disconnected by client"));
            root_transaction()->fail_all_pending(error::connection_error("database disconnected by client"));
            _current_command = root_transaction();
        }
        static_cast<qb::io::async::tcp::client<Database<QB_IO_, NotifyDerived>, QB_IO_, void> &>(*this).disconnect();
        // Same rationale as `Redis::await()` / `Transaction::await()`: may run from a
        // coroutine or nested I/O path where `async::run()` would throw.
        qb::io::async::listener::current.run(EVRUN_NOWAIT);
    }
};

/**
 * @brief CRTP base for LISTEN/NOTIFY consumers (mirrors `RedisConsumer` + derived).
 *
 * `Derived` must implement `deliver_pg_notify(::qb::pg::notification &&)` and inherit this class as
 * `notify_consumer<QB_IO_, Derived>` (see `notify_co_consumer`).
 */
template <typename QB_IO_, typename Derived>
class notify_consumer : public Database<QB_IO_, Derived> {
public:
    notify_consumer()
        : Database<QB_IO_, Derived>() {}

    explicit notify_consumer(std::string const &connection_opts)
        : Database<QB_IO_, Derived>(connection_opts) {}

    void
    consume_pg_notify(::qb::pg::notification &&n) {
        static_cast<Derived *>(this)->deliver_pg_notify(std::move(n));
    }
};

/**
 * @brief LISTEN/NOTIFY consumer: optional callback + `co_await receive()` queue (mirrors Redis
 * `cb_consumer` / `co_consumer` in one type — PostgreSQL allows normal queries on the same link).
 */
template <typename QB_IO_>
class notify_co_consumer : public notify_consumer<QB_IO_, notify_co_consumer<QB_IO_>> {
    using base_type = notify_consumer<QB_IO_, notify_co_consumer<QB_IO_>>;

    static constexpr std::size_t default_notify_channel_capacity = 8192;

    qb::io::async::channel<::qb::pg::notification> notify_channel_{default_notify_channel_capacity};
    std::function<void(::qb::pg::notification &&)> on_notify_dropped_{};
    std::function<void(::qb::pg::notification &&)> on_notify_callback_{};

public:
    notify_co_consumer()
        : base_type() {}

    explicit notify_co_consumer(std::string const &opts, std::size_t notify_capacity = default_notify_channel_capacity)
        : base_type(opts)
        , notify_channel_(notify_capacity) {}

    /**
     * @brief Optional callback invoked for each NOTIFY before the message is queued for `receive()`.
     */
    notify_co_consumer<QB_IO_> &
    on_notify(std::function<void(::qb::pg::notification &&)> cb) {
        on_notify_callback_ = std::move(cb);
        return *this;
    }

    void
    deliver_pg_notify(::qb::pg::notification &&n) {
        if (on_notify_callback_) {
            try {
                on_notify_callback_(::qb::pg::notification{n});
            } catch (std::exception const &ex) {
                LOG_WARN("[pgsql] notify_co_consumer on_notify callback error: " << ex.what());
            }
        }
        if (notify_channel_.try_send(std::move(n)))
            return;
        if (on_notify_dropped_) {
            try {
                on_notify_dropped_(std::move(n));
            } catch (std::exception const &ex) {
                LOG_WARN("[pgsql] notify_co_consumer on_notify_dropped error: " << ex.what());
            }
        } else {
            LOG_WARN("[pgsql] notify_co_consumer: notification dropped (buffer full)");
        }
    }

    void
    on_pg_notify_consumer_disconnected(qb::io::async::event::disconnected const &) {
        notify_channel_.close();
    }

    notify_co_consumer<QB_IO_> &
    on_notify_dropped(std::function<void(::qb::pg::notification &&)> cb) {
        on_notify_dropped_ = std::move(cb);
        return *this;
    }

    [[nodiscard]] std::size_t
    notify_channel_capacity() const noexcept {
        return notify_channel_.capacity();
    }

    /**
     * @brief Await the next NOTIFY; `std::nullopt` when the channel is closed (e.g. disconnect).
     *
     * Implemented as a direct coroutine member (NOT an immediately-invoked lambda
     * `[this]{...}()`): the lambda closure would be a temporary destroyed at the end
     * of this call, leaving the coroutine frame referencing freed memory for `this`
     * (dangling-closure UAF — ASan-blind stack corruption; crashes under some
     * compilers' frame layouts). The consumer object owns the coroutine's `this`.
     */
    [[nodiscard]] qb::io::async::task<std::optional<::qb::pg::notification>>
    receive() {
        co_return co_await notify_channel_.recv();
    }

    ~notify_co_consumer() {
        notify_channel_.close();
    }
};

/** @brief Same type as `notify_co_consumer` (Redis-style name for callback-first usage via
 * `on_notify`). */
template <typename QB_IO_>
using notify_cb_consumer = notify_co_consumer<QB_IO_>;

} // namespace detail

/**
 * @brief Type alias for database with custom I/O handler
 *
 * Provides a convenient alias for creating database clients with
 * different I/O handlers. This allows for flexibility in choosing
 * the networking implementation while maintaining a consistent API.
 *
 * @tparam QB_IO_ I/O handler type that provides networking capabilities
 */
template <typename QB_IO_>
using database = detail::Database<QB_IO_, void>;

/**
 * @brief Type alias for transaction base class
 *
 * Provides a convenient alias for the transaction base class.
 * Use this type to work with database transactions including
 * begin, commit, rollback, and savepoint operations.
 */
using transaction = detail::Transaction;

/**
 * @brief Type alias for query result set
 *
 * Provides a convenient alias for working with query results.
 * The result set contains the rows and columns returned by a query.
 */
using results = detail::resultset;

/**
 * @brief Type alias for query parameters
 *
 * Provides a convenient alias for binding parameters to prepared statements.
 * Parameters can be bound by position or name, depending on the query style.
 */
using params = detail::QueryParams;

/**
 * @brief No-op success handler for callback-style `execute` / `prepare` when chaining with
 * `.await()`.
 */
struct discard_query_results_t {
    void
    operator()(detail::Transaction &, results) const noexcept {}
};

struct discard_error_t {
    void
    operator()(error::db_error const &) const noexcept {}
};

struct discard_prepare_t {
    void
    operator()(detail::Transaction &, detail::PreparedQuery const &) const noexcept {}
};

inline constexpr discard_query_results_t discard_query{};
inline constexpr discard_error_t         discard_error{};
inline constexpr discard_prepare_t       discard_prepare{};

/**
 * @brief Coroutine awaiter for `Reply<T>` (no-callback overloads of `execute`, `prepare`, …).
 */
template <typename T>
using pg_reply_awaiter = detail::pg_reply_awaiter<T>;

/**
 * @brief TCP transport namespace
 *
 * Contains database clients that use TCP transport for PostgreSQL communication.
 */
struct tcp {
    /**
     * @brief Database client with plain TCP transport
     *
     * Database client implementation using unencrypted TCP connections.
     * Suitable for local networks or when using an external encryption layer.
     */
    using database = detail::Database<qb::io::transport::tcp, void>;

    /**
     * @brief LISTEN/NOTIFY consumer with callback delivery (see `redis::tcp::cb_consumer`).
     */
    using notify_cb_consumer = detail::notify_cb_consumer<qb::io::transport::tcp>;

    /**
     * @brief LISTEN/NOTIFY consumer with `co_await receive()` (see `redis::tcp::co_consumer`).
     */
    using notify_co_consumer = detail::notify_co_consumer<qb::io::transport::tcp>;
#ifdef QB_HAS_SSL
    /**
     * @brief SSL transport namespace
     *
     * Contains database clients that use SSL/TLS encrypted transport
     * for secure communication with PostgreSQL servers.
     */
    struct ssl {
        /**
         * @brief Database client with SSL transport
         *
         * Database client implementation using SSL/TLS encrypted connections.
         * Recommended for production environments and connections over
         * public networks for enhanced security.
         */
        using database = detail::Database<qb::io::transport::stcp, void>;

        using notify_cb_consumer = detail::notify_cb_consumer<qb::io::transport::stcp>;
        using notify_co_consumer = detail::notify_co_consumer<qb::io::transport::stcp>;
    };
#endif
};

} // namespace qb::pg

#include "./src/with_transaction.h"

namespace qb::allocator {

/**
 * @brief Specialization for pipe allocation with PostgreSQL messages
 *
 * Allows PostgreSQL messages to be allocated with the pipe allocator.
 * This specialization enables efficient memory management for message processing.
 *
 * @param message PostgreSQL message to allocate
 * @return pipe<char>& Reference to the pipe containing the allocated message
 */
template <>
pipe<char> &pipe<char>::put<qb::pg::detail::message>(const qb::pg::detail::message &);

} // namespace qb::allocator
