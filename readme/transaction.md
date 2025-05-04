# `qbm-pgsql`: Transaction Handling

This document explains how to manage database transactions using the `qbm-pgsql` module's fluent API.

## Core Concept: `qb::pg::detail::Transaction`

*(Base class defined in `src/transaction.h`, concrete commands in `src/commands.h`)*

While `qb::pg::detail::Transaction` is the internal base class, you primarily interact with transactions through methods on the `qb::pg::tcp::database` object (or its SSL variant).

The API is designed to be **fluent**, allowing you to chain multiple database operations together within a single transaction block. Each operation (like `execute`, `prepare`, `savepoint`, `then`, `error`) returns a reference to the transaction object, enabling this chaining.

## Starting a Transaction: `db.begin()`

This method initiates a new transaction block.

```cpp
db.begin(
    // --- Success Callback (Required) ---
    // This lambda is executed *if* the BEGIN command succeeds.
    // It receives a reference to the transaction context.
    [](qb::pg::transaction& tr) {
        // --- Chain operations within the transaction here ---
        tr.execute("...")
          .prepare("...")
          .then([](qb::pg::transaction& tr_step2) { /* ... */ });
        // --- End of chained operations ---
    },
    // --- Error Callback (Optional) ---
    // This lambda is executed *if* the BEGIN command itself fails,
    // or if any subsequent operation within the transaction fails
    // and causes a rollback.
    [](const qb::pg::error::db_error& err) {
        std::cerr << "Transaction failed: " << err.what() << std::endl;
    },
    // --- Transaction Mode (Optional) ---
    // Specify isolation level, read-only status, etc.
    qb::pg::transaction_mode{qb::pg::isolation_level::serializable, true /*read only*/}
);
```

*   **Asynchronous Execution:** The `begin()` call itself is non-blocking. The code inside the success lambda is queued for execution.
*   **Commit/Rollback:** The transaction is automatically **committed** if all operations chained within the success lambda complete without error. It is automatically **rolled back** if the `begin` fails, or if any operation within the success lambda fails (either by throwing an exception in a callback or by receiving a database error).

## Chaining Operations

Inside the `begin()` success callback, you chain operations using the provided `qb::pg::transaction& tr` reference:

*   **`tr.execute(sql, [params], [on_success], [on_error])`**: Executes a simple or prepared query. See [Query Execution](./queries.md).
*   **`tr.prepare(name, sql, [types], [on_success], [on_error])`**: Prepares a statement. See [Query Execution](./queries.md).
*   **`tr.savepoint(name, on_success, [on_error])`**: Creates a nested savepoint. See below.
*   **`tr.then(on_success)`**: Executes the `on_success` lambda *only if* the preceding operation in the chain succeeded. This is crucial for sequential logic.
    ```cpp
    tr.execute("INSERT...")
      .then([](qb::pg::transaction& tr_next) {
          // This runs only if INSERT was successful
          return tr_next.execute("SELECT..."); // Return to continue chain
      });
    ```
*   **`tr.error(on_error)`**: Attaches an error handler specifically to the preceding operation(s) in the chain *within the current transaction block*. If an error occurs before this point, the `on_error` lambda is executed.
    ```cpp
    tr.execute("UPDATE...")
      .error([](const qb::pg::error::db_error& err) {
          // Handle specific UPDATE error
      })
      .then([](qb::pg::transaction& tr_next) { /* ... */ });
    ```
*   **`tr.success(on_success)`**: Similar to `then`, but typically used at the very end of a chain to signify the successful completion of the *entire sequence* within that transaction block. Often redundant as the main transaction commit handles overall success.

## Savepoints (Nested Transactions)

Savepoints allow you to establish points within a transaction to which you can later roll back, without aborting the entire transaction.

```cpp
db.begin([](qb::pg::transaction& tr_outer) {
    tr_outer.execute("INSERT INTO main_table ...");

    tr_outer.savepoint("my_sp", // Savepoint name
        // --- Savepoint Success Callback ---
        [](qb::pg::transaction& tr_inner) {
            std::cout << "Inside savepoint.\n";
            tr_inner.execute("INSERT INTO secondary_table ...")
                  .then([](auto& tr) { /* More work */ })
                  .error([](const auto& err) {
                      // If this error handler is called, the work done
                      // inside this savepoint lambda will be rolled back
                      // to the state just before the savepoint.
                      std::cerr << "Error inside savepoint: " << err.what() << std::endl;
                      // IMPORTANT: This error typically propagates and rolls back
                      // the *entire* outer transaction unless handled differently.
                      // To *only* rollback the savepoint requires more complex logic
                      // (e.g., catching specific errors and manually issuing
                      // ROLLBACK TO SAVEPOINT my_sp). The default fluent API
                      // promotes all-or-nothing semantics for the outer transaction.
                  });
        },
        // --- Savepoint Error Callback (if SAVEPOINT command fails) ---
        [](const qb::pg::error::db_error& err) {
            std::cerr << "Failed to create savepoint: " << err.what() << std::endl;
        }
    )
    .then([](qb::pg::transaction& tr_after) {
        // This executes if the savepoint block completed (either committed
        // internally or rolled back internally due to an error caught
        // by its .error() handler).
        // The outer transaction continues.
        tr_after.execute("INSERT INTO log_table ...");
    });
});
```

*   **Rollback:** An error occurring *inside* the savepoint's success lambda will, by default, cause the **entire outer transaction** to roll back when the error propagates up. To achieve partial rollback *only* to the savepoint, you would need to catch the specific error within the savepoint's chain (`.error()`) and prevent it from failing the outer transaction, potentially by executing `ROLLBACK TO SAVEPOINT my_sp` manually (though this deviates from the simple fluent API).
*   **Naming:** Savepoint names should be unique within their transaction scope.

## Transaction Isolation Levels

*(Enum defined in `src/common.h`)*

You can specify the isolation level when beginning a transaction:

*   `qb::pg::isolation_level::read_committed` (Default): Each statement sees data committed before it began.
*   `qb::pg::isolation_level::repeatable_read`: All statements in the transaction see a snapshot as of the first statement.
*   `qb::pg::isolation_level::serializable`: Strictest level, ensures transactions behave as if run sequentially.

```cpp
qb::pg::transaction_mode mode;
mode.isolation = qb::pg::isolation_level::serializable;
mode.read_only = true; // Optional: make it read-only

db.begin(
    [](qb::pg::transaction& tr) { /* ... */ },
    [](const qb::pg::error::db_error& err) { /* ... */ },
    mode // Pass the mode struct
);
```

Refer to PostgreSQL documentation for the detailed implications of each isolation level.

## Synchronous Execution with `await()`

While the primary API is asynchronous, you can force synchronous execution by calling `.await()` at the end of a transaction chain.

```cpp
auto status = db.begin([](qb::pg::transaction& tr) {
                      tr.execute("...");
                   })
                  .await(); // Blocks until transaction completes or fails

if (status) {
    std::cout << "Transaction committed.\n";
} else {
    std::cerr << "Transaction failed: " << status.error().what() << std::endl;
}
```

This is useful for testing, simple scripts, or integrating with synchronous code, but should be avoided in performance-critical actor code as it blocks the caller. 