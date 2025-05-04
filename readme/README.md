# QB PostgreSQL Module (`qbm-pgsql`) - Detailed Documentation

Welcome to the detailed documentation for the `qbm-pgsql` module of the QB C++ Actor Framework. This module provides a high-performance, fully asynchronous client for interacting with PostgreSQL databases.

This documentation covers the internal design, key classes, and usage patterns specific to this module.

## Core Components

*   **[Connection Management](./connection.md):** Establishing, managing, and authenticating connections (`qb::pg::tcp::database`, `connection_options`).
*   **[Transaction Handling](./transaction.md):** Managing transactions, savepoints, isolation levels, and the fluent API (`qb::pg::transaction`).
*   **[Query Execution](./queries.md):** Executing simple and prepared statements, parameter binding (`qb::pg::params`).
*   **[Result Set Processing](./results.md):** Accessing and iterating over query results (`qb::pg::results`, `row`, `field`).
*   **[Data Type Handling](./types.md):** Mapping between C++ types and PostgreSQL types, serialization, and deserialization.
*   **[Error Handling](./error_handling.md):** Understanding error types and SQLSTATE codes.

## Internal Details (Advanced)

*   *(Placeholder: Could add sections on `protocol.h`, `commands.h`, internal serialization if needed for contributors)*

## Examples

Refer to the examples within the main QB framework repository that utilize the PostgreSQL module for practical implementation details. 