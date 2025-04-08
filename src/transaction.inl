/**
 * @file transaction.inl
 * @brief PostgreSQL transaction implementation for the QB Actor Framework
 *
 * This file provides the templated implementation of the Transaction class methods:
 *
 * - Transaction management (begin, commit, rollback)
 * - Savepoint creation and management
 * - Query execution with callbacks
 * - Prepared statement handling
 * - Success and error callback management
 * - Fluent API for transaction operations chaining
 *
 * The implementation uses modern C++ features including perfect forwarding,
 * SFINAE for function overloading, and lambda callback support to provide a
 * flexible and type-safe transaction API.
 *
 * @see qb::pg::detail::Transaction
 * @see qb::pg::transaction
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include "./commands.h"
#include "./transaction.h"

namespace qb::pg::detail {

/**
 * @brief Begins a new transaction with success and error callbacks
 *
 * Initiates a new PostgreSQL transaction with the specified mode and callbacks.
 * If the database is already in a transaction, the error callback is invoked.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam CB_ERROR Type of error callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param on_success Callback invoked when transaction begins successfully
 * @param on_error Callback invoked if transaction fails to begin
 * @param mode Transaction isolation mode
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::begin(CB_SUCCESS &&on_success, CB_ERROR &&on_error, transaction_mode mode) {
    if (_parent) {
        on_error((error::db_error) error::query_error("already in transaction"));
    } else {
        auto end = new End<CB_ERROR>(this, std::forward<CB_ERROR>(on_error));
        push_transaction(std::unique_ptr<Transaction>(new Begin<CB_SUCCESS, CB_ERROR>(
            this, end, mode, std::forward<CB_SUCCESS>(on_success))));
        push_transaction(std::unique_ptr<Transaction>(end));
    }
    return *this;
}

/**
 * @brief Begins a new transaction with only success callback
 *
 * Simplified version that uses an empty error callback.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @param on_success Callback invoked when transaction begins successfully
 * @param mode Transaction isolation mode
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS>
Transaction &
Transaction::begin(CB_SUCCESS &&on_success, transaction_mode mode) {
    return begin(
        std::forward<CB_SUCCESS>(on_success), [](error::db_error const &) {}, mode);
}

/**
 * @brief Creates a savepoint within a transaction
 *
 * Creates a named savepoint within the current transaction, allowing for
 * partial rollback if needed.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam CB_ERROR Type of error callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param name Name of the savepoint to create
 * @param on_success Callback invoked when savepoint is created successfully
 * @param on_error Callback invoked if savepoint creation fails
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::savepoint(std::string_view name, CB_SUCCESS &&on_success,
                       CB_ERROR &&on_error) {
    auto end = new EndSavePoint<CB_ERROR>(this, std::string(name),
                                          std::forward<CB_ERROR>(on_error));
    push_transaction(std::unique_ptr<Transaction>(new SavePoint<CB_SUCCESS, CB_ERROR>(
        this, end, std::forward<CB_SUCCESS>(on_success))));
    push_transaction(std::unique_ptr<Transaction>(end));
    return *this;
}

/**
 * @brief Creates a savepoint with only success callback
 *
 * Simplified version that uses an empty error callback.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @param name Name of the savepoint to create
 * @param on_success Callback invoked when savepoint is created successfully
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS>
Transaction &
Transaction::savepoint(std::string_view name, CB_SUCCESS &&on_success) {
    return savepoint(name, std::forward<CB_SUCCESS>(on_success), [](error::db_error) {});
}

/**
 * @brief Executes a SQL query with success and error callbacks
 *
 * Executes the specified SQL expression and invokes appropriate callbacks
 * based on the result. Automatically detects callback signature to determine
 * if result data should be returned.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam CB_ERROR Type of error callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param expr SQL expression to execute
 * @param on_success Callback invoked when query succeeds
 * @param on_error Callback invoked if query fails
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::execute(std::string_view expr, CB_SUCCESS &&on_success,
                     CB_ERROR &&on_error) {
    if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &, resultset>) {
        push_transaction(std::unique_ptr<Transaction>(
            new ResultQuery<CB_SUCCESS, CB_ERROR>(this, std::string(expr),
                                                  std::forward<CB_SUCCESS>(on_success),
                                                  std::forward<CB_ERROR>(on_error))));
    } else if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &>) {
        push_transaction(std::unique_ptr<Transaction>(new Query<CB_SUCCESS, CB_ERROR>(
            this, std::string(expr), std::forward<CB_SUCCESS>(on_success),
            std::forward<CB_ERROR>(on_error))));
    } else
        static_assert("execute call_back requires -> [](qb::pg::transaction &tr, "
                      "(optional) qb::pg::results res)");

    return *this;
}

/**
 * @brief Executes a SQL query with only success callback
 *
 * Simplified version that uses an empty error callback.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param expr SQL expression to execute
 * @param on_success Callback invoked when query succeeds
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS, typename>
Transaction &
Transaction::execute(std::string_view expr, CB_SUCCESS &&on_success) {
    return execute(std::string(expr), std::forward<CB_SUCCESS>(on_success),
                   [](error::db_error const &) {});
}

/**
 * @brief Prepares a statement with success and error callbacks
 *
 * Creates a prepared statement that can be reused with different parameters.
 * Parameter types must be specified at preparation time.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam CB_ERROR Type of error callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param query_name Name to assign to the prepared statement
 * @param expr SQL expression with parameter placeholders
 * @param types Sequence of PostgreSQL OIDs for parameter types
 * @param on_success Callback invoked when preparation succeeds
 * @param on_error Callback invoked if preparation fails
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::prepare(std::string_view query_name, std::string_view expr,
                     type_oid_sequence &&types, CB_SUCCESS &&on_success,
                     CB_ERROR &&on_error) {
    PreparedQuery query{
        std::string(query_name), std::string(expr), std::move(types), {}};

    push_transaction(std::unique_ptr<Transaction>(new Prepare<CB_SUCCESS, CB_ERROR>(
        this, std::move(query), std::forward<CB_SUCCESS>(on_success),
        std::forward<CB_ERROR>(on_error))));
    return *this;
}

/**
 * @brief Prepares a statement with only success callback
 *
 * Simplified version that uses an empty error callback.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param query_name Name to assign to the prepared statement
 * @param expr SQL expression with parameter placeholders
 * @param types Sequence of PostgreSQL OIDs for parameter types
 * @param on_success Callback invoked when preparation succeeds
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS, typename>
Transaction &
Transaction::prepare(std::string_view query_name, std::string_view expr,
                     type_oid_sequence &&types, CB_SUCCESS &&on_success) {
    return prepare(query_name, expr, types, std::forward<CB_SUCCESS>(on_success),
                   [](error::db_error const &) {});
}

/**
 * @brief Executes a prepared statement with parameters and callbacks
 *
 * Executes a previously prepared statement with the given parameters.
 * Automatically detects callback signature to determine if result data
 * should be returned.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam CB_ERROR Type of error callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param query_name Name of the prepared statement to execute
 * @param params Parameters to bind to the prepared statement
 * @param on_success Callback invoked when execution succeeds
 * @param on_error Callback invoked if execution fails
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::execute(std::string_view query_name, QueryParams &&params,
                     CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &, resultset>) {
        push_transaction(std::unique_ptr<Transaction>(
            new QueryPrepared<CB_SUCCESS, CB_ERROR>(this, std::string(query_name),
                                                    std::move(params),
                                                    std::forward<CB_SUCCESS>(on_success),
                                                    std::forward<CB_ERROR>(on_error))));
    } else if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &>) {
        push_transaction(
            std::unique_ptr<Transaction>(new ExecutePrepared<CB_SUCCESS, CB_ERROR>(
                this, std::string(query_name), std::move(params),
                std::forward<CB_SUCCESS>(on_success),
                std::forward<CB_ERROR>(on_error))));
    } else
        static_assert("execute call_back requires -> [](qb::pg::transaction &tr, "
                      "(optional) qb::pg::results res)");
    return *this;
}

/**
 * @brief Executes a prepared statement with only success callback
 *
 * Simplified version that uses an empty error callback.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param query_name Name of the prepared statement to execute
 * @param params Parameters to bind to the prepared statement
 * @param on_success Callback invoked when execution succeeds
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS, typename>
Transaction &
Transaction::execute(std::string_view query_name, QueryParams &&params,
                     CB_SUCCESS &&on_success) {
    return execute(query_name, std::move(params), std::forward<CB_SUCCESS>(on_success),
                   [](error::db_error const &) {});
}

/**
 * @brief Executes a prepared statement with parameters in different order
 *
 * Alternative overload with reordered parameters for convenience.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param query_name Name of the prepared statement to execute
 * @param on_success Callback invoked when execution succeeds
 * @param params Parameters to bind to the prepared statement
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS, typename>
Transaction &
Transaction::execute(std::string_view query_name, CB_SUCCESS &&on_success,
                     QueryParams &&params) {
    return execute(query_name, std::move(params), std::forward<CB_SUCCESS>(on_success),
                   [](error::db_error const &) {});
}

/**
 * @brief Registers a callback to be executed upon successful operation
 *
 * Adds a callback to be executed after the previous operation completes
 * successfully. Used for chaining operations in a fluent API style.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @param on_success Callback to execute on success
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS>
Transaction &
Transaction::then(CB_SUCCESS &&on_success) {
    push_transaction(std::unique_ptr<Transaction>(
        new Then<CB_SUCCESS>(this, std::forward<CB_SUCCESS>(on_success))));
    return *this;
}

/**
 * @brief Alias for then() - registers a success callback
 *
 * Alternative name for the then() method with identical functionality.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @param on_success Callback to execute on success
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS>
Transaction &
Transaction::success(CB_SUCCESS &&on_success) {
    push_transaction(std::unique_ptr<Transaction>(
        new Then<CB_SUCCESS>(this, std::forward<CB_SUCCESS>(on_success))));
    return *this;
}

/**
 * @brief Registers a callback to be executed upon operation failure
 *
 * Adds a callback to be executed if the previous operation fails.
 * Used for error handling in a fluent API style.
 *
 * @tparam CB_ERROR Type of error callback function
 * @param on_error Callback to execute on error
 * @return Reference to this transaction for method chaining
 */
template <typename CB_ERROR>
Transaction &
Transaction::error(CB_ERROR &&on_error) {
    push_transaction(std::unique_ptr<Transaction>(
        new Error<CB_ERROR>(this, std::forward<CB_ERROR>(on_error))));
    return *this;
}

} // namespace qb::pg::detail