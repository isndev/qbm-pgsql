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
*   **Type OIDs (`qb::pg::oid`):** Provide a `qb::pg::type_oid_sequence` (which is `std::vector<qb::pg::oid>`) to specify the expected PostgreSQL data type for each parameter. This helps the server optimize and validate. Common OIDs are defined in `src/pg_types.h` (e.g., `oid::int4`, `oid::text`, `oid::timestamp`, `oid::uuid`). If omitted, the server attempts type inference.
*   **Caching:** The `qbm-pgsql` client caches prepared statements internally (`PreparedQueryStorage` in `src/queries.h`). Preparing the same named statement again usually has no effect unless the connection was lost.

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

## Asynchronous Nature

Remember that `execute()` and `prepare()` calls are **asynchronous**. They queue the operation and return immediately. The actual database interaction and the execution of your success/error callbacks happen later within the QB event loop (`qb::io::async::run()` or actor processing).

Use the chaining methods (`.then()`, `.error()`, `.success()`) or `.await()` to manage the flow of asynchronous operations. 