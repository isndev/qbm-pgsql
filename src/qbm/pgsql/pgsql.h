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

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <qb/io/async.h>
#include <qb/io/async/tcp/connector.h>
#include <qb/io/crypto.h>
#include <qb/io/reconnect.h>
#ifdef QB_HAS_SSL
#include <qb/io/tcp/ssl/socket.h>
#endif
#include <qb/system/allocator/pipe.h>
#include <qb/system/cpu.h>   // qb::scope_guard
#include <qb/system/parse.h> // qb::to_number (locale-free, non-throwing)

// P1-1: Socket includes for keepalive support
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include "./commands.h"
#include "./pg_reply.h"
#include "./transaction.h"

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
        if (!message_) {
            if (in.size() < header_size)
                return 0; // header incomplete
        } else if (in.size() <= offset_) {
            return 0; // no new body bytes
        }

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
                QB_LOG_CRIT("[pgsql] Invalid wire message length " << wire_len << " (must be 4.." << qb::pg::PG_PROTOCOL_MAX_MESSAGE_BYTES
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
            QB_LOG_CRIT("[pgsql] exception in message handler, dropping connection: " << e.what());
            this->not_ok();
        } catch (...) {
            QB_LOG_CRIT("[pgsql] unknown exception in message handler, dropping connection");
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
 * @brief The reconnection backoff: qb-io's shared `qb::io::reconnect_policy` (`qb/io/reconnect.h`).
 *
 * The same type as `qb::redis::RetryPolicy` and `qb::http::RetryPolicy` (Huly QB-111): exponential backoff
 * from `initial_delay` by `multiplier` up to `max_delay`, jittered by a quarter either way, `max_attempts`
 * per run (-1 = unlimited), `on_retry(attempt, next_delay)` before each wait. Its `connect_timeout` bounds
 * each attempt -- the transport and the whole handshake; zero (the default) means the connection's own
 * `connect_timeout`, else 10 s. Used by `connect_with_retry()` and `enable_auto_reconnect()`.
 */
using RetryPolicy = ::qb::io::reconnect_policy;

/**
 * @brief What a `qb::pg::notification` carries (Huly QB-111).
 */
enum class notification_kind : std::uint8_t {
    notify, ///< a NOTIFY the server delivered: `server_backend_pid`, `channel` and `payload` are set
    gap,    ///< a gap marker: notifications were lost at this point of the stream; `gap` says why and how many
};

/**
 * @brief Asynchronous payload from PostgreSQL NOTIFY (after LISTEN on the same or another session).
 *
 * Delivered on the I/O thread when a `NotificationResponse` is received; see
 * `tcp::notify_cb_consumer` / `tcp::notify_co_consumer`, or `database::on_incoming_notify`. In a
 * `notify_co_consumer`'s stream an element can also be a gap marker (`kind == notification_kind::gap`):
 * PostgreSQL does not replay the NOTIFYs a session missed, so the stream says where it has a hole --
 * after a reconnection (`gap.reason == qb::io::gap_reason::reconnect`) or where the consumer's own queue
 * was full (`overflow`, `gap.lost` notifications dropped) -- and the reader resynchronises from its
 * source of truth.
 */
struct notification {
    int                server_backend_pid{};
    std::string        channel;
    std::string        payload;
    notification_kind  kind = notification_kind::notify; ///< a NOTIFY, or a gap marker (Huly QB-111)
    ::qb::io::gap_info gap{};                            ///< why and how much was lost; meaningful when `kind == gap`
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

template <typename QB_IO_, typename NotifyDerived>
class Database;

/**
 * @brief What a connection shares with the handshake sessions it hands out (Huly QB-111).
 *
 * Held by `shared_ptr`, so a session copied out of its hook and kept past the connection reads a gate
 * that says "over" instead of a freed connection.
 */
struct handshake_gate {
    std::uint64_t live      = 0;     ///< the id of the handshake writing on the link now; 0: none
    bool          admitting = false; ///< a session is queueing a query: the connection admits it while not ready
};

/**
 * @brief The handshake hook's write handle (`database::set_handshake`, Huly QB-111).
 *
 * While a connection is not `ready` it refuses every query submitted through it (`connection is not
 * established`), so a hook cannot use the connection itself; it is handed this session instead. The
 * session queues on the connection's link only while the handshake it was made for runs: a copy kept
 * past the hook, or used after the link dropped, fails every query with "handshake over" -- no other
 * coroutine can slip a query past the gate while the hook is suspended. Its queries run before any user
 * query of the new session, in the order they are awaited.
 */
class handshake_session {
public:
    /** @brief Simple-query protocol (`Reply<resultset>`), like `database::execute(sql)`. */
    [[nodiscard]] pg_reply_awaiter<resultset>
    execute(std::string_view sql) {
        return admitted<resultset>([sql = std::string(sql)](Transaction &root, pg_coro_complete<resultset> complete) {
            root.execute(
                std::string_view(sql),
                [complete](Transaction &, resultset rs) mutable { complete(::qb::pg::Reply<resultset>::success(rs.deep_snapshot())); },
                [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<resultset>::failure(e)); });
        });
    }

    /** @brief Same as `execute(sql)`. */
    [[nodiscard]] pg_reply_awaiter<resultset>
    query(std::string_view sql) {
        return execute(sql);
    }

    /**
     * @brief Prepare a named statement on the new session (`Reply<PreparedQuery>`), like
     *        `database::prepare(name, sql, types)` -- the statements a session needs are prepared here.
     */
    [[nodiscard]] pg_reply_awaiter<PreparedQuery>
    prepare(std::string_view name, std::string_view sql, type_oid_sequence types = {}) {
        return admitted<PreparedQuery>([n = std::string(name), q = std::string(sql),
                                        t = std::move(types)](Transaction &root, pg_coro_complete<PreparedQuery> complete) mutable {
            root.prepare(
                std::string_view(n), std::string_view(q), std::move(t),
                [complete](Transaction &, PreparedQuery const &pq) mutable {
                    complete(::qb::pg::Reply<PreparedQuery>::success(PreparedQuery{pq}));
                },
                [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<PreparedQuery>::failure(e)); });
        });
    }

    /** @brief Execute a statement prepared on this session (`Reply<resultset>`). */
    [[nodiscard]] pg_reply_awaiter<resultset>
    execute(std::string_view name, QueryParams &&params) {
        return admitted<resultset>(
            [n = std::string(name), p = std::move(params)](Transaction &root, pg_coro_complete<resultset> complete) mutable {
                root.execute(
                    std::string_view(n), std::move(p),
                    [complete](Transaction &, resultset rs) mutable { complete(::qb::pg::Reply<resultset>::success(rs.deep_snapshot())); },
                    [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<resultset>::failure(e)); });
            });
    }

    /** @brief Whether this session may still queue: its handshake is the one running on the link. */
    [[nodiscard]] bool
    live() const noexcept {
        return _gate && _id != 0 && _gate->live == _id;
    }

private:
    template <typename, typename>
    friend class Database;

    handshake_session(Transaction &root, std::shared_ptr<handshake_gate> gate, std::uint64_t id) noexcept
        : _root(&root)
        , _gate(std::move(gate))
        , _id(id) {}

    /// The awaiter of one query: queued through the gate when awaited, or failed when the handshake is over.
    template <typename Payload, typename Queue>
    [[nodiscard]] pg_reply_awaiter<Payload>
    admitted(Queue &&queue) {
        return pg_reply_awaiter<Payload>{[root = _root, gate = _gate, id = _id,
                                          queue = std::forward<Queue>(queue)](pg_coro_complete<Payload> complete) mutable {
            if (!gate || id == 0 || gate->live != id) {
                complete(::qb::pg::Reply<Payload>::failure(error::connection_error("handshake over")));
                return;
            }
            gate->admitting = true;
            auto admitted   = qb::scope_guard([&gate] { gate->admitting = false; });
            queue(*root, std::move(complete));
        }};
    }

    Transaction                    *_root;
    std::shared_ptr<handshake_gate> _gate;
    std::uint64_t                   _id;
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
 *         `consume_pg_notify` / `deliver_pg_notify`. See `notify_consumer`, `notify_cb_consumer` and
 *         `notify_co_consumer`.
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
    connection_options   conn_opts_;      ///< Database connection options
    client_options_type  client_opts_;    ///< Client-supplied startup options (sent in the StartupMessage)
    client_options_type  server_params_;  ///< Server-reported ParameterStatus cache (NOT echoed back at connect)
    integer              serverPid_{};    ///< Server process ID
    integer              serverSecret_{}; ///< Server secret for protocol operations
    PreparedQueryStorage storage_;        ///< Storage for prepared statements

public:
    /// The handshake hook: `true` lets the new connection become ready, `false` refuses it for good.
    using handshake_hook = std::function<qb::io::async::task<bool>(handshake_session &)>;

private:
    // --- The reconnect contract (Huly QB-111; qb/io/reconnect.h) ----------------------------------
    /// How establish_() runs its attempts.
    enum class run_mode : std::uint8_t {
        single,   ///< a plain connect(): one attempt, and its failure is the link's cause
        run,      ///< connect_with_retry(): the policy's attempts, spent = retries_exhausted
        recovery, ///< the reconnection run of a lost ready connection: `reconnecting` between attempts
    };

    /// How one connection attempt -- the transport, then the handshake -- ended.
    enum class attempt_outcome : std::uint8_t {
        ready,            ///< the connection is ready on the new link
        transport_failed, ///< no link, a lost link, a startup the server failed, a deadline: another attempt may follow
        refused,          ///< the server (SQLSTATE class 28, 3D000), the authentication check or the hook refused it
        cancelled,        ///< superseded (disconnect(), a new connect(), destruction): the new owner decides
    };

    /// The protocol's own handshake on a link: authentication, then the first ReadyForQuery.
    enum class startup_outcome : std::uint8_t { pending, up, refused, failed };

    /// What the attempt waits on while the protocol handshake runs: settled by the message handlers, by
    /// the link's loss and by the destructor -- shared, so a settle after destruction finds no freed object.
    struct startup_slot {
        startup_outcome         outcome = startup_outcome::pending;
        std::coroutine_handle<> waiter{};

        void
        settle(startup_outcome value) noexcept {
            if (outcome != startup_outcome::pending)
                return;
            outcome = value;
            if (auto h = std::exchange(waiter, {}))
                qb::io::async::coro_scheduler().schedule_resume(h);
        }
    };

    /// The LISTEN replay queued at AuthenticationOk, awaited by the handshake once its hook returned.
    struct replay_slot {
        bool                           done = false;
        std::optional<error::db_error> error{};
        std::coroutine_handle<>        waiter{};

        void
        settle(std::optional<error::db_error> failure) noexcept {
            if (done)
                return;
            done  = true;
            error = std::move(failure);
            if (auto h = std::exchange(waiter, {}))
                qb::io::async::coro_scheduler().schedule_resume(h);
        }
    };

    /// Awaits a startup or replay slot; the frame holds the slot, never the connection.
    template <typename Slot>
    struct slot_awaiter {
        std::shared_ptr<Slot> slot;

        [[nodiscard]] bool
        await_ready() const noexcept {
            if constexpr (std::is_same_v<Slot, startup_slot>)
                return slot->outcome != startup_outcome::pending;
            else
                return slot->done;
        }
        void
        await_suspend(std::coroutine_handle<> h) noexcept {
            qb::io::async::track_suspension(h, "pgsql handshake");
            slot->waiter = h;
        }
        void
        await_resume() const noexcept {}
    };

    /// Where a connect() reports: its awaiter's coroutine.
    struct connect_completion {
        std::coroutine_handle<> waiter{};
        bool                    ok = false;

        void
        settle(bool value) {
            ok = value;
            if (auto h = std::exchange(waiter, {}))
                qb::io::async::coro_scheduler().schedule_resume(h);
        }
    };

    std::optional<RetryPolicy> _reconnect_policy{};
    qb::io::connection_state   _state         = qb::io::connection_state::idle;
    qb::io::down_reason        _down_reason   = qb::io::down_reason::user;
    qb::io::down_reason        _loss_cause    = qb::io::down_reason::peer; ///< why the last link went away
    bool                       _session_live  = false;                     ///< an authenticated protocol session runs on the installed link
    bool                       _link_open     = false;                     ///< a link is installed and its teardown has not begun
    bool                       _cause_set     = false;                     ///< the client named _loss_cause before it dropped the link
    bool                       _session_ready = false;                     ///< the installed link reached ready: its loss fires on_disconnected
    bool                       _recovering    = false;                     ///< a reconnection run is in progress (is_reconnecting())
    bool                       _ever_ready    = false;                     ///< a connection was ready once: the next one follows a gap
    /// Bumped by disconnect(), by every connect() that starts a lifecycle and by every terminal transition:
    /// the run, the attempt and the handshake that captured an older value were superseded and step aside.
    std::uint64_t                   _epoch           = 0;
    std::uint64_t                   _user_down_epoch = 0; ///< the epoch disconnect() left a ready link in
    std::uint64_t                   _handshake_seq   = 0;
    std::shared_ptr<handshake_gate> _gate            = std::make_shared<handshake_gate>();
    std::shared_ptr<startup_slot>   _startup{};
    std::shared_ptr<replay_slot>    _replay{};
    error::db_error                 _refusal{"handshake refused"}; ///< what refused the last handshake
    handshake_hook                  _hs_hook{};
    /// The channels listen() / unlisten() / unlisten_all() leave this connection listening on: replayed at
    /// the AuthenticationOk of every new session. A LISTEN written as raw SQL is not in it.
    std::set<std::string, std::less<>>                _listened{};
    std::deque<::qb::pg::notification>                _held_notifications{}; ///< received while handshaking: released at ready
    std::optional<typename QB_IO_::transport_io_type> _adopted{};            ///< connect(dsn, raw_io)'s transport, until its attempt takes it
    bool                                              _raw_listen_seen   = false; ///< a CommandComplete "LISTEN" no API call claimed yet
    bool                                              _raw_listen_logged = false;
    // Observability hooks: empty until set, read only on the cold transitions.
    std::function<void()>                    _on_connected{};
    std::function<void(qb::io::down_reason)> _on_disconnected{};
    std::function<void(qb::io::down_reason)> _on_down{};
    std::shared_ptr<bool>                    _alive{std::make_shared<bool>(true)};

    /// Owned handshake-deadline timer. MUST be a ScopedTimeout (cancelled on destruction),
    /// not a fire-and-forget async::callback: the latter is a self-deleting heap Timeout that
    /// outlives this Database and would dereference a freed `this` if the connection is dropped
    /// within the timeout window (the common case — handshake finishes in ms, the deadline is
    /// seconds). Destroying this member with the Database stops the watcher.
    std::unique_ptr<qb::io::async::ScopedTimeout<std::function<void()>>> connect_deadline_timer_{};

    /// When `NotifyDerived` is `void`, optional handler for `NotificationResponse` (plain
    /// `database`).
    std::function<void(::qb::pg::notification &&)> inbound_notify_handler_{};

    /// `on_notice`: every NoticeResponse, with all its fields (Huly QB-108); empty: logged at INFO.
    std::function<void(::qb::pg::notice const &)> notice_handler_{};

    /// Out of line and cold: a notice is rare. An exception escaping the handler is contained and
    /// logged -- it runs inside the protocol handler.
    QB_NOINLINE QB_COLD void
    deliver_notice(::qb::pg::notice const &n) noexcept {
        try {
            notice_handler_(n);
        } catch (std::exception const &e) {
            QB_LOG_WARN("[pgsql] on_notice handler threw: " << e.what());
        } catch (...) {
            QB_LOG_WARN("[pgsql] on_notice handler threw a non-std exception");
        }
    }

    template <typename Fn, typename... Args>
    static void
    notify_(Fn &hook, char const *what, Args... args) noexcept {
        if (!hook)
            return;
        try {
            hook(args...);
        } catch (std::exception const &ex) {
            QB_LOG_WARN("[pgsql] " << what << " hook threw: " << ex.what());
        } catch (...) {
            QB_LOG_WARN("[pgsql] " << what << " hook threw a non-std exception");
        }
    }

    /// The notify consumer's part of a transition, when the derived class is one.
    template <typename Fn>
    void
    consumer_(Fn &&fn) {
        if constexpr (!std::is_same_v<NotifyDerived, void>)
            std::forward<Fn>(fn)(*static_cast<NotifyDerived *>(this));
    }

    /// The cause a transport loss the client did not ask for maps to.
    [[nodiscard]] static qb::io::down_reason
    cause_of_(int reason) noexcept {
        using qb::io::async::event::disconnect_reason;
        switch (static_cast<disconnect_reason>(reason)) {
            case disconnect_reason::protocol_error:
            case disconnect_reason::message_too_large:
            case disconnect_reason::buffer_overflow:
                return qb::io::down_reason::protocol;
            default:
                return qb::io::down_reason::peer;
        }
    }

    /// Whether the protocol handshake of the installed link is still waiting for its outcome.
    [[nodiscard]] bool
    startup_pending_() const noexcept {
        return _startup && _startup->outcome == startup_outcome::pending;
    }

    /// The server or the client's own check refused the startup: terminal for the attempt and for any run.
    void
    refuse_startup_(error::db_error const &why) {
        _refusal = why;
        _error   = why;
        if (_startup)
            _startup->settle(startup_outcome::refused);
    }

    /// Whether the handshake `id` is the one writing on the link now.
    [[nodiscard]] bool
    handshake_live_(std::uint64_t id) const noexcept {
        return id != 0 && _gate->live == id && _link_open;
    }

    /// Tear the link down now, naming the cause; on(disconnected) runs before this returns, or right after
    /// the message handler that called it.
    void
    drop_link_(qb::io::down_reason cause) noexcept {
        if (!_link_open)
            return;
        _loss_cause = cause;
        _cause_set  = true;
        _link_open  = false;
        this->disconnect_now();
    }

#ifndef QB_HAS_SSL
    /**
     * @brief Refuse a server-selected authentication method this build cannot perform.
     *
     * MD5 and SCRAM-SHA-256 both need real cryptography (MD5 / PBKDF2-HMAC-SHA256 /
     * HMAC / CSPRNG nonce), which only OpenSSL provides. A `QB_WITH_SSL=OFF` build has
     * none of it, so those code paths are not compiled at all — but the *server* picks
     * the method, so the mismatch is only knowable at runtime, on receipt of the
     * Authentication message. Fail here, loudly and specifically, rather than sending a
     * bogus or empty password and letting the server report a generic auth failure.
     *
     * Uses the same terminal-failure shape as the SCRAM MITM refusal above (QB_LOG_CRIT +
     * flags + resume): this runs inside the protocol message handler on the event-loop
     * thread, so an escaping exception would tear down the loop rather than the
     * connection.
     *
     * @param method Human-readable name of the method the server asked for.
     */
    void
    refuse_auth_without_ssl(const char *method) {
        QB_LOG_CRIT("[pgsql] Server requested " << method
                                                << " authentication, which this build cannot perform: qb was built without OpenSSL "
                                                   "(QB_HAS_SSL undefined / QB_WITH_SSL=OFF). Rebuild qb with OpenSSL, or configure the "
                                                   "PostgreSQL role for 'trust' or 'password' (cleartext) authentication.");
        // Retrying cannot heal a build without OpenSSL: a refusal, terminal (Huly QB-111).
        refuse_startup_(error::connection_error{
            std::string("the server requested ") + method + " authentication, which this build cannot perform (qb built without OpenSSL)"
        });
    }
#endif // !QB_HAS_SSL

#ifdef QB_HAS_SSL
    /**
     * @brief The value-semantic client TLS context the connection options describe -- built the
     *        same way for the session and for an out-of-band `cancel_async()` (Huly QB-113):
     *          default          -> Context::client() (TLS 1.2+, system trust store, verify chain + host);
     *          ssl_verify=none  -> verification off (encrypt only);
     *          ssl_root_cert    -> trust a private CA IN ADDITION to the system store (libpq sslrootcert);
     *          ssl_cert+ssl_key -> present a client certificate (mutual TLS; libpq sslcert/sslkey).
     *        A bad CA/cert/key path leaves `ok()` false: callers fail CLOSED on it.
     */
    [[nodiscard]] qb::io::ssl::Context
    make_client_tls_context() const {
        auto tls = qb::io::ssl::Context::client();
        if (conn_opts_.ssl_verify != qb::pg::ssl_verify_mode::full)
            tls.verify(qb::io::ssl::VerifyMode::none);
        if (!conn_opts_.ssl_root_cert.empty())
            tls.trust(conn_opts_.ssl_root_cert);
        if (!conn_opts_.ssl_cert.empty() && !conn_opts_.ssl_key.empty())
            tls.identity(conn_opts_.ssl_cert, conn_opts_.ssl_key);
        return tls;
    }
#endif // QB_HAS_SSL

    /**
     * @brief The 16-byte PostgreSQL CancelRequest for this session's backend, all big-endian:
     *        int32 length = 16, int32 request code = 80877102 (0x04D2162E), int32 backend process
     *        id, int32 backend secret key -- the two captured from BackendKeyData at connect.
     */
    [[nodiscard]] std::array<std::uint8_t, 16>
    cancel_request_packet() const noexcept {
        std::array<std::uint8_t, 16> pkt{};
        const std::uint32_t          len  = htonl(16u);
        const std::uint32_t          code = htonl(80877102u);
        const std::uint32_t          pid  = htonl(static_cast<std::uint32_t>(serverPid_));
        const std::uint32_t          key  = htonl(static_cast<std::uint32_t>(serverSecret_));
        std::memcpy(pkt.data() + 0, &len, 4);
        std::memcpy(pkt.data() + 4, &code, 4);
        std::memcpy(pkt.data() + 8, &pid, 4);
        std::memcpy(pkt.data() + 12, &key, 4);
        return pkt;
    }

    /**
     * @brief The connect budget of an out-of-band cancel: `connect_timeout` (10 s when unset),
     *        capped at 2 s. The cancel targets the SAME already-reachable endpoint as the live
     *        session, so the connect is normally sub-millisecond; the cap only bounds the
     *        pathological unreachable case -- the whole loop for `cancel()`, one coroutine for
     *        `cancel_async()`.
     */
    [[nodiscard]] qb::duration
    cancel_connect_budget() const noexcept {
        const qb::duration cfg = conn_opts_.connect_timeout > qb::duration::zero()
                                     ? conn_opts_.connect_timeout
                                     : std::chrono::duration_cast<qb::duration>(std::chrono::seconds(10));
        return std::min(cfg, std::chrono::duration_cast<qb::duration>(std::chrono::seconds(2)));
    }

    /// The deadline of one attempt: the policy's, else the connection's `connect_timeout`, else 10 s.
    [[nodiscard]] qb::duration
    attempt_timeout_(qb::duration policy_timeout) const noexcept {
        if (policy_timeout > qb::duration::zero())
            return policy_timeout;
        if (conn_opts_.connect_timeout > qb::duration::zero())
            return conn_opts_.connect_timeout;
        return std::chrono::duration_cast<qb::duration>(std::chrono::seconds(10));
    }

    /**
     * @brief Awaits one transport connect -- TCP, and for a secure database the SSLRequest and the TLS
     *        handshake -- and hands the socket back, open or not. The socket is the frame's: a frame
     *        destroyed first closes it, and a callback that fires after the frame is gone does nothing.
     */
    struct transport_awaiter {
        using transport_sock = std::remove_cvref_t<typename QB_IO_::transport_io_type>;

        Database                     &db;
        qb::duration                  timeout;
        std::optional<transport_sock> sock{};
        std::coroutine_handle<>       handle{};
        std::shared_ptr<bool>         valid{std::make_shared<bool>(true)};
        bool                          failed_now = false; ///< nothing was started (a TLS context that cannot be built)

        transport_awaiter(Database &d, qb::duration t)
            : db(d)
            , timeout(t) {}
        transport_awaiter(transport_awaiter const &)            = delete;
        transport_awaiter &operator=(transport_awaiter const &) = delete;
        ~transport_awaiter() {
            *valid = false;
        }

        [[nodiscard]] bool
        await_ready() const noexcept {
            return false;
        }

        bool
        await_suspend(std::coroutine_handle<> h) {
            qb::io::async::track_suspension(h, "pgsql connect");
            handle  = h;
            auto cb = [this, v = valid](transport_sock &&s) {
                if (!*v)
                    return; // the frame is gone; the socket closes with s
                sock.emplace(std::move(s));
                qb::io::async::coro_scheduler().schedule_resume(handle);
            };
            const qb::io::uri connect_uri{db.conn_opts_.schema + "://" + db.conn_opts_.uri};
            if constexpr (transport_sock::is_secure()) {
#ifdef QB_HAS_SSL
                // PostgreSQL negotiates TLS in-band (the cleartext SSLRequest packet) BEFORE
                // the handshake. Drive the whole connect → SSLRequest → TLS handshake through
                // the connector's STARTTLS path so it runs fully asynchronously on the event
                // loop (no blocking send/recv/handshake). A server that declines SSL fails the
                // connect — a secure database requires TLS; use the plain tcp::database for
                // cleartext.
                // ssl_verify_mode::full -> verify the chain + host (the connector passes the remote host to
                // ssl::socket::init_client); ::none -> encrypt only. The socket's ssl::Context (built below)
                // governs verification, so the STARTTLS connector is handed a ready socket, not a verify bool.
                const bool verify = (db.conn_opts_.ssl_verify == qb::pg::ssl_verify_mode::full);
                if (!verify) {
                    // P2: encrypt-only (ssl_verify=none, the documented libpq-matching default) does NOT
                    // check the server certificate chain or hostname, leaving an active-MITM window on an
                    // untrusted network. SCRAM-SHA-256 mutual auth (enforced since the P1 fix) still
                    // authenticates the server, but non-SCRAM auth and the TLS channel itself are
                    // unprotected. Surface it ONCE so an unverified secure connection is never silent.
                    static std::atomic<bool> warned_unverified_tls{false};
                    if (!warned_unverified_tls.exchange(true, std::memory_order_relaxed)) {
                        QB_LOG_WARN("[pgsql] TLS WITHOUT certificate verification (ssl_verify=none): the server "
                                    "certificate chain/hostname is NOT verified. Set ssl_verify_mode::full to "
                                    "authenticate the server (or rely on SCRAM-SHA-256 mutual auth).");
                    }
                }
                auto tls = db.make_client_tls_context();
                if (!tls.ok()) {
                    // Fail CLOSED on a bad CA/cert/key path rather than silently connecting without it.
                    db._error  = error::connection_error{"pgsql: TLS context configuration failed: " + tls.error()};
                    failed_now = true;
                    return false;
                }
                transport_sock s{std::move(tls)};
                qb::io::async::tcp::starttls_connect<transport_sock, postgres_ssl_negotiator>(std::move(s), connect_uri, std::move(cb),
                                                                                              timeout);
#else
                db._error  = error::connection_error{"ssl transport requires QB_HAS_SSL"};
                failed_now = true;
                return false;
#endif
            } else {
                qb::io::async::tcp::connect<transport_sock>(connect_uri, std::move(cb), timeout);
            }
            return true;
        }

        transport_sock
        await_resume() {
            return sock ? std::move(*sock) : transport_sock{};
        }
    };

    /**
     * @brief Install an opened socket as the connection's link and start the PostgreSQL session on it.
     *
     * Every connect path funnels through here -- a plain `connect()`, `connect_with_retry()` and each
     * attempt of a reconnection run -- so this is where the previous session's state goes:
     * - the bytes and the protocol of the previous link (a bare reconnect sent the bytes of the queries
     *   `disconnect()` failed ahead of the StartupMessage, Huly QB-202);
     * - the per-backend state: the prepared statements and the protocol `Close` messages still owed to
     *   them, the ParameterStatus cache, the transaction status and the cancel key (a bare reconnect kept
     *   the statement cache, and an `execute(name)` of the old backend's statement failed on the server
     *   with 26000 instead of being refused client-side, Huly QB-1095);
     * - the SCRAM mutual-auth gate: a stale `_scram_server_verified` from a previous login would let an
     *   impersonating server on this link skip SASL and send a bare AuthenticationOk.
     * Then the StartupMessage leaves, the connection is `handshaking`, and the attempt's deadline covers
     * the whole handshake -- authentication, the user's hook and the LISTEN replay.
     *
     * @param sock The connected transport (a secure one has run its SSLRequest and TLS handshake).
     * @param limit The attempt's deadline.
     */
    template <typename Sock_>
    void
    install_link_(Sock_ &&sock, qb::duration limit) {
        this->reset_for_reconnect();
        storage_.clear();
        server_params_.clear();
        _txn_status            = 'I';
        serverPid_             = 0;
        serverSecret_          = 0;
        this->transport()      = std::forward<Sock_>(sock);
        _scram_pending         = false;
        _scram_server_verified = false;
        _password_salt.clear();
        _auth_message.clear();
        _error                 = error::db_error{"unknown error"};
        _current_command       = root_transaction();
        _current_query         = nullptr;
        _replan                = replan_phase::none;
        _query_sent            = false;
        _ready_for_query       = false;
        _raw_listen_seen       = false;
        _link_open             = true;
        _cause_set             = false;
        _session_ready         = false;
        _session_live          = false;
        _state                 = qb::io::connection_state::handshaking;
        const std::uint64_t id = ++_handshake_seq;
        _gate->live            = id;
        _startup               = std::make_shared<startup_slot>();
        _replay.reset();
        _held_notifications.clear(); // a session that never became ready has no notifications to give
        this->template switch_protocol<pg_protocol>(*this);
        this->start();
        send_startup_message();
        // Owned, cancellable deadline (see connect_deadline_timer_): reassigning here cancels the previous
        // one, and ~Database cancels this one -- the callback never fires into a freed `this`. A handshake
        // still running when it falls is a transport failure: the link drops, and a run may try again.
        connect_deadline_timer_ = qb::io::async::scoped_callback(std::function<void()>([this, id, limit]() {
                                                                     if (!handshake_live_(id))
                                                                         return;
                                                                     QB_LOG_WARN("[pgsql] handshake did not complete within "
                                                                                 << qb::detail::to_ev_seconds(limit) << "s; dropping the link");
                                                                     drop_link_(qb::io::down_reason::peer);
                                                                     _error = error::db_error{"connection timeout"};
                                                                 }),
                                                                 limit);
    }

    /**
     * @brief The new session's part of the handshake that cannot wait for a coroutine: run at
     *        AuthenticationOk, before the first ReadyForQuery can dispatch anything (Huly QB-111).
     * @details The listened set is replayed: one simple query of `LISTEN` statements, queued first, so it
     *          leaves before the hook's queries. The NOTIFYs it lets through before the session is ready
     *          are held, and released at `ready` behind the gap marker (`enter_ready_()`).
     */
    void
    begin_session_() {
        if (_listened.empty())
            return;
        std::string sql;
        for (auto const &channel : _listened) {
            sql += build_listen_sql(channel);
            sql += ';';
        }
        auto slot = std::make_shared<replay_slot>();
        _replay   = slot;
        admit_([&] {
            this->execute(
                std::string_view(sql),
                [this, slot](Transaction &, resultset) {
                    _raw_listen_seen = false; // the replay's LISTENs are not raw SQL
                    slot->settle(std::nullopt);
                },
                [this, slot](error::db_error const &e) {
                    _raw_listen_seen = false;
                    slot->settle(e);
                });
        });
    }

    /// Queue a query of the handshake while the connection is not ready.
    template <typename Fn>
    void
    admit_(Fn &&fn) {
        _gate->admitting = true;
        auto admitted    = qb::scope_guard([this] { _gate->admitting = false; });
        std::forward<Fn>(fn)();
    }

    /// The handshake succeeded: the connection is usable, then the user is told.
    void
    enter_ready_() {
        const bool follows_gap = _ever_ready; // an earlier ready session of this lifecycle: the server published meanwhile
        if (follows_gap)
            ++_stats.reconnects; // a ready connection reached after a lost one
        _state         = qb::io::connection_state::ready;
        _session_ready = true;
        _recovering    = false;
        _ever_ready    = true;
        _gate->live    = 0;
        _startup.reset();
        _replay.reset();
        connect_deadline_timer_.reset();
        // In this order, as qbm-redis releases a ready connection: the hole first, then what the new
        // session received while it was handshaking, then the user is told. Whoever learns of the gap can
        // query at once -- the connection is ready. A callback that disconnects or connects again (the
        // epoch moves) stops the release: what is left belonged to a session that is over.
        const auto epoch = _epoch;
        if (follows_gap) {
            consumer_([](auto &consumer) { consumer.on_pg_notify_gap(::qb::io::gap_info{::qb::io::gap_reason::reconnect, 0}); });
            if (epoch != _epoch || _state != qb::io::connection_state::ready)
                return;
        }
        while (!_held_notifications.empty()) {
            auto n = std::move(_held_notifications.front());
            _held_notifications.pop_front();
            deliver_notification_(std::move(n));
            if (epoch != _epoch || _state != qb::io::connection_state::ready)
                return;
        }
        notify_(_on_connected, "on_connected");
    }

    /// Terminal: a consumer's stream drains then ends, the user is told. Supersedes the current owner.
    void
    enter_down_(qb::io::down_reason reason) {
        _state       = qb::io::connection_state::down;
        _down_reason = reason;
        _recovering  = false;
        _held_notifications.clear();
        if (reason == qb::io::down_reason::handshake_refused)
            _error = _refusal;
        else if (reason == qb::io::down_reason::retries_exhausted)
            _error = error::connection_error{"reconnection attempts exhausted"};
        const auto epoch = ++_epoch;
        settle_down_(reason, epoch);
    }

    void
    settle_down_(qb::io::down_reason reason, std::uint64_t epoch) {
        consumer_([](auto &consumer) { consumer.on_pg_notify_down(); });
        if (epoch != _epoch)
            return; // a callback connected again: the client did not stay down
        notify_(_on_down, "on_down", reason);
    }

    /// The reconnection run: the policy's attempts, until ready, refused, spent or superseded.
    qb::io::async::task<void>
    run_(RetryPolicy policy, std::shared_ptr<bool> alive, std::uint64_t epoch) {
        // A task first runs at the scheduler's next drain, not at spawn(): the client may be gone by then.
        if (!*alive || epoch != _epoch)
            co_return;
        QB_LOG_INFO("[pgsql] auto-reconnect starting");
        const bool ok = co_await establish_(std::move(policy), run_mode::recovery, alive, epoch);
        if (!*alive)
            co_return;
        if (ok)
            QB_LOG_INFO("[pgsql] auto-reconnect succeeded");
        else
            QB_LOG_WARN("[pgsql] auto-reconnect failed");
    }

    /**
     * @brief The one engine behind every connect form and the reconnection run: attempts until the
     *        connection is ready, the handshake is refused, the policy is spent, or the lifecycle is
     *        superseded.
     * @param policy The attempts and their backoff (one attempt for a plain connect()).
     * @param mode A plain connect() (its failure is the link's cause), an explicit run
     *             (connect_with_retry), or the reconnection run (`reconnecting` between attempts).
     */
    qb::io::async::task<bool>
    establish_(RetryPolicy policy, run_mode mode, std::shared_ptr<bool> alive, std::uint64_t epoch) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        const auto         between = mode == run_mode::recovery ? qb::io::connection_state::reconnecting : qb::io::connection_state::connecting;
        const qb::duration limit   = attempt_timeout_(policy.connect_timeout);
        int                attempt = 0;
        while (policy.allows_attempt(attempt)) {
            if (!*alive || epoch != _epoch)
                co_return false;
            ++attempt;
            _state             = between;
            const auto outcome = co_await attempt_(limit, alive, epoch);
            if (!*alive || epoch != _epoch || outcome == attempt_outcome::cancelled)
                co_return false; // disconnect(), a new connect() or destruction owns the state now
            if (outcome == attempt_outcome::ready)
                co_return true;
            if (outcome == attempt_outcome::refused) {
                enter_down_(qb::io::down_reason::handshake_refused);
                co_return false;
            }
            _state = between; // the attempt's link is gone: the run waits, or ends
            if (!policy.allows_attempt(attempt))
                break;
            const qb::duration wait = policy.next_delay(attempt, &rng);
            notify_(policy.on_retry, "on_retry", attempt, wait);
            if (!*alive || epoch != _epoch)
                co_return false; // on_retry disconnected
            co_await qb::io::async::sleep(wait);
        }
        if (!*alive || epoch != _epoch)
            co_return false;
        enter_down_(mode == run_mode::single ? _loss_cause : qb::io::down_reason::retries_exhausted);
        co_return false;
    }

    /// One attempt: the transport, then the handshake on the new link.
    qb::io::async::task<attempt_outcome>
    attempt_(qb::duration limit, std::shared_ptr<bool> alive, std::uint64_t epoch) {
        transport_awaiter connecting{*this, limit};
        auto              sock = co_await connecting;
        if (!*alive || epoch != _epoch)
            co_return attempt_outcome::cancelled; // the socket closes with this frame
        if (!sock.is_open()) {
            if (!connecting.failed_now)
                _error = error::connection_error{"connection / TLS handshake failed"};
            _loss_cause = qb::io::down_reason::peer;
            co_return attempt_outcome::transport_failed;
        }
        install_link_(std::move(sock), limit);
        co_return co_await handshake_(alive, epoch);
    }

    /**
     * @brief The handshake on a fresh link: the protocol's, the user's hook, the LISTEN replay.
     * @details The protocol's own handshake is message-driven (authentication, ParameterStatus,
     *          BackendKeyData, the first ReadyForQuery); this coroutine waits for its outcome. A server
     *          that refuses the session -- SQLSTATE class 28 (credentials, authorization) or 3D000 (no
     *          such database) -- or a server proof that does not verify is a refusal: terminal. Any other
     *          failure of the startup, a link lost on the way or a deadline is one more attempt. The hook
     *          then runs with a `handshake_session`, and the replay queued at AuthenticationOk is awaited:
     *          a replay the server failed is one more attempt (only class 28 and 3D000 refuse).
     */
    qb::io::async::task<attempt_outcome>
    handshake_(std::shared_ptr<bool> alive, std::uint64_t epoch) {
        const std::uint64_t id      = _gate->live;
        auto                startup = _startup;
        co_await slot_awaiter<startup_slot>{startup};
        if (!*alive || epoch != _epoch)
            co_return attempt_outcome::cancelled;
        if (startup->outcome == startup_outcome::refused)
            co_return finish_attempt_(attempt_outcome::refused);
        if (startup->outcome != startup_outcome::up || !handshake_live_(id))
            co_return finish_attempt_(attempt_outcome::transport_failed);
        if (_hs_hook) {
            // A copy: the hook may be replaced while it runs, and the closure it calls must outlive it.
            auto              hook    = _hs_hook;
            bool              verdict = false;
            std::string       why;
            handshake_session session{*root_transaction(), _gate, id};
            try {
                verdict = co_await hook(session);
            } catch (std::exception const &ex) {
                why = ex.what();
            } catch (...) {
                why = "the handshake hook threw";
            }
            if (!*alive || epoch != _epoch)
                co_return attempt_outcome::cancelled;
            if (!handshake_live_(id))
                co_return finish_attempt_(attempt_outcome::transport_failed); // the link went away under the hook
            if (!verdict) {
                _refusal = error::connection_error{
                    why.empty() ? std::string{"the handshake hook refused the connection"} : "the handshake hook threw: " + why
                };
                co_return finish_attempt_(attempt_outcome::refused);
            }
        }
        if (auto replay = _replay) {
            co_await slot_awaiter<replay_slot>{replay};
            if (!*alive || epoch != _epoch)
                co_return attempt_outcome::cancelled;
            if (!handshake_live_(id))
                co_return finish_attempt_(attempt_outcome::transport_failed);
            if (replay->error) {
                QB_LOG_WARN("[pgsql] LISTEN replay failed: " << replay->error->what());
                _error = *replay->error;
                co_return finish_attempt_(attempt_outcome::transport_failed);
            }
        }
        enter_ready_();
        co_return attempt_outcome::ready;
    }

    /// An attempt that ends before ready leaves no handshake live and no link.
    attempt_outcome
    finish_attempt_(attempt_outcome outcome) {
        _gate->live = 0;
        _startup.reset();
        _replay.reset();
        connect_deadline_timer_.reset();
        drop_link_(qb::io::down_reason::peer); // a link already gone keeps the cause it went with
        return outcome;
    }

    /// Start a lifecycle from idle or down, or say why not.
    [[nodiscard]] bool
    begin_lifecycle_() {
        if (_state != qb::io::connection_state::idle && _state != qb::io::connection_state::down)
            return false; // a connection, a handshake or a run is in progress: it owns the state
        ++_epoch;
        _state = qb::io::connection_state::connecting;
        consumer_([](auto &consumer) { consumer.on_pg_notify_lifecycle_start(); });
        return true;
    }

    qb::io::async::task<void>
    connect_flow_(RetryPolicy policy, std::shared_ptr<connect_completion> done, std::shared_ptr<bool> alive, std::uint64_t epoch) {
        const bool ok = co_await establish_(std::move(policy), run_mode::single, alive, epoch);
        done->settle(ok && *alive && _state == qb::io::connection_state::ready);
    }

    /// Start one connect() attempt for `done`; false when the client was busy (nothing started).
    bool
    begin_connect_(qb::duration timeout, std::shared_ptr<connect_completion> done) {
        if (!begin_lifecycle_())
            return false;
        auto policy = RetryPolicy{}.with_max_attempts(1).with_connect_timeout(attempt_timeout_(timeout));
        qb::io::async::coro_scheduler().spawn(connect_flow_(std::move(policy), std::move(done), _alive, _epoch));
        return true;
    }

    /// Start a lifecycle on a transport connect(dsn, raw_io) was given; false when busy or ready (nothing taken).
    bool
    begin_adopt_(typename QB_IO_::transport_io_type &&raw_io, qb::duration timeout, std::shared_ptr<connect_completion> done) {
        if (!begin_lifecycle_())
            return false;
        _adopted.emplace(std::move(raw_io));
        qb::io::async::coro_scheduler().spawn(adopted_flow_(attempt_timeout_(timeout), std::move(done), _alive, _epoch));
        return true;
    }

    /// One attempt on the adopted transport: the handshake on it, or down -- never a dial of the DSN.
    qb::io::async::task<void>
    adopted_flow_(qb::duration limit, std::shared_ptr<connect_completion> done, std::shared_ptr<bool> alive, std::uint64_t epoch) {
        if (!*alive || epoch != _epoch) {
            done->settle(false);
            co_return;
        }
        auto sock = std::move(*_adopted);
        _adopted.reset();
        attempt_outcome outcome = attempt_outcome::transport_failed;
        if (sock.is_open()) {
            install_link_(std::move(sock), limit);
            outcome = co_await handshake_(alive, epoch);
        } else {
            _loss_cause = qb::io::down_reason::peer;
            _error      = error::connection_error{"the transport given to connect(dsn, raw_io) is not open"};
        }
        if (!*alive) {
            done->settle(false);
            co_return;
        }
        if (epoch == _epoch && outcome != attempt_outcome::ready && outcome != attempt_outcome::cancelled)
            enter_down_(outcome == attempt_outcome::refused ? qb::io::down_reason::handshake_refused : _loss_cause);
        done->settle(outcome == attempt_outcome::ready && _state == qb::io::connection_state::ready);
    }

    /// The connection's part of a LISTEN / UNLISTEN that `listen()` / `unlisten()` / `unlisten_all()` ran.
    void
    on_subscription_changed(subscription_change change, std::string_view channel) override {
        _raw_listen_seen = false; // the CommandComplete "LISTEN" was this call's
        switch (change) {
            case subscription_change::listen:
                _listened.emplace(channel);
                break;
            case subscription_change::unlisten:
                if (auto it = _listened.find(channel); it != _listened.end())
                    _listened.erase(it);
                break;
            case subscription_change::unlisten_all:
                _listened.clear();
                break;
        }
    }

    /// A CommandComplete "LISTEN" that no API call claimed by ReadyForQuery was raw SQL: not replayed.
    QB_NOINLINE QB_COLD void
    note_raw_listen_() noexcept {
        _raw_listen_seen = false;
        if (_state != qb::io::connection_state::ready)
            return; // a LISTEN of the handshake hook runs again with the hook on every session
        if (std::exchange(_raw_listen_logged, true))
            return;
        QB_LOG_DEBUG("[pgsql] a LISTEN written as SQL is not replayed after a reconnection; use listen(channel), which is");
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
        send_message(m);
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

    Transaction *_current_command           = this;    ///< Current transaction being processed
    ISqlQuery   *_current_query             = nullptr; ///< Current query being executed
    bool         _ready_for_query           = false;   ///< Flag indicating if ready for next query
    unsigned     _completion_callback_depth = 0;       ///< Keep a callback's Transaction owner alive until it returns
    enum class DeferredDisconnect { none, client, transport };
    DeferredDisconnect _deferred_disconnect = DeferredDisconnect::none;

    // --- Observability (Huly QB-115) -----------------------------------------------------------
    ::qb::pg::stats                                   _stats{};                 ///< The counts kept here; prepared_* are storage_'s
    std::function<void(::qb::pg::query_info const &)> _query_hook{};            ///< on_query_complete; empty: nothing is reported
    qb::mono_time                                     _query_sent_at{};         ///< When the current query's message was written
    bool                                              _query_sent = false;      ///< The current query left the client
    qb::mono_time                                     _ready_clock{};           ///< The clock, read once per ReadyForQuery handler
    bool                                              _ready_clock_set = false; ///< Inside that handler: _ready_clock is the time

    /// The time: inside a ReadyForQuery handler the instant read on entry, which the query it completes
    /// and the next one it sends share -- one clock read per query on a busy connection.
    [[nodiscard]] qb::mono_time
    clock_now() const noexcept {
        return _ready_clock_set ? _ready_clock : qb::mono_now();
    }

    /**
     * @brief One query completed: counted, and reported to the `on_query_complete` hook if any.
     * @param query The query
     * @param sent Whether its message left the client (its duration is then measured)
     * @param err Its failure; null on success
     */
    void
    count_query(ISqlQuery const &query, bool sent, error::db_error const *err) noexcept {
        ++_stats.queries;
        if (err)
            ++_stats.errors;
        qb::duration took{};
        if (sent) {
            took                       = clock_now() - _query_sent_at;
            _stats.last_query_duration = took;
        }
        if (_query_hook) [[unlikely]]
            report_query(query, took, err);
    }

    /// Out of line and cold: without a hook a completion pays one branch. An exception escaping the
    /// hook is contained and logged -- the hook runs inside the protocol handler.
    QB_NOINLINE QB_COLD void
    report_query(ISqlQuery const &query, qb::duration took, error::db_error const *err) noexcept {
        try {
            auto info     = query.describe();
            info.duration = took;
            info.ok       = err == nullptr;
            info.error    = err;
            _query_hook(info);
        } catch (std::exception const &e) {
            QB_LOG_WARN("[pgsql] on_query_complete hook threw: " << e.what());
        } catch (...) {
            QB_LOG_WARN("[pgsql] on_query_complete hook threw a non-std exception");
        }
    }

    /// Counts the queries a disconnect drains; the in-flight one keeps its send time.
    struct drain_counter final : drained_query_observer {
        Database &db;

        explicit drain_counter(Database &owner) noexcept
            : db(owner) {}

        void
        on_drained_query(ISqlQuery const &query, error::db_error const &err) noexcept override {
            const bool sent = &query == db._current_query && std::exchange(db._query_sent, false);
            db.count_query(query, sent, &err);
        }
    };

    /// Fail every queued query of the connection, each one counted and reported (Huly QB-115).
    void
    fail_all_pending_counted(error::db_error const &err) {
        drain_counter counter{*this};
        root_transaction()->fail_all_pending(err, &counter);
        _query_sent = false;
    }

    /// Write one protocol message, counting the bytes the output buffer took.
    void
    send_message(message const &m) {
        const auto before = this->out().size();
        *this << m;
        const auto after = this->out().size();
        if (after > before)
            _stats.bytes_sent += static_cast<std::uint64_t>(after - before);
    }

    void
    finish_query_callback() {
        if (--_completion_callback_depth != 0 || _deferred_disconnect == DeferredDisconnect::none)
            return;

        const auto reason    = _deferred_disconnect;
        _deferred_disconnect = DeferredDisconnect::none;
        if (reason == DeferredDisconnect::client)
            fail_all_pending_counted(error::connection_error("database disconnected by client"));
        else
            fail_all_pending_counted(error::client_error("database disconnected"));
        _current_command = root_transaction();
        _current_query   = nullptr;
        _replan          = replan_phase::none;
        _ready_for_query = false;
    }

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
        if (!_session_live)
            return false;
        if (!cmd)
            cmd = root_transaction();
        _current_command = next_transaction(cmd);
        if (!_current_command)
            _current_command = root_transaction();
        _current_query = _current_command->next_query();
        // A COPY source/sink is reserved when its coroutine queues SQL, but only
        // the matching query may use it. An older plain COPY can be ahead of it.
        _active_copy_operation = (_copy_operation && _copy_operation->query == _current_query) ? _copy_operation : nullptr;

        if (_current_query) {
            if (qb::likely(_current_query->is_valid())) {
                _query_bound = false;
                if (storage_.has_stale()) [[unlikely]] {
                    if (reprepare_before_execute_())
                        return true;
                }
                const auto msg = _current_query->get();
                _query_sent_at = clock_now();
                _query_sent    = true;
                send_message(msg);
                return true;
            } else {
                QB_LOG_DEBUG("[pgsql] error processing query not valid");
                _error = error::client_error{"query couldn't be processed check logs for more infos"};
                on_error_query(error());
                if (!_session_live)
                    return false;
                const bool dispatched = process_query(_current_command);
                if (!dispatched && _session_live)
                    _ready_for_query = true;
                return dispatched;
            }
        } else if (_current_command->parent()) {
            auto next_cmd = _current_command->parent();
            do {
                // pop_transaction() may invoke on_before_pop(), then a Then/Error
                // destructor callback. A callback can disconnect: defer draining the
                // parent until no code below still reads next_cmd.
                ++_completion_callback_depth;
                auto callback_guard = qb::scope_guard([this] { finish_query_callback(); });
                next_cmd->pop_transaction();
                if (!_session_live)
                    return false;
                if (next_cmd->result())
                    break;
                next_cmd = next_cmd->parent();
            } while (next_cmd);

            return process_query(next_cmd);
        }
        return false;
    }

    /**
     * @brief Processes queries if the client is ready
     */
    void
    process_if_query_ready() {
        if (_ready_for_query && _session_live) {
            const bool dispatched = process_query(_current_command);
            if (!dispatched && _session_live)
                _ready_for_query = true;
        }
    }

    /**
     * @brief Handles successful query completion
     */
    void
    on_success_query() {
        if (!_current_query)
            return;
        _replan = replan_phase::none;
        if (!_current_command) {
            _current_query = nullptr;
            _query_sent    = false;
            return;
        }
        auto query     = _current_command->pop_query();
        _current_query = nullptr; // a callback may disconnect and re-enter error handling
        ++_completion_callback_depth;
        auto callback_guard = qb::scope_guard([this] { finish_query_callback(); });
        count_query(*query, std::exchange(_query_sent, false), nullptr);
        query->on_success();
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
        _replan = replan_phase::none;
        if (!_current_command) {
            _current_query = nullptr;
            _query_sent    = false;
            return;
        }
        _current_command->result(false);
        auto query     = _current_command->pop_query();
        _current_query = nullptr;
        ++_completion_callback_depth;
        auto callback_guard = qb::scope_guard([this] { finish_query_callback(); });
        count_query(*query, std::exchange(_query_sent, false), &err);
        query->on_error(err);
    }

private:
    std::string          _nonce;         ///< Client nonce for SCRAM authentication
    std::vector<uint8_t> _password_salt; ///< Salted password for SCRAM authentication
    std::string          _auth_message;  ///< Authentication message for SCRAM protocol
    /// SECURITY (SCRAM mutual auth): true once a SCRAM-SHA-256 exchange has started. While set,
    /// AuthenticationOk MUST NOT be accepted unless _scram_server_verified is also true — otherwise
    /// an impersonating server / active MITM that does not know the password can skip
    /// AuthenticationSASLFinal and send AuthenticationOk to be trusted (defeats SCRAM mutual auth).
    bool _scram_pending = false;
    /// Set only when the server's SASLFinal ServerSignature (`v=`) verified successfully.
    bool                       _scram_server_verified = false;
    std::string                _gs2_header;      ///< SCRAM gs2-header chosen at SASL init (`n,,` / `y,,` / `p=tls-server-end-point,,`)
    std::vector<unsigned char> _channel_binding; ///< SCRAM-SHA-256-PLUS channel-binding data (tls-server-end-point); empty when unbound
    struct CopyOperation {
        struct SinkInvocation {
            CopyOperation                                         &owner;
            std::shared_ptr<CopyOperation>                         owner_pin;
            std::shared_ptr<std::function<void(std::string_view)>> sink_pin;
            bool                                                   outermost;

            explicit SinkInvocation(CopyOperation &operation) noexcept
                : owner(operation)
                , outermost(!owner.sink_invocation) {
                if (outermost)
                    owner.sink_invocation = this;
            }

            ~SinkInvocation() noexcept {
                // Clear the stack pointer before either pin releases a callback or
                // operation. Nested calls share the outermost invocation's lifetime.
                if (outermost)
                    owner.sink_invocation = nullptr;
            }

            SinkInvocation(SinkInvocation const &)            = delete;
            SinkInvocation &operator=(SinkInvocation const &) = delete;
        };

        std::shared_ptr<std::function<void(std::string_view)>>       sink;
        std::shared_ptr<std::function<std::optional<std::string>()>> source;
        ISqlQuery                                                   *query{};
        SinkInvocation                                              *sink_invocation{};

        void
        detach_sink() noexcept {
            if (!sink)
                return;
            if (sink_invocation)
                sink_invocation->sink_pin = std::move(sink);
            else
                sink.reset();
        }
    };
    // The query callbacks release the reservation. A cancelled coroutine detaches its
    // user callback immediately, but its SQL may still be in flight and must not let a
    // later COPY's callback answer the old server response.
    std::shared_ptr<CopyOperation> _copy_operation;
    std::shared_ptr<CopyOperation> _active_copy_operation;

    void
    finish_copy(std::shared_ptr<CopyOperation> const &owner) {
        // Snapshot identity before moving a connection-owned handle: owner may
        // alias it. Retention is only needed when completion re-enters a sink.
        auto *const copy       = owner.get();
        auto *const invocation = copy->sink_invocation;
        if (invocation && !invocation->owner_pin) {
            if (_active_copy_operation.get() == copy)
                invocation->owner_pin = std::move(_active_copy_operation);
            else if (_copy_operation.get() == copy)
                invocation->owner_pin = std::move(_copy_operation);
            else
                invocation->owner_pin = owner;
        }
        if (_active_copy_operation.get() == copy)
            _active_copy_operation.reset();
        if (_copy_operation.get() == copy)
            _copy_operation.reset();
    }

    [[nodiscard]] pg_reply_awaiter<resultset>
    execute_copy(std::string sql, std::shared_ptr<CopyOperation> owner) {
        return pg_reply_awaiter<resultset>{[this, sql = std::move(sql),
                                            owner = std::move(owner)](pg_coro_complete<resultset> complete) mutable {
            auto success = [this, owner, complete](Transaction &, resultset rs) mutable {
                finish_copy(owner);
                complete(::qb::pg::Reply<resultset>::success(rs.deep_snapshot()));
            };
            auto failure = [this, owner, complete](error::db_error const &err) mutable {
                finish_copy(owner);
                complete(::qb::pg::Reply<resultset>::failure(err));
            };
            using Command = ResultQuery<decltype(success), decltype(failure)>;
            auto command  = std::make_unique<Command>(this, std::move(sql), std::move(success), std::move(failure));
            owner->query  = command->next_query();
            this->push_transaction(std::move(command));
        }};
    }
    char _txn_status{'I'}; ///< Last ReadyForQuery transaction status: 'I' idle, 'T' in a block, 'E' failed block

    /// Where the execution in flight stands with the re-preparation of its statement (Huly QB-114).
    enum class replan_phase : std::uint8_t {
        none,    ///< No re-preparation
        owed,    ///< The server refused it, its cached plan stale, outside a block: the next ReadyForQuery re-prepares
        parsing, ///< The Close + Parse + Describe of its statement is in flight
        resent   ///< Sent again after its statement was re-prepared: it is not retried twice
    };
    replan_phase         _replan{replan_phase::none};
    row_description_type _replan_columns{};        ///< The re-prepared statement's result columns
    bool                 _replan_columns_ok{true}; ///< false: their RowDescription was malformed
    bool                 _query_bound{false};      ///< The query in flight got its BindComplete: a stale plan raised after it is the caller's

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

        QB_LOG_DEBUG("[pgsql] Handle auth_event");
        switch (auth_state) {
            case OK: {
                // SCRAM mutual auth: if a SCRAM-SHA-256 exchange is in flight, the server MUST have
                // proved knowledge of the password via a verified SASLFinal ServerSignature before
                // we accept AuthenticationOk. Without this gate an impersonating server / active
                // MITM that never sent (or failed) AuthenticationSASLFinal would be trusted — the
                // client's own SCRAM proof leaks nothing, so nothing else stops it.
                if (_scram_pending && !_scram_server_verified) {
                    QB_LOG_CRIT("[pgsql] AuthenticationOk without a verified SCRAM server signature — refusing (possible MITM/impersonation)");
                    refuse_startup_(error::connection_error{"AuthenticationOk without a verified SCRAM server signature"});
                    break;
                }
                QB_LOG_INFO("[pgsql] Authenticated with server");
                _session_live = true;
                // Apply keepalive settings if configured (P1-1)
                apply_keepalive_settings();
                begin_session_();
            } break;
            case Cleartext: {
                QB_LOG_INFO("[pgsql] Clear text authentication requested");
                message pm(password_message_tag);
                pm.write(conn_opts_.password);

                send_message(pm);
            } break;
            case MD5Password: {
#ifndef QB_HAS_SSL
                // qb::crypto::md5 does not exist without OpenSSL.
                refuse_auth_without_ssl("MD5");
#else
                QB_LOG_INFO("[pgsql] MD5 authentication requested");
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

                send_message(pm);
#endif // QB_HAS_SSL
            } break;
            case SCRAM_SHA256: {
#ifndef QB_HAS_SSL
                // The client nonce needs a CSPRNG and the proof needs PBKDF2/HMAC —
                // neither exists without OpenSSL.
                refuse_auth_without_ssl("SCRAM-SHA-256");
#else
                QB_LOG_INFO("[pgsql] SCRAM-SHA-256 authentication requested");
                _scram_pending         = true; // gate AuthenticationOk until SASLFinal verifies (mutual auth)
                _scram_server_verified = false;

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
                send_message(pm);
#endif // QB_HAS_SSL
            } break;
            case SCRAM_SHA256_CLIENT_PROOF: {
#ifndef QB_HAS_SSL
                // Unreachable against a well-behaved server (we refused the SASL start
                // above), but a hostile/buggy peer can still send it unsolicited.
                refuse_auth_without_ssl("SCRAM-SHA-256");
#else
                QB_LOG_INFO("[pgsql] SCRAM-SHA-256 authentication client proof check");
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
                send_message(pm);
                _password_salt = std::move(saltedPassword);
#endif // QB_HAS_SSL
            } break;
            case SCRAM_SHA256_SERVER_CHECK: {
#ifndef QB_HAS_SSL
                refuse_auth_without_ssl("SCRAM-SHA-256");
#else
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
                    // Constant-time compare of the raw 32-byte HMAC values (base64-decode the
                    // received `v=`) rather than the base64 strings, whose operator!= short-circuits.
                    // This value is the SCRAM mutual-auth linchpin, so it must not leak via timing.
                    std::vector<unsigned char> receivedServerSignature = qb::crypto::base64_decode(receivedServerSignatureBase64);
                    if (!qb::crypto::constant_time_compare(computedServerSignature, receivedServerSignature)) {
                        throw std::runtime_error("server signature does not match. Authentication failed");
                    }
                    _scram_server_verified = true; // mutual auth satisfied — AuthenticationOk may now be accepted
                    QB_LOG_INFO("[pgsql] SCRAM-SHA-256 Authentication successful: server "
                                "signature verified");
                    break;
                } catch (std::exception &ex) {
                    QB_LOG_CRIT("[pgsql] SCRAM-SHA-256 Failed verifying server signature: " << ex.what());
                    refuse_startup_(error::connection_error{std::string("SCRAM-SHA-256 server signature: ") + ex.what()});
                }
#endif // QB_HAS_SSL
            } break;
            default: {
                QB_LOG_CRIT("[pgsql] Unsupported authentication scheme " << auth_state << "requested by server");
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
        QB_LOG_DEBUG("[pgsql] Command complete (" << cmpl.command_tag << ")");
        if (cmpl.command_tag.size() == 6 && cmpl.command_tag[0] == 'L' && cmpl.command_tag == "LISTEN") [[unlikely]]
            _raw_listen_seen = true; // cleared by listen()'s own completion, before ReadyForQuery reads it
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
        QB_LOG_DEBUG("[pgsql] Received backend key data");
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

        QB_LOG_WARN("[pgsql] Error " << notice);
        // Every field the server sent, kept whole and shared by the error's copies (Huly QB-108).
        error::query_error err(
            std::make_shared<const ::qb::pg::server_error_fields>(std::move(static_cast<::qb::pg::server_error_fields &>(notice))));

        if (startup_pending_()) [[unlikely]] {
            on_startup_error_(err);
            return;
        }
        if (is_stale_cached_plan(err)) [[unlikely]] {
            if (keep_for_replan_())
                return;
        }
        on_error_query(err);
    }

    /**
     * @brief SQLSTATE 0A000 from the server's plan cache: the result columns of the statement changed
     *        since it was prepared -- an ALTER TABLE, a `SELECT *` over a table that gained a column
     *        (Huly QB-114)
     * @details The routine field names it: the message is localized, and 0A000 is any feature the
     *          server does not support.
     */
    [[nodiscard]] static bool
    is_stale_cached_plan(error::db_error const &err) noexcept {
        return err.sqlstate == ::qb::pg::sqlstate::feature_not_supported && err.routine() == "RevalidateCachedQuery";
    }

    /**
     * @brief The execution in flight was refused because its statement's cached plan went stale
     *        (Huly QB-114): the statement is marked, so its next execution re-prepares it first
     * @details Only a refused Bind is the statement's own stale plan: the server sends BindComplete
     *          once that plan is valid, so a 0A000 after it was raised by the execution -- a function
     *          that EXECUTEs a SQL-level prepared statement, with or without rows before it. That one
     *          is the caller's error: the statement is not marked, and the execution, which may have
     *          done things a transaction does not undo, is not replayed. A refused Bind is kept, and
     *          retried once its statement is prepared again, when it ran outside a transaction block and
     *          was not already retried: the server refused it before it did anything, and the implicit
     *          transaction it ran in ended with the error. In a block the error is the caller's: the
     *          block is aborted, and a retry would only fail with 25P02 (in_failed_sql_transaction).
     * @return true when the execution is kept for its retry, its error unreported
     */
    QB_NOINLINE QB_COLD bool
    keep_for_replan_() {
        if (!_current_query)
            return false;
        const auto name = _current_query->prepared_statement();
        if (name.empty() || _query_bound)
            return false;
        storage_.mark_stale(name);
        if (_replan != replan_phase::none || _txn_status != 'I' || !storage_.is_stale(name))
            return false;
        _replan = replan_phase::owed;
        return true;
    }

    /**
     * @brief The query about to be sent binds a statement marked stale (Huly QB-114): its
     *        re-preparation goes first, and the ReadyForQuery that answers it sends the execution
     * @details Not in a failed block (`'E'`): the server would refuse the Parse with 25P02 as it
     *          refuses the Bind, so the Bind goes as it is and the statement stays marked for the
     *          first execution after the ROLLBACK.
     * @return true when the re-preparation was sent in the execution's place
     */
    QB_NOINLINE QB_COLD bool
    reprepare_before_execute_() {
        const auto name = _current_query->prepared_statement();
        if (name.empty() || _txn_status == 'E' || !storage_.is_stale(name))
            return false;
        _query_sent_at = clock_now();
        _query_sent    = true;
        send_reprepare_(*storage_.find(name));
        return true;
    }

    /// Close + Parse + Describe + Sync of a stale statement, counted in `stats().prepared_replans`.
    void
    send_reprepare_(PreparedQuery const &statement) {
        QB_LOG_DEBUG("[pgsql] Re-prepare \"" << statement.name << "\": its cached plan is stale");
        _replan = replan_phase::parsing;
        _replan_columns.clear();
        _replan_columns_ok = true;
        ++_stats.prepared_replans;
        send_message(reprepare_statement(storage_, statement));
    }

    /**
     * @brief The ReadyForQuery that ends a step of a re-preparation (Huly QB-114)
     * @details After the refused execution, the Close + Parse + Describe of its statement leaves. After
     *          that, the statement's new result columns are recorded -- the Bind takes its result formats
     *          from them -- and the execution is sent again. A statement evicted meanwhile
     *          (`prepared_cache_capacity()`), or a malformed RowDescription, fails the execution.
     * @param msg The ReadyForQuery
     */
    QB_NOINLINE QB_COLD void
    continue_replan_(message &msg) {
        char stat(0);
        msg.read(stat);
        if (stat == 'I' || stat == 'T' || stat == 'E')
            _txn_status = stat;
        PreparedQuery const *const statement = storage_.find(_current_query->prepared_statement());
        const char                *failure   = nullptr;
        if (!statement)
            failure = "the prepared statement was evicted while it was re-prepared";
        else if (_replan == replan_phase::owed) {
            send_reprepare_(*statement);
            return;
        } else if (!_replan_columns_ok)
            failure = "malformed RowDescription while re-preparing a statement";
        if (failure) {
            on_error_query(error::client_error{failure});
            if (_session_live && !process_query(_current_command) && _session_live)
                _ready_for_query = true;
            return;
        }
        storage_.refresh(statement->name, std::move(_replan_columns));
        _replan = replan_phase::resent;
        send_message(_current_query->get());
    }

    /**
     * @brief The server answered the StartupMessage with an error (Huly QB-111).
     * @details SQLSTATE class 28 (invalid authorization: a wrong password, a role that may not log in, a
     *          pg_hba rejection) and 3D000 (the database does not exist) refuse the session: no run retries
     *          them. Anything else -- 57P03 (the server is starting up), 53300 (too many connections), ... --
     *          fails this attempt only; the server closes the link after a startup error either way.
     */
    QB_NOINLINE QB_COLD void
    on_startup_error_(error::db_error const &err) {
        _error = err;
        if (err.code.size() == 5 && (err.code.starts_with("28") || err.code == "3D000")) {
            refuse_startup_(err);
            return;
        }
        _startup->settle(startup_outcome::failed);
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

        QB_LOG_DEBUG("[pgsql] Received parameter " << key << "=" << value);

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

        if (notice_handler_) {
            deliver_notice(notice);
            return;
        }
        QB_LOG_INFO("[pgsql] Received notice" << notice);
    }

    /**
     * @brief Handles ready for query messages
     *
     * @param msg Ready for query message
     */
    void
    on_ready_for_query(message &msg) {
        // One clock read serves the query this completes and the next one process_query sends.
        _ready_clock     = qb::mono_now();
        _ready_clock_set = true;
        auto clock_guard = qb::scope_guard([this] { _ready_clock_set = false; });
        if (startup_pending_() && _session_live) [[unlikely]]
            _startup->settle(startup_outcome::up);
        if (_replan == replan_phase::owed || _replan == replan_phase::parsing) [[unlikely]] {
            if (_current_query && _session_live) {
                continue_replan_(msg);
                return;
            }
            _replan = replan_phase::none;
        }
        on_success_query();
        if (_raw_listen_seen) [[unlikely]]
            note_raw_listen_();
        if (!_session_live)
            return; // a success callback may have disconnected this session
        char stat(0);
        msg.read(stat);
        // I = idle, T = in transaction block, E = failed transaction (must ROLLBACK)
        if (stat == 'I' || stat == 'T' || stat == 'E')
            _txn_status = stat;
        if (stat == 'E') {
            QB_LOG_WARN("[pgsql] ReadyForQuery: backend session is in failed transaction "
                        "(SQLSTATE implicit); issue ROLLBACK before new commands");
        }

        if (!process_query(_current_command) && _session_live) {
            _ready_for_query = true;
            QB_LOG_DEBUG("[pgsql] Database " << conn_opts_.uri << "[" << conn_opts_.database << "]"
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
            QB_LOG_WARN("[pgsql] RowDescription with missing or negative column count");
            if (_replan == replan_phase::parsing) [[unlikely]] {
                _replan_columns_ok = false;
                return;
            }
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
                QB_LOG_WARN("[pgsql] Failed to read field description " << i);
                if (_replan == replan_phase::parsing) [[unlikely]] {
                    _replan_columns_ok = false;
                    return;
                }
                _current_command->result(false);
                break;
            }
        }
        if (_replan == replan_phase::parsing) [[unlikely]] {
            _replan_columns = std::move(fields); // the re-prepared statement's, not a result's (Huly QB-114)
            return;
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
            QB_LOG_WARN("[pgsql] Failed to read data row");
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
        QB_LOG_DEBUG("[pgsql] Parse complete");
    }

    /**
     * @brief Handles parameter description messages
     *
     * @param msg Parameter description message
     */
    void
    on_parameter_description(message &) {
        QB_LOG_DEBUG("[pgsql] Parameter descriptions");
    }

    /**
     * @brief Handles bind complete messages
     *
     * @param msg Bind complete message
     */
    void
    on_bind_complete(message &) {
        _query_bound = true; // its statement's plan is valid: a stale plan raised after this is the caller's (Huly QB-114)
        QB_LOG_DEBUG("[pgsql] Bind complete");
    }

    /**
     * @brief Handles no data messages
     *
     * @param msg No data message
     */
    void
    on_no_data(message &) {
        QB_LOG_DEBUG("[pgsql] No data");
    }

    /**
     * @brief Handles portal suspended messages
     *
     * @param msg Portal suspended message
     */
    void
    on_portal_suspended(message &) {
        QB_LOG_DEBUG("[pgsql] Portal suspended");
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
        QB_LOG_DEBUG("[pgsql] Empty query response");
    }

    /**
     * @note COPY is a product feature: `copy_in()` streams **COPY FROM STDIN** from a source and
     *       `copy_out()` hands **COPY TO STDOUT** CopyData chunks to a sink (the handlers below and
     *       the API further down). CopyBoth -- replication's bidirectional mode -- is NOT supported:
     *       `on_copy_both_response` handles it as a COPY OUT and logs a warning.
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
        send_message(fail);
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
            QB_LOG_WARN("[pgsql] Malformed NotificationResponse");
            return;
        }
        ++_stats.notifications_received;
        ::qb::pg::notification n;
        n.server_backend_pid = static_cast<int>(pid);
        n.channel            = std::move(channel);
        n.payload            = std::move(payload);
        if (_state != qb::io::connection_state::ready) [[unlikely]] {
            // The replayed LISTEN is live while the hook still runs: what arrives now waits for `ready`,
            // behind the gap the new session opened (Huly QB-111).
            _held_notifications.push_back(std::move(n));
            return;
        }
        deliver_notification_(std::move(n));
    }

    /// Hand one NOTIFY to the consumer, the plain database's handler, or the log.
    void
    deliver_notification_(::qb::pg::notification &&n) {
        if constexpr (!std::is_same_v<NotifyDerived, void>) {
            static_cast<NotifyDerived *>(this)->consume_pg_notify(std::move(n));
        } else if (inbound_notify_handler_) {
            inbound_notify_handler_(std::move(n));
        } else {
            QB_LOG_INFO("[pgsql] NOTIFY pid=" << n.server_backend_pid << " channel=" << n.channel << " payload=" << n.payload);
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
            QB_LOG_WARN("[pgsql] Malformed CopyInResponse");
            send_copy_fail();
            return;
        }
        if (ncols < 0 || ncols > 4096) {
            QB_LOG_WARN("[pgsql] CopyInResponse invalid column count " << ncols);
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
        auto copy   = _active_copy_operation;
        auto source = copy ? copy->source : nullptr;
        if (!source || !*source) {
            QB_LOG_WARN("[pgsql] CopyInResponse with no copy_in() source registered; failing the COPY");
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
            while (true) {
                std::optional<std::string> chunk = (*source)();
                if (!_session_live)
                    return; // the source may have disconnected the session
                // The source may destroy its own awaiting coroutine. Its scope guard
                // detaches the callback, but this local shared_ptr keeps the current
                // invocation alive. Do not call it again or send its returned bytes.
                if (_active_copy_operation != copy || copy->source != source) {
                    QB_LOG_WARN("[pgsql] copy_in source detached during callback; aborting the COPY with CopyFail");
                    send_copy_fail();
                    return;
                }
                if (!chunk)
                    break;
                std::string_view rest{*chunk};
                while (!rest.empty()) { // empty chunk -> skipped; large chunk -> split
                    const std::size_t take = std::min(rest.size(), kMaxCopyDataBody);
                    message           d(copy_data_tag);
                    d.write_sv(rest.substr(0, take));
                    send_message(d);
                    rest.remove_prefix(take);
                }
            }
        } catch (...) {
            QB_LOG_WARN("[pgsql] copy_in source threw; aborting the COPY with CopyFail");
            if (_session_live)
                send_copy_fail();
            return;
        }
        if (!_session_live)
            return;
        message done(copy_done_tag);
        send_message(done);
    }

    /**
     * @brief CopyOutResponse — server will send COPY data (COPY ... TO STDOUT)
     */
    void
    on_copy_out_response(message &msg) {
        char     overall{};
        smallint ncols{};
        if (!msg.read(overall) || !msg.read(ncols)) {
            QB_LOG_WARN("[pgsql] Malformed CopyOutResponse");
            msg.discard_remaining();
            return;
        }
        if (ncols < 0 || ncols > 4096) {
            QB_LOG_WARN("[pgsql] CopyOutResponse invalid column count " << ncols);
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
        QB_LOG_DEBUG("[pgsql] CopyOutResponse received (data will pass as CopyData messages)");
    }

    /**
     * @brief CopyBothResponse — bidirectional COPY (replication / rare)
     */
    void
    on_copy_both_response(message &msg) {
        on_copy_out_response(msg);
        QB_LOG_WARN("[pgsql] CopyBothResponse: bidirectional COPY not fully supported");
    }

    /**
     * @brief CopyData from server during COPY OUT / BOTH (payload is opaque row bytes)
     */
    void
    on_copy_data(message &msg) {
        // During a COPY ... TO STDOUT, hand the opaque payload (one row, or a chunk in
        // binary format) to the active sink without copying. No sink -> drop the chunk.
        // The operation owns the callable through ordinary rows. Detachment moves
        // its strong handle into the outermost invocation only on cancellation;
        // finish_copy retains the operation there before clearing its owners.
        auto *const copy = _active_copy_operation.get();
        auto *const sink = copy ? copy->sink.get() : nullptr;
        if (sink && *sink) {
            const std::string_view chunk = msg.remaining();
            if (!chunk.empty()) {
                typename CopyOperation::SinkInvocation invocation(*copy);
                (*sink)(chunk);
            }
        }
        msg.discard_remaining();
    }

    /**
     * @brief CopyDone from server (end of COPY OUT data stream)
     */
    void
    on_copy_done(message &msg) {
        QB_LOG_DEBUG("[pgsql] CopyDone (server)");
        msg.discard_remaining();
    }

    /**
     * @brief CloseComplete — acknowledgment of Close (frontend); usually empty body
     */
    void
    on_close_complete(message &msg) {
        QB_LOG_DEBUG("[pgsql] CloseComplete");
        msg.discard_remaining();
    }

    /**
     * @brief FunctionCallResponse — legacy fast-path function protocol (rare)
     */
    void
    on_function_call_response(message &msg) {
        QB_LOG_WARN("[pgsql] FunctionCallResponse (V): fast-path function protocol not "
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
        QB_LOG_WARN("[pgsql] Unhandled backend message tag " << (char) msg.tag() << " (length " << msg.length()
                                                             << ") — check protocol coverage");
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
     * Marks the connection gone for every engine coroutine that is still suspended (they check the
     * liveness token before touching `*this`), and settles the protocol-handshake wait, which no message
     * can settle any more, so its attempt unwinds instead of staying suspended. A session copied out of
     * a handshake hook fails from here on with "handshake over". The socket is closed by the TCP client
     * base class.
     */
    ~Database() {
        *_alive          = false;
        _gate->live      = 0;
        _gate->admitting = false;
        if (auto startup = std::exchange(_startup, nullptr))
            startup->settle(startup_outcome::failed);
        if (auto replay = std::exchange(_replay, nullptr))
            replay->settle(error::connection_error{"database destroyed"});
    }

    /**
     * @struct connect_awaiter
     * @brief Coroutine awaiter for `co_await db.connect()` (see also `run_sync` in tests): `true` once the
     *        connection is ready.
     * @details Ready means the whole handshake ran on the new connection: authentication, the first
     *          ReadyForQuery, the hook installed with `set_handshake()` and the replay of the listened
     *          channels (Huly QB-111). Resolves `false` when the attempt failed, was refused or was
     *          superseded by `disconnect()` -- the connection is then `down`, `down_reason()` says why and
     *          `error()` holds the failure -- and at once when a connection, a handshake or a reconnection
     *          run is already in progress. Resolves `true` at once on a connection that is already ready.
     *          An awaiter made by `connect(dsn, raw_io)` carries that transport and adopts it when awaited
     *          (Huly QB-1096); destroyed unawaited, it closes it.
     */
    struct connect_awaiter {
        Database<QB_IO_, NotifyDerived>                  &db;
        qb::duration                                      timeout{};
        std::shared_ptr<connect_completion>               done{};
        std::optional<typename QB_IO_::transport_io_type> adopted{}; ///< the transport to run the handshake on, if any

        explicit connect_awaiter(Database<QB_IO_, NotifyDerived> &d, qb::duration t = qb::duration::zero()) noexcept
            : db(d)
            , timeout(t) {}

        connect_awaiter(Database<QB_IO_, NotifyDerived> &d, typename QB_IO_::transport_io_type &&raw_io)
            : db(d)
            , adopted(std::in_place, std::move(raw_io)) {}

        ~connect_awaiter() {
            if (done)
                done->waiter = {}; // a frame destroyed while connecting is never resumed
        }

        connect_awaiter(connect_awaiter const &)            = delete;
        connect_awaiter &operator=(connect_awaiter const &) = delete;
        connect_awaiter(connect_awaiter &&)                 = default;
        connect_awaiter &operator=(connect_awaiter &&)      = default;

        [[nodiscard]] bool
        await_ready() const noexcept {
            // An adopting connect always goes through await_suspend: on a ready connection it must still
            // close the transport it was given, and say false.
            return !adopted && db._state == qb::io::connection_state::ready;
        }

        bool
        await_suspend(std::coroutine_handle<> h) {
            qb::io::async::track_suspension(h, "pgsql connect");
            done         = std::make_shared<connect_completion>();
            done->waiter = h;
            if (adopted ? db.begin_adopt_(std::move(*adopted), timeout, done) : db.begin_connect_(timeout, done))
                return true;
            adopted.reset(); // busy or ready: the transport was not adopted, and closes here
            done->waiter = {};
            return false; // resolves false at once
        }

        [[nodiscard]] bool
        await_resume() const noexcept {
            return done ? done->ok : db._state == qb::io::connection_state::ready;
        }
    };

    /**
     * @brief Start async connection (`co_await` or `run_sync(db.connect())`): `true` once the connection
     *        is ready (see `connect_awaiter`). One attempt; `connect_with_retry()` makes several.
     */
    [[nodiscard]] connect_awaiter
    connect() {
        return connect_awaiter{*this, qb::duration::zero()};
    }

    /** @brief Same as connect() with an explicit timeout override: the deadline of the transport connect
     *         and, from the StartupMessage on, of the whole handshake. */
    [[nodiscard]] connect_awaiter
    connect(qb::duration timeout) {
        return connect_awaiter{*this, timeout};
    }

    /** @brief Parse connection string then connect. The options change only while the connection is idle
     *         or down: a live connection, and the reconnection run that may follow it, keep theirs. */
    [[nodiscard]] connect_awaiter
    connect(std::string const &conn_opts) {
        if (_state == qb::io::connection_state::idle || _state == qb::io::connection_state::down)
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
     * ssl_verify_mode::full; co_await db.connect(o);`. As with the string form, the options change
     * only while the connection is idle or down.
     */
    [[nodiscard]] connect_awaiter
    connect(connection_options opts) {
        if (_state == qb::io::connection_state::idle || _state == qb::io::connection_state::down)
            conn_opts_ = std::move(opts);
        return connect();
    }

    /**
     * @brief Connect with exponential-backoff retry: the policy's attempts, each a transport connect and a
     *        whole handshake, until the connection is ready (Huly QB-111).
     *
     * Attempt 1 is immediate; between attempts the run sleeps the policy's `next_delay()` (jittered) and
     * calls `on_retry(attempt, next_delay)` first. A handshake the server refuses -- SQLSTATE class 28 or
     * 3D000 -- or that the hook refuses ends the run at once, `down` with `handshake_refused`; a spent
     * policy ends it `down` with `retries_exhausted`. Resolves `false` at once when a connection, a
     * handshake or a run is already in progress. If the connection is destroyed while the run waits, the
     * task ends without touching it.
     *
     * @param policy The attempts and their backoff (defaults: unlimited attempts).
     * @return A task yielding `true` once the connection is ready, `false` otherwise.
     */
    qb::io::async::task<bool>
    connect_with_retry(RetryPolicy policy = RetryPolicy{}) {
        if (_state == qb::io::connection_state::ready)
            co_return true;
        if (!begin_lifecycle_())
            co_return false;
        co_return co_await establish_(std::move(policy), run_mode::run, _alive, _epoch);
    }

    /** @brief Parse connection string then `connect_with_retry(policy)` (the options change only while idle or down). */
    qb::io::async::task<bool>
    connect_with_retry(std::string const &conn_opts, RetryPolicy policy = RetryPolicy{}) {
        if (_state == qb::io::connection_state::idle || _state == qb::io::connection_state::down)
            conn_opts_ = connection_options::parse(conn_opts);
        co_return co_await connect_with_retry(std::move(policy));
    }

    /**
     * @brief Arm the reconnection run for a lost ready connection (Huly QB-111).
     * @details A loss the client did not ask for -- the server closed or reset the link, a message the
     *          parser refused -- starts a run of the policy's attempts, each one a whole handshake
     *          (authentication, the hook, the replay of the listened channels); an explicit
     *          `disconnect()` never does. The policy is read when the loss happens: arming it later arms
     *          the next loss. Work submitted while the connection is not ready is refused, as before
     *          (`connection is not established`); what was in flight when the link dropped fails once, with
     *          `database disconnected`, and is never replayed.
     * @param policy Retry/backoff policy of the run.
     * @return *this
     */
    Database<QB_IO_, NotifyDerived> &
    enable_auto_reconnect(RetryPolicy policy = RetryPolicy{}) noexcept {
        _reconnect_policy = std::move(policy);
        return *this;
    }

    /** @brief Disarm the reconnection run for the next loss (a run in progress still finishes). */
    Database<QB_IO_, NotifyDerived> &
    disable_auto_reconnect() noexcept {
        _reconnect_policy.reset();
        return *this;
    }

    /** @brief Whether a reconnection run is in progress: from the loss until it is ready or down. */
    [[nodiscard]] bool
    is_reconnecting() const noexcept {
        return _recovering;
    }

    /**
     * @brief Whether the connection is ready: the whole handshake ran on the current link (Huly QB-111).
     * @details `state() == qb::io::connection_state::ready`. Until 3.3 this was the io base's flag, true
     *          on a database that had never connected; it hides that flag now.
     */
    [[nodiscard]] bool
    is_connected() const noexcept {
        return _state == qb::io::connection_state::ready;
    }

    /** @brief Where the connection stands (`qb::io::connection_state`, Huly QB-111). */
    [[nodiscard]] qb::io::connection_state
    state() const noexcept {
        return _state;
    }

    /** @brief Why the connection is down; meaningful in `connection_state::down` (Huly QB-111). */
    [[nodiscard]] qb::io::down_reason
    down_reason() const noexcept {
        return _down_reason;
    }

    /**
     * @brief Install the handshake hook: it runs on every connection, the first one included, after the
     *        protocol's handshake and before the connection is ready (Huly QB-111).
     * @details The hook receives a `qb::pg::handshake_session&` -- the only way to send a query while the
     *          connection is not ready -- and returns `true` to let the connection become ready. `false`, or
     *          a throw, refuses it: the connection goes `down` with `handshake_refused` and no run retries
     *          it. A query of the hook that fails is the hook's to judge; a link lost under the hook is one
     *          more attempt. Its typical work is what a new session needs before the program's own
     *          queries: `SET` a session parameter, prepare the statements the program executes by name.
     *          Set it before `connect()`; an empty function removes it.
     * @return *this
     */
    Database<QB_IO_, NotifyDerived> &
    set_handshake(handshake_hook hook) {
        _hs_hook = std::move(hook);
        return *this;
    }

    /** @brief Called each time the connection becomes ready: the first connection, and after every
     *         reconnection (Huly QB-111). */
    Database<QB_IO_, NotifyDerived> &
    on_connected(std::function<void()> hook) {
        _on_connected = std::move(hook);
        return *this;
    }

    /**
     * @brief Called once for every ready connection lost, with its cause (`user`, `peer`, `protocol`)
     *        (Huly QB-111).
     * @details After the work in flight on it failed, before the reconnection decision: `state()` reads
     *          `reconnecting` or `down` as the policy stands, and a hook that disarms the policy prevents
     *          the run. `on_connected` and `on_disconnected` strictly alternate.
     */
    Database<QB_IO_, NotifyDerived> &
    on_disconnected(std::function<void(qb::io::down_reason)> hook) {
        _on_disconnected = std::move(hook);
        return *this;
    }

    /** @brief Called each time the connection reaches `down`, with the reason (Huly QB-111). */
    Database<QB_IO_, NotifyDerived> &
    on_down(std::function<void(qb::io::down_reason)> hook) {
        _on_down = std::move(hook);
        return *this;
    }

    /**
     * @brief The channels this connection keeps listening on across reconnections (Huly QB-111).
     * @details Maintained by `listen()`, `unlisten()` and `unlisten_all()` when the server confirms them,
     *          replayed at the start of every new session of the same lifecycle, and cleared by an explicit
     *          `disconnect()`, which ends the session. A `LISTEN` written as SQL is not in it.
     */
    [[nodiscard]] std::set<std::string, std::less<>> const &
    listened_channels() const noexcept {
        return _listened;
    }

    /**
     * @brief Adopt an already-connected transport and run the handshake on it (Huly QB-1096).
     *
     * For a transport a program already holds -- one from a pool, one a supervisor handed over: the
     * first connection of the new lifecycle uses it, with the whole handshake (authentication, the hook
     * installed with `set_handshake()`, the LISTEN replay) under the connection's `connect_timeout`.
     * `conn_opts` is the session's DSN all the same: it names the user and the database the
     * StartupMessage carries, and the address a reconnection run (`enable_auto_reconnect`) or a later
     * `connect()` dials -- only the first connection adopts.
     *
     * The transport must be connected. For `tcp::ssl::database` the caller must ALSO have run the
     * PostgreSQL TLS negotiation on it -- the SSLRequest packet, the server's `'S'`, then the TLS
     * handshake -- because the client writes the StartupMessage at once, as on a plain socket.
     *
     * Resolves `true` once the connection is ready. A transport that is not open fails the attempt:
     * `false`, the connection `down` with `down_reason::peer`. While a connection, a handshake or a run is
     * in progress, or once the connection is ready, the call resolves `false` and closes the moved-in
     * transport unused -- the connection already has its link and the transport was not adopted; a plain
     * `connect()` on a ready connection resolves `true`, since it asks for no particular transport. The
     * options change only while the connection is idle or down. Until 3.3 this overload stored the
     * transport and then dialled `conn_opts` itself, so the given transport was never used.
     *
     * @param conn_opts The session's DSN
     * @param raw_io The connected transport to adopt
     */
    [[nodiscard]] connect_awaiter
    connect(std::string const &conn_opts, typename QB_IO_::transport_io_type &&raw_io) {
        if (_state == qb::io::connection_state::idle || _state == qb::io::connection_state::down)
            conn_opts_ = connection_options::parse(conn_opts);
        return connect_awaiter{*this, std::move(raw_io)};
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
     * @brief Be told of every NoticeResponse the server sends on this connection (Huly QB-108).
     *
     * A notice is the non-error message a statement can emit -- `RAISE NOTICE` / `WARNING`, an
     * implicit index creation, a `DROP ... IF EXISTS` of nothing. @p fn receives every field the
     * server sent (`qb::pg::notice`, the same struct a server error keeps), on the connection's
     * thread, as the message is read; it must not drive this connection, and an exception escaping
     * it is contained and logged. Without a handler -- the default, or an empty function -- a
     * notice is logged at INFO as before. Replaces any previous handler.
     *
     * @param fn The handler, or an empty one to remove it
     * @return *this
     */
    Database<QB_IO_, NotifyDerived> &
    on_notice(std::function<void(::qb::pg::notice const &)> fn) {
        notice_handler_ = std::move(fn);
        return *this;
    }

    /**
     * @brief Bound the connection's prepared-statement cache (default 100 statements).
     *
     * The cache is an LRU of the statements `prepare()` created on this connection (the inline
     * `query(sql, args...)` path keeps its unnamed statement in one slot of it). A named statement
     * it evicts is closed on the server as well: `Close('S', name)` travels in the message of the
     * Parse that evicted it, or — after a shrink, which evicts at once — of the next Parse or Bind,
     * so the backend holds no more named statements than the cache does. Executing an evicted
     * name fails client-side ("not registered"); prepare it again. `0` restores the default.
     *
     * @param capacity Maximum number of cached statements
     * @return *this
     */
    Database<QB_IO_, NotifyDerived> &
    prepared_cache_capacity(std::size_t capacity) {
        storage_.set_max_size(capacity);
        return *this;
    }

    /** @brief The prepared-statement cache's bound (see `prepared_cache_capacity(std::size_t)`). */
    [[nodiscard]] std::size_t
    prepared_cache_capacity() const noexcept {
        return storage_.max_size();
    }

    /**
     * @brief What this connection has done: a snapshot, by value (Huly QB-115).
     *
     * Monotonic counts since the object was constructed -- two snapshots give a rate, and a
     * reconnect resets nothing -- plus the send-to-completion time of the last query that was
     * sent. A query is one round trip of the serial queue (`qb::pg::query_kind`); one that failed
     * before it was sent (refused locally, or drained by a disconnect) counts as a query and an
     * error. See `qb::pg::stats` for each field.
     */
    [[nodiscard]] ::qb::pg::stats
    stats() const noexcept {
        ::qb::pg::stats snapshot  = _stats;
        snapshot.prepared_hits    = storage_.hits();
        snapshot.prepared_misses  = storage_.misses();
        snapshot.prepared_evicted = static_cast<std::uint64_t>(storage_.evicted_count());
        return snapshot;
    }

    /**
     * @brief Be told of every completed query (Huly QB-115).
     *
     * @p hook receives one `qb::pg::query_info` per query, on the connection's thread, as the
     * query completes and before its own callback or awaiter sees the result: success, server
     * error, local refusal, or a failure drained by a disconnect. Keep it short; it must not drive
     * this connection (no query, no `disconnect()`). The views it receives are valid only during
     * the call, and `sql` may carry literal values: never log it raw. An exception escaping it is
     * contained and logged. An empty `std::function` -- the default -- removes it; without a hook
     * a completion costs one predictable branch.
     *
     * @param hook The callable, or an empty one to remove it
     * @return *this
     */
    Database<QB_IO_, NotifyDerived> &
    on_query_complete(std::function<void(::qb::pg::query_info const &)> hook) {
        _query_hook = std::move(hook);
        return *this;
    }

protected:
    /// A notify consumer could not queue a notification (`stats().notifications_dropped`).
    void
    count_notification_dropped() noexcept {
        ++_stats.notifications_dropped;
    }

    /// A notify consumer emitted a gap marker (`stats().gaps`, Huly QB-111).
    void
    count_gap() noexcept {
        ++_stats.gaps;
    }

public:
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

        if (_session_live) {
            apply_keepalive_settings();
        }
    }

    /**
     * @brief A new query may only be enqueued while the connection is ready (Huly QB-917, QB-111).
     *
     * Overrides Transaction::is_connection_usable() so coroutine entry points and the callback command
     * queue fail fast -- once, with `connection is not established` -- instead of enqueuing a command
     * that could leave before the handshake, or never. qbm-pgsql REFUSES work outside `ready` (idle,
     * connecting, handshaking, reconnecting, down); qbm-redis holds it. The one exception is the
     * handshake's own queries, queued through a live `handshake_session` and the LISTEN replay.
     * `disconnect()` and the loss of a ready link leave `ready` synchronously, before the work in flight
     * fails, so a failure callback that submits again is refused too.
     */
    [[nodiscard]] bool
    is_connection_usable() const noexcept override {
        return _state == qb::io::connection_state::ready || _gate->admitting;
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
        if (!_session_live) {
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
    in_transaction() const noexcept override {
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
     * @brief Did this connection authenticate with SCRAM-SHA-256, mutual auth included?
     * @return `true` only if the handshake ran a SASL/SCRAM-SHA-256 exchange **and** the
     *         server's `AuthenticationSASLFinal` ServerSignature (`v=`) verified.
     *
     * The negotiated method is chosen entirely by the server (`pg_hba.conf`) and is otherwise
     * invisible to the caller, so a downgrade — a server that starts asking for cleartext or
     * MD5, or a client change that stops running SASL — produces a perfectly working connection
     * and no observable difference at all. This is the observable: it is the same flag the
     * mutual-auth gate in `on_authentication` uses to decide whether `AuthenticationOk` may be
     * trusted, so it is `true` exactly when a real SCRAM exchange completed *and* the peer
     * proved it knew the password.
     *
     * Read-only; per connection. Reset by `prepare_reconnect()` and re-established by the next
     * handshake, so it always describes the CURRENT session, never a previous one.
     */
    [[nodiscard]] bool
    scram_authenticated() const noexcept {
        return _scram_server_verified;
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
     * Only one COPY operation may be active on a connection. An overlapping `copy_in`
     * or `copy_out` returns a failed reply without sending its SQL or invoking its
     * callback; retry it after the first COPY has completed.
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
        if (_copy_operation)
            co_return ::qb::pg::Reply<resultset>::failure(error::client_error{"another COPY is active on this connection"});
        auto owner      = std::make_shared<CopyOperation>();
        owner->sink     = std::make_shared<std::function<void(std::string_view)>>(std::move(sink));
        _copy_operation = owner;
        // A destroyed awaiting frame may own the sink's captures. Detach it now,
        // while the command keeps the reservation until its query callback runs.
        auto guard = qb::scope_guard([owner] { owner->detach_sink(); });
        co_return co_await execute_copy(std::move(sql), owner);
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
     * An overlapping COPY returns a failed reply without reading @p source. If
     * the awaiting coroutine is destroyed, its callback is detached while the
     * command keeps the connection's COPY reservation until it finishes.
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
        if (_copy_operation)
            co_return ::qb::pg::Reply<resultset>::failure(error::client_error{"another COPY is active on this connection"});
        auto owner      = std::make_shared<CopyOperation>();
        owner->source   = std::make_shared<std::function<std::optional<std::string>()>>(std::move(source));
        _copy_operation = owner;
        auto guard      = qb::scope_guard([owner] { owner->source.reset(); });
        co_return co_await execute_copy(std::move(sql), owner);
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
     * Cursors require a transaction. Inside a caller-opened transaction the cursor is simply
     * declared there and only the cursor is closed; otherwise `query_stream` opens its own
     * (`BEGIN` … `COMMIT`, or `ROLLBACK` on failure). PostgreSQL has a single transaction per
     * session, so streams that overlap on one connection **share** that self-opened block: the
     * first opens it, the last one to finish ends it, and it is rolled back if any of them
     * failed (see `_stream_txn_users`). Sharing one block also means a *server* error in one
     * stream aborts it for the overlapping others. If @p on_row throws, the cursor is closed
     * and the exception is rethrown.
     *
     * @note A caller-opened transaction is only recognised once its `BEGIN` has completed —
     *       `in_transaction()` mirrors the last ReadyForQuery. Do not start a stream while your
     *       own `begin()` is still in flight.
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

        // Who owns the transaction is decided HERE, synchronously, before the first co_await.
        // `in_transaction()` alone cannot decide it — it mirrors the last ReadyForQuery, so a
        // second stream starting while the first is still suspended on its own BEGIN reads 'I'
        // (idle) and emits a second BEGIN: PostgreSQL warns 25001 and keeps the one session
        // transaction, then the first stream to reach COMMIT ends it under the other, whose next
        // FETCH dies with "cursor does not exist". `_stream_txn_users` is the synchronous mirror
        // of that block (commands are enqueued in co_await order), so a non-zero count means a
        // BEGIN of ours is already ahead of us in the queue and this stream must only join.
        const bool caller_owns_txn = _stream_txn_users == 0 && !_stream_txn_closing && in_transaction();
        const bool opens_txn       = !caller_owns_txn && _stream_txn_users == 0;
        bool       registered      = !caller_owns_txn; // holds a seat in the self-opened block
        bool       closing         = false;            // this stream is the one ending that block
        if (registered) {
            if (opens_txn)
                _stream_txn_failed = false;
            ++_stream_txn_users;
        }
        // Release whatever this stream still holds even if its frame is destroyed mid-co_await
        // (cancellation / task drop — e.g. a when_all branch torn down): a leaked seat would make
        // every later stream believe a block is open and declare its cursor outside one. The
        // normal paths below clear both flags first, leaving this a no-op.
        auto txn_guard = qb::scope_guard([this, &registered, &closing] {
            if (registered)
                --_stream_txn_users;
            if (closing)
                _stream_txn_closing = false;
        });

        // Unique per stream on this connection — see `_stream_cursor_seq`. The name is built from
        // an internal counter only (never from caller input), so it stays a valid, unquoted SQL
        // identifier and carries no injection surface.
        const std::string cursor      = "qb_stream_cursor_" + std::to_string(_stream_cursor_seq++);
        const std::string declare_sql = "DECLARE " + cursor + " CURSOR FOR " + sql;
        const std::string fetch_sql   = "FETCH " + std::to_string(batch_size) + " FROM " + cursor;
        const std::string close_sql   = "CLOSE " + cursor;

        if (opens_txn) {
            auto b = co_await this->begin();
            if (!b.ok()) {
                // No block was opened, so there is nothing to end here — the guard drops the seat.
                // Mark the failure for any stream that joined behind us: it will ROLLBACK rather
                // than COMMIT (both only warn when no block is in progress).
                _stream_txn_failed = true;
                co_return qb::pg::Reply<void>::failure(b.error());
            }
        }

        bool                    failed   = false;
        bool                    declared = false;
        qb::pg::error::db_error err{"unknown error"};
        std::exception_ptr      user_exc;

        auto declare_reply = co_await this->execute(std::string_view{declare_sql});
        if (declare_reply.ok()) {
            declared = true;
        } else {
            err    = declare_reply.error();
            failed = true;
        }

        if (declared) {
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
        }

        if (registered) {
            registered = false;
            if (failed)
                _stream_txn_failed = true;
            if (--_stream_txn_users == 0) {
                // Last one out ends the shared block. The seat is dropped BEFORE the COMMIT /
                // ROLLBACK is enqueued and `_stream_txn_closing` is raised for exactly as long as
                // it is in flight, so a stream starting in that window opens a FRESH block — whose
                // BEGIN queues behind this COMMIT — instead of declaring its cursor into a block
                // that is already being closed (`in_transaction()` still reports the stale 'T').
                closing             = true;
                _stream_txn_closing = true;
                if (_stream_txn_failed) {
                    (void) co_await this->rollback();
                } else {
                    auto c = co_await this->commit();
                    if (!c.ok()) {
                        failed = true;
                        err    = c.error();
                    }
                }
                closing             = false;
                _stream_txn_closing = false;
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
     *       server that mandates SSL on every connection will reject it: `cancel_async()`
     *       negotiates TLS for a secure database, and blocks nothing (Huly QB-113).
     */
    bool
    cancel() {
        if (serverPid_ == 0)
            return false; // never received BackendKeyData -> nothing to cancel

        const auto pkt = cancel_request_packet();

        // cancel() is synchronous (like libpq's PQcancel) and is typically fired from a
        // timer ON the event loop, so the blocking connect+send must NOT stall the loop
        // for the full connect_timeout: `cancel_connect_budget()` caps it at 2 s. The
        // non-blocking form of the same request is `cancel_async()` (Huly QB-113).
        const qb::duration t_out = cancel_connect_budget();

        // Plain TCP / unix socket to the same endpoint (scheme resolved by tcp::socket;
        // an ssl:// endpoint connects plaintext here — the TLS layer is skipped).
        qb::io::tcp::socket sock;
        if (sock.connect(qb::io::uri{conn_opts_.schema + "://" + conn_opts_.uri}, t_out) != 0)
            return false;
        const int n = qb::io::socket::send_n(sock.native_handle(), pkt.data(), static_cast<int>(pkt.size()), t_out);
        sock.disconnect();
        return n == static_cast<int>(pkt.size());
    }

    /**
     * @brief Request cancellation of the running query WITHOUT blocking the event loop
     *        (Huly QB-113): the same out-of-band CancelRequest as `cancel()`, driven by the async
     *        connector the session itself connects through -- TLS included.
     *
     * The cancel connection is opened by `qb::io::async::tcp::connect` (a plain database) or by
     * the STARTTLS connector with `postgres_ssl_negotiator` (a secure database: SSLRequest, the
     * server's `'S'`, the TLS handshake, exactly like the session), the coroutine suspended
     * meanwhile so every other actor on the core keeps running; then the 16 bytes are written
     * non-blocking and the socket closed. A secure database therefore reaches a `hostssl`-only
     * server, which rejects `cancel()`'s plaintext request, and NEVER falls back to plaintext: a
     * server that declines SSL on the cancel connection fails the cancel, the rule the session's
     * own connect applies. The connect budget is `cancel()`'s (`connect_timeout` capped at 2 s).
     *
     * Same verdict as `cancel()`: `sqlstate::query_canceled` (57014) on the awaiting caller when
     * the server acts on it; the connection survives.
     *
     * @return `true` if the CancelRequest was delivered; `false` if no backend key is known yet
     *         (never connected), the cancel connection failed (unreachable, TLS declined or
     *         failed, budget exhausted), or the write failed. A `false` only means the request
     *         could not be sent -- not that the query survived.
     */
    [[nodiscard]] qb::io::async::task<bool>
    cancel_async() {
        if (serverPid_ == 0)
            co_return false; // never received BackendKeyData -> nothing to cancel

        const auto         pkt   = cancel_request_packet();
        const qb::duration t_out = cancel_connect_budget();
        using transport_sock     = std::remove_cvref_t<typename QB_IO_::transport_io_type>;

        // The connector hands the coroutine the ready socket -- closed when the connect, the
        // SSLRequest exchange or the TLS handshake failed -- through the generic callback bridge. The
        // step reads the database's options itself (URI; TLS context when secure); nothing of `this` after.
        auto sock = co_await qb::io::async::async_awaiter<transport_sock>([this, t_out](std::function<void(transport_sock)> complete) {
            const qb::io::uri uri{conn_opts_.schema + "://" + conn_opts_.uri};
            auto              deliver = [complete](transport_sock &&s) mutable {
                complete(std::move(s));
            };
            if constexpr (transport_sock::is_secure()) {
#ifdef QB_HAS_SSL
                auto tls = make_client_tls_context();
                if (!tls.ok()) {
                    complete(transport_sock{}); // the session's rule: fail CLOSED on a bad CA/cert/key path
                    return;
                }
                transport_sock s{std::move(tls)};
                qb::io::async::tcp::starttls_connect<transport_sock, postgres_ssl_negotiator>(std::move(s), uri, std::move(deliver), t_out);
#else
                complete(transport_sock{});
#endif
            } else {
                qb::io::async::tcp::connect<transport_sock>(uri, std::move(deliver), t_out);
            }
        });
        if (!sock.is_open())
            co_return false;

        // 16 bytes on a fresh socket go out at once; a would-block (or a TLS layer wanting I/O,
        // which `ssl::socket::write` reports as 0) parks the coroutine on the socket's readiness,
        // never the loop.
        std::size_t sent = 0;
        for (int spins = 0; sent < pkt.size() && spins < 64; ++spins) {
            const int n = sock.write(pkt.data() + sent, pkt.size() - sent);
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0) {
                const int err = qb::io::socket::get_last_errno();
#ifdef _WIN32
                if (err != QB_WINDOWS_WOULDBLOCK_ERROR && !qb::io::socket_no_error(err))
                    break;
#else
                if (!qb::io::socket_no_error(err))
                    break;
#endif
            }
            int events = EV_WRITE; // A plain would-block write needs output capacity, not input.
            if constexpr (transport_sock::is_secure()) {
                events = EV_READ | EV_WRITE; // Preserve custom transports without native TLS state.
#ifdef QB_HAS_SSL
                if constexpr (requires { SSL_want_read(sock.ssl_handle()); }) {
                    const auto *ssl = sock.ssl_handle();
                    // A TLS write can need peer input; watching the writable TCP fd then spins.
                    events = ssl && SSL_want_read(ssl) ? EV_READ : EV_WRITE;
                }
#endif
            }
            co_await qb::io::async::wait_for_io(sock.native_handle(), events);
        }
        sock.disconnect();
        co_return sent == pkt.size();
    }

private:
    /**
     * @brief Serial number handed to the next `query_stream()` cursor on this connection.
     * @details The cursor name used to be the fixed literal `qb_stream_cursor`, which silently made
     *          `query_stream` a once-at-a-time API: two streams interleaving on the same `Database`
     *          — or one started from inside another's `on_row` — issued a second
     *          `DECLARE qb_stream_cursor`, and PostgreSQL rejected it with an opaque
     *          "cursor already exists". Nothing documented that restriction. Naming each cursor
     *          after this counter removes it; the single-stream case is byte-identical apart from
     *          the suffix. Per-connection and only touched from the connection's own thread, so no
     *          synchronisation is needed (and none of the rest of `Database` has any).
     */
    std::uint64_t _stream_cursor_seq{0};

    /**
     * @brief Bookkeeping for the transaction `query_stream()` opens to hold its cursors.
     * @details A server-side cursor only lives inside a transaction, so a stream started on an idle
     *          connection issues its own `BEGIN` … `COMMIT`. `in_transaction()` cannot decide who
     *          owns that block: it mirrors the last ReadyForQuery, so a stream starting while
     *          another is still suspended on its `BEGIN` reads 'I' and opens a *second* one —
     *          PostgreSQL warns 25001 and keeps the single session transaction, and then the first
     *          COMMIT to run ends it under every other stream, killing their cursors.
     *
     *          These members are the synchronous mirror of that block, which is exact because
     *          commands are enqueued on this connection in `co_await` order:
     *          - `_stream_txn_users` counts the streams sharing the block; 0 means none is ours, so
     *            a live `in_transaction()` there really is a caller-opened transaction (guest mode:
     *            declare the cursor, close only the cursor).
     *          - `_stream_txn_closing` covers the window where the last stream's COMMIT/ROLLBACK is
     *            enqueued but not yet reflected in `_txn_status`; a stream starting there must open
     *            a fresh block rather than join the one being closed.
     *          - `_stream_txn_failed` makes that COMMIT a ROLLBACK when any sharing stream failed.
     *
     *          Per-connection and only touched from the connection's own thread, like
     *          `_stream_cursor_seq`, so no synchronisation is needed. Every registered stream gives
     *          its seat back as it unwinds — including a coroutine frame destroyed mid-`co_await`,
     *          via the scope guard in `query_stream()` — and a lost connection fails every pending
     *          query, which unwinds those frames too. Deliberately NOT reset in
     *          `prepare_reconnect()` (unlike `_txn_status`): a frame that has been failed but has
     *          not run its unwind yet still owes one decrement, and zeroing the count under it would
     *          wrap `_stream_txn_users` and wedge every later stream.
     */
    std::uint32_t _stream_txn_users{0};
    bool          _stream_txn_closing{false};
    bool          _stream_txn_failed{false};

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
            QB_LOG_WARN("[pgsql] Failed to enable TCP keepalive");
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

        QB_LOG_INFO("[pgsql] TCP keepalive enabled: idle=" << conn_opts_.keepalive_idle << "s, interval=" << conn_opts_.keepalive_interval
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
        _stats.bytes_received += static_cast<std::uint64_t>(msg->buffer_size());
        const auto it = routes_.find(msg->tag());
        if (qb::likely(it != routes_.end()))
            (this->*(it->second))(*msg);
        else
            on_unhandled_message(*msg);
    }

    /**
     * @brief The link is gone: what was in flight on it fails, and the connection decides what follows
     *        (Huly QB-111).
     * @details A handshaking link's loss only fails the handshake's queries: the attempt that installed it
     *          decides (another attempt, or down). A ready link's loss leaves `ready` first -- a failure
     *          callback that submits again is refused, never queued on a link that is gone -- then fails
     *          the work in flight and queued, once, with `database disconnected`; then `on_disconnected`
     *          fires with the cause, BEFORE the reconnection decision (a hook that disarms the policy
     *          prevents the run); then a run starts when the cause is not `user` and a policy is armed,
     *          else the connection is down.
     *
     * @param ev Disconnection event
     */
    void
    on(qb::io::async::event::disconnected const &ev) {
        const bool was_ready = _session_ready;
        const bool was_live  = _session_live;
        _session_ready       = false;
        _session_live        = false;
        _link_open           = false;
        _gate->live          = 0; // no session of this link may queue any more
        if (!_cause_set)
            _loss_cause = cause_of_(ev.reason);
        _cause_set       = false;
        const auto cause = _loss_cause;
        // The backend cancel key is per-connection; drop it so a post-disconnect
        // cancel() can't address a recycled PID on the server.
        serverPid_    = 0;
        serverSecret_ = 0;
        if (auto startup = _startup)
            startup->settle(startup_outcome::failed); // the attempt that installed the link decides
        if (was_ready && cause != qb::io::down_reason::user) {
            _state = _reconnect_policy ? qb::io::connection_state::reconnecting : qb::io::connection_state::down;
            if (!_reconnect_policy)
                _down_reason = cause;
        }
        if (was_live) {
            if (_completion_callback_depth != 0)
                _deferred_disconnect = DeferredDisconnect::transport;
            else
                on_error_query(error::client_error("database disconnected"));
        }
        // on_error_query() only fails the single in-flight query. Drain every
        // queued query too. A nested disconnect during a completion callback defers
        // this drain until the callback returns, preserving its Transaction owner.
        if (_completion_callback_depth != 0) {
            if (_deferred_disconnect == DeferredDisconnect::none)
                _deferred_disconnect = DeferredDisconnect::transport;
        } else {
            fail_all_pending_counted(error::client_error("database disconnected"));
            _current_command = root_transaction();
            _current_query   = nullptr;
            _replan          = replan_phase::none;
            _ready_for_query = false;
        }
        if (!was_ready)
            return;
        if (cause == qb::io::down_reason::user) {
            // disconnect() is already down: what remains is to say so, in order -- unless a connect() since
            // then started another lifecycle, which this down must not end.
            const bool current = _epoch == _user_down_epoch;
            notify_(_on_disconnected, "on_disconnected", cause);
            if (current && _epoch == _user_down_epoch)
                settle_down_(cause, _epoch);
            return;
        }
        // The state the connection is heading for -- set above, readable from the hook; the hook may still
        // change the policy, and a callback may have connected or disconnected meanwhile (the epoch says so).
        const auto epoch = _epoch;
        notify_(_on_disconnected, "on_disconnected", cause);
        if (epoch != _epoch)
            return; // the hook disconnected or connected: that call owns the state now
        if (!_reconnect_policy) {
            enter_down_(cause);
            return;
        }
        _state      = qb::io::connection_state::reconnecting;
        _recovering = true;
        auto policy = *_reconnect_policy;
        qb::io::async::coro_scheduler().spawn(run_(std::move(policy), _alive, _epoch));
    }

    /**
     * @brief Close the previous link's descriptor early and clear its per-backend state.
     *
     * Optional: every `connect()` installs its link through the same reset (fresh buffers and protocol,
     * an empty prepared-statement cache, no ParameterStatus, an idle transaction status, a re-armed
     * SCRAM gate), so a bare `connect()` after `disconnect()` needs none of it. Call it to release the
     * old descriptor before the next `connect()`. Does nothing while a connection, a handshake or a
     * reconnection run owns the link.
     *
     * @pre No pending queries on this connection (finish or drain the transaction queue first).
     */
    void
    prepare_reconnect() noexcept {
        if (_state != qb::io::connection_state::idle && _state != qb::io::connection_state::down)
            return;
        // Transport is a private base of `tcp::client`; use public `in`/`out`/`transport`.
        this->in().reset();
        this->out().reset();
        // `tcp::socket::disconnect()` is shutdown-only; the fd stays open. The next
        // `n_connect()` path requires a closed socket so `init()` opens a new fd.
        this->transport().close();
        this->reset_io_state();
        serverPid_    = 0;
        serverSecret_ = 0;
        server_params_.clear(); // server ParameterStatus is per-backend; drop the stale cache
        // P7: prepared statements are per-backend: a fresh backend does not know a cached name.
        storage_.clear();
        // P9: transaction status is per-session.
        _txn_status = 'I';
        // P1: the next handshake renegotiates SCRAM from scratch; clear the mutual-auth gate.
        _scram_pending         = false;
        _scram_server_verified = false;
        _error                 = error::db_error{"unknown error"};
        _current_command       = root_transaction();
        _current_query         = nullptr;
        _replan                = replan_phase::none;
        _query_sent            = false;
        _ready_for_query       = false;
    }

    /**
     * @brief Disconnect for good: terminal, `down` with `down_reason::user` (Huly QB-111)
     *
     * Whatever the reconnection policy, an explicit disconnect starts no run, and it ends the session:
     * the listened channels are forgotten, and a later `connect()` starts a fresh session -- no LISTEN
     * replayed, no gap marker. A connection, a handshake or a run in progress is abandoned.
     *
     * Fails every in-flight and queued query, then completes the io teardown before it returns.
     * From inside a query or fluent transaction callback, queued failures run after it returns:
     * draining sooner would destroy the Transaction that owns the executing callback.
     * Outside message dispatch, `on(disconnected)` has run and the watcher is stopped. It runs no loop pass, so no other
     * watcher and no coroutine is resumed under the caller -- safe from a coroutine body; the
     * coroutines awaiting a failed query resume at the caller's next pass (Huly QB-253). From
     * inside one of this client's own protocol handlers the teardown runs right after that
     * dispatch returns, in the same pass -- qb-io's `disconnect_now()`, as qbm-redis uses it.
     */
    void
    disconnect() {
        switch (_state) {
            case qb::io::connection_state::idle:
            case qb::io::connection_state::down:
                return;
            case qb::io::connection_state::ready:
                // Leave `ready` before teardown so a new query is refused immediately.
                _state           = qb::io::connection_state::down;
                _down_reason     = qb::io::down_reason::user;
                _user_down_epoch = ++_epoch;
                end_session_();
                // The teardown, in the call (drop_link_ -> disconnect_now()): on(disconnected) says so and
                // settles down. It used to be a deferred disconnect() followed by a nested listener pass,
                // which re-entered the coroutine scheduler from a coroutine body (Huly QB-253).
                drop_link_(qb::io::down_reason::user);
                return;
            default:
                ++_epoch; // the attempt or the run in progress steps aside at its next step
                end_session_();
                drop_link_(qb::io::down_reason::user);
                enter_down_(qb::io::down_reason::user);
                return;
        }
    }

private:
    /**
     * @brief An explicit disconnect ends the session: what was queued fails now, with the client's
     *        reason, and the session's state -- the listened set, the gap a next connection would follow
     *        -- is forgotten.
     * @details Keep the active callback's Transaction owner queued until it returns; its scope guard
     *          drains pending work at that point. Outside a callback, fail synchronously.
     */
    void
    end_session_() {
        _listened.clear();
        _ever_ready = false;
        if (!_session_live)
            return;
        _session_live = false;
        if (_completion_callback_depth != 0) {
            _deferred_disconnect = DeferredDisconnect::client;
        } else {
            on_error_query(error::connection_error("database disconnected by client"));
            fail_all_pending_counted(error::connection_error("database disconnected by client"));
            _current_command = root_transaction();
        }
    }
};

/**
 * @brief CRTP base for LISTEN/NOTIFY consumers (mirrors `RedisConsumer` + derived).
 *
 * `Derived` implements `deliver_pg_notify(::qb::pg::notification &&)` and inherits this class as
 * `notify_consumer<QB_IO_, Derived>` (see `notify_cb_consumer`, `notify_co_consumer`). The connection
 * calls three more hooks, defaulted here, that a consumer hides when it needs them (Huly QB-111):
 * `on_pg_notify_gap(gap_info)` when a session that follows an earlier ready one becomes ready, before any
 * notification of that session; `on_pg_notify_down()` when the connection reaches `down`;
 * `on_pg_notify_lifecycle_start()` when a `connect()` leaves `idle` or `down`.
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

    void
    on_pg_notify_gap(::qb::io::gap_info) {}
    void
    on_pg_notify_down() {}
    void
    on_pg_notify_lifecycle_start() {}
};

/**
 * @brief LISTEN/NOTIFY consumer that delivers to callbacks only (Huly QB-1092; see `redis::tcp::cb_consumer`).
 *
 * Every NOTIFY goes to `on_notify`, on the connection's thread, as it is read; nothing is queued, so
 * nothing is ever dropped -- until 3.3 this name was an alias of `notify_co_consumer`, whose queue a
 * callback-only program never read: it filled, and every later NOTIFY was counted and logged as dropped.
 * A hole in the stream is reported to `on_gap` (Huly QB-111): after a reconnection the server's
 * NOTIFYs of the gap are lost, so `on_gap({qb::io::gap_reason::reconnect, 0})` is called when the new
 * session is ready, before its first `on_notify` and before `on_connected` -- the moment to resynchronise
 * from the source of truth, on this very connection if need be: it accepts queries already. An exception
 * escaping either callback is contained and logged. Normal queries run on the same connection.
 */
template <typename QB_IO_>
class notify_cb_consumer : public notify_consumer<QB_IO_, notify_cb_consumer<QB_IO_>> {
    using base_type = notify_consumer<QB_IO_, notify_cb_consumer<QB_IO_>>;

    std::function<void(::qb::pg::notification &&)> on_notify_{};
    std::function<void(::qb::io::gap_info)>        on_gap_{};

    template <typename Fn, typename Arg>
    static void
    call_(Fn &fn, Arg &&arg, char const *what) noexcept {
        try {
            fn(std::forward<Arg>(arg));
        } catch (std::exception const &ex) {
            QB_LOG_WARN("[pgsql] notify_cb_consumer " << what << " callback error: " << ex.what());
        } catch (...) {
            QB_LOG_WARN("[pgsql] notify_cb_consumer " << what << " callback threw a non-std exception");
        }
    }

public:
    notify_cb_consumer()
        : base_type() {}

    explicit notify_cb_consumer(std::string const &opts)
        : base_type(opts) {}

    /** @brief The callback each NOTIFY is delivered to (replaces any previous one). */
    notify_cb_consumer<QB_IO_> &
    on_notify(std::function<void(::qb::pg::notification &&)> cb) {
        on_notify_ = std::move(cb);
        return *this;
    }

    /** @brief The callback told of each hole in the stream, before the next `on_notify` (Huly QB-111). */
    notify_cb_consumer<QB_IO_> &
    on_gap(std::function<void(::qb::io::gap_info)> cb) {
        on_gap_ = std::move(cb);
        return *this;
    }

    void
    deliver_pg_notify(::qb::pg::notification &&n) {
        if (on_notify_) {
            call_(on_notify_, std::move(n), "on_notify");
            return;
        }
        QB_LOG_INFO("[pgsql] NOTIFY pid=" << n.server_backend_pid << " channel=" << n.channel << " payload=" << n.payload);
    }

    void
    on_pg_notify_gap(::qb::io::gap_info info) {
        this->count_gap();
        if (on_gap_) {
            call_(on_gap_, info, "on_gap");
            return;
        }
        QB_LOG_INFO("[pgsql] notify_cb_consumer: notifications were lost here (" << ::qb::io::gap_reason_name(info.reason)
                                                                                 << "); set on_gap to resynchronise");
    }
};

/**
 * @brief LISTEN/NOTIFY consumer with `co_await receive()` (see `redis::tcp::co_consumer`): a bounded
 *        stream of notifications and gap markers, plus an optional `on_notify` callback.
 *
 * The stream holds up to `notify_channel_capacity()` elements (8192 by default). It ends only when the
 * connection reaches `down` (Huly QB-111): `receive()` then yields what it holds and `std::nullopt` --
 * `down_reason()` says why. A loss the reconnection run recovers does not end it: the run replays the
 * listened channels and the stream says where it has a hole, with a gap marker
 * (`notification::kind == notification_kind::gap`) placed before the first notification of the new
 * session (`gap.reason == qb::io::gap_reason::reconnect`). A full stream is a hole too: the NOTIFY
 * that does not fit is counted in `stats().notifications_dropped`, handed to `on_notify_dropped` (or
 * logged), and a marker with `gap.reason == overflow` and `gap.lost` -- every drop up to the next
 * delivered notification, coalesced -- precedes the next notification the stream takes, or ends what the
 * reader drained. Markers are always on. A later `connect()` after `down` serves a new stream, starting
 * with what the old one held and was not read.
 */
template <typename QB_IO_>
class notify_co_consumer : public notify_consumer<QB_IO_, notify_co_consumer<QB_IO_>> {
    using base_type           = notify_consumer<QB_IO_, notify_co_consumer<QB_IO_>>;
    using notify_channel_type = qb::io::async::channel<::qb::pg::notification>;

    static constexpr std::size_t default_notify_channel_capacity = 8192;
    /// Room beyond the capacity: a marker queued with the notification after it, and the last marker of a
    /// stream that ends full -- so a marker never needs a place a notification took.
    static constexpr std::size_t marker_room = 2;

    std::size_t                          notify_capacity_ = default_notify_channel_capacity;
    std::unique_ptr<notify_channel_type> notify_channel_ = std::make_unique<notify_channel_type>(default_notify_channel_capacity + marker_room);
    std::function<void(::qb::pg::notification &&)> on_notify_dropped_{};
    std::function<void(::qb::pg::notification &&)> on_notify_callback_{};
    std::optional<::qb::io::gap_info>              pending_gap_{};    ///< losses no marker in the stream carries yet
    std::deque<::qb::io::gap_info>                 marker_infos_{};   ///< what each marker in the stream says, oldest first
    bool                                           marker_is_tail_{}; ///< the last element queued is a marker not yet read

    /// Merge a loss into `into`: a reconnection among them makes the reason; drops add up.
    static void
    merge_(::qb::io::gap_info &into, ::qb::io::gap_info const &loss) noexcept {
        if (loss.reason == ::qb::io::gap_reason::reconnect)
            into.reason = ::qb::io::gap_reason::reconnect;
        into.lost += loss.lost;
    }

    /// A loss at the current end of the stream: into the marker that already ends it, else pending.
    void
    note_loss_(::qb::io::gap_info const &loss) {
        if (marker_is_tail_) {
            merge_(marker_infos_.back(), loss);
            return;
        }
        if (pending_gap_)
            merge_(*pending_gap_, loss);
        else
            pending_gap_ = loss;
    }

    /// Queue the pending loss as a marker; the caller made sure there is room for it.
    void
    queue_marker_() {
        ::qb::pg::notification marker;
        marker.kind = ::qb::pg::notification_kind::gap;
        if (!notify_channel_->try_send(std::move(marker)))
            return; // closed: the stream already ended
        marker_infos_.push_back(*pending_gap_);
        pending_gap_.reset();
        marker_is_tail_ = true;
        this->count_gap();
    }

public:
    notify_co_consumer()
        : base_type() {}

    explicit notify_co_consumer(std::string const &opts, std::size_t notify_capacity = default_notify_channel_capacity)
        : base_type(opts)
        , notify_capacity_(notify_capacity)
        , notify_channel_(std::make_unique<notify_channel_type>(notify_capacity + marker_room)) {}

    /**
     * @brief Optional callback invoked for each NOTIFY before it is queued for `receive()` (markers are
     *        not passed to it).
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
                QB_LOG_WARN("[pgsql] notify_co_consumer on_notify callback error: " << ex.what());
            } catch (...) {
                QB_LOG_WARN("[pgsql] notify_co_consumer on_notify callback threw a non-std exception");
            }
        }
        if (notify_channel_->size() < notify_capacity_) {
            if (pending_gap_)
                queue_marker_(); // the hole goes before the first notification after it
            if (notify_channel_->try_send(std::move(n))) {
                marker_is_tail_ = false;
                return;
            }
        }
        this->count_notification_dropped();
        note_loss_(::qb::io::gap_info{::qb::io::gap_reason::overflow, 1});
        if (on_notify_dropped_) {
            try {
                on_notify_dropped_(std::move(n));
            } catch (std::exception const &ex) {
                QB_LOG_WARN("[pgsql] notify_co_consumer on_notify_dropped error: " << ex.what());
            } catch (...) {
                QB_LOG_WARN("[pgsql] notify_co_consumer on_notify_dropped threw a non-std exception");
            }
        } else {
            QB_LOG_WARN("[pgsql] notify_co_consumer: notification dropped (buffer full)");
        }
    }

    /// A reconnected session is ready (Huly QB-111): the hole's marker is queued now, unless the stream is full,
    /// ahead of the notifications that session received while it was handshaking.
    void
    on_pg_notify_gap(::qb::io::gap_info info) {
        note_loss_(info);
        if (pending_gap_ && notify_channel_->size() < notify_capacity_ + 1)
            queue_marker_();
    }

    /// The connection is down: the stream ends -- its last hole said first -- and drains, then nullopt.
    void
    on_pg_notify_down() {
        if (pending_gap_)
            queue_marker_(); // marker_room keeps a place for it
        notify_channel_->close();
    }

    /**
     * @brief A connect() leaves `down`: a new stream, carrying over, in order, what the old one held and
     *        was not read (Huly QB-252: a closed channel never reopens).
     */
    void
    on_pg_notify_lifecycle_start() {
        if (!notify_channel_->is_closed())
            return;
        auto fresh = std::make_unique<notify_channel_type>(notify_capacity_ + marker_room);
        while (auto unread = notify_channel_->try_recv())
            (void) fresh->try_send(std::move(*unread)); // never full: same capacity, at most as many
        notify_channel_ = std::move(fresh);
    }

    notify_co_consumer<QB_IO_> &
    on_notify_dropped(std::function<void(::qb::pg::notification &&)> cb) {
        on_notify_dropped_ = std::move(cb);
        return *this;
    }

    /** @brief How many elements the stream holds before a NOTIFY is dropped (markers have their own room). */
    [[nodiscard]] std::size_t
    notify_channel_capacity() const noexcept {
        return notify_capacity_;
    }

    /**
     * @brief Await the next element of the stream: a NOTIFY or a gap marker (`kind`); `std::nullopt`
     *        once the connection is down and what the stream held is read (Huly QB-111).
     *
     * A hole the reader caught up with -- drops after the last notification queued -- is queued as a
     * marker here, so a reader that drained the stream still learns of it.
     *
     * Implemented as a direct coroutine member (NOT an immediately-invoked lambda
     * `[this]{...}()`): the lambda closure would be a temporary destroyed at the end
     * of this call, leaving the coroutine frame referencing freed memory for `this`
     * (dangling-closure UAF — ASan-blind stack corruption; crashes under some
     * compilers' frame layouts). The consumer object owns the coroutine's `this`.
     */
    [[nodiscard]] qb::io::async::task<std::optional<::qb::pg::notification>>
    receive() {
        if (pending_gap_ && notify_channel_->size() == 0 && !notify_channel_->is_closed())
            queue_marker_();
        auto next = co_await notify_channel_->recv();
        if (next && next->kind == ::qb::pg::notification_kind::gap && !marker_infos_.empty()) {
            next->gap = marker_infos_.front();
            marker_infos_.pop_front();
            if (marker_infos_.empty())
                marker_is_tail_ = false;
        }
        co_return next;
    }

    ~notify_co_consumer() {
        notify_channel_->close();
    }
};

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
 * @brief The handshake hook's write handle (`database::set_handshake`, Huly QB-111).
 */
using handshake_session = detail::handshake_session;

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
     * @brief LISTEN/NOTIFY consumer with callback-only delivery: nothing queued, nothing dropped (see
     *        `redis::tcp::cb_consumer`).
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

#include "./with_transaction.h"

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
