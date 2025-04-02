/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2025 qb - isndev (cpp.actor). All rights reserved.
 *
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
 *         limitations under the License.
 */

/**
 * @file transaction.h
 * @brief PostgreSQL transaction management for QB Actor Framework
 * 
 * This file implements the transaction management system for the PostgreSQL client.
 * It provides a fluent interface for executing SQL commands within transactions,
 * supporting nested transactions through savepoints, and handling success/error callbacks.
 * 
 * The Transaction class serves as a base for all database operations, allowing
 * commands to be chained together in a natural and readable syntax.
 * 
 * @see qb::pg::detail::Database
 * @see qb::pg::detail::ISqlQuery
 */

#ifndef QBM_PGSQL_DETAIL_TRANSACTION_H
#define QBM_PGSQL_DETAIL_TRANSACTION_H

#include "queries.h"
#include "../not-qb/result_impl.h"
#include "../not-qb/resultset.h"
#include <queue>
#include <qb/io/async.h>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace qb::pg::detail {
using namespace qb::pg;

/**
 * @brief Base class for database transaction operations
 * 
 * The Transaction class provides the core functionality for managing
 * database transactions, including:
 * - Starting and ending transactions
 * - Creating and managing savepoints
 * - Executing SQL queries and prepared statements
 * - Handling success and error callbacks
 * 
 * It uses a fluent interface to allow chaining operations together
 * in a natural and readable syntax.
 */
class Transaction {
protected:
    Transaction* _parent{nullptr};                 ///< Parent transaction (for nested transactions)
    std::queue<std::unique_ptr<Transaction>> _sub_commands; ///< Queue of sub-transactions
    std::queue<std::unique_ptr<ISqlQuery>> _queries;     ///< Queue of SQL queries to execute
    PreparedQueryStorage& _query_storage; ///< Storage for prepared queries
    bool _result{true};                  ///< Result status of the transaction
    error::db_error _error;               ///< Error message of the transaction
    result_impl _results;                 ///< Last results of the transaction

    Transaction() = delete;
    Transaction(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction& operator=(Transaction&&) = delete;
    
    /**
     * @brief Constructs a nested transaction
     * 
     * @param parent Pointer to the parent transaction
     */
    explicit Transaction(Transaction* parent) noexcept;
    
    /**
     * @brief Constructs a root transaction
     * 
     * @param storage Reference to prepared query storage
     */
    explicit Transaction(PreparedQueryStorage& storage) noexcept;

public:
    /**
     * @brief Virtual destructor
     * 
     * Cleans up any remaining queries and sub-transactions
     */
    virtual ~Transaction();

    /**
     * @brief Sets the result status of the transaction
     * 
     * @param value New result status (true for success, false for failure)
     */
    void result(bool value);
    
    /**
     * @brief Gets the current result status of the transaction
     * 
     * @return bool Current result status
     */
    [[nodiscard]] bool result() const;
    
    /**
     * @brief Gets the parent transaction
     * 
     * @return Transaction* Pointer to parent transaction or nullptr for root
     */
    [[nodiscard]] Transaction* parent() const;

    /**
     * @brief Adds a sub-transaction to the queue
     * 
     * @param cmd Pointer to the sub-transaction
     */
    void push_transaction(std::unique_ptr<Transaction> cmd);
    
    /**
     * @brief Removes and returns the next sub-transaction from the queue
     * 
     * @return Transaction* Pointer to the removed sub-transaction or nullptr if empty
     */
    std::unique_ptr<Transaction> pop_transaction();
    
    /**
     * @brief Returns the next sub-transaction without removing it
     * 
     * @return Transaction* Pointer to the next sub-transaction or nullptr if empty
     */
    [[nodiscard]] Transaction* next_transaction();

    /**
     * @brief Adds a query to the queue
     * 
     * @param qry Pointer to the query
     */
    void push_query(std::unique_ptr<ISqlQuery> qry);
    
    /**
     * @brief Returns the next query without removing it
     * 
     * @return ISqlQuery* Pointer to the next query or nullptr if empty
     */
    [[nodiscard]] ISqlQuery* next_query();
    
    /**
     * @brief Removes and returns the next query from the queue
     * 
     * @return ISqlQuery* Pointer to the removed query or nullptr if empty
     */
    std::unique_ptr<ISqlQuery> pop_query();

    /**
     * @brief Handles the result status of a sub-command
     * 
     * Called when a sub-command completes to update this transaction's status
     * 
     * @param status Result status of the sub-command
     */
    virtual void on_sub_command_status(bool status);
    
    /**
     * @brief Called when a new command is started
     * 
     * Notification that a new command is being processed
     */
    virtual void on_new_command();
    
    /**
     * @brief Called when a query returns a row description
     * 
     * @param Row description metadata from the result
     */
    virtual void on_new_row_description(row_description_type&&);
    
    /**
     * @brief Called when a query returns a data row
     * 
     * @param Row data from the result
     */
    virtual void on_new_data_row(row_data&&);

    /**
     * @brief Begins a new transaction with success and error callbacks
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @tparam CB_ERROR Type of error callback function
     * @param on_success Callback called when transaction starts successfully
     * @param on_error Callback called if transaction start fails
     * @param mode Optional transaction mode settings
     * @return Transaction& Reference to this transaction for chaining
     */
    template<typename CB_SUCCESS, typename CB_ERROR,
            typename = std::enable_if<std::is_function_v<CB_SUCCESS> &&
                                      std::is_function_v<CB_ERROR>>>
    Transaction &begin(CB_SUCCESS &&on_success, CB_ERROR &&on_error,
                       transaction_mode mode = {});

    /**
     * @brief Begins a new transaction with only a success callback
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @param on_success Callback called when transaction starts successfully
     * @param mode Optional transaction mode settings
     * @return Transaction& Reference to this transaction for chaining
     */
    template<typename CB_SUCCESS>
    Transaction &begin(CB_SUCCESS &&on_success, transaction_mode mode = {});

    /**
     * @brief Creates a savepoint within the current transaction
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @tparam CB_ERROR Type of error callback function
     * @param name Name of the savepoint
     * @param on_success Callback called when savepoint is created successfully
     * @param on_error Callback called if savepoint creation fails
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS, typename CB_ERROR,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS> &&
                                        std::is_function_v<CB_ERROR>>>
    Transaction &savepoint(std::string_view name, CB_SUCCESS &&on_success,
                           CB_ERROR &&on_error);

    /**
     * @brief Creates a savepoint with only a success callback
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @param name Name of the savepoint
     * @param on_success Callback called when savepoint is created successfully
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS>
    Transaction &savepoint(std::string_view name, CB_SUCCESS &&on_success);

    /**
     * @brief Executes a SQL query with success and error callbacks
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @tparam CB_ERROR Type of error callback function
     * @param expr SQL query to execute
     * @param on_success Callback called when query executes successfully
     * @param on_error Callback called if query execution fails
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS, typename CB_ERROR,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS> &&
                                        std::is_function_v<CB_ERROR>>>
    Transaction &execute(std::string_view expr, CB_SUCCESS &&on_success,
                         CB_ERROR &&on_error);

    /**
     * @brief Executes a SQL query with only a success callback
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @param expr SQL query to execute
     * @param on_success Callback called when query executes successfully
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS>>>
    Transaction &execute(std::string_view expr, CB_SUCCESS &&on_success);

    /**
     * @brief Executes a SQL query without callbacks
     * 
     * @param expr SQL query to execute
     * @return Transaction& Reference to this transaction for chaining
     */
    Transaction &execute(std::string_view expr);

    /**
     * @brief Prepares a SQL query with parameter types and callbacks
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @tparam CB_ERROR Type of error callback function
     * @param query_name Name for the prepared query
     * @param expr SQL query to prepare
     * @param types Sequence of parameter types
     * @param on_success Callback called when query is prepared successfully
     * @param on_error Callback called if query preparation fails
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS, typename CB_ERROR,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS> &&
                                        std::is_function_v<CB_ERROR>>>
    Transaction &prepare(std::string_view query_name, std::string_view expr,
                         type_oid_sequence &&types, CB_SUCCESS &&on_success,
                         CB_ERROR &&on_error);

    /**
     * @brief Prepares a SQL query with parameter types and success callback
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @param query_name Name for the prepared query
     * @param expr SQL query to prepare
     * @param types Sequence of parameter types
     * @param on_success Callback called when query is prepared successfully
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS>>>
    Transaction &prepare(std::string_view query_name, std::string_view expr,
                         type_oid_sequence &&types, CB_SUCCESS &&on_success);

    /**
     * @brief Prepares a SQL query with parameter types without callbacks
     * 
     * @param query_name Name for the prepared query
     * @param expr SQL query to prepare
     * @param types Sequence of parameter types
     * @return Transaction& Reference to this transaction for chaining
     */
    Transaction &prepare(std::string_view query_name, std::string_view expr,
                         type_oid_sequence &&types = {});

    /**
     * @brief Executes a prepared query with parameters and callbacks
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @tparam CB_ERROR Type of error callback function
     * @param query_name Name of the prepared query to execute
     * @param params Parameters for the prepared query
     * @param on_success Callback called when query executes successfully
     * @param on_error Callback called if query execution fails
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS, typename CB_ERROR,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS> &&
                                        std::is_function_v<CB_ERROR>>>
    Transaction &execute(std::string_view query_name, QueryParams &&params,
                         CB_SUCCESS &&on_success, CB_ERROR &&on_error);

    /**
     * @brief Executes a prepared query with parameters and success callback
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @param query_name Name of the prepared query to execute
     * @param params Parameters for the prepared query
     * @param on_success Callback called when query executes successfully
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS>>>
    Transaction &execute(std::string_view query_name, QueryParams &&params,
                         CB_SUCCESS &&on_success);

    /**
     * @brief Executes a prepared query with parameters and success callback (alternative syntax)
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @param query_name Name of the prepared query to execute
     * @param on_success Callback called when query executes successfully
     * @param params Parameters for the prepared query
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS>>>
    Transaction &execute(std::string_view query_name, CB_SUCCESS &&on_success,
                         QueryParams &&params);

    /**
     * @brief Executes a prepared query with parameters without callbacks
     * 
     * @param query_name Name of the prepared query to execute
     * @param params Parameters for the prepared query
     * @return Transaction& Reference to this transaction for chaining
     */
    Transaction &execute(std::string_view query_name, QueryParams &&params);

    /**
     * @brief Adds a callback to be executed after the next operation
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @param on_success Callback to be executed after the next operation
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS>
    Transaction &then(CB_SUCCESS &&on_success);

    /**
     * @brief Adds a success callback to the transaction
     * 
     * @tparam CB_SUCCESS Type of success callback function
     * @param on_success Callback to be executed on transaction success
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS>
    Transaction &success(CB_SUCCESS &&on_success);

    /**
     * @brief Adds an error callback to the transaction
     * 
     * @tparam CB_ERROR Type of error callback function
     * @param on_error Callback to be executed on transaction error
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_ERROR>
    Transaction &error(CB_ERROR &&on_error);

    /**
     * @brief Checks if the transaction has an error
     * 
     * @return bool True if the transaction has an error, false otherwise
     */
    [[nodiscard]] bool has_error() const;

    /**
     * @brief Gets the error message of the transaction
     * 
     * @return std::string& Reference to the error message
     */
    [[nodiscard]] const error::db_error& error() const;

    /**
     * @brief Gets the last results of the transaction
     * 
     * @return result_impl& Reference to the last results
     */
    result_impl& results();

    class status {
        friend class Transaction;
        result_impl _results;
        error::db_error _error{"unknown error"};

        public:
        status() = default;
        ~status() = default;
        status(status&) = default;
        status(status&&) = default;
        status& operator=(status&) = default;
        status& operator=(status&&) = default;

        status(result_impl results, error::db_error error)
            : _results(std::move(results))
            , _error(std::move(error)) {}

        [[nodiscard]] explicit operator bool() const {
            return _error.code.empty();
        }

        [[nodiscard]] bool operator()() const {
            return static_cast<bool>(*this);
        }

        [[nodiscard]] resultset results() { return {&_results}; }
        [[nodiscard]] error::db_error& error() { return _error; }
    };

    status await();
};

} // namespace qb::pg::detail

#include "transaction.inl"

namespace qb::pg {
    inline detail::Transaction::status await(detail::Transaction& t) {
        return t.await();
    }
}

#endif // QBM_PGSQL_DETAIL_TRANSACTION_H
