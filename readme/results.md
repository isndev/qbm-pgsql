# `qbm-pgsql`: Result Set Processing

This document explains how to access and process data returned from PostgreSQL queries using the `qb::pg::results` class and its associated components.

## Core Class: `qb::pg::results`

*(Defined in `src/resultset.h`, uses `src/result_impl.h` internally)*

This class represents the entire result set returned by a query (e.g., a `SELECT` statement). It acts like a container of rows.

### Key Features:

*   **Container Interface:** Supports range-based for loops, `begin()`, `end()`, `size()`, `empty()`, `front()`, `back()`, `operator[]` for row access.
*   **Metadata:** Provides access to column information (names, types) via `columns_size()` and `row_description()`.
*   **Row Access:** Access individual rows using iterators or `operator[]`.

```cpp
db.execute("SELECT id, name, age FROM users",
    [](qb::pg::transaction& tr, qb::pg::results results) {
        if (results.empty()) {
            std::cout << "No users found.\n";
            return;
        }

        std::cout << "Found " << results.size() << " users.\n";
        std::cout << "Number of columns: " << results.columns_size() << "\n";

        // Get description of the first column
        const qb::pg::field_description& first_col_desc = results.field(0);
        std::cout << "First column name: " << first_col_desc.name
                  << ", Type OID: " << static_cast<int>(first_col_desc.type_oid) << "\n";

        // Iterate over rows
        for (const qb::pg::resultset::row& row : results) {
            // Process each row (see qb::pg::resultset::row below)
        }

        // Access specific row by index
        if (results.size() > 0) {
            const qb::pg::resultset::row& first_row = results[0];
            // ... access fields in first_row ...
        }
    }
);
```

## Row Access: `qb::pg::resultset::row`

*(Defined in `src/resultset.h`)*

This class represents a single row within a `resultset`. It acts like a container of fields.

### Key Features:

*   **Field Access by Index:** `row[column_index]`
*   **Field Access by Name:** `row["column_name"]` (less performant than by index due to name lookup).
*   **Container Interface:** Supports range-based for loops, `begin()`, `end()`, `size()`, `empty()` for iterating over fields.
*   **Tuple Conversion:** `row.to(my_tuple)` or `row.to(std::tie(var1, var2))` for easy data extraction. See [Data Type Handling](./types.md).
*   **Reference Semantics:** A `row` object is lightweight and references data within the parent `resultset`. It must not outlive the `resultset`.

```cpp
void process_user_row(const qb::pg::resultset::row& row) {
    // Access by index
    int id = row[0].as<int>();

    // Access by name
    std::string name = row["name"].as<std::string>();
    std::optional<int> age_opt = row["age"].as<std::optional<int>>();

    std::cout << "Row Index: " << row.row_index() << " | ID: " << id << ", Name: " << name;
    if (age_opt) {
        std::cout << ", Age: " << *age_opt;
    }
    std::cout << std::endl;

    // Iterate over fields in the row
    for (const qb::pg::resultset::field& field : row) {
        std::cout << "  Field Name: " << field.name()
                  << ", Is Null: " << field.is_null() << std::endl;
        if (!field.is_null()) {
            // Access raw buffer (advanced)
            // qb::pg::field_buffer buffer = field.input_buffer();
            // ... process buffer ...
        }
    }

    // Extract to tuple (C++17 structured binding)
    auto [user_id, user_name, user_age] = std::tuple<int, std::string, std::optional<int>>();
    row.to(std::tie(user_id, user_name, user_age)); // Uses std::tie for references
    std::cout << "Extracted via tuple: " << user_id << ", " << user_name << std::endl;
}
```

## Field Access: `qb::pg::resultset::field`

*(Defined in `src/resultset.h`, uses `src/field_handler.h` internally)*

This class represents a single field (column value) within a `row`.

### Key Features:

*   **`.as<T>()`:** The primary method for extracting the field's value, converting it to the specified C++ type `T`. Throws `qb::pg::error::value_is_null` if the field is SQL NULL and `T` is not `std::optional`. Throws `qb::pg::error::field_type_mismatch` or other exceptions on conversion errors.
*   **`.to<T>(T& value)`:** Extracts the value into the provided reference `value`. Returns `false` if the field is SQL NULL (does not throw `value_is_null`), `true` otherwise. Throws on conversion errors.
*   **`.is_null()`:** Checks if the field contains SQL NULL.
*   **`.name()`:** Gets the column name.
*   **`.description()`:** Gets the `field_description` struct containing metadata (OID, format code, etc.).
*   **`.input_buffer()`:** Gets a `field_buffer` (a `qb::util::input_iterator_buffer`) providing access to the raw byte data received from the server (advanced use).

```cpp
void process_field(const qb::pg::resultset::field& field) {
    std::cout << "Processing Field: " << field.name()
              << " (OID: " << static_cast<int>(field.description().type_oid) << ")\n";

    if (field.is_null()) {
        std::cout << "  Value is NULL\n";
    } else {
        try {
            // Attempt conversion to string (usually safe)
            std::string string_val = field.as<std::string>();
            std::cout << "  Value as string: " << string_val << "\n";

            // Attempt conversion to int (might throw)
            if (field.description().type_oid == qb::pg::oid::int4) { // Check type first
                int int_val = field.as<int>();
                std::cout << "  Value as int: " << int_val << "\n";
            }

            // Safe conversion using to()
            int safe_int_val;
            if (field.to(safe_int_val)) {
                 std::cout << "  Value safely converted to int: " << safe_int_val << "\n";
            } else {
                 // This handles the NULL case if to<int> was called on a NULL field
                 // but as() would have thrown value_is_null earlier.
                 // More useful for optional types.
            }

            // Handling optional types
            std::optional<double> opt_double;
            if(field.to(opt_double)) { // Always returns true for optional
                if (opt_double) {
                    std::cout << "  Value as optional<double>: " << *opt_double << "\n";
                } else {
                    std::cout << "  Value is NULL (checked via optional)\n";
                }
            }

        } catch (const qb::pg::error::value_is_null& e) {
            std::cerr << "  Error: Tried to get value of NULL field '" << field.name() << "'\n";
        } catch (const qb::pg::error::field_type_mismatch& e) {
            std::cerr << "  Error: Type mismatch for field '" << field.name() << "': " << e.what() << "\n";
        } catch (const std::exception& e) {
            std::cerr << "  Error converting field '" << field.name() << "': " << e.what() << "\n";
        }
    }
}
```

See [Data Type Handling](./types.md) for more on type conversions. 