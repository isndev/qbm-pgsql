# `qbm-pgsql`: Connection Management

This document details how to establish and manage connections to a PostgreSQL database using the `qbm-pgsql` module.

## Core Class: `qb::pg::tcp::database`

*(Defined in `qbm/pgsql/pgsql.h`, implemented in `qbm/pgsql/pgsql.cpp`)*

This is the main class representing a connection to a PostgreSQL server over TCP/IP. For SSL/TLS connections, use `qb::pg::tcp::ssl::database`.

It inherits from:
*   `qb::io::async::tcp::client`: Provides the asynchronous TCP client capabilities.
*   `qb::pg::detail::Transaction`: Provides the base for the fluent transaction API (although top-level `db.execute()` etc. are usually used outside an explicit `db.begin()` block).

### Establishing a Connection

Connections are typically established using a connection string or a `connection_options` struct.

**1. Using Connection String:**

The easiest way is to provide a connection string to the constructor or the `connect()` method.

```cpp
#include <pgsql/pgsql.h>

// Via constructor
qb::pg::tcp::database db1("tcp://user:pwd@host:port[database]");
if (!db1.connect()) {
    // Handle error: db1.error() contains details
}

// Via connect method
qb::pg::tcp::database db2;
if (!db2.connect("tcp://test:test@localhost:5432[test]")) {
    // Handle error
}
```

**Connection String Format:**

`[alias=]schema://[user[:password]@]host[:port][database]`

*   `alias` (Optional): A short name for the connection (e.g., `mydb=`).
*   `schema`: `tcp` (for plain TCP) or `ssl` (for SSL/TLS - requires `qb::pg::tcp::ssl::database`).
*   `user`, `password` (Optional): Credentials for authentication.
*   `host`: Hostname or IP address of the PostgreSQL server.
*   `port` (Optional): Port number (defaults to 5432).
*   `database`: The name of the database to connect to.

*(Parsing logic is in `src/common.cpp` using `connect_string_parser`)*

**2. Using `connection_options` Struct:**

*(Defined in `src/common.h`)*

This provides more structured control over connection parameters.

```cpp
qb::pg::connection_options opts;
opts.schema = "tcp";
opts.uri = "localhost:5432"; // Combines host and port
opts.database = "test";
opts.user = "test";
opts.password = "test";
// opts.alias = "my_db"; // Optional

qb::pg::tcp::database db;
if (!db.connect(opts)) { // Pass the struct to connect()
    // Handle error
}
```

### Connection Lifecycle

*   **`connect()`:** Initiates the asynchronous connection process. This involves resolving the hostname, establishing the TCP connection, and performing the PostgreSQL startup and authentication handshake.
*   **Asynchronous Nature:** The `connect()` call returns quickly. The actual connection status is determined later via the QB event loop. You typically need `qb::io::async::run()` or `db.await()` (for synchronous waits) after calling `connect()`.
*   **`disconnect()`:** Closes the connection gracefully.
*   **Destructor:** The `database` destructor automatically calls `disconnect()`.

### Authentication

*(Logic primarily in `Database::on_authentication` in `pgsql.h`)*

The client handles the authentication flow dictated by the server:

*   **Startup Message:** Client sends protocol version, user, database, and client options.
*   **Server Response (`Authentication*` messages):** The server responds with the required authentication method.
*   **Supported Methods:**
    *   `OK` (0): Authentication successful (rarely the first step).
    *   `Cleartext` (3): Client sends the password in plain text.
    *   `MD5Password` (5): Client receives a salt, computes `md5(md5(password + user) + salt)`, and sends the result.
    *   `SCRAM-SHA-256` (10, 11, 12): Client engages in the SCRAM challenge-response mechanism, involving nonce exchange, salting, hashing (PBKDF2), and signature verification. Requires OpenSSL.
*   **Error Handling:** Invalid credentials or unsupported methods result in a connection error.

### Backend Parameters

*(Handled in `Database::on_parameter_status` in `pgsql.h`)*

After successful authentication, the server sends various runtime parameters (like `client_encoding`, `server_version`, `TimeZone`, etc.). The client stores these in its internal `client_opts_` map, though direct access isn't typically needed by user code.

### Connection Errors (`qb::pg::error::connection_error`)

*(Defined in `src/error.h`, implemented in `src/error.cpp`)*

If `connect()` fails, it returns `false`, and `db.error()` will contain a `connection_error` (or a derived type). Common causes include:

*   Network issues (host unreachable, port closed).
*   Invalid hostname or address.
*   Authentication failure (wrong user/password).
*   Server configuration issues (e.g., `pg_hba.conf` restrictions).
*   SSL handshake failure.

### SSL/TLS Connections

To use SSL/TLS:

1.  Ensure QB was built with OpenSSL (`QB_IO_WITH_SSL=ON`).
2.  Use the `qb::pg::tcp::ssl::database` class instead of `qb::pg::tcp::database`.
3.  Use the `ssl://` schema in the connection string or set `opts.schema = "ssl";`.
4.  The underlying `qb::io::transport::stcp` handles the SSL handshake during connection. 