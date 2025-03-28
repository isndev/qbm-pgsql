# QB PostgreSQL Module (qbm-pgsql)

The PostgreSQL module for the QB C++ Actor Framework provides a high-performance, asynchronous client implementation for PostgreSQL databases. It offers a comprehensive API for SQL operations with transaction support, prepared statements, and error handling.

## Features

- **Asynchronous API**: Non-blocking database operations integrated with QB's event loop.
- **Transaction Support**: Full ACID transaction management with commit/rollback.
- **Prepared Statements**: Support for server-side prepared statements to improve performance and security.
- **Connection Pooling**: Efficient connection management.
- **Authentication Methods**: Support for multiple authentication methods including MD5 and SCRAM-SHA-256.
- **Type Safety**: Strong type checking for query parameters and results.
- **Error Handling**: Comprehensive error reporting and handling.
- **Savepoints**: Support for nested transactions using savepoints.
- **Fluent Interface**: Chain-style API for building complex queries.
- **Result Set Processing**: Structured access to query results.
- **SSL/TLS Support**: Secure database connections when built with SSL support.

## Requirements

- QB C++ Actor Framework
- C++17 compatible compiler
- CMake 3.14 or newer
- Boost libraries
- OpenSSL (for secure connections and SCRAM authentication)

## Integration

Add the module to your QB project:

```cmake
# In your CMakeLists.txt
qb_use_module(pgsql)
```

## Connection Setup

### Basic Connection

```cpp
#include <qb/pgsql.h>

// Create a database connection
qb::pg::tcp::database db("postgresql://username:password@localhost:5432/mydb");

// Connect to the database
if (!db.connect()) {
    std::cerr << "Failed to connect to database" << std::endl;
    return 1;
}

// Run the event loop
qb::io::async::run();
```

### Connection with SSL/TLS

```cpp
#include <qb/pgsql.h>

// Create a secure database connection
qb::pg::tcp::ssl::database db("postgresql://username:password@localhost:5432/mydb?sslmode=require");

// Connect to the database
if (!db.connect()) {
    std::cerr << "Failed to connect to database" << std::endl;
    return 1;
}

// Run the event loop
qb::io::async::run();
```

## Basic Usage

### Simple Query Execution

```cpp
// Execute a simple query
db.execute("CREATE TABLE users (id SERIAL PRIMARY KEY, username VARCHAR(50) UNIQUE NOT NULL, email VARCHAR(100))",
    []() {
        std::cout << "Table created successfully" << std::endl;
    },
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Error creating table: " << err.message << std::endl;
    }
);
```

### Query with Results

```cpp
// Query with result processing
db.execute("SELECT * FROM users",
    [](auto& tr, qb::pg::results results) {
        // Process results
        for (const auto& row : results) {
            std::cout << "User ID: " << row["id"].as<int>() << std::endl;
            std::cout << "Username: " << row["username"].as<std::string>() << std::endl;
            std::cout << "Email: " << row["email"].as<std::string>() << std::endl;
        }
    },
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Query error: " << err.message << std::endl;
    }
);
```

### Parameterized Queries

```cpp
// Prepare a statement
db.prepare("insert_user", 
    "INSERT INTO users (username, email) VALUES ($1, $2) RETURNING id",
    {qb::pg::text_oid, qb::pg::text_oid}
);

// Execute the prepared statement with parameters
qb::pg::params params;
params.add("john_doe");
params.add("john@example.com");

db.execute("insert_user", std::move(params),
    [](auto& tr, qb::pg::results results) {
        if (results.rows_count() > 0) {
            int user_id = results[0]["id"].as<int>();
            std::cout << "Inserted user with ID: " << user_id << std::endl;
        }
    },
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Insert error: " << err.message << std::endl;
    }
);
```

## Transaction Management

### Basic Transaction

```cpp
// Begin a transaction
db.begin(
    [](qb::pg::transaction& tr) {
        // Execute queries inside the transaction
        tr.execute("INSERT INTO users (username, email) VALUES ('user1', 'user1@example.com')")
          .execute("INSERT INTO users (username, email) VALUES ('user2', 'user2@example.com')");
    },
    [](const qb::pg::error::db_error& err) {
        // Transaction failed
        std::cerr << "Transaction error: " << err.message << std::endl;
    }
);
```

### Transaction with Savepoints

```cpp
// Transaction with savepoints for partial rollback
db.begin(
    [](qb::pg::transaction& tr) {
        tr.execute("INSERT INTO users (username, email) VALUES ('user1', 'user1@example.com')");
        
        // Create a savepoint
        tr.savepoint("user_2",
            [](qb::pg::transaction& tr) {
                tr.execute("INSERT INTO users (username, email) VALUES ('user2', 'user2@example.com')");
                
                // This will cause the savepoint to be rolled back, but the outer transaction continues
                throw std::runtime_error("Force savepoint rollback");
            }
        );
        
        // This will still be executed
        tr.execute("INSERT INTO users (username, email) VALUES ('user3', 'user3@example.com')");
    },
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Transaction error: " << err.message << std::endl;
    }
);
```

## Advanced Features

### Handling Query Results

```cpp
db.execute("SELECT username, email FROM users WHERE id > $1", 
    qb::pg::params().add(100),
    [](auto& tr, qb::pg::results results) {
        std::cout << "Found " << results.rows_count() << " users" << std::endl;
        
        // Column information
        for (const auto& col : results.columns()) {
            std::cout << "Column: " << col.name << " (OID: " << col.type_oid << ")" << std::endl;
        }
        
        // Row access by index
        for (size_t i = 0; i < results.rows_count(); ++i) {
            const auto& row = results[i];
            std::cout << "Row " << i << ": " << row["username"].as<std::string>() << std::endl;
        }
        
        // Row iterator
        for (const auto& row : results) {
            std::cout << "Username: " << row["username"].as<std::string>() << std::endl;
            std::cout << "Email: " << row["email"].as<std::string>() << std::endl;
        }
    }
);
```

### Transaction Chaining

```cpp
db.begin(
    [](qb::pg::transaction& tr) {
        tr.execute("INSERT INTO logs (message) VALUES ('Transaction started')")
          .then([](qb::pg::transaction& tr) {
              // This is executed only if the previous query succeeds
              tr.execute("UPDATE stats SET transaction_count = transaction_count + 1");
          })
          .success([](qb::pg::transaction& tr) {
              // This is executed only if all previous operations succeed
              std::cout << "Transaction successful" << std::endl;
          })
          .error([](const qb::pg::error::db_error& err) {
              // This is executed if any operation fails
              std::cerr << "Transaction operation failed: " << err.message << std::endl;
          });
    },
    [](const qb::pg::error::db_error& err) {
        // This is executed if the transaction itself fails
        std::cerr << "Transaction failed: " << err.message << std::endl;
    }
);
```

## Error Handling

The module provides detailed error information through the `qb::pg::error::db_error` class:

```cpp
db.execute("SELECT * FROM non_existent_table",
    [](auto& tr, qb::pg::results results) {
        // Handle successful query
    },
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Error message: " << err.message << std::endl;
        std::cerr << "Error severity: " << err.severity << std::endl;
        std::cerr << "SQL state: " << err.sqlstate << std::endl;
        std::cerr << "Detail: " << err.detail << std::endl;
    }
);
```

## Real-world Example

Here's an example of initializing a database with tables and prepared statements, as seen in the qb-auth project:

```cpp
bool
Database::init() {
    if (!connect())
        return false;
    
    bool status[2] = {true, true};
    begin(
        [](qb::pg::transaction &tr) {
            tr
                // Prepare statements for user management
                .prepare(
                    "q_user_add",
                    "INSERT INTO users VALUES($1, $2, $3, $4, $5) "
                    "ON CONFLICT (username, app_id) "
                    "DO UPDATE SET password=$4, claims=$5, deleted='f', updated_at=now() "
                    "RETURNING id"
                )
                .prepare(
                    "q_user_get_by_id",
                    "SELECT id, app_id, username, claims "
                    "FROM users "
                    "WHERE id=$1 AND NOT deleted"
                )
                // More prepared statements...
        },
        [&status](const qb::pg::error::db_error &err) {
            LOG_CRIT(err.detail);
            status[0] = status[1] = false;
        })
        .then([&status](auto &) {
            status[0] = false;
        });

    try {
        qb::io::async::run_until(status[0]);
    } catch (std::exception &e) {
        LOG_CRIT("Database initialization error: " << e.what());
        status[1] = false;
    }
    
    return status[1];
}
```

## Architecture

The PostgreSQL module is built around these key components:

- **Connection Management**: Handles the establishment and maintenance of database connections.
- **Protocol Implementation**: Implements the PostgreSQL wire protocol for communication with the server.
- **Transaction System**: Manages transactions, savepoints, and query execution.
- **Result Processing**: Converts raw PostgreSQL data into usable C++ types.
- **Error Handling**: Provides comprehensive error reporting and recovery mechanisms.

The module integrates with QB's asynchronous I/O system, allowing database operations to be non-blocking and event-driven.

## License

Licensed under the Apache License, Version 2.0. See LICENSE file for details.