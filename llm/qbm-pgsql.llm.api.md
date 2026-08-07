<!-- Verified-against: qbm-pgsql @ qb 3.0.0. Source of truth: the headers under qbm/pgsql/src/qbm/pgsql/. -->
# `qb::pg` (PostgreSQL) Module API Reference

This document provides a detailed API reference for the `qb::pg` module, part of the QB C++ Actor Framework. It is intended for use by other AI models to understand the capabilities and usage of this PostgreSQL client library.

## Table of Contents

- [Overview](#overview)
- [Core Classes](#core-classes)
  - [Database Client Classes](#database-client-classes)
    - [`qb::pg::tcp::database`](#qbpgtcpdatabase)
    - [`qb::pg::tcp::ssl::database`](#qbpgtcpssldatabase)
  - [Transaction Management](#transaction-management)
    - [`qb::pg::transaction` (alias for `qb::pg::detail::Transaction`)](#qbpgtransaction-alias-for-qbpgdetailtransaction)
- [Connection Handling](#connection-handling)
  - [`qb::pg::connection_options`](#qbpgconnection_options)
  - [`qb::pg::dbalias`](#qbpgdbalias)
- [Query Execution](#query-execution)
  - [`qb::pg::params` (alias for `qb::pg::detail::QueryParams`)](#qbpgparams-alias-for-qbpgdetailqueryparams)
  - [Prepared Statements (`qb::pg::detail::PreparedQuery`, `qb::pg::detail::PreparedQueryStorage`)](#prepared-statements-qbpgdetailpreparedquery-qbpgdetailpreparedquerystorage)
- [Result Set Processing](#result-set-processing)
  - [`qb::pg::results` (alias for `qb::pg::detail::resultset`)](#qbpgresults-alias-for-qbpgdetailresultset)
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
- [Protocol Details](#protocol-details)
  - [`qb::protocol::pgsql<IO_>`](#qbprotocolpgsqlio_)
  - [Message Tags (`qb::pg::detail::message_tag`)](#message-tags-qbpgdetailmessage_tag)

## Overview

The `qb::pg` module provides a high-performance, asynchronous C++20/23 client for PostgreSQL databases, designed for integration with the QB Actor Framework. It utilizes `qb-io` for its asynchronous I/O operations. It is a compiled library (**STATIC** by default, **SHARED** when `QB_BUILD_SHARED_LIBS`/`BUILD_SHARED_LIBS` is set; alias `qbm::pgsql`); the umbrella header is `<qbm/pgsql/pgsql.h>`.

Key features include:
- Two interchangeable completion styles with the **same method names**: a coroutine API (`co_await`-only, yielding `Reply<T>`) and a fluent, callback-based transaction API. The single-op coroutine entry points (`execute`, `query(sql)`, `prepare`, `begin`/`commit`/`rollback`, savepoints, `notify`/`listen`) return `pg_reply_awaiter<T>`; the helpers that chain multiple awaits internally (`query(sql, args...)`, `copy_out`, `copy_in`, `query_stream`) return `qb::io::async::task<Reply<T>>`. Both are `co_await`-only and yield `Reply<T>`.
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
    // `connect(on_ok, on_err)` callback overload. The awaiter resolves to `bool`
    // (true once the handshake reached ReadyForQuery).
    [[nodiscard]] connect_awaiter connect();
    [[nodiscard]] connect_awaiter connect(qb::duration timeout);
    [[nodiscard]] connect_awaiter connect(std::string const &conn_opts_str);
    [[nodiscard]] connect_awaiter connect(connection_options opts); // carries TLS/keepalive fields the DSN can't
    [[nodiscard]] connect_awaiter connect(std::string const &conn_opts_str, typename QB_IO_::transport_io_type &&raw_io);
    void disconnect();
    void prepare_reconnect() noexcept; // must precede a re-connect() on the same object

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
    bool cancel();                                      // out-of-band PostgreSQL CancelRequest (SYNCHRONOUS, ≤2s); NOT [[nodiscard]] (pgsql.h:2266-2267)
    [[nodiscard]] bool in_transaction() const noexcept; // backend session in a transaction block ('T'/'E')
    [[nodiscard]] bool used_channel_binding() const noexcept; // SCRAM-SHA-256-PLUS tls-server-end-point binding negotiated
    [[nodiscard]] std::optional<std::string_view> parameter_status(std::string_view key) const; // PQparameterStatus
    [[nodiscard]] int server_version() const;           // PQserverVersion (e.g. 160002)
    [[nodiscard]] int backend_pid() const noexcept;     // PQbackendPID

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
- `connect_awaiter connect(qb::duration timeout)`: Connects, overriding the handshake deadline.
- `void disconnect()`: Closes the connection. Call `prepare_reconnect()` before re-connecting the same object.
- **Streaming / bulk-load (coroutine, `co_await`-only; each returns `qb::io::async::task<Reply<T>>`):**
    - `copy_out(std::string sql, std::function<void(std::string_view)> sink) -> task<Reply<resultset>>`: Runs a `COPY … TO STDOUT` and delivers each `CopyData` chunk to `sink` as it arrives. Streams in **constant memory** (rows are never buffered in a result set). The `string_view` is valid only during the call. Resolves to `ok()` on success.
    - `copy_in(std::string sql, std::function<std::optional<std::string>()> source) -> task<Reply<resultset>>`: Runs a `COPY … FROM STDIN`, calling `source` repeatedly and sending each returned chunk as `CopyData` until it returns `std::nullopt` (then `CopyDone`). A throwing `source` aborts the COPY with `CopyFail`. Resolves to `ok()` (the `COPY n` count) on success; the connection stays usable on error. **NOT constant-memory** — it drains the whole source into the output pipe synchronously (capped by the write-buffer ceiling); only `copy_out` and `query_stream` stream in constant memory.
    - `copy_in(std::string sql, std::string data) -> task<Reply<resultset>>`: Convenience overload that sends the entire `data` payload in one shot.
    - `query_stream(std::string sql, std::size_t batch_size, RowFn on_row) -> task<Reply<void>>`: Streams a large result via a server-side `CURSOR` (`DECLARE`/`FETCH`, `batch_size` rows per round trip), invoking `on_row` per row in **constant memory**. Auto-manages a transaction when idle (`BEGIN`/`COMMIT`, `ROLLBACK` on failure); when `in_transaction()` it declares the cursor in the caller's transaction and touches only the cursor. Cursor names are unique per connection (`qb_stream_cursor_<n>`), so streams **may overlap** on one `database` — and because a session has a single transaction, overlapping streams **share** the self-opened block: the first opens it, later ones only join, the last one out `COMMIT`s (or `ROLLBACK`s if any participant failed), so a server error in one stream aborts the block for the others. A caller-opened transaction is recognised as caller-owned only once its `BEGIN` has **completed** (`in_transaction()` mirrors the last `ReadyForQuery`), so do not start a stream while your own `begin()` is still in flight. `on_row` exceptions are rethrown after the cursor is closed and any self-opened transaction rolled back. `batch_size` is clamped to ≥ 1.
- **Connection introspection / control:**
    - `bool cancel()`: Sends an out-of-band PostgreSQL `CancelRequest` on a short-lived **separate** connection (libpq `PQcancel` style). It is **synchronous / blocking** (capped at ≤ 2s) and plaintext even when the main link is SSL. Surfaces on the in-flight query as `sqlstate::query_canceled` (57014). Note: it briefly blocks the calling thread — unlike the rest of the client, do not expect it to be non-blocking.
    - `bool in_transaction() const noexcept`: True when the backend session is in a transaction block (`'T'`) or a failed transaction block (`'E'`); false when idle (`'I'`).
    - `bool used_channel_binding() const noexcept`: True when **SCRAM-SHA-256-PLUS** with `tls-server-end-point` channel binding was negotiated (only over TLS, when the server offers the `-PLUS` mechanism).
    - `std::optional<std::string_view> parameter_status(std::string_view key) const`: Value of a server `ParameterStatus` report (libpq `PQparameterStatus`); e.g. `"server_version"`, `"server_encoding"`, `"TimeZone"`. `std::nullopt` if the key was never reported.
    - `int server_version() const`: Server version as a libpq-style integer (`PQserverVersion`), e.g. `160002` for 16.2. Returns 0 if the `server_version` parameter is unknown.
    - `int backend_pid() const noexcept`: Server-side backend PID captured at connect (`BackendKeyData`); libpq `PQbackendPID`. Returns 0 if not connected.
- **Transaction API:**
    - `begin(...)`: Starts a new transaction. Takes success/error callbacks and optional `transaction_mode`.
    - `execute(...)`: Executes a simple SQL query or a prepared statement. Callbacks handle results or errors.
    - `prepare(...)`: Prepares a named SQL statement. Callbacks for success/failure.
    - `execute_file(...)`: Executes SQL commands from a file.
    - `prepare_file(...)`: Prepares SQL commands from a file.
    - `savepoint(...)`: Creates a savepoint within a transaction.
    - `then(...)`: Chains an operation to be executed if the previous one succeeds.
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
  ```cpp
  class PreparedStorage {      // queries.h:65 — `PreparedQueryStorage` is the ALIAS (queries.h:174)
  public:
      bool has(std::string_view name) const;
      const PreparedQuery& push(PreparedQuery &&query);
      PreparedQuery const& get(std::string_view name) const;
  };
  ```

## Result Set Processing

### `qb::pg::results` (alias for `qb::pg::detail::resultset`)

Represents the set of rows returned by a query. Provides a container-like interface to access rows. There is no public `qb::pg::resultset`: the class lives in `namespace qb::pg::detail` and the only public spelling is the alias `using results = detail::resultset;` (`qbm/pgsql/src/qbm/pgsql/pgsql.h:2677`). Row and field below are reachable as `qb::pg::results::row` / `qb::pg::results::field`.

**Definition (`qbm/pgsql/src/qbm/pgsql/resultset.h`):**
```cpp
namespace qb::pg {
// Public alias (pgsql.h:2677); the class itself is qb::pg::detail::resultset.
using results = detail::resultset;
}

namespace qb::pg::detail {
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

    qb::json json() const; // Convert resultset to JSON array of objects

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
} // namespace qb::pg::detail
```

### `qb::pg::results::row`

Represents a single row within a `results` set (`qb::pg::detail::resultset::row`). Provides access to individual fields.

**Definition (`qbm/pgsql/src/qbm/pgsql/resultset.h`):**
```cpp
namespace qb::pg::detail {
class resultset { // public alias: qb::pg::results
public:
    class row {
    public:
        // STL-like container typedefs for fields (const_iterator, value_type=field, etc.)
        size_type row_index() const;

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

        size_type index_of_name(std::string const &name) const;
    };
};
} // namespace qb::pg::detail
```

### `qb::pg::results::field`

Represents a single field (column value) within a `results::row` (`qb::pg::detail::resultset::field`).

**Definition (`qbm/pgsql/src/qbm/pgsql/resultset.h`):**
```cpp
namespace qb::pg::detail {
class resultset { // public alias: qb::pg::results
public:
    class field {
    public:
        size_type row_index() const;
        row::size_type field_index() const;

        std::string const& name() const;
        field_description const& description() const;
        bool is_null() const;
        bool empty() const; // (Note: empty() might be misleading, is_null() is primary for null checks)

        // Value extraction:
        template <typename T>
        typename std::decay<T>::type as() const; // Throws on NULL if T is not optional
        template <typename T>
        bool to(T &val) const; // For non-optional T, THROWS error::value_is_null on NULL. For std::optional<T>/nullable T, sets the target to null and returns true. The bool return reports parse success, not NULL-ness — use as<std::optional<T>>() or is_null() to detect NULL.

        field_buffer input_buffer() const; // Raw data buffer
    };
};
} // namespace qb::pg::detail
```
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
    static void write_integer(std::vector<byte>& buffer, integer value); // public — TypeConverter<T>'s only access specifier is `public:` (type_converter.h:131, :685)
};
// Specializations: qb::uuid, qb::wall_time (timestamptz/timestamp), qb::json, qb::jsonb,
// std::chrono::duration (type_converter.h:1018), std::string (:1130), numeric (:1219),
// qb::date (:1257), qb::time_of_day (:1279), qb::time_of_day_tz (:1301),
// qb::calendar_interval (:1321), std::vector<std::byte> (:1346)
// (the retired qb::Timestamp / qb::UtcTimestamp / qb::LocalTimestamp are NOT specialized).
} // namespace qb::pg::detail
```
Internally uses `qb::pg::detail::ParamSerializer` for serialization logic (see `param_serializer.h`) and `qb::pg::detail::ParamUnserializer` for deserialization (see `param_unserializer.h` and `field_reader.h`).

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

    std::string    severity; // ERROR, FATAL, WARNING, etc.
    std::string    code;     // PostgreSQL error code string (e.g., "42P01")
    std::string    detail;
    sqlstate::code sqlstate; // Enum representation of the SQLSTATE
};
} // namespace qb::pg::error
```

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
-   **`qb::pg::detail::notice_message`**: Structure to hold fields from PostgreSQL NOTICE or ERROR messages.
-   **`qb::pg::detail::command_complete`**: Structure holding the command tag from a CommandComplete message.

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

This API reference provides a comprehensive guide to the `qb::pg` module for AI-assisted development. 