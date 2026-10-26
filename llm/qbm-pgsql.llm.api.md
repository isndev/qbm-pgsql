<!-- Verified-against: qbm-pgsql @ qb 3.3.0. Source of truth: the headers under qbm/pgsql/src/qbm/pgsql/. -->
# `qb::pg` (PostgreSQL) Module API Reference

This document provides a detailed API reference for the `qb::pg` module, part of the QB C++ Actor Framework. It is intended for use by other AI models to understand the capabilities and usage of this PostgreSQL client library.

## Table of Contents

- [Overview](#overview)
- [Core Classes](#core-classes)
  - [Database Client Classes](#database-client-classes)
    - [`qb::pg::tcp::database`](#qbpgtcpdatabase)
    - [`qb::pg::tcp::ssl::database`](#qbpgtcpssldatabase)
    - [LISTEN/NOTIFY consumers (`notify_cb_consumer`, `notify_co_consumer`)](#listennotify-consumers-notify_cb_consumer-notify_co_consumer)
  - [Transaction Management](#transaction-management)
    - [`qb::pg::transaction` (alias for `qb::pg::detail::Transaction`)](#qbpgtransaction-alias-for-qbpgdetailtransaction)
- [Connection Handling](#connection-handling)
  - [`qb::pg::connection_options`](#qbpgconnection_options)
  - [`qb::pg::dbalias`](#qbpgdbalias)
- [Query Execution](#query-execution)
  - [`qb::pg::params` (alias for `qb::pg::detail::QueryParams`)](#qbpgparams-alias-for-qbpgdetailqueryparams)
  - [Prepared Statements (`qb::pg::detail::PreparedQuery`, `qb::pg::detail::PreparedQueryStorage`)](#prepared-statements-qbpgdetailpreparedquery-qbpgdetailpreparedquerystorage)
- [Result Set Processing](#result-set-processing)
  - [`qb::pg::results` (alias for `qb::pg::resultset`)](#qbpgresults-alias-for-qbpgresultset)
  - [`qb::pg::results::row`](#qbpgresultsrow)
  - [`qb::pg::results::field`](#qbpgresultsfield)
  - [`qb::pg::field_description`](#qbpgfield_description)
- [Data Type Handling](#data-type-handling)
  - [PostgreSQL OIDs (`qb::pg::oid`)](#postgresql-oids-qbpgoid)
  - [C++ Type Mapping (`qb::pg::detail::type_mapping`)](#c-type-mapping-qbpgdetailtype_mapping)
  - [Type Conversion (`qb::pg::detail::TypeConverter`)](#type-conversion-qbpgdetailtypeconverter)
- [Error Handling](#error-handling)
  - [`qb::pg::error::db_error`](#qbpgerrordb_error)
  - [`qb::pg::error::connection_error`](#qbpgerrorconnection_error)
  - [`qb::pg::error::query_error`](#qbpgerrorquery_error)
  - [Other Error Types](#other-error-types)
  - [SQLSTATE Codes (`qb::pg::sqlstate::code`)](#sqlstate-codes-qbpgsqlstatecode)
- [Supporting Types and Enums](#supporting-types-and-enums)
  - [Enums](#enums)
  - [Structs and Type Aliases](#structs-and-type-aliases)
  - [Observability (`qb::pg::stats`, `qb::pg::query_info`, `qb::pg::query_kind`)](#observability-qbpgstats-qbpgquery_info-qbpgquery_kind)
- [Protocol Details](#protocol-details)
  - [`qb::protocol::pgsql<IO_>`](#qbprotocolpgsqlio_)
  - [Message Tags (`qb::pg::detail::message_tag`)](#message-tags-qbpgdetailmessage_tag)

## Overview

The `qb::pg` module provides a high-performance, asynchronous C++20/23 client for PostgreSQL databases, designed for integration with the QB Actor Framework. It utilizes `qb-io` for its asynchronous I/O operations. It is a compiled library (**STATIC** by default, **SHARED** when `QB_BUILD_SHARED_LIBS`/`BUILD_SHARED_LIBS` is set; alias `qbm::pgsql`); the umbrella header is `<qbm/pgsql/pgsql.h>`.

Key features include:
- Two interchangeable completion styles with the **same method names**: a coroutine API (`co_await`-only, yielding `Reply<T>`) and a fluent, callback-based transaction API. The single-op coroutine entry points (`execute`, `query(sql)`, `prepare`, `begin`/`commit`/`rollback`, savepoints, `notify`/`listen`) return `pg_reply_awaiter<T>`. Inline `query(sql, args...)` returns `qb::io::async::task<Reply<T>>` and admits Parse/Describe plus Bind/Execute as one logical queued command; `copy_out`, `copy_in`, and `query_stream` also return `qb::io::async::task<Reply<T>>`. All are `co_await`-only and yield `Reply<T>`.
- TCP and SSL/TLS encrypted connections.
- Comprehensive transaction management including savepoints.
- Support for simple and prepared statements with type-safe parameter binding.
- Efficient processing of query results.
- Detailed error handling and mapping of PostgreSQL error codes.

## Core Classes

### Database Client Classes

These are the primary classes used for establishing and managing connections to a PostgreSQL server, as well as executing queries and transactions.

#### `qb::pg::tcp::database`

Alias for `qb::pg::detail::Database<qb::io::transport::tcp>`.
This is the standard TCP-based PostgreSQL client.

**Definition (Simplified from `pgsql.h`):**
```cpp
namespace qb::pg::detail {

template <typename QB_IO_, typename NotifyDerived = void>
class Database
    : public qb::io::async::tcp::client<Database<QB_IO_, NotifyDerived>, QB_IO_, void>,
      public Transaction { // Transaction provides the fluent API methods
public:
    Database();
    explicit Database(std::string const &conn_opts_str);

    // connect() returns a [[nodiscard]] awaiter — drive it with `co_await` or
    // `qb::io::async::run_sync(...)`. There is NO blocking `bool connect()` and NO
    // `connect(on_ok, on_err)` callback overload. The awaiter resolves to `bool`:
    // true once the connection is READY -- authentication, the first ReadyForQuery,
    // the set_handshake() hook and the LISTEN replay ran (Huly QB-111).
    [[nodiscard]] connect_awaiter connect();
    [[nodiscard]] connect_awaiter connect(qb::duration timeout);
    [[nodiscard]] connect_awaiter connect(std::string const &conn_opts_str);
    [[nodiscard]] connect_awaiter connect(connection_options opts); // carries TLS/keepalive fields the DSN can't
    [[nodiscard]] connect_awaiter connect(std::string const &conn_opts_str, typename QB_IO_::transport_io_type &&raw_io);
    void disconnect();
    void prepare_reconnect() noexcept; // optional: closes the old fd early (connect() resets the session itself)

    // The reconnect contract (Huly QB-111; qb/io/reconnect.h):
    using handshake_hook = std::function<qb::io::async::task<bool>(qb::pg::handshake_session &)>;
    qb::io::async::task<bool> connect_with_retry(RetryPolicy policy = RetryPolicy{});
    qb::io::async::task<bool> connect_with_retry(std::string const &conn_opts_str, RetryPolicy policy = RetryPolicy{});
    Database &enable_auto_reconnect(RetryPolicy policy = RetryPolicy{}) noexcept;
    Database &disable_auto_reconnect() noexcept;
    [[nodiscard]] bool is_reconnecting() const noexcept;
    [[nodiscard]] bool is_connected() const noexcept;                   // state() == ready
    [[nodiscard]] qb::io::connection_state state() const noexcept;
    [[nodiscard]] qb::io::down_reason down_reason() const noexcept;     // meaningful in `down`
    Database &set_handshake(handshake_hook hook);                      // empty function: none
    Database &on_connected(std::function<void()> hook);
    Database &on_disconnected(std::function<void(qb::io::down_reason)> hook);
    Database &on_down(std::function<void(qb::io::down_reason)> hook);
    [[nodiscard]] std::set<std::string, std::less<>> const &listened_channels() const noexcept;

    // Streaming / bulk-load helpers (coroutine; return qb::io::async::task<Reply<T>>):
    [[nodiscard]] qb::io::async::task<Reply<resultset>>
        copy_out(std::string sql, std::function<void(std::string_view)> sink);
    [[nodiscard]] qb::io::async::task<Reply<resultset>>
        copy_in(std::string sql, std::function<std::optional<std::string>()> source);
    [[nodiscard]] qb::io::async::task<Reply<resultset>>
        copy_in(std::string sql, std::string data);
    template <typename RowFn>
    [[nodiscard]] qb::io::async::task<Reply<void>>
        query_stream(std::string sql, std::size_t batch_size, RowFn on_row);

    // Connection introspection / control:
    bool cancel();                                      // out-of-band PostgreSQL CancelRequest (SYNCHRONOUS, ≤2s, plaintext); NOT [[nodiscard]] (pgsql.h:3583-3584)
    [[nodiscard]] qb::io::async::task<bool> cancel_async(); // the same request, non-blocking, TLS on a secure database (pgsql.h:3628-3629)
    [[nodiscard]] bool in_transaction() const noexcept; // backend session in a transaction block ('T'/'E')
    [[nodiscard]] bool used_channel_binding() const noexcept; // SCRAM-SHA-256-PLUS tls-server-end-point binding negotiated
    [[nodiscard]] std::optional<std::string_view> parameter_status(std::string_view key) const; // PQparameterStatus
    [[nodiscard]] int server_version() const;           // PQserverVersion (e.g. 160002)
    [[nodiscard]] int backend_pid() const noexcept;     // PQbackendPID
    Database &prepared_cache_capacity(std::size_t capacity);      // bound of the statement LRU (default 100; 0 = default)
    [[nodiscard]] std::size_t prepared_cache_capacity() const noexcept;
    [[nodiscard]] qb::pg::stats stats() const noexcept;          // counts since construction (Huly QB-115)
    Database &on_query_complete(std::function<void(qb::pg::query_info const &)> hook); // empty = none
    Database &on_notice(std::function<void(qb::pg::notice const &)> fn);                 // empty = log at INFO

    // Transaction methods (fluent API - from qb::pg::detail::Transaction):
    // template <typename CB_SUCCESS, typename CB_ERROR>
    // Transaction& begin(CB_SUCCESS &&on_success, CB_ERROR &&on_error, transaction_mode mode = {});
    // template <typename CB_SUCCESS>
    // Transaction& begin(CB_SUCCESS &&on_success, transaction_mode mode = {});

    // Other transaction methods like execute, prepare, savepoint, then, error, success, await
    // are defined in qb::pg::detail::Transaction and inherited.
    // See [Transaction Management](#transaction-management) and relevant headers.

    // Message handlers (invoked by protocol):
    // void on_authentication(message &msg);
    // void on_command_complete(message &msg);
    // ... and other on_... handlers for backend messages
};

} // namespace qb::pg::detail

namespace qb::pg {
struct tcp {
    using database = detail::Database<qb::io::transport::tcp, void>;
    // ...
};
} // namespace qb::pg
```

**Key Public Methods (from `detail::Database` and `detail::Transaction`):**
- `Database()`: Default constructor.
- `explicit Database(std::string const &conn_opts_str)`: Constructor with a connection string.
- `connect_awaiter connect()`: Starts the async handshake using previously set options. `co_await` or `run_sync` it; resolves to `bool`.
- `connect_awaiter connect(std::string const &conn_opts_str)`: Re-parses the DSN, then connects. DSN form `tcp://[user[:pass]@]host[:port][database]` — the database name is in **square brackets**.
- `connect_awaiter connect(connection_options opts)`: Connects with a fully-specified options struct. Use this overload (not the DSN string) to set fields the DSN cannot carry — `ssl_verify` (TLS verification level), `ssl_root_cert`/`ssl_cert`/`ssl_key` (optional private-CA + client-cert mTLS PEM paths for `ssl://`), `connect_timeout`, and keepalive. Example: `auto o = connection_options::parse(dsn); o.ssl_verify = ssl_verify_mode::full; o.ssl_root_cert = "ca.pem"; co_await db.connect(o);`.
- `connect_awaiter connect(std::string const &conn_opts_str, transport_io_type &&raw_io)`: Adopts an already-connected transport (one from a pool, one a supervisor handed over) for the FIRST connection of the lifecycle and runs the whole handshake on it (authentication, the `set_handshake()` hook, the LISTEN replay); the DSN still names the user and the database, and the address a reconnection run or a later `connect()` dials -- only the first connection adopts. The transport must be connected; for `tcp::ssl::database` the caller must ALSO have run the PostgreSQL TLS negotiation on it (the SSLRequest packet, the server's `'S'`, the TLS handshake), since the client writes the StartupMessage at once. A transport that is not open fails the attempt (`false`, `down`, `down_reason::peer`). While a connection, a handshake or a run is in progress, or on a ready connection, it resolves `false` and closes the moved-in transport unused: the connection already has its link and the transport was not adopted (a plain `connect()` on a ready connection resolves `true`: it asks for no particular transport). Until 3.3 the overload stored the transport and dialled the DSN itself, so the transport was never used. (Huly QB-1096)
- `connect_awaiter connect(qb::duration timeout)`: Connects, overriding the attempt's deadline (the transport connect and, from the StartupMessage on, the whole handshake).
- Every `connect()` form resolves `true` once the connection is `ready` and `false` when the attempt failed, was refused or superseded (the connection is then `down`: `down_reason()`, `error()`); `false` at once while a connection, a handshake or a run is in progress, `true` at once on a ready connection. The string and `connection_options` forms change the stored options only while the connection is idle or down. (Huly QB-111)
- `task<bool> connect_with_retry(RetryPolicy policy = {})` (and a DSN-first overload): the policy's attempts, each a transport connect and a whole handshake; between attempts the policy's `next_delay()` (jittered) after `on_retry(attempt, next_delay)`. A refusal ends it at once (`handshake_refused`), a spent policy `down(retries_exhausted)`. `qb::pg::RetryPolicy` is `qb::io::reconnect_policy` (`max_attempts` -1 = unlimited, `initial_delay`, `max_delay`, `multiplier`, `jitter`, `connect_timeout` -- zero: the connection's own `connect_timeout`, else 10 s -- `on_retry`). (Huly QB-111)
- `Database &enable_auto_reconnect(RetryPolicy policy = {})` / `disable_auto_reconnect()` / `bool is_reconnecting() const`: arm (disarm) a reconnection run for a ready connection the server or the network takes away -- never for `disconnect()`. Read when the loss happens; `is_reconnecting()` is true from the loss until the run is ready or down. (Huly QB-111)
- `qb::io::connection_state state() const` / `qb::io::down_reason down_reason() const` / `bool is_connected() const`: where the connection stands (`idle`, `connecting`, `handshaking`, `ready`, `reconnecting`, `down`) and why it is down (`user`, `peer`, `protocol`, `retries_exhausted`, `handshake_refused`); `is_connected()` is `state() == ready` -- until 3.3 it was the io base's flag, true on a database that never connected. (Huly QB-111)
- `Database &set_handshake(handshake_hook hook)`: a `task<bool>(qb::pg::handshake_session &)` run on EVERY connection, the first one included, after authentication and the first ReadyForQuery and before the connection is ready. `false` or a throw refuses the connection for good (`handshake_refused`). The session (`execute(sql)`, `query(sql)`, `prepare(name, sql, types)`, `execute(name, params)`, `live()`) is the only way to queue while the connection is not ready -- the connection itself refuses work outside `ready` -- and only while its own handshake runs: a kept copy fails with `"handshake over"`. (Huly QB-111)
- `on_connected(fn)` (each time the connection becomes ready), `on_disconnected(fn(down_reason))` (once per ready connection lost, cause `user` / `peer` / `protocol`, after the in-flight work failed and before the reconnection decision -- a hook that disarms the policy prevents the run; it alternates strictly with `on_connected`), `on_down(fn(down_reason))` (each time the connection reaches `down`). Empty until set; an exception escaping one is contained and logged. (Huly QB-111)
- `listened_channels()`: the channels `listen()` / `unlisten()` / `unlisten_all()` left the connection listening on, maintained when the server confirms them and replayed at the start of every new session; cleared by `disconnect()`. A `LISTEN` written as SQL is not in it. (Huly QB-111)
- `void disconnect()`: Marks the connection down and refuses new queries immediately; callback commands receive one connection error and coroutine calls return failed replies. Outside a completion callback, it fails in-flight/queued work and completes io teardown before returning, without a loop pass (safe from a coroutine body). Teardown shuts the sending side down, so the server reads EOF and the backend leaves `pg_stat_activity` while the object lives (qb-io, Huly QB-1060); the descriptor is released at destruction or the next `connect()`. When called inside a query or fluent `.then` / `.error` callback, queued failures run after that callback returns so its transaction owner stays alive; io teardown follows message dispatch. A later ordinary `connect()` on the same object opens a fresh socket and handshake and starts the session with an empty prepared-statement cache, an empty ParameterStatus cache and an idle transaction status (Huly QB-1095); `prepare_reconnect()` is optional, to close the old fd early.
- **Streaming / bulk-load (coroutine, `co_await`-only; each returns `qb::io::async::task<Reply<T>>`):**
    - `copy_out(std::string sql, std::function<void(std::string_view)> sink) -> task<Reply<resultset>>`: Runs a `COPY … TO STDOUT` and delivers each `CopyData` chunk to `sink` as it arrives. Streams in **constant memory** (rows are never buffered in a result set). The `string_view` is valid only during the call. Resolves to `ok()` on success.
    - `copy_in(std::string sql, std::function<std::optional<std::string>()> source) -> task<Reply<resultset>>`: Runs a `COPY … FROM STDIN`, calling `source` repeatedly and sending each returned chunk as `CopyData` until it returns `std::nullopt` (then `CopyDone`). A throwing `source` aborts the COPY with `CopyFail`. Resolves to `ok()` (the `COPY n` count) on success; the connection stays usable on error. **NOT constant-memory** — it drains the whole source into the output pipe synchronously (capped by the write-buffer ceiling); only `copy_out` and `query_stream` stream in constant memory.
    - `copy_in(std::string sql, std::string data) -> task<Reply<resultset>>`: Convenience overload that sends the entire `data` payload in one shot.
    - The connection admits one COPY at a time, in either direction. A second COPY returns a failed `Reply` without sending SQL or invoking its source/sink. Destroying the awaiting coroutine detaches its callback; its command keeps the reservation until completion, so a later COPY cannot answer the old server response. A source that destroys its own waiter causes its returned chunk to be discarded and `CopyFail` to be sent without another source call.
    - A COPY OUT sink and its owned captures stay alive until its invocation returns, including self-cancellation or disconnect; detachment prevents later calls. Ordinary chunk delivery borrows the operation and callable without copying either shared owner. Cancellation or completion during the call retains the needed ownership in the operation's outermost sink invocation, shared by nested calls until return. Borrowed capture data needs its own lifetime. Command admission and query association remain unchanged (`src/qbm/pgsql/pgsql.h:1825-1864,1872-1889,2601-2617,3355-3364`).
    - `query_stream(std::string sql, std::size_t batch_size, RowFn on_row) -> task<Reply<void>>`: Streams a large result via a server-side `CURSOR` (`DECLARE`/`FETCH`, `batch_size` rows per round trip), invoking `on_row` per row in **constant memory**. Auto-manages a transaction when idle (`BEGIN`/`COMMIT`, `ROLLBACK` on failure); when `in_transaction()` it declares the cursor in the caller's transaction and touches only the cursor. Cursor names are unique per connection (`qb_stream_cursor_<n>`), so streams **may overlap** on one `database` — and because a session has a single transaction, overlapping streams **share** the self-opened block: the first opens it, later ones only join, the last one out `COMMIT`s (or `ROLLBACK`s if any participant failed), so a server error in one stream aborts the block for the others. A caller-opened transaction is recognised as caller-owned only once its `BEGIN` has **completed** (`in_transaction()` mirrors the last `ReadyForQuery`), so do not start a stream while your own `begin()` is still in flight. `on_row` exceptions are rethrown after the cursor is closed and any self-opened transaction rolled back. `batch_size` is clamped to ≥ 1.
- **Connection introspection / control:**
    - `bool cancel()`: Sends an out-of-band PostgreSQL `CancelRequest` on a short-lived **separate** connection (libpq `PQcancel` style). It is **synchronous / blocking** (capped at ≤ 2s) and plaintext even when the main link is SSL. Surfaces on the in-flight query as `sqlstate::query_canceled` (57014). Note: it briefly blocks the calling thread — prefer `cancel_async()`.
    - `[[nodiscard]] task<bool> cancel_async()`: The same `CancelRequest`, driven by the async connector the session connects through (`qb::io::async::tcp::connect`, or the STARTTLS connector + `postgres_ssl_negotiator` for a secure database: SSLRequest → `'S'` → TLS, never a plaintext fallback), the coroutine suspended meanwhile — nothing blocks the loop. Same connect budget as `cancel()` (`connect_timeout` capped at 2 s), same verdict, same `false` meaning "not delivered" (never connected, unreachable, TLS declined/failed, budget exhausted). `co_await db.cancel_async()` from a timer coroutine, an actor, or a second connection's trigger. (Huly QB-113)
    - `bool in_transaction() const noexcept`: True when the backend session is in a transaction block (`'T'`) or a failed transaction block (`'E'`); false when idle (`'I'`).
    - Packet-write readiness in `cancel_async()`: native TLS `WANT_READ` waits for read only;
      other native TLS/plain writes wait for write. Custom secure transports without native
      TLS state retain the combined read/write fallback (`pgsql.h:3684-3695`).
    - `bool used_channel_binding() const noexcept`: True when **SCRAM-SHA-256-PLUS** with `tls-server-end-point` channel binding was negotiated (only over TLS, when the server offers the `-PLUS` mechanism).
    - `std::optional<std::string_view> parameter_status(std::string_view key) const`: Value of a server `ParameterStatus` report (libpq `PQparameterStatus`); e.g. `"server_version"`, `"server_encoding"`, `"TimeZone"`. `std::nullopt` if the key was never reported.
    - `int server_version() const`: Server version as a libpq-style integer (`PQserverVersion`), e.g. `160002` for 16.2. Returns 0 if the `server_version` parameter is unknown.
    - `int backend_pid() const noexcept`: Server-side backend PID captured at connect (`BackendKeyData`); libpq `PQbackendPID`. Returns 0 if not connected.
    - `qb::pg::stats stats() const noexcept`: A snapshot, by value, of what the connection has done — monotonic `std::uint64_t` counts since construction (a reconnect resets nothing) and the last query's duration. See [Observability](#observability-qbpgstats-qbpgquery_info-qbpgquery_kind). (Huly QB-115)
    - `Database &on_query_complete(std::function<void(qb::pg::query_info const &)> hook)`: One call per completed query, on the connection's thread, before the query's own callback or awaiter: success, server error, local refusal, or a disconnect's drain. An empty function (the default) removes it; without one a completion pays one predictable branch. Must not drive the connection; an exception escaping it is contained and logged. (Huly QB-115)
    - `Database &on_notice(std::function<void(qb::pg::notice const &)> fn)`: Every NoticeResponse (`RAISE NOTICE` / `WARNING`, server notices) with all its fields, on the connection's thread as the message is read; must not drive the connection; an exception escaping it is contained and logged. Without a handler (the default, or an empty function) a notice is logged at INFO. Replaces any previous handler. (Huly QB-108)
    - `Database &prepared_cache_capacity(std::size_t capacity)` / `std::size_t prepared_cache_capacity() const noexcept`: The bound of the connection's prepared-statement LRU — 100 by default, `0` restores it. A statement the LRU evicts is closed on the server as well: a protocol `Close` message for it precedes the evicting Parse in the same message, or, after a shrink (which evicts at once), the next Parse or Bind. The backend therefore holds no more named statements than the cache; executing an evicted name fails client-side and the name can be prepared again. The inline `query(sql, args...)` path's unnamed statement takes one slot and is never closed. (Huly QB-207)
- **Transaction API:**
    - `begin(...)`: Starts a new transaction. Takes success/error callbacks and optional `transaction_mode`.
    - `execute(...)`: Executes a simple SQL query or a prepared statement. Callbacks handle results or errors.
    - `prepare(...)`: Prepares a named SQL statement. Callbacks for success/failure.
    - `execute_file(...)`: Executes SQL commands from a file.
    - `prepare_file(...)`: Prepares SQL commands from a file.
    - `notify(channel, payload={}) -> pg_reply_awaiter<void>` or `notify(channel, payload, success, error)`:
      payloads must contain fewer than 8,000 bytes. A 7,999-byte payload is accepted; 8,000 bytes or more
      fail locally before SQL is queued, with `error::client_error` and `code == "00000"`.
      The callback form invokes its error callback and rethrows; the coroutine form returns a failed `Reply<void>`
      (`src/qbm/pgsql/transaction.h:489-501`, `src/qbm/pgsql/pg_notify_sql.cpp:65-74`,
      `src/qbm/pgsql/error.cpp:144-155`, `src/qbm/pgsql/commands.h:1249-1256,1673-1675`).
    - `savepoint(...)`: Creates a savepoint within a transaction. A handled child failure rolls back to and releases it, then the outer block continues; failed creation or cleanup aborts the outer block. The savepoint error callback runs once after cleanup, including when its success callback throws.
    - `then(...)`: Chains an operation to be executed if the previous one succeeds. A thrown callback becomes a client error visible to `error(...)` and `await()`; completed SQL is not undone.
    - `error(...)`: Registers an error handler for the preceding operation(s).
    - `success(...)`: Registers a success handler (similar to `then`).
    - `await()`: Synchronously waits for the transaction chain to complete. Returns `Transaction::status`.

See `qbm/pgsql/src/qbm/pgsql/transaction.h` (declarations) and the merged tail of `qbm/pgsql/src/qbm/pgsql/commands.h` (definitions) for the full fluent API of `qb::pg::detail::Transaction`.

#### `qb::pg::tcp::ssl::database`

Alias for `qb::pg::detail::Database<qb::io::transport::stcp, void>`.
This is the SSL/TLS-secured TCP-based PostgreSQL client. The `tcp::ssl::database` alias **only exists when `QB_HAS_SSL` is defined** (the framework found OpenSSL). Guard SSL code with `#ifdef QB_HAS_SSL`.

**Definition (Simplified from `pgsql.h`):**
```cpp
namespace qb::pg {
struct tcp {
#ifdef QB_HAS_SSL
    struct ssl {
        using database = detail::Database<qb::io::transport::stcp, void>;
        using notify_cb_consumer = detail::notify_cb_consumer<qb::io::transport::stcp>;
        using notify_co_consumer = detail::notify_co_consumer<qb::io::transport::stcp>;
    };
#endif
};
} // namespace qb::pg
```
**Usage:** Similar to `qb::pg::tcp::database`, but uses `qb::io::transport::stcp` for secure communication. Transport selection is a **compile-time choice of the alias** (`tcp::database` vs `tcp::ssl::database`), not a DSN scheme: an `ssl://` scheme string on a `tcp::database` does **not** enable TLS, and a `tcp://` string on a `tcp::ssl::database` still negotiates TLS. There is **no** libpq `sslmode` parsing. TLS certificate verification is off by default (`ssl_verify_mode::none` ≈ libpq `sslmode=require`: encrypted but unauthenticated); set `connection_options::ssl_verify = qb::pg::ssl_verify_mode::full` (≈ libpq `verify-full`: chain + hostname checks) and pass the options to `connect(connection_options)` before connecting. For a private CA set `ssl_root_cert` (libpq `sslrootcert`); for mutual TLS set `ssl_cert` + `ssl_key` (libpq `sslcert`/`sslkey`) — a bad path fails the connect closed. The DSN string cannot carry TLS fields.

#### LISTEN/NOTIFY consumers (`notify_cb_consumer`, `notify_co_consumer`)

Two `database` subtypes dedicated to receiving NOTIFY (`qb::pg::tcp::` and, with `QB_HAS_SSL`,
`qb::pg::tcp::ssl::`); both run normal queries too. Publish from a separate connection.

```cpp
namespace qb::pg::detail {
template <typename QB_IO_>
class notify_cb_consumer : public Database<QB_IO_, notify_cb_consumer<QB_IO_>> /* via notify_consumer */ {
public:
    notify_cb_consumer();
    explicit notify_cb_consumer(std::string const &opts);
    notify_cb_consumer &on_notify(std::function<void(qb::pg::notification &&)> cb);
};

template <typename QB_IO_>
class notify_co_consumer : public Database<QB_IO_, notify_co_consumer<QB_IO_>> /* via notify_consumer */ {
public:
    notify_co_consumer();
    explicit notify_co_consumer(std::string const &opts, std::size_t notify_capacity = 8192);
    notify_co_consumer &on_notify(std::function<void(qb::pg::notification &&)> cb);
    notify_co_consumer &on_notify_dropped(std::function<void(qb::pg::notification &&)> cb);
    [[nodiscard]] std::size_t notify_channel_capacity() const noexcept;
    [[nodiscard]] qb::io::async::task<std::optional<qb::pg::notification>> receive();
};
} // namespace qb::pg::detail
```

- `notify_cb_consumer`: every NOTIFY goes to `on_notify`, on the connection's thread, as it is read.
  Nothing is queued, so nothing is dropped (`stats().notifications_dropped` stays 0). Without a callback a
  NOTIFY is logged at INFO; an exception escaping the callback is contained and logged. Until 3.3 this name
  was an alias of `notify_co_consumer`, whose queue a callback-only program never read: after 8192
  notifications every later one was counted and logged as dropped. (Huly QB-1092) `on_gap(fn(qb::io::gap_info))`
  is called when a session that follows an earlier ready one becomes ready -- before its first `on_notify`
  and before `on_connected`; the connection accepts queries there, so it is where to resynchronise. (Huly QB-111)
- `notify_co_consumer`: `on_notify` (optional) is called first, then the NOTIFY is queued for
  `co_await receive()` in a bounded stream (`notify_capacity` elements, default 8192). An element is a NOTIFY
  or a gap marker (`kind == notification_kind::gap`, `gap.reason`, `gap.lost`): a reconnected session's
  marker comes when it is ready, before its first notification (the NOTIFYs it received while handshaking
  are held until then); a NOTIFY that does not fit goes to `on_notify_dropped` (or a WARN log), counts in
  `stats().notifications_dropped`, and ONE marker (`overflow`, `gap.lost` = the coalesced drops) precedes the
  next notification the stream takes, or ends what the reader drained. The stream ends only at `down`:
  `receive()` yields what it holds, then `std::nullopt`; a `connect()` after `down` serves a new stream that
  starts with what was not read (Huly QB-252, QB-111).

### Transaction Management

#### `qb::pg::transaction` (alias for `qb::pg::detail::Transaction`)

The `qb::pg::detail::Transaction` class provides the fluent API for managing database transactions and executing queries within them. The `qb::pg::tcp::database` and `qb::pg::tcp::ssl::database` classes inherit from `qb::pg::detail::Transaction`, making these methods directly available on database objects.

**Definition (declared in `qbm/pgsql/src/qbm/pgsql/transaction.h`, defined at the tail of `qbm/pgsql/src/qbm/pgsql/commands.h`):**
```cpp
namespace qb::pg::detail {
class Transaction {
public:
    // ... (constructors, destructor) ...

    // Fluent API methods:
    template <typename CB_SUCCESS, typename CB_ERROR, ...>
    Transaction& begin(CB_SUCCESS &&on_success, CB_ERROR &&on_error, transaction_mode mode = {});
    // ... other overloads for begin ...

    template <typename CB_SUCCESS, typename CB_ERROR, ...>
    Transaction& savepoint(std::string_view name, CB_SUCCESS &&on_success, CB_ERROR &&on_error);
    // ... other overloads for savepoint ...

    template <typename CB_SUCCESS, typename CB_ERROR, ...>
    Transaction& execute(std::string_view expr, CB_SUCCESS &&on_success, CB_ERROR &&on_error);
    // ... other overloads for execute (simple query) ...

    template <typename CB_SUCCESS, typename CB_ERROR, ...>
    Transaction& prepare(std::string_view query_name, std::string_view expr, type_oid_sequence &&types, CB_SUCCESS &&on_success, CB_ERROR &&on_error);
    // ... other overloads for prepare ...

    template <typename CB_SUCCESS, typename CB_ERROR, ...>
    Transaction& execute(std::string_view query_name, QueryParams &&params, CB_SUCCESS &&on_success, CB_ERROR &&on_error);
    // ... other overloads for execute (prepared statement) ...

    template <typename CB_SUCCESS, typename CB_ERROR, ...>
    Transaction& execute_file(const std::filesystem::path& file_path, CB_SUCCESS &&on_success, CB_ERROR &&on_error);
    // ... other overloads for execute_file ...

    template <typename CB_SUCCESS, typename CB_ERROR, ...>
    Transaction& prepare_file(std::string_view query_name, const std::filesystem::path& file_path, type_oid_sequence &&types, CB_SUCCESS &&on_success, CB_ERROR &&on_error);
    // ... other overloads for prepare_file ...

    template <typename CB_SUCCESS>
    Transaction& then(CB_SUCCESS &&on_success);

    template <typename CB_SUCCESS>
    Transaction& success(CB_SUCCESS &&on_success);

    template <typename CB_ERROR>
    Transaction& error(CB_ERROR &&on_error);

    struct status { // For await()
        // ... members: _results, _error ...
        explicit operator bool() const;
        resultset results();
        error::db_error& error();
    };
    status await();

    bool result() const; // Current success/failure status of the transaction chain
    // ... other protected/internal methods ...
protected:
    Transaction(Transaction *parent) noexcept;
    Transaction(PreparedQueryStorage &storage) noexcept;
    // ... queue management, error handling ...
};
} // namespace qb::pg::detail

namespace qb::pg {
    using transaction = detail::Transaction; // Alias
} // namespace qb::pg
```
**Key Fluent API Callbacks:**
- `on_success` for `begin`, `savepoint`, `prepare`, `execute` (simple query without results): `void(qb::pg::transaction& tr)`
- `on_success` for `execute` (query with results): `void(qb::pg::transaction& tr, qb::pg::results results)`
- Any other `execute` success signature fails compilation for both simple and prepared statements.
- `on_success` for `prepare`: `void(qb::pg::transaction& tr, const qb::pg::detail::PreparedQuery& prepared_query_details)`
- `on_error` for all operations: `void(const qb::pg::error::db_error& err)`
- `then`/`success`: `void(qb::pg::transaction& tr)`
- `error` (error handler attachment): `void(const qb::pg::error::db_error& err)`

## Connection Handling

### `qb::pg::connection_options`

Structure to hold parameters for establishing a PostgreSQL connection.

**Definition (`qbm/pgsql/src/qbm/pgsql/common.h`):**
```cpp
namespace qb::pg {
struct connection_options {
    dbalias      alias;      // Optional alias for the connection
    std::string  schema;     // tcp, ssl, socket
    std::string  uri;        // host:port or /path/to/socket
    std::string  database;   // Database name
    std::string  user;       // Username
    std::string  password;   // Password
    qb::duration connect_timeout{std::chrono::seconds(10)}; // handshake deadline (default 10s)
    ssl_verify_mode ssl_verify{ssl_verify_mode::none};      // TLS cert verification level (default: none)
    int          keepalive_interval{0};                     // TCP keepalive interval (s); 0 = disabled
    int          keepalive_probes{3};
    int          keepalive_idle{60};

    void generate_alias(); // Generates an alias if not set
    static connection_options parse(std::string const &connstr);
};

// TLS certificate verification level for ssl:// (secure) connections.
enum class ssl_verify_mode {
    none, // encrypt only; do NOT verify the certificate (≈ libpq sslmode=require). Default.
    full, // verify the chain against the system trust store AND that it matches the host (≈ libpq verify-full).
};
} // namespace qb::pg
```
**User-defined literal:** `"tcp://user:pass@host:port[dbname]"_pg` creates `connection_options`. The DSN form is `scheme://[user[:password]@]host[:port][database]` — the **database name is in square brackets**, not `/dbname`. The DSN only carries user/password/database/host:port; set timeouts, TLS verification, and keepalive on the struct. The DSN string cannot carry TLS fields: to enable certificate verification, set `ssl_verify` on a `connection_options` and pass it to the `connect(connection_options)` overload (see below).

```cpp
// Enable strict TLS verification (chain + hostname) before connecting:
auto o = qb::pg::connection_options::parse(dsn);
o.ssl_verify = qb::pg::ssl_verify_mode::full;
co_await db.connect(o);
```

### `qb::pg::dbalias`

A string wrapper for naming database connections.

**Definition (`qbm/pgsql/src/qbm/pgsql/common.h`):**
```cpp
namespace qb::pg {
struct dbalias : std::string {
    dbalias();
    explicit dbalias(std::string const &rhs);
    // ... swap, assignment ...
};
} // namespace qb::pg
```
**User-defined literal:** `"mydb"_db` creates a `dbalias`.

## Query Execution

### `qb::pg::params` (alias for `qb::pg::detail::QueryParams`)

Class for managing parameters passed to prepared statements. Handles serialization of C++ types to PostgreSQL binary format.

**Definition (`qbm/pgsql/src/qbm/pgsql/queries.h`):**
```cpp
namespace qb::pg::detail {
class QueryParams {
public:
    QueryParams();
    template <typename... T>
    QueryParams(T &&...args); // Variadic constructor to add parameters

    std::vector<byte>& get(); // Get mutable serialized parameter buffer
    const std::vector<byte>& get() const; // Get const serialized buffer
    const std::vector<integer>& param_types() const; // Get OIDs of parameters
    smallint param_count() const;
    bool empty() const;
};
} // namespace qb::pg::detail

namespace qb::pg {
    using params = detail::QueryParams;
} // namespace qb::pg
```
**Usage:**
- Inline: `db.execute("stmt_name", params{123, std::string("text"), std::optional<double>{}}, on_ok, on_err);` — name the type explicitly (`params{...}`), as every test and §3.3 of the concepts doc do; the variadic constructor serializes there.
- Built first: `qb::pg::params p{123, std::string("text"), std::optional<double>{}}; db.execute("stmt_name", std::move(p));` — same constructor, all serialization happens there. (`qb::pg::params` has **no** `add_param` member; `add_param` is internal to `ParamSerializer`.)

### Prepared Statements (`qb::pg::detail::PreparedQuery`, `qb::pg::detail::PreparedQueryStorage`)

- **`qb::pg::detail::PreparedQuery` (`qbm/pgsql/src/qbm/pgsql/queries.h`):**
  ```cpp
  struct PreparedQuery {
      std::string          name;
      std::string          expression;
      std::vector<oid>     param_types; // Was type_oid_sequence
      row_description_type row_description;
  };
  ```
- **`qb::pg::detail::PreparedQueryStorage` (`qbm/pgsql/src/qbm/pgsql/queries.h`):**
  Manages a collection of `PreparedQuery` objects, typically one per database connection/session. Used internally by `Transaction`.
  Copies rebuild their own LRU iterators; self-move assignment preserves the cache. `get()` promotes recency even on a const storage object.
  Every eviction of a named statement queues its server-side Close: `make_room_for(name)` runs as a Parse is written (so the Close rides in that message), `set_max_size()` evicts down to the new bound at once, and the next Parse or Bind drains the queue (`has_pending_close()` / `take_pending_close()`, prefixed by `close_evicted_statements_before`). `clear()` drops the queue with the cache. `hits()` / `misses()` count named executions found / refused at dispatch (`lookup_for_execute`); the unnamed `""` slot is not counted. (Huly QB-207)
  ```cpp
  class PreparedStorage {      // queries.h:76 — `PreparedQueryStorage` is the ALIAS (queries.h:285)
  public:
      bool has(std::string_view name) const;
      const PreparedQuery& push(PreparedQuery &&query);
      PreparedQuery const& get(std::string_view name) const;
      void set_max_size(size_t max_size);                    // 0 = the default 100; a shrink evicts at once
      size_t max_size() const; size_t size() const; size_t evicted_count() const;
      std::uint64_t hits() const noexcept; std::uint64_t misses() const noexcept;
      bool lookup_for_execute(std::string_view name) const;  // has() + hit/miss count (named only)
      void make_room_for(std::string_view name);             // before a Parse is written
      bool has_pending_close() const noexcept;
      std::vector<std::string> take_pending_close() noexcept;
  };
  ```

## Result Set Processing

### `qb::pg::results` (alias for `qb::pg::resultset`)

Represents the set of rows returned by a query. The class is declared publicly as `qb::pg::resultset` in `resultset.h`; `qb::pg::results` is its public alias (`qbm/pgsql/src/qbm/pgsql/pgsql.h:4338`). Both spellings are valid, with `results` preferred for application code. Row and field are reachable as `qb::pg::results::row` / `qb::pg::results::field`.

**Definition (`qbm/pgsql/src/qbm/pgsql/resultset.h`):**
```cpp
namespace qb::pg {
class resultset {
public:
    // STL-like container typedefs (const_iterator, value_type=row, etc.)
    resultset();
    // ... (constructors, not typically user-created directly) ...

    size_type size() const; // Number of rows
    bool empty() const;

    const_iterator begin() const;
    const_iterator end() const;
    // ... rbegin, rend, front, back ...

    reference operator[](size_type index) const; // Access row by index
    reference at(size_type index) const; // Access row by index with bounds check

    operator bool() const; // True if not empty
    bool operator!() const; // True if empty

    qb::json json() const; // Array of objects; present cells are strings, SQL NULL is JSON null

    // Field metadata access:
    row::size_type columns_size() const;
    row_description_type const& row_description() const;
    size_type index_of_name(std::string const &name) const;
    field_description const& field(size_type col_index) const;
    field_description const& field(std::string const &name) const;
    std::string const& field_name(size_type col_index) const;

    // Internal access (used by row/field):
    // field_buffer at(size_type r, row::size_type c) const;
    // bool is_null(size_type r, row::size_type c) const;
};
} // namespace qb::pg
```

### `qb::pg::results::row`

Represents a single row within a `results` set (`qb::pg::resultset::row`). Provides access to individual fields.

**Definition (`qbm/pgsql/src/qbm/pgsql/resultset.h`):**
```cpp
namespace qb::pg {
class resultset { // also exposed as qb::pg::results
public:
    class row {
    public:
        // STL-like container typedefs for fields (const_iterator, value_type=field, etc.)
        resultset::size_type row_index() const; // Full row ordinal; row::size_type counts fields
        // row_a - row_b -> resultset::difference_type (std::ptrdiff_t, the row iterator's): any two ordinals' distance

        size_type size() const; // Number of fields in the row
        bool empty() const;

        const_iterator begin() const; // Iterator to first field
        const_iterator end() const;   // Iterator past last field
        // ... rbegin, rend ...

        reference operator[](size_type field_index) const; // Access field by index
        reference operator[](std::string const &field_name) const; // Access field by name

        // Tuple conversion:
        template <typename... T>
        void to(std::tuple<T...> &tpl) const;
        template <typename... T>
        void to(std::tuple<T&...> tpl_refs) const; // For std::tie
        template <typename... T>
        void to(T&... args) const; // Direct to variables
        // ... named versions: to({"name1", "name2"}, ...) ...
        // Too few names throw error::db_error before modifying targets.

        resultset::size_type index_of_name(std::string const &name) const;
    };
};
} // namespace qb::pg
```

### `qb::pg::results::field`

Represents a single field (column value) within a `results::row` (`qb::pg::resultset::field`).

**Definition (`qbm/pgsql/src/qbm/pgsql/resultset.h`):**
```cpp
namespace qb::pg {
class resultset { // also exposed as qb::pg::results
public:
    class field {
    public:
        size_type row_index() const;
        row::size_type field_index() const;

        std::string const& name() const;
        field_description const& description() const;
        bool is_null() const;
        bool empty() const; // (Note: empty() might be misleading, is_null() is primary for null checks)
        std::span<const std::byte> view() const; // raw wire bytes
        std::string_view text() const; // raw bytes as characters, including binary version byte

        std::string_view jsonb_text() const; // PostgreSQL canonical JSONB text, borrows backing row storage
        std::string jsonb_text_copy() const; // owning copy; both validate NULL/OID/format/version

        // Value extraction:
        template <typename T>
        typename std::decay<T>::type as() const; // Throws on NULL if T is not optional; optional<T> preserves present empty text and OID-aware numeric conversion
        template <typename T>
        bool to(T &val) const; // For non-optional T, THROWS error::value_is_null on NULL. For std::optional<T>/nullable T, sets the target to null and returns true. The bool return reports parse success, not NULL-ness — use as<std::optional<T>>() or is_null() to detect NULL.

    };
};
} // namespace qb::pg
```
`jsonb_text()` returns PostgreSQL's canonical JSONB text for a non-NULL OID 3802
field. It validates the result format and version byte, and the view lasts only
while its backing row storage remains alive. A callback's `results` is borrowed:
copying it or keeping its `field` does not keep rows alive after the callback.
Call `jsonb_text_copy()` inside the callback, or take a `deep_snapshot()` and
read the view from that owning snapshot. Coroutine replies already own their
result snapshot. The field API expects version-byte-plus-text result bytes;
the direct converter's legacy four-byte prefix does not apply to fields.
`as<qb::jsonb>()` throws `error::client_error` when a PostgreSQL JSONB number
would change decimal value or overflow the nlohmann DOM (`1e400` is a valid
PostgreSQL JSONB number). Malformed JSON keeps a distinct `std::runtime_error`
path. Equal values with different spellings such as `1.2300` and `1.23` remain
readable. A rejected input may already have allocated an owning DOM prefix; unwinding
destroys it and never returns it. Canonical-text accessors retain their ownership rules.

There are **no** global `qb::pg::get<T>(field)` / `qb::pg::get(field, T&)` helpers. Two such
templates are written in `qbm/pgsql/src/qbm/pgsql/field_handler.h`, but that file is dead — no
translation unit includes it, it does not compile on its own, and as of 3.0 it is excluded from
the install, so it is not in a consumer's prefix at all. Use `field.as<T>()` / `field.to(val)`
above.

### `qb::pg::field_description`

Structure holding metadata for a single column in a result set.

**Definition (`qbm/pgsql/src/qbm/pgsql/common.h`):**
```cpp
namespace qb::pg {
struct field_description {
    std::string name;            // Field name
    integer table_oid;           // OID of the table, if applicable
    smallint attribute_number;   // Column number in table, if applicable
    oid type_oid;                // Data type OID of the field
    smallint type_size;          // Data type size (see pg_type.typlen)
    integer type_mod;            // Type modifier (e.g., varchar(n) length)
    protocol_data_format format_code; // Binary (1) or Text (0)
    integer max_size;            // Max storage size for the field
};

using row_description_type = std::vector<field_description>;
} // namespace qb::pg
```

## Data Type Handling

### PostgreSQL OIDs (`qb::pg::oid`)

An enum class defining Object Identifiers for standard PostgreSQL data types.

**Definition (`qbm/pgsql/src/qbm/pgsql/pg_types.h`):**
```cpp
namespace qb::pg {
enum class oid : int { // Renamed from oid_t to avoid conflict
    boolean          = 16,
    bytea            = 17,
    char_            = 18, // Renamed from char to char_
    name             = 19,
    int8             = 20,   // bigint
    int2             = 21,   // smallint
    int4             = 23,   // integer
    text             = 25,
    oid_t            = 26,   // PostgreSQL OID type itself
    json             = 114,
    xml              = 142,
    float4           = 700,  // real
    float8           = 701,  // double precision
    unknown          = 705,
    int2_array       = 1005,
    int4_array       = 1007,
    text_array       = 1009,
    oid_array        = 1028,
    float4_array     = 1021,
    varchar          = 1043,
    date             = 1082,
    time             = 1083,
    timestamp        = 1114, // timestamp without time zone
    timestamptz      = 1184, // timestamp with time zone
    interval         = 1186,
    numeric          = 1700,
    uuid             = 2950,
    jsonb            = 3802,
    // ... many other OIDs ...
    record           = 2249, // For anonymous records
    record_array     = 2287,
    void_            = 2278 // Renamed from void to void_
};
std::ostream& operator<<(std::ostream& out, oid val);
std::istream& operator>>(std::istream& in, oid& val);
} // namespace qb::pg
```
Includes C++ types like `qb::pg::smallint` (int16_t), `qb::pg::integer` (int32_t), `qb::pg::bigint` (int64_t), `qb::pg::bytea` (`std::vector<char>`), `qb::pg::uuid`, `qb::pg::json`, `qb::pg::jsonb`. PostgreSQL `timestamptz` (OID 1184) and `timestamp` (OID 1114) both map to `qb::wall_time` (a UTC instant on `system_clock`), round-tripped as integer microseconds. `qb::duration` is for *timeouts* (connect / statement), never for timestamp column values. Never emit the retired `qb::Timestamp` / `qb::UtcTimestamp` / `qb::LocalTimestamp` / `to_timestamp(` / `to_time_point(` names.

### C++ Type Mapping (`qb::pg::detail::type_mapping`)

Template struct mapping C++ types to their corresponding PostgreSQL `oid` values.

**Definition (`qbm/pgsql/src/qbm/pgsql/type_mapping.h`):**
```cpp
namespace qb::pg::detail {
template <typename T, typename Enable = void>
struct type_mapping {
    // FAIL-LOUD: the primary template has NO `type_oid`. An unmapped C++ type is a
    // COMPILE error, not a silent bind as OID 705 'unknown' — that fallback was removed.
    static_assert(!sizeof(T), "qbm-pgsql: no PostgreSQL OID mapping for this C++ type. "
                              "Bind a supported type or add a type_mapping<> specialization. "
                              "The generic fallback no longer silently sends OID 705 'unknown'.");
};   // src/qbm/pgsql/type_mapping.h:58-63
// Specializations for bool, int16_t, int32_t, int64_t, float, double,
// std::string, const char*, std::string_view, char[N],
// std::vector<char>, std::vector<unsigned char>, bytea,
// qb::uuid, qb::json, qb::jsonb,
// qb::wall_time  (-> timestamptz, OID 1184; integer-microsecond round-trip),
// std::optional<T>
// NOTE: timestamps map to qb::wall_time. The retired types qb::Timestamp /
// qb::UtcTimestamp / qb::LocalTimestamp are NOT specialized here — never emit them.

template <typename T>
inline integer get_type_oid();

template <typename... T>
void fill_types(std::vector<integer>& types_to_fill);
} // namespace qb::pg::detail
```

### Type Conversion (`qb::pg::detail::TypeConverter`)

Template class responsible for actual data conversion between C++ types and PostgreSQL binary/text formats.

**Definition (`qbm/pgsql/src/qbm/pgsql/type_converter.h`):**
```cpp
namespace qb::pg::detail {
template <typename T>
class TypeConverter {
public:
    using value_type = typename std::decay<T>::type;

    static integer get_oid();
    static void to_binary(const value_type& value, std::vector<byte>& buffer);
    static std::string to_text(const value_type& value);
    static value_type from_binary(std::span<const byte> buffer);
    static value_type from_text(const std::string& text);
    static void write_integer(std::vector<byte>& buffer, integer value); // public — TypeConverter<T>'s only access specifier is `public:` (type_converter.h:181, :699-700)
};
// Specializations: qb::uuid, qb::wall_time (timestamptz/timestamp), qb::json, qb::jsonb,
// std::chrono::duration (type_converter.h:1060), std::string (:1172), numeric (:1267),
// qb::date (type_converter.h:1308), qb::time_of_day (:1330), qb::time_of_day_tz (:1352),
// qb::calendar_interval (type_converter.h:1372), std::vector<std::byte> (:1397)
// (the retired qb::Timestamp / qb::UtcTimestamp / qb::LocalTimestamp are NOT specialized).
} // namespace qb::pg::detail
```
Internally uses `qb::pg::detail::ParamSerializer` for serialization logic (see `param_serializer.h`) and `qb::pg::detail::ParamUnserializer` for deserialization (see `param_unserializer.h` and `field_reader.h`).

Array specializations provide `get_oid`, `to_binary`, `from_binary`, `to_text` and `from_text`
for both `std::vector<T>` and `std::vector<std::optional<T>>`. Supported elements are `bool`,
`smallint`, `integer`, `bigint`, `float`, `double`, `std::string`, `qb::uuid`, `qb::wall_time`,
`qb::pg::detail::numeric`, `qb::json` and `qb::jsonb`. The last five map to `uuid[]` (2951),
`timestamptz[]` (1185), `numeric[]` (1231), `json[]` (199) and `jsonb[]` (3807).
<!-- src: src/qbm/pgsql/type_converter.h:1872-1876 -->

`from_binary` consumes array value bytes without a length prefix; `to_binary` writes the
length prefix. `from_text` and `to_text` consume/produce PostgreSQL array literals.
SQL NULL elements require `std::optional<T>`; JSON `null` is a present element. Timestamp
and JSON/JSONB text elements are quoted and escaped. Empty arrays remain empty arrays,
and multi-dimensional arrays throw `error::field_type_mismatch`.
`results::json()` supports these binary arrays too: JSON/JSONB elements retain their wire text
without DOM conversion; the array literal preserves NULL elements and lower bounds. It writes the
literal into one string (Huly QB-1087): `decode_pg_array` validates the framing once and yields
element views, the `[lower:upper]=` prefix comes first, and `append_pg_binary_element<T>` appends
each present element in place -- through `TypeConverter<qb::uuid>::append_text`,
`TypeConverter<qb::wall_time>::append_text`, `TypeConverter<numeric>::append_text_from_binary`
(`append_pg_numeric_text`) and `append_pg_array_quoted` -- with no typed vector and no string per
element. `render_pg_array_text` writes its elements through `append_pg_array_element`, the one
place that decides which elements are quoted.
Temporary element views are internal to that rendering call; the exported JSON owns its strings.
No public array converter for `std::vector<std::string_view>` is provided.
<!-- src: src/qbm/pgsql/resultset.cpp:424-500; src/qbm/pgsql/type_converter.h:1725-1816,1872-1876 -->

`TypeConverter<qb::wall_time>::to_text` keeps UTC text and its `+00` suffix. Years 1970–9999
use a stack formatter over the existing qb calendar; other years retain the previous formatter.
`TypeConverter<numeric>::from_binary` still returns an owning `numeric` with the same scale
and special values; integer limb formatting and transfer of the decoded string change no signature.
`TypeConverter<qb::wall_time>::append_text` writes `to_text`'s text into a caller's string, and
`decode_pg_numeric`, `from_binary` and `append_text_from_binary` decode through
`append_pg_numeric_text`, which writes the digits in place (Huly QB-1087).
<!-- src: src/qbm/pgsql/type_converter.cpp:138-212,681-758,891-914 -->

## Error Handling

Exceptions inherit from `std::runtime_error` and `qb::pg::error::db_error`.

### `qb::pg::error::db_error`

Base class for PostgreSQL related exceptions.

**Definition (`qbm/pgsql/src/qbm/pgsql/error.h`):**
```cpp
namespace qb::pg::error {
class db_error : public std::runtime_error {
public:
    explicit db_error(std::string const &what_arg);
    explicit db_error(char const *what_arg);
    db_error(std::string const &message, std::string severity, std::string code, std::string detail);
    explicit db_error(std::shared_ptr<const server_error_fields> fields); // a server error, every field kept

    std::string    severity; // ERROR, FATAL, WARNING, etc.
    std::string    code;     // PostgreSQL error code string (e.g., "42P01")
    std::string    detail;
    sqlstate::code sqlstate; // Enum representation of the SQLSTATE

    // Every field the server sent (Huly QB-108): null / empty for an error the client raised.
    [[nodiscard]] server_error_fields const *server_fields() const noexcept;
    [[nodiscard]] std::string_view severity_nonlocalized() const noexcept; // V
    [[nodiscard]] std::string_view hint() const noexcept;                  // H
    [[nodiscard]] std::string_view position() const noexcept;              // P (decimal text)
    [[nodiscard]] std::string_view internal_position() const noexcept;     // p
    [[nodiscard]] std::string_view internal_query() const noexcept;        // q
    [[nodiscard]] std::string_view where() const noexcept;                 // W
    [[nodiscard]] std::string_view schema_name() const noexcept;           // s
    [[nodiscard]] std::string_view table_name() const noexcept;            // t
    [[nodiscard]] std::string_view column_name() const noexcept;           // c
    [[nodiscard]] std::string_view data_type_name() const noexcept;        // d
    [[nodiscard]] std::string_view constraint_name() const noexcept;       // n
    [[nodiscard]] std::string_view file_name() const noexcept;             // F
    [[nodiscard]] std::string_view line() const noexcept;                  // L
    [[nodiscard]] std::string_view routine() const noexcept;               // R
};
} // namespace qb::pg::error
```

The fields live in one `std::shared_ptr<const server_error_fields>`, shared by the error's copies and allocated only for
a server error: `db_error` (and so every `Reply<T>`) grows by 16 bytes on a 64-bit target, pinned by a `static_assert`
in `tests/unit/protocol/protocol-message-codec.cpp`. `query_error` has the same `explicit query_error(std::shared_ptr<const server_error_fields>)`.

### `qb::pg::error::connection_error`

Thrown for errors during connection establishment or if a connection is lost.

**Definition (`qbm/pgsql/src/qbm/pgsql/error.h`):**
```cpp
namespace qb::pg::error {
class connection_error : public db_error { /* ... */ };
} // namespace qb::pg::error
```

### `qb::pg::error::query_error`

Thrown when the database server reports an error during query execution (syntax errors, constraint violations, etc.).

**Definition (`qbm/pgsql/src/qbm/pgsql/error.h`):**
```cpp
namespace qb::pg::error {
class query_error : public db_error { /* ... */ };
} // namespace qb::pg::error
```

### Other Error Types

-   **`qb::pg::error::transaction_closed`**: Inherits `query_error`. Operation on a committed/rolled-back transaction.
-   **`qb::pg::error::client_error`**: Inherits `db_error`. Wraps exceptions from user callbacks.
-   **`qb::pg::error::value_is_null`**: Inherits `db_error`. Attempt to access a NULL field as a non-optional type.
-   **`qb::pg::error::field_is_null`**: Inherits `value_is_null`. More specific version for field conversion.
-   **`qb::pg::error::field_type_mismatch`**: Inherits `db_error`. Field cannot be converted to the requested C++ type.

### SQLSTATE Codes (`qb::pg::sqlstate::code`)

An enum class defining symbolic names for standard SQLSTATE error codes.

**Definition (`qbm/pgsql/src/qbm/pgsql/sqlstates.h`):**
```cpp
namespace qb::pg::sqlstate {
enum code {
    unknown_code,
    successful_completion, // 00000
    warning,               // 01000
    // ... many other SQLSTATE codes ...
    division_by_zero,      // 22012
    unique_violation,      // 23505
    serialization_failure, // 40001
    deadlock_detected,     // 40P01
    // ...
};
code code_to_state(std::string const& val); // Converts 5-char string to enum
// std::ostream& operator<<(std::ostream&, code); (from sqlstates.cpp)
} // namespace qb::pg::sqlstate
```
See `qbm/pgsql/src/qbm/pgsql/sqlstates.cpp` for the full mapping.

## Supporting Types and Enums

Primarily defined in `common.h`, `pg_types.h`, and `error.h`.

### Enums
-   `qb::pg::isolation_level`: `read_committed` (default), `repeatable_read`, `serializable`.
-   `qb::pg::protocol_data_format`: `Text` (0), `Binary` (1).
-   `qb::pg::detail::message_tag`: Defines tags for all PostgreSQL frontend/backend messages (e.g., `query_tag` 'Q', `data_row_tag` 'D').
-   `qb::pg::detail::auth_states`: `OK` (0), `Cleartext` (3), `MD5Password` (5), `SCRAM_SHA256` (10), etc.

### Structs and Type Aliases
-   **`qb::pg::transaction_mode`**: `{isolation_level isolation, bool read_only, bool deferrable}`.
-   **`qb::pg::byte`**: `char`.
-   **`qb::pg::bytea`**: `struct bytea : std::vector<byte> {}`.
-   **`qb::pg::nullable<T>`**: `std::optional<T>`.
-   **Numeric types**: `smallint` (int16_t), `integer` (int32_t), `bigint` (int64_t).
-   **`qb::pg::field_buffer`**: `qb::util::input_iterator_buffer`.
-   **`qb::pg::detail::row_data`**: Internal storage for a row's data.
-   **`qb::pg::server_error_fields`** (`error.h`): every field of an ErrorResponse / NoticeResponse, one `std::string` each, empty when absent — `severity` (S), `severity_nonlocalized` (V), `sqlstate` (C), `message` (M), `detail` (D), `hint` (H), `position` (P), `internal_position` (p), `internal_query` (q), `where` (W), `schema_name` (s), `table_name` (t), `column_name` (c), `data_type_name` (d), `constraint_name` (n), `file_name` (F), `line` (L), `routine` (R). (Huly QB-108)
-   **`qb::pg::notice`**: `using notice = server_error_fields;` — what `database::on_notice(std::function<void(qb::pg::notice const &)>)` receives.
-   **`qb::pg::detail::notice_message`**: the decoder's view of NOTICE / ERROR messages: it derives from `server_error_fields` and maps each field code to its member (`has_field`, `field`).
-   **`qb::pg::detail::command_complete`**: Structure holding the command tag from a CommandComplete message.
-   **`qb::pg::notification`**: `{int server_backend_pid; std::string channel; std::string payload; notification_kind kind = notify; qb::io::gap_info gap;}` -- `notification_kind { notify, gap }`; `gap` (`qb::io::gap_info{gap_reason reason; std::uint64_t lost;}`) is meaningful on a marker. (Huly QB-111)
-   **`qb::pg::RetryPolicy`**: `using RetryPolicy = qb::io::reconnect_policy;` (`<qb/io/reconnect.h>`). (Huly QB-111)
-   **`qb::pg::handshake_session`**: the handshake hook's write handle -- `execute(sql)`, `query(sql)`, `prepare(name, sql, types = {})`, `execute(name, QueryParams&&)` (each a `pg_reply_awaiter`), `live()`; copyable, queues only while its own handshake runs. (Huly QB-111)

### Observability (`qb::pg::stats`, `qb::pg::query_info`, `qb::pg::query_kind`)

Declared in `qbm/pgsql/src/qbm/pgsql/stats.h` (reached through `<qbm/pgsql/pgsql.h>`); returned by
`database::stats()` and passed to the `database::on_query_complete` hook (Huly QB-115).

```cpp
namespace qb::pg {
enum class query_kind : std::uint8_t {   // stats.h:31
    simple, prepare, execute, begin, commit, rollback, savepoint, release_savepoint, rollback_savepoint
};
struct query_info {                      // stats.h:50
    query_kind             kind{};
    std::string_view       name;         // statement or savepoint name; "" otherwise (and for the unnamed statement)
    std::string_view       sql;          // when the client has it: simple, prepare, an execute's cached statement
    qb::duration           duration{};   // send to completion; zero when never sent
    bool                   ok = false;
    error::db_error const *error = nullptr; // null on success
};
struct stats {                           // stats.h:75
    std::uint64_t queries, errors, bytes_sent, bytes_received,
                  notifications_received, notifications_dropped,
                  prepared_hits, prepared_misses, prepared_evicted,
                  reconnects, gaps;                                   // all = 0
    qb::duration  last_query_duration{};
};
}
```

- A query is one round trip of the serial queue; the inline `query(sql, args...)` is a `prepare` then an `execute`.
  One that failed before it was sent (refused locally, drained by a disconnect) counts as a query and an error, with
  a zero duration; a call refused at submission on a disconnected handle never entered the queue and is not counted.
- `bytes_*` count PostgreSQL protocol messages (TLS framing excluded), the handshake included.
  `notifications_dropped` counts what a `notify_co_consumer`'s bounded channel refused. `reconnects` counts the
  ready connections reached after a lost one (an explicit `disconnect()` ends the session: the next connect is
  not one), `gaps` the gap markers a notify consumer emitted -- the names qbm-redis uses. (Huly QB-111)
- The views of a `query_info` are valid during the hook call only; `sql` may carry literal values — never log it raw.
- Internals that feed it: `ISqlQuery::describe()` (each query's kind, name and SQL), and
  `Transaction::fail_all_pending(err, drained_query_observer *)`, through which the connection counts a drained queue.

### Metrics adapter (`<qbm/pgsql/metrics.h>`, Huly QB-154)

`void qb::metrics::describe(qb::metrics::snapshot&, qb::pg::stats const&, qb::metrics::label_set labels)`.
Explicit include, not part of the `pgsql.h` umbrella. Out-of-line in `qbm::pgsql`; no
HTTP dependency, hook installation or clock reads. Emits exact integer counters named
`qb_pgsql_<field>_total` for all eleven integer fields above, and gauge
`qb_pgsql_last_query_duration_seconds` (chrono seconds, initial zero means unobserved).
Caller supplies stable labels distinguishing sources; canonicalize sums equal label sets,
which is unsuitable for unrelated last-duration gauges. _(metrics.h:21)_

## Protocol Details

### `qb::protocol::pgsql<IO_>`

Implements the PostgreSQL wire protocol message framing and parsing for the QB I/O system. Used internally by `qb::pg::detail::Database`.

**Definition (`qbm/pgsql/src/qbm/pgsql/pgsql.h`):**
```cpp
namespace qb::protocol {
template <typename IO_>
class pgsql final : public qb::io::async::AProtocol<IO_> {
public:
    using message = std::unique_ptr<pg::detail::message>;

    explicit pgsql(IO_ &io) noexcept;

    std::size_t getMessageSize() noexcept final;
    void onMessage(std::size_t size) noexcept final;
    void reset() noexcept final;
    // ... internal methods ...
};
} // namespace qb::protocol
```

### Message Tags (`qb::pg::detail::message_tag`)

Enum defined in `qbm/pgsql/src/qbm/pgsql/protocol.h` listing all message tags like `authentication_tag` ('R'), `query_tag` ('Q'), `data_row_tag` ('D'), `ready_for_query_tag` ('Z'), etc.

---

This API reference documents the `qb::pg` module for application developers.
