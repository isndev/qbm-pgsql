# QB PostgreSQL Module (`qbm-pgsql`)

**High-Performance, Asynchronous PostgreSQL Client for the QB Actor Framework**

<p align="center">
  <img src="https://img.shields.io/badge/PostgreSQL-13%2B-blue.svg" alt="PostgreSQL"/>
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue.svg" alt="C++17"/>
  <img src="https://img.shields.io/badge/Cross--Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg" alt="Cross Platform"/>
  <img src="https://img.shields.io/badge/Arch-x86__64%20%7C%20ARM64-lightgrey.svg" alt="Architecture"/>
  <img src="https://img.shields.io/badge/SSL-TLS-green.svg" alt="SSL/TLS"/>
  <img src="https://img.shields.io/badge/License-Apache%202.0-green.svg" alt="License"/>
</p>

This module provides a robust, asynchronous PostgreSQL client seamlessly integrated with the QB Actor Framework's event-driven architecture. Built on QB's non-blocking I/O foundation, it enables database operations without blocking your actors, delivering exceptional performance in high-concurrency scenarios.

## Quick Integration with QB

### Adding to Your QB Project

```bash
# Add the module as a submodule
git submodule add https://github.com/isndev/qbm-pgsql qbm/pgsql
```

### CMake Setup

```cmake
# QB framework setup
add_subdirectory(qb)
include_directories(${QB_PATH}/include)

# Load QB modules (automatically discovers qbm-pgsql)
qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")

# Link against the PostgreSQL module
target_link_libraries(your_target PRIVATE qbm::pgsql)
```

### Include and Use

```cpp
#include <pgsql/pgsql.h>
```

## Why Choose `qbm-pgsql`?

**True Asynchronous Operations**: Every database operation is non-blocking, integrating perfectly with QB's event loop for maximum concurrency.

**Type Safety**: Automatic type conversion between C++ and PostgreSQL types with compile-time safety. No more manual string building or casting.

**Transaction Control**: Fluent API for transactions, savepoints, and complex multi-statement operations with clean error handling.

**Performance Focused**: Prepared statements, connection pooling, and zero-copy operations where possible.

**Cross-Platform**: Same code runs on Linux, macOS, Windows (x86_64, ARM64) with identical performance.

## Quick Start: Your First Database Actor

```cpp
#include <pgsql/pgsql.h>
#include <qb/main.h>

class DatabaseActor : public qb::Actor {
    qb::pg::tcp::database _db;
    
public:
    DatabaseActor() : _db("tcp://user:password@localhost:5432[mydb]") {}
    
    bool onInit() override {
        if (!_db.connect()) {
            qb::io::cout() << "Failed to connect: " << _db.error().what() << std::endl;
            return false;
        }
        
        // Simple query with callback
        _db.execute("SELECT COUNT(*) as user_count FROM users", {},
            [this](qb::pg::transaction& tr, qb::pg::results result) {
                int count = result[0]["user_count"].as<int>();
                qb::io::cout() << "Total users: " << count << std::endl;
                kill(); // Done
            },
            [this](const qb::pg::error::db_error& err) {
                qb::io::cout() << "Query failed: " << err.what() << std::endl;
                kill();
            }
        );
        
        return true;
    }
};

int main() {
    qb::Main engine;
    engine.addActor<DatabaseActor>(0);
    engine.start();
    return 0;
}
```

That's it! No connection management complexity, no thread blocking, just clean async database access.

## Real-World Examples

### User Management System

```cpp
#include <pgsql/pgsql.h>
#include <qb/main.h>
#include <qb/json.h>

class UserService : public qb::Actor {
    qb::pg::tcp::database _db;
    
public:
    UserService() : _db("tcp://app_user:password@localhost:5432[app_db]") {}
    
    bool onInit() override {
        if (!_db.connect()) {
            qb::io::cout() << "Database connection failed" << std::endl;
            return false;
        }
        
        // Prepare frequently used statements
        auto prepare_status = _db.prepare(
            "find_user_by_email", 
            "SELECT id, name, email, created_at FROM users WHERE email = $1",
            {qb::pg::oid::text}
        ).await();
        
        if (!prepare_status) {
            qb::io::cout() << "Failed to prepare statement: " << prepare_status.error().what() << std::endl;
            return false;
        }
        
        // Demo: Find a user
        find_user("user@example.com");
        
        return true;
    }
    
private:
    void find_user(const std::string& email) {
        _db.execute("find_user_by_email", {email},
            [this](qb::pg::transaction& tr, qb::pg::results result) {
                if (result.empty()) {
                    qb::io::cout() << "User not found" << std::endl;
                    create_user("John Doe", "john@example.com");
                } else {
                    auto& row = result[0];
                    qb::io::cout() << "Found user: " << row["name"].as<std::string>() 
                                   << " (ID: " << row["id"].as<int>() << ")" << std::endl;
                    
                    // Demo: Update user
                    update_user_login(row["id"].as<int>());
                }
            },
            [this](const qb::pg::error::db_error& err) {
                qb::io::cout() << "Find user failed: " << err.what() << std::endl;
                kill();
            }
        );
    }
    
    void create_user(const std::string& name, const std::string& email) {
        _db.execute(
            "INSERT INTO users (name, email, created_at) VALUES ($1, $2, NOW()) RETURNING id",
            {name, email},
            [this, name](qb::pg::transaction& tr, qb::pg::results result) {
                int user_id = result[0]["id"].as<int>();
                qb::io::cout() << "Created user '" << name << "' with ID: " << user_id << std::endl;
                
                // Demo: Complex transaction
                perform_user_setup(user_id);
            },
            [this](const qb::pg::error::db_error& err) {
                qb::io::cout() << "Create user failed: " << err.what() << std::endl;
                kill();
            }
        );
    }
    
    void update_user_login(int user_id) {
        _db.execute(
            "UPDATE users SET last_login = NOW() WHERE id = $1", 
            {user_id},
            [this](qb::pg::transaction& tr, qb::pg::results result) {
                qb::io::cout() << "Updated last login for user " << std::endl;
                kill(); // Example complete
            },
            [this](const qb::pg::error::db_error& err) {
                qb::io::cout() << "Update failed: " << err.what() << std::endl;
                kill();
            }
        );
    }
    
    void perform_user_setup(int user_id) {
        // Transaction with multiple operations
        _db.begin(
            [this, user_id](qb::pg::transaction& tr) {
                qb::io::cout() << "Starting user setup transaction..." << std::endl;
                
                // First operation
                tr.execute("INSERT INTO user_profiles (user_id, status) VALUES ($1, 'active')", {user_id})
                .then([user_id](qb::pg::transaction& tr2) {
                    // Second operation
                    return tr2.execute("INSERT INTO user_settings (user_id, theme) VALUES ($1, 'default')", {user_id});
                })
                .success([this](qb::pg::transaction& tr_final) {
                    qb::io::cout() << "User setup completed successfully!" << std::endl;
                    kill();
                })
                .error([this](const qb::pg::error::db_error& err) {
                    qb::io::cout() << "User setup failed: " << err.what() << std::endl;
                    kill();
                });
            },
            [this](const qb::pg::error::db_error& err) {
                qb::io::cout() << "Failed to begin transaction: " << err.what() << std::endl;
                kill();
            }
        );
    }
};

int main() {
    qb::Main engine;
    engine.addActor<UserService>(0);
    engine.start();
    return 0;
}
```

### Data Analytics with Prepared Statements

```cpp
#include <pgsql/pgsql.h>
#include <qb/main.h>

class AnalyticsActor : public qb::Actor {
    qb::pg::tcp::database _db;
    
public:
    AnalyticsActor() : _db("tcp://analytics:password@localhost:5432[analytics_db]") {}
    
    bool onInit() override {
        if (!_db.connect()) {
            return false;
        }
        
        // Prepare analytics queries
        prepare_statements();
        return true;
    }
    
private:
    void prepare_statements() {
        _db.prepare("daily_stats", 
            "SELECT date, COUNT(*) as events, AVG(value) as avg_value "
            "FROM events WHERE date >= $1 AND date <= $2 GROUP BY date ORDER BY date",
            {qb::pg::oid::date, qb::pg::oid::date})
        .then([this](qb::pg::transaction& tr) {
            return _db.prepare("user_activity",
                "SELECT user_id, COUNT(*) as activity_count "
                "FROM user_events WHERE created_at >= $1 GROUP BY user_id "
                "HAVING COUNT(*) > $2 ORDER BY activity_count DESC LIMIT $3",
                {qb::pg::oid::timestamptz, qb::pg::oid::int4, qb::pg::oid::int4});
        })
        .success([this](qb::pg::transaction& tr) {
            qb::io::cout() << "All statements prepared, running analytics..." << std::endl;
            run_daily_report();
        })
        .error([this](const qb::pg::error::db_error& err) {
            qb::io::cout() << "Failed to prepare statements: " << err.what() << std::endl;
            kill();
        });
    }
    
    void run_daily_report() {
        // Type-safe date parameters
        auto today = qb::time::now().to_date();
        auto week_ago = today - qb::Duration::days(7);
        
        _db.execute("daily_stats", {week_ago, today},
            [this](qb::pg::transaction& tr, qb::pg::results result) {
                qb::io::cout() << "=== Daily Statistics ===" << std::endl;
                for (const auto& row : result) {
                    auto date = row["date"].as<qb::Date>();
                    int events = row["events"].as<int>();
                    auto avg_val = row["avg_value"].as<std::optional<double>>();
                    
                    qb::io::cout() << date.to_string() << ": " << events << " events";
                    if (avg_val) {
                        qb::io::cout() << ", avg value: " << *avg_val;
                    }
                    qb::io::cout() << std::endl;
                }
                
                run_activity_report();
            },
            [this](const qb::pg::error::db_error& err) {
                qb::io::cout() << "Daily stats failed: " << err.what() << std::endl;
                kill();
            }
        );
    }
    
    void run_activity_report() {
        auto cutoff_time = qb::time::now() - qb::Duration::hours(24);
        
        _db.execute("user_activity", {cutoff_time, 10, 20}, // 10+ activities, top 20 users
            [this](qb::pg::transaction& tr, qb::pg::results result) {
                qb::io::cout() << "=== Top Active Users ===" << std::endl;
                for (const auto& row : result) {
                    int user_id = row["user_id"].as<int>();
                    int activity = row["activity_count"].as<int>();
                    qb::io::cout() << "User " << user_id << ": " << activity << " activities" << std::endl;
                }
                kill(); // Analysis complete
            },
            [this](const qb::pg::error::db_error& err) {
                qb::io::cout() << "Activity report failed: " << err.what() << std::endl;
                kill();
            }
        );
    }
};

int main() {
    qb::Main engine;
    engine.addActor<AnalyticsActor>(0);
    engine.start();
    return 0;
}
```

### Synchronous Usage (for Scripts)

```cpp
#include <pgsql/pgsql.h>

int main() {
    qb::io::async::init(); // Required for sync usage
    
    qb::pg::tcp::database db("tcp://user:password@localhost:5432[testdb]");
    
    if (!db.connect()) {
        std::cerr << "Connection failed: " << db.error().what() << std::endl;
        return 1;
    }
    
    // Synchronous query with await()
    auto result = db.execute("SELECT version() as pg_version").await();
    
    if (result) {
        auto version = result.results()[0]["pg_version"].as<std::string>();
        std::cout << "PostgreSQL version: " << version << std::endl;
    } else {
        std::cerr << "Query failed: " << result.error().what() << std::endl;
        return 1;
    }
    
    // Synchronous transaction
    auto tx_result = db.begin([](qb::pg::transaction& tr) {
        tr.execute("INSERT INTO test_table (name) VALUES ($1)", {"test_value"});
    }).await();
    
    if (tx_result) {
        std::cout << "Transaction completed successfully" << std::endl;
    } else {
        std::cerr << "Transaction failed: " << tx_result.error().what() << std::endl;
    }
    
    return 0;
}
```

## Features

**Fully Asynchronous**: All operations integrate with QB's event loop for maximum concurrency.

**Type-Safe**: Automatic conversion between C++ and PostgreSQL types:
- Standard types: `int`, `long long`, `float`, `double`, `bool`, `std::string`
- Special types: `qb::uuid`, `qb::Timestamp`, `qb::json`, `qb::jsonb`
- Nullable types: `std::optional<T>` for NULL-safe operations
- Arrays: `std::vector<T>` for PostgreSQL array types

**Transaction Control**: Fluent API with chaining, savepoints, and nested transactions.

**Prepared Statements**: Compile once, execute many times with optimal performance.

**Connection Management**: Automatic reconnection, SSL support, and connection pooling.

**Error Handling**: Detailed PostgreSQL error codes and SQLSTATE information.

## Build Information

### Requirements
- **QB Framework**: This module requires the QB Actor Framework as its foundation
- **C++17** compatible compiler
- **CMake 3.14+**
- **OpenSSL Development Libraries**: Required for SSL connections and SCRAM authentication

### Building with QB
When using the QB project template, simply add this module as shown in the integration section above. The `qb_load_modules()` function will automatically handle the configuration.

### Manual Build (Advanced)
```cmake
# If building outside QB framework context
find_package(qb REQUIRED)
target_link_libraries(your_target PRIVATE qbm-pgsql)
```

## Advanced Documentation

For in-depth technical documentation, implementation details, and comprehensive API reference:

**📖 [Complete PostgreSQL Module Documentation](./readme/README.md)**

This detailed documentation covers:
- **Connection Management & Configuration** - Advanced connection options, SSL setup, and performance tuning
- **Query Execution Patterns** - Synchronous vs asynchronous patterns, error handling strategies
- **Transaction Handling** - Nested transactions, savepoints, and rollback strategies  
- **Prepared Statements** - Statement lifecycle, parameter binding, and performance optimization
- **Type System Integration** - Complete mapping between C++ and PostgreSQL types
- **Error Handling & Diagnostics** - SQLSTATE codes, error recovery, and debugging techniques
- **Performance Optimization** - Connection pooling, batch operations, and profiling
- **Security Considerations** - SSL/TLS configuration, authentication methods, and best practices

## Documentation & Examples

For detailed usage patterns and comprehensive examples:

- **[QB Examples Repository](https://github.com/isndev/qb-examples):** Real-world PostgreSQL integration patterns
- **[Full Module Documentation](./readme/README.md):** Complete API reference and guides

**Example Categories:**
- Connection management and configuration
- Query execution patterns
- Transaction handling and savepoints
- Prepared statements and type safety
- Error handling strategies
- Performance optimization

## Contributing

We welcome contributions! Please see the main [QB Contributing Guidelines](https://github.com/isndev/qb/blob/master/CONTRIBUTING.md) for details.

## License

This module is licensed under the Apache License 2.0. See the [LICENSE](./LICENSE) file for details.

---

**Part of the [QB Actor Framework](https://github.com/isndev/qb) ecosystem - Build the future of concurrent C++ applications.**