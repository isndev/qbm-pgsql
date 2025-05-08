# `qbm-pgsql`: Query Execution

This document describes how to execute SQL queries, both simple and prepared, using the `qbm-pgsql` module.

## Executing Simple Queries: `db.execute()` / `tr.execute()`

Simple queries are executed directly without prior preparation. They are suitable for one-off queries or dynamic SQL, but generally less performant and less secure (against SQL injection if not careful with input) than prepared statements for repeated queries.

```cpp
// --- Outside a transaction (auto-commit for single statement) ---
db.execute(
    "SELECT name, email FROM users WHERE id = 1",
    // Success Callback (receives results)
    [](qb::pg::transaction& tr, qb::pg::results result) { /* Process result */ },
    // Error Callback
    [](const qb::pg::error::db_error& err) { /* Handle error */ }
);

// --- Inside a transaction ---
db.begin([](qb::pg::transaction& tr) {
    tr.execute("UPDATE products SET price = price * 1.1 WHERE category = 'electronics'");
    // ... other operations ...
});
```

*   **Parameters:** Simple queries **do not support direct parameter binding** in the same way as prepared statements. If you need to include variable data, you must manually and **safely** construct the SQL string (e.g., using proper escaping functions, though prepared statements are highly recommended for this).
*   **Return Value:** The `execute` method (and others like `prepare`, `then`) returns a reference to the `Transaction` object, allowing chaining.
*   **Result Handling:** If the query returns data (like `SELECT`), the success callback receives a `qb::pg::results` object. See [Result Set Processing](./results.md).

## Executing Queries from Files: `db.execute_file()` / `tr.execute_file()`

For complex queries or SQL scripts stored in separate files, the `execute_file` method provides a convenient way to load and execute SQL directly from a file.

```cpp
// Execute a query stored in a file
db.execute_file(
    std::filesystem::path("queries/complex_report.sql"),
    // Success Callback (receives results)
    [](qb::pg::transaction& tr, qb::pg::results result) { 
        // Process result from the file-based query
    },
    // Error Callback
    [](const qb::pg::error::db_error& err) { 
        std::cerr << "File execution error: " << err.what() << std::endl;
    }
);

// Simpler version with only success callback
db.execute_file(
    std::filesystem::path("queries/simple_update.sql"),
    [](qb::pg::transaction& tr, qb::pg::results result) {
        std::cout << "SQL file executed successfully" << std::endl;
    }
);

// Simplest version without callbacks
db.execute_file(std::filesystem::path("queries/maintenance.sql"));
```

*   **File Handling:** The method reads the entire file content and executes it as a single SQL statement.
*   **Error Handling:** If the file doesn't exist or can't be read, the error callback is invoked and an exception is thrown.
*   **Convenience:** This is particularly useful for large or complex queries, database initialization scripts, or migration files.

## Prepared Statements

Prepared statements offer significant advantages:

*   **Performance:** The query plan is cached by the server after the first preparation, leading to faster execution for subsequent calls.
*   **Security:** Parameters are sent separately from the SQL command, inherently preventing SQL injection vulnerabilities.
*   **Type Safety:** The library helps manage parameter types.

### 1. Preparing a Statement: `db.prepare()` / `tr.prepare()`

```cpp
db.prepare(
    "find_user_by_email", // Unique name for the prepared statement
    "SELECT id, name, age FROM users WHERE email = $1", // SQL with placeholder ($1)
    {qb::pg::oid::text}, // Optional: Specify parameter types (OIDs)
                         // If omitted, the server might infer types, or you
                         // might rely on implicit conversions.
                         // Providing OIDs is generally safer.
    // Success Callback (Optional)
    [](qb::pg::transaction& tr, const qb::pg::detail::PreparedQuery& pq) {
        std::cout << "Statement '" << pq.name << "' prepared successfully.\n";
    },
    // Error Callback (Optional)
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Failed to prepare statement: " << err.what() << "\n";
    }
);
```

*   **Naming:** Choose a unique name (`find_user_by_email`) for the statement within the current database session.
*   **Placeholders:** Use `$1`, `$2`, etc., for parameters in the SQL string.
*   **Type OIDs (`qb::pg::oid`):** Provide a `qb::pg::type_oid_sequence` (which is `std::vector<qb::pg::oid>`) to specify the expected PostgreSQL data type for each parameter. This helps the server optimize and validate. Common OIDs are defined in `src/pg_types.h` (e.g., `oid::int4`, `oid::text`, `oid::timestamp`, `oid::uuid`). 
    *   **Optional Parameter:** The OID array is optional - PostgreSQL can infer types from the context or the actual parameters provided during execution.
    *   **Type Control:** Explicitly providing OIDs allows you to force specific type conversions when the server's type inference might choose differently.
    *   **Best Practice:** For complex queries or when parameter types might be ambiguous, explicitly providing OIDs improves reliability across different PostgreSQL versions and configurations.
*   **Caching:** The `qbm-pgsql` client caches prepared statements internally (`PreparedQueryStorage` in `src/queries.h`). Preparing the same named statement again usually has no effect unless the connection was lost.

### 1.1 Preparing a Statement from a File: `db.prepare_file()` / `tr.prepare_file()`

When your SQL queries are stored in external files, you can use `prepare_file` to load and prepare them directly:

```cpp
db.prepare_file(
    "complex_query", // Unique name for the prepared statement
    std::filesystem::path("queries/user_analytics.sql"), // Path to SQL file
    {qb::pg::oid::int4, qb::pg::oid::timestamp}, // Parameter types
    // Success Callback
    [](qb::pg::transaction& tr, const qb::pg::detail::PreparedQuery& pq) {
        std::cout << "Statement from file prepared successfully.\n";
    },
    // Error Callback
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Failed to prepare from file: " << err.what() << "\n";
    }
);

// Simpler version with only success callback
db.prepare_file(
    "report_query",
    std::filesystem::path("queries/monthly_report.sql"),
    {qb::pg::oid::text, qb::pg::oid::date},
    [](qb::pg::transaction& tr, const qb::pg::detail::PreparedQuery& pq) {
        std::cout << "Report query prepared: " << pq.query << std::endl;
    }
);

// Minimal version without callbacks
db.prepare_file("simple_stats", std::filesystem::path("queries/stats.sql"), {qb::pg::oid::int4});
```

*   **File Loading:** The method reads the SQL query from the specified file and prepares it as a named statement.
*   **Error Handling:** If the file doesn't exist or can't be read, the error callback is invoked and an exception is thrown.
*   **Usage:** Once prepared, the statement is executed the same way as any other prepared statement, using the assigned name.

### 2. Executing a Prepared Statement: `db.execute()` / `tr.execute()`

Once prepared, execute the statement by name, providing the parameters.

```cpp
std::string user_email = "jane@example.com";

db.execute(
    "find_user_by_email", // Name of the prepared statement
    {user_email},         // Parameters in order ($1)
    // Success Callback (receives results)
    [](qb::pg::transaction& tr, qb::pg::results result) {
        if (!result.empty()) {
            // Process result[0]["id"], result[0]["name"], etc.
        } else {
            std::cout << "User not found.\n";
        }
    },
    // Error Callback
    [](const qb::pg::error::db_error& err) { /* ... */ }
);
```

### 3. Parameter Handling: `qb::pg::params`

*(Defined in `src/queries.h`, uses `src/param_serializer.h` internally)*

The `qb::pg::params` class (and its implicit construction via `{}`) handles the serialization of C++ values into the PostgreSQL binary wire format.

```cpp
// Implicit construction (convenient)
db.execute("update_user", {123, std::string("new_email@test.com"), true});

// Explicit construction
qb::pg::params user_params;
user_params.add_param(123); // Automatically determines OID (int4)
user_params.add_param(std::string("new_email@test.com")); // OID (text)
user_params.add_param(true); // OID (boolean)
db.execute("update_user", std::move(user_params));

// Handling NULL values with std::optional
std::optional<int> age_param; // Represents NULL
std::optional<std::string> name_param = "Optional Name";
db.execute("insert_optional", {age_param, name_param});
```

*   **Type Mapping:** The library automatically maps common C++ types to appropriate PostgreSQL OIDs. See [Data Type Handling](./types.md).
*   **Binary Format:** Parameters are typically sent in binary format for efficiency and type safety.
*   **NULLs:** Use `std::optional<T>` for parameters that might be NULL. An empty `std::optional` is serialized as SQL NULL.

## Best Practices for Prepared Statements

Prepared statements improve performance and security when used effectively. Here are recommended patterns:

### Initialization During Application Startup

It's common and efficient to prepare frequently used statements during application initialization:

```cpp
// Initialize all prepared statements within a transaction during startup
db.begin([](qb::pg::transaction& tr) {
    // Chain multiple prepare calls for clean code organization
    tr.prepare("find_user", "SELECT * FROM users WHERE id = $1")
      .prepare("update_user", "UPDATE users SET name = $2, email = $3 WHERE id = $1")
      .prepare("insert_user", "INSERT INTO users (name, email) VALUES ($1, $2) RETURNING id")
      .prepare_file("complex_report", std::filesystem::path("queries/report.sql"), {qb::pg::oid::date, qb::pg::oid::text});
      
    // You can mix other initialization commands
    tr.execute("CREATE TABLE IF NOT EXISTS app_status (last_startup TIMESTAMP)");
    tr.execute("INSERT INTO app_status VALUES (NOW())");
})
.error([](const qb::pg::error::db_error& err) {
    std::cerr << "Failed to initialize prepared statements: " << err.what() << std::endl;
    // Application might need to terminate or retry
});
```

### Benefits of This Approach

*   **Performance:** All statement preparations happen in a single transaction, reducing round-trips to the database.
*   **Organization:** Centralizes SQL queries in one place rather than scattered throughout the codebase.
*   **Error Handling:** Consolidated error handling for all prepare operations.
*   **Maintainability:** Makes it easier to track which prepared statements are available in the application.
*   **Consistency:** Ensures all necessary statements are prepared before the application starts handling requests.

### Reuse Throughout the Application

After preparation, statements can be executed anywhere in the application using just their names, keeping the actual SQL logic centralized:

```cpp
// In user creation logic
db.execute("insert_user", {user_name, user_email}, 
    [](qb::pg::transaction& tr, qb::pg::results result) {
        int new_id = result[0]["id"].as<int>();
        // Process the new user...
    });

// In user update logic
db.execute("update_user", {user_id, new_name, new_email});

// In reporting logic
db.execute("complex_report", {report_date, department},
    [](qb::pg::transaction& tr, qb::pg::results result) {
        // Generate report from results...
    });
```

This approach provides consistency, maintainability, and security by separating SQL definition from its execution points.

## Asynchronous Nature

Remember that `execute()`, `execute_file()`, `prepare()`, and `prepare_file()` calls are **asynchronous**. They queue the operation and return immediately. The actual database interaction and the execution of your success/error callbacks happen later within the QB event loop (`qb::io::async::run()` or actor processing).

Use the chaining methods (`.then()`, `.error()`, `.success()`) or `.await()` to manage the flow of asynchronous operations. 