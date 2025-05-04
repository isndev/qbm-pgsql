# QB PostgreSQL Module (`qbm-pgsql`)

**High-Performance, Asynchronous PostgreSQL Client for the QB C++ Actor Framework**

This module provides a robust, asynchronous client for interacting with PostgreSQL databases, seamlessly integrated with the QB Actor Framework's event-driven architecture.

Leveraging the non-blocking I/O foundation of `qb-io`, `qbm-pgsql` allows your actors to perform database operations without stalling their event loop, enabling highly concurrent and responsive applications.

## Features

*   🚀 **Fully Asynchronous:** Executes all database operations non-blockingly, integrating perfectly with the QB event loop (`qb::io::async`).
*   ⛓️ **Fluent Transaction API:** Manage transactions (`BEGIN`, `COMMIT`, `ROLLBACK`), including nested transactions via `SAVEPOINT`s, using a clean, chainable interface.
*   🛡️ **Prepared Statements:** Prepare SQL statements once and execute them multiple times with different parameters for optimal performance and security against SQL injection.
*   ⚙️ **Type-Safe Parameter Binding:** Automatically maps C++ types (`int`, `std::string`, `double`, `bool`, `qb::uuid`, `qb::Timestamp`, `std::vector`, `std::optional`, JSON, etc.) to their corresponding PostgreSQL types using a modern type conversion system.
*   📄 **Rich Result Set Handling:** Access query results through a container-like interface (`qb::pg::results`) with iterators (`row`, `field`) and type-safe value extraction (`.as<T>()`, `.is_null()`).
*   🔒 **Secure Connections:** Supports SSL/TLS encrypted connections (`qb::pg::tcp::ssl::database`) when built with OpenSSL.
*   🔑 **Multiple Authentication Methods:** Handles Password (cleartext, MD5) and SCRAM-SHA-256 authentication.
*   ❗ **Comprehensive Error Handling:** Provides detailed error information (`qb::pg::error::db_error`, `query_error`) including PostgreSQL error codes and SQLSTATE values.
*   🔄 **Connection Management:** Handles connection establishment and protocol negotiation.
*   ⏳ **Synchronous `await()`:** Provides an optional blocking `await()` method on transactions for simpler sequential workflows or testing scenarios.

## Prerequisites

*   QB C++ Actor Framework (`qb-core`, `qb-io`)
*   C++17 Compiler
*   CMake (3.14+)
*   **OpenSSL Development Libraries:** Required for SSL connections and SCRAM authentication.

## Integration

To use this module in your CMake project after finding the main `qb` package:

```cmake
# Assuming find_package(qb REQUIRED) or add_subdirectory(path/to/qb) was done

# Link your target against the pgsql module
# The module target is typically namespaced, e.g., qb::qbm-pgsql
# You might need to adjust based on your qb_register_module setup

# Find the specific module package if QB was installed
find_package(qbm-pgsql CONFIG)

# Link (replace my_target and potentially qb::qbm-pgsql)
target_link_libraries(my_target PRIVATE qb::qbm-pgsql)
```

Ensure your main QB build includes OpenSSL (`QB_IO_WITH_SSL=ON` during CMake configuration).

## Basic Usage

*(See examples and tests for more advanced usage)*

### 1. Include Header

```cpp
#include <pgsql/pgsql.h>
```

### 2. Connection

```cpp
#include <qb/io/async.h> // For qb::io::async::run()

// Use the TCP database client (or tcp::ssl::database for SSL)
// The connection string format: "schema://user:password@host:port[database]"
qb::pg::tcp::database db("tcp://test:test@localhost:5432[test]");

if (!db.connect()) {
    std::cerr << "Connection failed: " << db.error().what() << std::endl;
    // Handle error
}

// Start the QB event loop if running outside an actor
// qb::io::async::run();
```

### 3. Simple Query (Asynchronous Callback)

```cpp
db.execute("SELECT id, name FROM users WHERE age > $1", {30},
    // Success Callback (receives results)
    [](qb::pg::transaction& tr, qb::pg::results result) {
        std::cout << "Found " << result.size() << " users older than 30:\n";
        for (const auto& row : result) {
            int id = row["id"].as<int>();
            std::string name = row["name"].as<std::string>();
            std::cout << "  ID: " << id << ", Name: " << name << "\n";
        }
    },
    // Error Callback
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Query failed: " << err.what() << " (SQLSTATE: " << err.code << ")\n";
    }
);

// Keep the event loop running for callbacks to execute
// qb::io::async::run();
```

### 4. Prepared Statement (Asynchronous Callback)

```cpp
// Prepare the statement once
db.prepare("get_user_by_id", "SELECT email FROM users WHERE id = $1", {qb::pg::oid::int4})
  .await(); // Use await for simplicity here, could be async

// Execute the prepared statement
db.execute("get_user_by_id", {42}, // Parameter {user_id = 42}
    [](qb::pg::transaction& tr, qb::pg::results result) {
        if (!result.empty()) {
            std::optional<std::string> email = result[0][0].as<std::optional<std::string>>();
            if (email) {
                std::cout << "Email for user 42: " << *email << "\n";
            } else {
                std::cout << "User 42 has no email.\n";
            }
        } else {
            std::cout << "User 42 not found.\n";
        }
    },
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Prepared statement execution failed: " << err.what() << "\n";
    }
);

// qb::io::async::run();
```

### 5. Transaction (Asynchronous Callbacks)

```cpp
db.begin(
    // Transaction Success Callback
    [](qb::pg::transaction& tr) {
        std::cout << "Transaction started.\n";
        tr.execute("INSERT INTO audit_log (message) VALUES ($1)", {"User logged in"})
          .then([](qb::pg::transaction& tr2) {
              // This executes only if the first execute succeeded
              return tr2.execute("UPDATE users SET last_login = NOW() WHERE id = $1", {123});
          })
          .success([](qb::pg::transaction& tr_final) {
              std::cout << "Transaction committed successfully!\n";
          })
          .error([](const qb::pg::error::db_error& err) {
              // Error callback for any step in the chain
              std::cerr << "Transaction step failed: " << err.what() << ", rolling back.\n";
          });
    },
    // Transaction Begin Error Callback
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Failed to begin transaction: " << err.what() << "\n";
    }
);

// Example with Savepoint
db.begin([](qb::pg::transaction& tr_outer) {
    tr_outer.execute("INSERT INTO operations (status) VALUES ('started')");

    // Create a savepoint
    tr_outer.savepoint("my_savepoint",
        // Savepoint Success Callback
        [](qb::pg::transaction& tr_inner) {
            std::cout << "Savepoint created. Performing operation...\n";
            // Perform operations within the savepoint
            tr_inner.execute("UPDATE products SET stock = stock - 1 WHERE id = $1", {10});
            // If something goes wrong here, only this savepoint rolls back
            // For example, trigger an error:
            // tr_inner.execute("SELECT * from non_existent_table_in_savepoint");
        },
        // Savepoint Error Callback (if savepoint creation fails)
        [](const qb::pg::error::db_error& err) {
            std::cerr << "Failed to create savepoint: " << err.what() << "\n";
        }
    )
    .then([](qb::pg::transaction& tr) {
        // This executes if the savepoint logic (or its rollback) completed
        std::cout << "Continuing outer transaction after savepoint block.\n";
        tr.execute("UPDATE operations SET status = 'post_savepoint' WHERE status = 'started'");
    })
    .error([](const qb::pg::error::db_error& err) {
        // This catches errors *within* the savepoint block or subsequent .then()
        std::cerr << "Error within savepoint or following steps: " << err.what() << "\n";
    });
});

// qb::io::async::run();
```

### 6. Synchronous Execution with `await()`

For simpler scripts, testing, or specific sequential workflows, use `.await()` after chaining operations.

```cpp
auto status = db.execute("SELECT version() AS pg_version").await();

if (status) { // Check if the operation succeeded (no errors)
    auto results = status.results(); // Get the result set
    if (!results.empty()) {
        std::cout << "PostgreSQL Version: " << results[0]["pg_version"].as<std::string>() << std::endl;
    }
} else {
    // Access error details
    const auto& err = status.error();
    std::cerr << "Await query failed: " << err.what()
              << " (SQLSTATE: " << err.code << ", Severity: " << err.severity << ")\n";
}

// Transaction with await
auto tx_status = db.begin([](qb::pg::transaction& tr) {
                        tr.execute("INSERT INTO logs (message) VALUES ($1)", {"Awaited Insert"});
                     })
                    .await();

if (tx_status) {
    std::cout << "Awaited transaction committed." << std::endl;
} else {
    std::cerr << "Awaited transaction failed: " << tx_status.error().what() << std::endl;
}
```

## Data Type Handling

`qbm-pgsql` automatically handles conversion between C++ types and PostgreSQL types during parameter binding and result fetching.

*   **Standard Types:** `int`, `short`, `long long`, `float`, `double`, `bool`, `std::string`, `const char*`, `std::string_view`.
*   **Binary Data:** `std::vector<qb::pg::byte>` or `qb::pg::bytea` map to `BYTEA`.
*   **UUID:** `qb::uuid` maps to `UUID`.
*   **Timestamps:** `qb::Timestamp` maps to `TIMESTAMP`, `qb::UtcTimestamp` maps to `TIMESTAMPTZ`.
*   **JSON:** `qb::json` maps to `JSON`, `qb::jsonb` maps to `JSONB`.
*   **NULL Values:** Use `std::optional<T>` in C++ to handle nullable PostgreSQL columns gracefully.
*   **Arrays:** `std::vector<T>` (where T is a supported type) maps to PostgreSQL arrays (e.g., `std::vector<int>` maps to `INTEGER[]`).

See `src/type_mapping.h` and `src/type_converter.h` for details.

## Further Information

*   **Examples:** Explore the `examples/` directory within the main QB repository for practical usage patterns.
*   **Tests:** The `qbm/pgsql/tests/` directory contains detailed unit and integration tests covering most features.
*   **Source Code:** Refer to the header files (`.h`) in `qbm/pgsql/src/` for the definitive API reference.

## License

This module is licensed under the Apache License 2.0. See the [LICENSE](./LICENSE) file.

## Dive Deeper 📚

For comprehensive technical details, explore the specific documentation:

➡️ **[Detailed Module Documentation](./readme/README.md)**

*   **[Connection Management](./readme/connection.md)**
*   **[Transaction Handling](./readme/transaction.md)**
*   **[Query Execution](./readme/queries.md)**
*   **[Result Set Processing](./readme/results.md)**
*   **[Data Type Handling](./readme/types.md)**
*   **[Error Handling](./readme/error_handling.md)**

## Contributing ❤️