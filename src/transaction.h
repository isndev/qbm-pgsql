/**
 * @file transaction.h
 * @brief PostgreSQL transaction management for the QB Actor Framework
 *
 * This file implements comprehensive transaction management for PostgreSQL databases
 * within the QB Actor Framework. It provides a modern, fluent interface for
 * database operations including:
 *
 * - Transaction control (begin, commit, rollback)
 * - Savepoint creation and management
 * - SQL query execution with callback handling
 * - Prepared statement support with parameter binding
 * - Asynchronous operation support with completion callbacks
 * - Result handling and error management
 *
 * The implementation uses template methods to provide type safety and flexibility
 * when working with database operations. The Transaction class serves as a base class
 * that provides a fluent API allowing operations to be chained together in a
 * natural, readable syntax.
 *
 * Key features:
 * - Fluent interface for chaining database operations
 * - Comprehensive error handling and reporting
 * - Support for nested transactions via savepoints
 * - Typed parameter binding for prepared statements
 * - Callback-based asynchronous result processing
 *
 * @see qb::pg::detail::ISqlQuery
 * @see qb::pg::detail::result_impl
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

#include <filesystem>
#include <memory>
#include <qb/io/async.h>
#include <queue>
#include <string_view>
#include <type_traits>
#include <utility>

#include "./pg_awaiter.h"
#include "./queries.h"
#include "./result_impl.h"
#include "./resultset.h"

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
    Transaction *_parent{nullptr}; ///< Parent transaction (for nested transactions)
    std::queue<std::unique_ptr<Transaction>> _sub_commands;  ///< Queue of sub-transactions
    std::queue<std::unique_ptr<ISqlQuery>>   _queries;       ///< Queue of SQL queries to execute
    PreparedQueryStorage                    &_query_storage; ///< Storage for prepared queries
    bool                                     _result{true};  ///< Result status of the transaction
    error::db_error                          _error;         ///< Error message of the transaction
    result_impl                              _results;       ///< Last results of the transaction

    // Statement timeout: applied on the server in the same simple-query batch as `BEGIN`
    // (`SET LOCAL statement_timeout`, transaction-scoped). Not socket idle time — see set_timeout().
    int _query_timeout_ms{0}; ///< Milliseconds for SET LOCAL on next BEGIN (0 = omit)

    Transaction() = delete;

    Transaction(const Transaction &) = delete;

    Transaction(Transaction &&) = delete;

    Transaction &operator=(const Transaction &) = delete;

    Transaction &operator=(Transaction &&) = delete;

    /**
     * @brief Constructs a nested transaction
     *
     * @param parent Pointer to the parent transaction
     */
    explicit Transaction(Transaction *parent) noexcept;

    /**
     * @brief Constructs a root transaction
     *
     * @param storage Reference to prepared query storage
     */
    explicit Transaction(PreparedQueryStorage &storage) noexcept;

public:
    /**
     * @brief Virtual destructor
     *
     * Cleans up any remaining queries and sub-transactions
     */
    virtual ~Transaction();

    /**
     * @brief Hook invoked when this command is popped from the parent's queue
     *
     * Used to finalize transaction/savepoint command pairs without relying on
     * destructor side effects (explicit lifecycle, coroutine-friendly).
     */
    virtual void on_before_pop();

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
    [[nodiscard]] Transaction *parent() const;

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
    [[nodiscard]] Transaction *next_transaction();

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
    [[nodiscard]] ISqlQuery *next_query();

    /**
     * @brief Removes and returns the next query from the queue
     *
     * @return ISqlQuery* Pointer to the removed query or nullptr if empty
     */
    std::unique_ptr<ISqlQuery> pop_query();

    /**
     * @brief Fail and drain every still-queued query and sub-transaction.
     *
     * On a lost connection only the single in-flight query is failed by the
     * driver; queries queued behind it (pipelined calls, multi-statement
     * transaction blocks) and the queries of pending sub-transactions would
     * otherwise never have their error callback invoked — their callers'
     * `co_await` awaiters would suspend forever. This walks the whole subtree,
     * invoking each query's on_error so awaiters resume with the failure.
     *
     * Safe to call from on(disconnected): the coroutine completion path only
     * *schedules* a resume (it does not re-enter synchronously), and the queues
     * are swapped out before draining so a callback that enqueues new work does
     * not re-enter this traversal.
     *
     * @param err Error delivered to every drained query's on_error.
     */
    void fail_all_pending(error::db_error const &err);

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
    virtual void on_new_row_description(row_description_type &&);

    /**
     * @brief Called when a query returns a data row
     *
     * @param data Row data from the result
     */
    virtual void on_new_data_row(row_data &&);

    /**
     * @brief Called when a CommandComplete message is received
     *
     * Stores the command tag in the current result set so that
     * rows_affected() can be queried by the application.
     *
     * @param tag CommandComplete tag (e.g. "INSERT 0 5", "SELECT 10")
     */
    virtual void on_command_complete(const std::string &tag);

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
    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &begin(CB_SUCCESS &&on_success, CB_ERROR &&on_error, transaction_mode mode = {});

    /**
     * @brief Begins a new transaction with only a success callback
     *
     * @tparam CB_SUCCESS Type of success callback function
     * @param on_success Callback called when transaction starts successfully
     * @param mode Optional transaction mode settings
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS>
    Transaction &begin(CB_SUCCESS &&on_success, transaction_mode mode = {});

    /**
     * @brief Begins a transaction without callbacks (`co_await`; same SQL as callback `begin`).
     */
    [[nodiscard]] pg_reply_awaiter<resultset> begin();

    [[nodiscard]] pg_reply_awaiter<resultset> begin(transaction_mode mode);

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
    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &savepoint(std::string_view name, CB_SUCCESS &&on_success, CB_ERROR &&on_error);

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
     * @brief Creates a savepoint without callbacks (`co_await`).
     */
    [[nodiscard]] pg_reply_awaiter<resultset> savepoint(std::string_view name);

    [[nodiscard]] pg_reply_awaiter<resultset> rollback_savepoint(std::string_view name);

    [[nodiscard]] pg_reply_awaiter<resultset> release_savepoint(std::string_view name);

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
    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &execute(std::string_view expr, CB_SUCCESS &&on_success, CB_ERROR &&on_error);

    /**
     * @brief Executes a SQL query with only a success callback
     *
     * @tparam CB_SUCCESS Type of success callback function
     * @param expr SQL query to execute
     * @param on_success Callback called when query executes successfully
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS>
    Transaction &execute(std::string_view expr, CB_SUCCESS &&on_success);

    /**
     * @brief Executes SQL for coroutines only (`co_await` → Reply<resultset>).
     *
     * Synchronous blocking: use the callback overload with `qb::pg::discard_query` and
     * `qb::pg::discard_error`, then `Transaction::await()`.
     */
    [[nodiscard]] pg_reply_awaiter<resultset> execute(std::string_view expr);

    /**
     * @brief Simple-query protocol for coroutines (same as `execute(sql)`).
     *
     * Lets `with_transaction` bodies use `co_await tr.query("SELECT …")` on `Transaction&`.
     */
    [[nodiscard]] pg_reply_awaiter<resultset>
    query(std::string_view sql) {
        return execute(sql);
    }

    /**
     * @brief Sends NOTIFY (publisher side; use a normal `database` connection).
     *
     * Builds safe `NOTIFY "channel" [, 'payload']` SQL. Empty @p payload omits the payload
     * clause (server default). Payload length is capped (see `notify_payload_max_bytes`).
     */
    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &notify(std::string_view channel, std::string_view payload, CB_SUCCESS &&on_success,
                        CB_ERROR &&on_error);

    /**
     * @brief NOTIFY without payload (same as `notify(channel, "", cb, err)` but omits payload in
     * SQL).
     */
    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &notify(std::string_view channel, CB_SUCCESS &&on_success, CB_ERROR &&on_error);

    /** @brief Coroutine NOTIFY (`co_await` → `Reply<void>`). Empty @p payload omits payload in
     * SQL. */
    [[nodiscard]] pg_reply_awaiter<void> notify(std::string_view channel,
                                                std::string_view payload = {});

    /**
     * @brief LISTEN on a channel (SQL `LISTEN "name"`).
     */
    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &listen(std::string_view channel, CB_SUCCESS &&on_success, CB_ERROR &&on_error);

    [[nodiscard]] pg_reply_awaiter<void> listen(std::string_view channel);

    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &unlisten(std::string_view channel, CB_SUCCESS &&on_success, CB_ERROR &&on_error);

    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &unlisten_all(CB_SUCCESS &&on_success, CB_ERROR &&on_error);

    [[nodiscard]] pg_reply_awaiter<void> unlisten(std::string_view channel);
    [[nodiscard]] pg_reply_awaiter<void> unlisten_all();

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
    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &prepare(std::string_view query_name, std::string_view expr,
                         type_oid_sequence &&types, CB_SUCCESS &&on_success, CB_ERROR &&on_error);

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
    template <typename CB_SUCCESS>
    Transaction &prepare(std::string_view query_name, std::string_view expr,
                         type_oid_sequence &&types, CB_SUCCESS &&on_success);

    /**
     * @brief Prepares for coroutines only (`co_await` → Reply<PreparedQuery>).
     *
     * Synchronous blocking: use `prepare(..., discard_prepare, discard_error)` then
     * `Transaction::await()`.
     */
    [[nodiscard]] pg_reply_awaiter<PreparedQuery>
    prepare(std::string_view query_name, std::string_view expr, type_oid_sequence types = {});

    /**
     * @brief Prepares a SQL query from a file with parameter types and callbacks
     *
     * @tparam CB_SUCCESS Type of success callback function
     * @tparam CB_ERROR Type of error callback function
     * @param query_name Name for the prepared query
     * @param file_path Path to the file containing the SQL query
     * @param types Sequence of parameter types
     * @param on_success Callback called when query is prepared successfully
     * @param on_error Callback called if query preparation fails
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &prepare_file(std::string_view query_name, const std::filesystem::path &file_path,
                              type_oid_sequence &&types, CB_SUCCESS &&on_success,
                              CB_ERROR &&on_error);

    /**
     * @brief Prepares a SQL query from a file with parameter types and success callback
     *
     * @tparam CB_SUCCESS Type of success callback function
     * @param query_name Name for the prepared query
     * @param file_path Path to the file containing the SQL query
     * @param types Sequence of parameter types
     * @param on_success Callback called when query is prepared successfully
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS>
    Transaction &prepare_file(std::string_view query_name, const std::filesystem::path &file_path,
                              type_oid_sequence &&types, CB_SUCCESS &&on_success);

    /**
     * @brief Prepare from file for coroutines only (`co_await` → Reply<PreparedQuery>).
     *
     * Synchronous blocking: `prepare_file(..., types, discard_prepare, discard_error)` then
     * `Transaction::await()`.
     */
    [[nodiscard]] pg_reply_awaiter<PreparedQuery>
    prepare_file(std::string_view query_name, const std::filesystem::path &file_path,
                 type_oid_sequence types = {});

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
    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &execute(std::string_view query_name, QueryParams &&params, CB_SUCCESS &&on_success,
                         CB_ERROR &&on_error);

    /**
     * @brief Executes a prepared query with parameters and success callback
     *
     * @tparam CB_SUCCESS Type of success callback function
     * @param query_name Name of the prepared query to execute
     * @param params Parameters for the prepared query
     * @param on_success Callback called when query executes successfully
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS>
    Transaction &execute(std::string_view query_name, QueryParams &&params, CB_SUCCESS &&on_success);

    /**
     * @brief Executes a prepared query with parameters and success callback (alternative
     * syntax)
     *
     * @tparam CB_SUCCESS Type of success callback function
     * @param query_name Name of the prepared query to execute
     * @param on_success Callback called when query executes successfully
     * @param params Parameters for the prepared query
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS>
    Transaction &execute(std::string_view query_name, CB_SUCCESS &&on_success, QueryParams &&params);

    /**
     * @brief Executes a prepared statement for coroutines only (`co_await`).
     *
     * Synchronous blocking: `execute(name, params, discard_query, discard_error)` then
     * `Transaction::await()`.
     */
    [[nodiscard]] pg_reply_awaiter<resultset> execute(std::string_view query_name,
                                                      QueryParams    &&params);

    /**
     * @brief Executes a SQL query from a file
     *
     * @tparam CB_SUCCESS Type of success callback function
     * @tparam CB_ERROR Type of error callback function
     * @param file_path Path to the file containing the SQL query
     * @param on_success Callback called when query is executed successfully
     * @param on_error Callback called if query execution fails
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &execute_file(const std::filesystem::path &file_path, CB_SUCCESS &&on_success,
                              CB_ERROR &&on_error);

    /**
     * @brief Executes a SQL query from a file with success callback
     *
     * @tparam CB_SUCCESS Type of success callback function
     * @param file_path Path to the file containing the SQL query
     * @param on_success Callback called when query is executed successfully
     * @return Transaction& Reference to this transaction for chaining
     */
    template <typename CB_SUCCESS>
    Transaction &execute_file(const std::filesystem::path &file_path, CB_SUCCESS &&on_success);

    /**
     * @brief Execute SQL from file for coroutines only (`co_await` → Reply<resultset>).
     *
     * Synchronous blocking: `execute_file(path, discard_query, discard_error)` then
     * `Transaction::await()`.
     */
    [[nodiscard]] pg_reply_awaiter<resultset> execute_file(const std::filesystem::path &file_path);

    /**
     * @brief Set PostgreSQL **statement_timeout** for the **next** `BEGIN` on this connection.
     *
     * When @p timeout is positive, the following `begin()` (callback or `co_await`) sends
     * `SET LOCAL statement_timeout = N` in the **same** simple-query round-trip as `BEGIN`,
     * so the limit is **transaction-scoped** and cleared at `COMMIT`/`ROLLBACK`. Call **before**
     * `begin()`; use `0` to omit (server default for new transactions).
     *
     * @param timeout Statement timeout as a `qb::duration` (zero or negative disables it)
     * @return Transaction& Reference to this transaction for chaining
     */
    Transaction &
    set_timeout(qb::duration timeout) {
        _query_timeout_ms =
            timeout > qb::duration::zero()
                ? static_cast<int>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count())
                : 0;
        return *this;
    }

    /**
     * @brief Statement timeout applied by the next `SET LOCAL statement_timeout` with `begin()`
     *        (`qb::duration::zero()` = none).
     */
    [[nodiscard]] qb::duration
    get_timeout() const {
        return std::chrono::milliseconds(_query_timeout_ms);
    }

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
    [[nodiscard]] const error::db_error &error() const;

    /**
     * @brief Gets the last results of the transaction
     *
     * @return result_impl& Reference to the last results
     */
    result_impl &results();

    class status {
        friend class Transaction;

        result_impl     _results;
        error::db_error _error{"unknown error"};
        /// Reflects `Transaction::_result` after the work queue drained (set in `await()`).
        bool _command_ok{true};

    public:
        status() = default;

        ~status() = default;

        status(status &) = default;

        status(status &&) = default;

        status &operator=(status &) = default;

        status &operator=(status &&) = default;

        status(result_impl results, error::db_error error, bool command_ok)
            : _results(std::move(results))
            , _error(std::move(error))
            , _command_ok(command_ok) {}

        /**
         * True when the command batch completed without a failed sub-result and without a
         * PostgreSQL / client error on `_error` (SQLSTATE still `unknown_code` for success).
         */
        [[nodiscard]] explicit
        operator bool() const {
            return _command_ok && _error.sqlstate == sqlstate::unknown_code;
        }

        [[nodiscard]] bool
        operator()() const {
            return static_cast<bool>(*this);
        }

        [[nodiscard]] resultset
        results() {
            return {&_results};
        }

        [[nodiscard]] error::db_error &
        error() {
            return _error;
        }
    };

    /**
     * @brief Commits the current transaction without callbacks (`co_await`).
     */
    [[nodiscard]] pg_reply_awaiter<resultset> commit();

    /**
     * @brief Rolls back the current transaction without callbacks (`co_await`).
     */
    [[nodiscard]] pg_reply_awaiter<resultset> rollback();

    status await();
};

} // namespace qb::pg::detail

namespace qb::pg {
inline detail::Transaction::status
await(detail::Transaction &t) {
    return t.await();
}
} // namespace qb::pg