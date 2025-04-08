/**
 * @file commands.h
 * @brief PostgreSQL transaction command implementations
 *
 * This file implements the concrete command classes that represent
 * various database operations within transactions. These command classes
 * encapsulate the logic for executing specific types of SQL operations
 * while maintaining transaction state and handling callbacks.
 *
 * The implementation follows a command pattern, where each operation
 * is represented by a specialized command class that inherits from
 * the Transaction base class.
 *
 * @see qb::pg::detail::Transaction
 * @see qb::pg::detail::ISqlQuery
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

#include "./result_impl.h"
#include "./resultset.h"
#include "./transaction.h"

namespace qb::pg::detail {
using namespace qb::pg;

/**
 * @brief Command for ending a transaction
 *
 * Represents the final phase of a transaction, responsible for
 * committing or rolling back the transaction based on its result status.
 *
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_ERROR>
class End final : public Transaction {
    CB_ERROR _on_error; ///< Error callback

public:
    /**
     * @brief Constructs an End command
     *
     * @param parent Parent transaction
     * @param on_error Callback for error handling
     */
    End(Transaction *parent, CB_ERROR &&on_error)
        : Transaction(parent)
        , _on_error(std::forward<CB_ERROR>(on_error)) {}

    /**
     * @brief Gets the error callback
     *
     * @return CB_ERROR& Reference to the error callback
     */
    CB_ERROR &
    get_error_callback() {
        return _on_error;
    }

    /**
     * @brief Initiates the transaction end sequence
     *
     * Creates and queues either a COMMIT or ROLLBACK query
     * based on the transaction's result status.
     */
    void
    on_end_transaction() {
        push_query(_result ? std::unique_ptr<ISqlQuery>(new CommitQuery(
                                 []() {
                                     // commited
                                 },
                                 [this](auto const &err) { _on_error(err); }))
                           : std::unique_ptr<ISqlQuery>(new RollbackQuery(
                                 [this]() {
                                     _on_error((error::db_error) error::query_error(
                                         "rollback processed due to a query failure"));
                                 },
                                 [this](auto const &err) { _on_error(err); })));
    }
};

/**
 * @brief Command for beginning a transaction
 *
 * Initiates a new transaction with the specified mode and
 * manages its lifecycle, including setting up the end command.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class Begin final : public Transaction {
    End<CB_ERROR>   *_end;        ///< End command for this transaction
    transaction_mode _mode;       ///< Transaction mode (isolation level, etc.)
    CB_SUCCESS       _on_success; ///< Success callback

public:
    /**
     * @brief Constructs a Begin command
     *
     * @param parent Parent transaction
     * @param end End command for this transaction
     * @param mode Transaction mode
     * @param on_success Callback for successful transaction start
     */
    Begin(Transaction *parent, End<CB_ERROR> *end, transaction_mode mode,
          CB_SUCCESS &&on_success)
        : Transaction(parent)
        , _end(end)
        , _mode(mode)
        , _on_success(std::forward<CB_SUCCESS>(on_success)) {
        push_query(std::unique_ptr<ISqlQuery>(new BeginQuery(
            mode,
            [this]() {
                try {
                    _on_success(*this);
                } catch (std::exception const &e) {
                    _result = false;
                    _end->get_error_callback()(
                        (error::db_error) error::client_error{e.what()});
                }
            },
            [this](auto &&err) { _end->get_error_callback()(err); })));
    }

    /**
     * @brief Destructor
     *
     * Finalizes the transaction by updating the end command's
     * result status and triggering the end sequence.
     */
    ~Begin() {
        _end->result(_result);
        _end->on_end_transaction();
    }

    /**
     * @brief Handles sub-command status updates
     *
     * Updates this transaction's result status based on
     * the status of sub-commands.
     *
     * @param status Result status of the sub-command
     */
    void
    on_sub_command_status(bool status) final {
        _result &= status;
    }
};

/**
 * @brief Command for ending a savepoint
 *
 * Represents the final phase of a savepoint operation, responsible for
 * releasing or rolling back to the savepoint based on its result status.
 *
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_ERROR>
class EndSavePoint final : public Transaction {
    const std::string _name;                  ///< Savepoint name
    CB_ERROR          _on_error;              ///< Error callback
    bool              _force_rollback{false}; ///< Flag to force rollback

public:
    /**
     * @brief Constructs an EndSavePoint command
     *
     * @param parent Parent transaction
     * @param name Savepoint name
     * @param on_error Callback for error handling
     */
    EndSavePoint(Transaction *parent, std::string &&name, CB_ERROR &&on_error)
        : Transaction(parent)
        , _name(std::move(name))
        , _on_error(std::forward<CB_ERROR>(on_error)) {}

    /**
     * @brief Gets the savepoint name
     *
     * @return std::string const& Reference to the savepoint name
     */
    std::string const &
    get_name() {
        return _name;
    }

    /**
     * @brief Gets the error callback
     *
     * @return CB_ERROR& Reference to the error callback
     */
    CB_ERROR &
    get_error_callback() {
        return _on_error;
    }

    /**
     * @brief Force rollback of this savepoint
     *
     * This method can be called to explicitly force rollback
     * of the savepoint regardless of the result status.
     */
    void
    force_rollback() {
        _force_rollback = true;
    }

    /**
     * @brief Initiates the savepoint end sequence
     *
     * Creates and queues either a RELEASE or ROLLBACK TO savepoint query
     * based on the transaction's result status.
     */
    void
    on_end_savepoint() {
        bool should_release = _result && !_force_rollback;
        push_query(should_release
                       ? std::unique_ptr<ISqlQuery>(new ReleaseSavePointQuery(
                             _name,
                             []() {
                                 // committed
                             },
                             [this](auto const &err) { _on_error(err); }))
                       : std::unique_ptr<ISqlQuery>(new RollbackSavePointQuery(
                             _name,
                             [this]() {
                                 _on_error((error::db_error) error::query_error(
                                     "savepoint rollback processed due to a "
                                     "query failure"));
                             },
                             [this](auto const &err) { _on_error(err); })));
    }
};

/**
 * @brief Command for creating a savepoint
 *
 * Creates a new savepoint within a transaction and manages its lifecycle,
 * including setting up the end savepoint command.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class SavePoint final : public Transaction {
    EndSavePoint<CB_ERROR> *_end;        ///< End command for this savepoint
    CB_SUCCESS              _on_success; ///< Success callback

public:
    /**
     * @brief Constructs a SavePoint command
     *
     * @param parent Parent transaction
     * @param end End command for this savepoint
     * @param on_success Callback for successful savepoint creation
     */
    SavePoint(Transaction *parent, EndSavePoint<CB_ERROR> *end, CB_SUCCESS &&on_success)
        : Transaction(parent)
        , _end(end)
        , _on_success(std::forward<CB_SUCCESS>(on_success)) {
        push_query(std::unique_ptr<ISqlQuery>(new SavePointQuery(
            _end->get_name(),
            [this]() {
                try {
                    _on_success(*this);
                } catch (std::exception const &e) {
                    _result = false;
                    _end->force_rollback(); // Force rollback on exception
                    _end->get_error_callback()(
                        (error::db_error) error::client_error{e.what()});
                }
            },
            [this](auto const &err) {
                _result = false;        // Mark explicitly as failed on error
                _end->force_rollback(); // Force rollback on SQL error
                _end->get_error_callback()(err);
            })));
    }

    /**
     * @brief Destructor
     *
     * Finalizes the savepoint by updating the end command's
     * result status and triggering the end sequence.
     */
    ~SavePoint() {
        _end->result(_result);
        _end->on_end_savepoint();
    }

    /**
     * @brief Handles sub-command status updates
     *
     * Updates this savepoint's result status and propagates
     * the status to the parent transaction.
     *
     * @param status Result status of the sub-command
     */
    void
    on_sub_command_status(bool status) final {
        _result &= status;
        if (!status) {
            _end->force_rollback(); // Force rollback on sub-command failure
        }
        _parent->on_sub_command_status(status);
    }
};

/**
 * @brief Command for executing a simple query
 *
 * Executes a raw SQL expression and handles the callbacks.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class Query final : public Transaction {
    CB_SUCCESS _on_success; ///< Success callback
    CB_ERROR   _on_error;   ///< Error callback

public:
    /**
     * @brief Constructs a Query command
     *
     * @param parent Parent transaction
     * @param expr SQL expression to execute
     * @param on_success Callback for successful query execution
     * @param on_error Callback for query execution errors
     */
    Query(Transaction *parent, std::string &&expr, CB_SUCCESS &&on_success,
          CB_ERROR &&on_error)
        : Transaction(parent)
        , _on_success(std::forward<CB_SUCCESS>(on_success))
        , _on_error(std::forward<CB_ERROR>(on_error)) {
        push_query(std::unique_ptr<ISqlQuery>(new SimpleQuery(
            std::move(expr),
            [this]() {
                try {
                    _on_success(*this);
                } catch (std::exception const &e) {
                    _result = false;
                    _on_error((error::db_error) error::client_error{e.what()});
                    if (_parent)
                        _parent->on_sub_command_status(
                            false); // Propager l'erreur au parent
                }
            },
            [this](auto const &err) {
                _result = false;
                _on_error(err);
                if (_parent)
                    _parent->on_sub_command_status(false); // Propager l'erreur au parent
            })));
    }

    //    void
    //    on_sub_command_status(bool status) final {
    //        _parent->on_sub_command_status(status);
    //    }
};

/**
 * @brief Command for executing a query that returns results
 *
 * Executes a SQL query and collects the result set for processing.
 *
 * @tparam CB_SUCCESS Type of success callback that receives the result set
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class ResultQuery final : public Transaction {
    CB_SUCCESS  _on_success; ///< Success callback
    CB_ERROR    _on_error;   ///< Error callback
    result_impl _results;    ///< Result data storage

public:
    /**
     * @brief Constructs a ResultQuery command
     *
     * @param parent Parent transaction
     * @param expr SQL expression to execute
     * @param on_success Callback for successful query execution with results
     * @param on_error Callback for query execution errors
     */
    ResultQuery(Transaction *parent, std::string &&expr, CB_SUCCESS &&on_success,
                CB_ERROR &&on_error)
        : Transaction(parent)
        , _on_success(std::forward<CB_SUCCESS>(on_success))
        , _on_error(std::forward<CB_ERROR>(on_error)) {
        push_query(std::unique_ptr<ISqlQuery>(new SimpleQuery(
            std::move(expr),
            [this]() {
                try {
                    _on_success(*this, resultset(&_results));
                    _parent->results() = std::move(_results);
                } catch (std::exception const &e) {
                    _result = false;
                    _on_error((error::db_error) error::client_error{e.what()});
                    if (_parent)
                        _parent->on_sub_command_status(false);
                }
            },
            [this](auto const &err) {
                _result = false;
                _on_error(err);
                if (_parent)
                    _parent->on_sub_command_status(false);
            })));
    }

    /**
     * @brief Handles row description from the query result
     *
     * Stores the row description metadata for the result set.
     *
     * @param desc Row description metadata
     */
    void
    on_new_row_description(row_description_type &&desc) final {
        _results.row_description() = std::move(desc);
    };

    /**
     * @brief Handles a data row from the query result
     *
     * Adds a row of data to the result set.
     *
     * @param data Row data
     */
    void
    on_new_data_row(row_data &&data) final {
        _results.rows().push_back(std::move(data));
    }
};

/**
 * @brief Command for chaining operations
 *
 * Executes a callback after the previous operation if it was successful.
 * This allows for creating chains of dependent operations where each step
 * only executes if the previous operation completed successfully.
 *
 * @tparam CB_SUCCESS Type of success callback
 */
template <typename CB_SUCCESS>
class Then final : public Transaction {
    CB_SUCCESS _on_success; ///< Success callback

public:
    /**
     * @brief Constructs a Then command
     *
     * @param parent Parent transaction
     * @param on_success Callback to execute if the parent's result is successful
     */
    Then(Transaction *parent, CB_SUCCESS &&on_success)
        : Transaction(parent)
        , _on_success(std::forward<CB_SUCCESS>(on_success)) {}

    /**
     * @brief Destructor
     *
     * Executes the success callback if the parent transaction
     * has a successful result status. Any exceptions thrown from the callback
     * will be caught and will cause the parent transaction to be marked as failed.
     */
    ~Then() {
        if (!parent()->result())
            return;
        try {
            _on_success(*(parent()));
        } catch (...) {
            if (parent() && parent()->parent()) {
                parent()->result(false);
            }
        }
    }
};

/**
 * @brief Command for handling errors
 *
 * Executes a callback when an error occurs in the transaction. This provides
 * a mechanism for error handling and recovery within the transaction flow.
 * The error callback is only invoked if the parent transaction has failed.
 *
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_ERROR>
class Error final : public Transaction {
    CB_ERROR _on_error; ///< Error callback

public:
    /**
     * @brief Constructs an Error command
     *
     * @param parent Parent transaction
     * @param on_error Callback to execute if the parent's result is unsuccessful
     */
    Error(Transaction *parent, CB_ERROR &&on_error)
        : Transaction(parent)
        , _on_error(std::forward<CB_ERROR>(on_error)) {}

    /**
     * @brief Destructor
     *
     * Executes the error callback if the parent transaction
     * has an unsuccessful result status. Any exceptions thrown from the callback
     * will be caught and will cause the parent transaction to be marked as failed.
     */
    ~Error() {
        if (parent()->result())
            return;

        try {
            _on_error(*(parent()));
        } catch (...) {
            if (parent() && parent()->parent()) {
                parent()->result(false);
            }
        }
    }
};

/**
 * @brief Command for preparing a named query
 *
 * Prepares a SQL statement with the specified name and parameter types.
 * Stores the prepared query in the query storage for future execution.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class Prepare final : public Transaction {
    PreparedQuery _query;      ///< Query to prepare
    CB_SUCCESS    _on_success; ///< Success callback
    CB_ERROR      _on_error;   ///< Error callback

public:
    /**
     * @brief Constructs a Prepare command
     *
     * @param parent Parent transaction
     * @param query Prepared query definition
     * @param on_success Callback for successful preparation
     * @param on_error Callback for preparation errors
     */
    Prepare(Transaction *parent, PreparedQuery &&query, CB_SUCCESS &&on_success,
            CB_ERROR &&on_error)
        : Transaction(parent)
        , _query(std::move(query))
        , _on_success(std::forward<CB_SUCCESS>(on_success))
        , _on_error(std::forward<CB_ERROR>(on_error)) {
        push_query(std::unique_ptr<ISqlQuery>(new ParseQuery(
            _query,
            [this]() {
                try {
                    _on_success(*this, _query_storage.push(std::move(_query)));
                } catch (std::exception const &e) {
                    _result = false;
                    _on_error((error::db_error) error::client_error{e.what()});
                }
            },
            [this](auto const &err) { _on_error(err); })));
    }

    /**
     * @brief Handles row description from query preparation
     *
     * Stores the row description metadata in the prepared query.
     *
     * @param desc Row description metadata
     */
    void
    on_new_row_description(row_description_type &&desc) {
        _query.row_description = std::move(desc);
    }
};

/**
 * @brief Command for executing a prepared query
 *
 * Executes a previously prepared query with the specified parameters.
 * Does not collect result rows.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class ExecutePrepared final : public Transaction {
    const std::string _query_name; ///< Name of the prepared query
    CB_SUCCESS        _on_success; ///< Success callback
    CB_ERROR          _on_error;   ///< Error callback

public:
    /**
     * @brief Constructs an ExecutePrepared command
     *
     * @param parent Parent transaction
     * @param query_name Name of the prepared query
     * @param params Parameter values for the query
     * @param on_success Callback for successful execution
     * @param on_error Callback for execution errors
     */
    ExecutePrepared(Transaction *parent, std::string &&query_name, QueryParams &&params,
                    CB_SUCCESS &&on_success, CB_ERROR &&on_error)
        : Transaction(parent)
        , _query_name(std::move(query_name))
        , _on_success(std::forward<CB_SUCCESS>(on_success))
        , _on_error(std::forward<CB_ERROR>(on_error)) {
        push_query(std::unique_ptr<ISqlQuery>(new ExecuteQuery(
            _query_storage, _query_name, std::move(params),
            [this]() {
                try {
                    _on_success(*this);
                } catch (std::exception const &e) {
                    _result = false;
                    _on_error((error::db_error) error::client_error{e.what()});
                }
            },
            [this](auto const &err) { _on_error(err); })));
    }
};

/**
 * @brief Command for executing a prepared query with result retrieval
 *
 * Executes a previously prepared query with the specified parameters
 * and collects the result rows for processing.
 *
 * @tparam CB_SUCCESS Type of success callback that receives the result set
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class QueryPrepared final : public Transaction {
    CB_SUCCESS        _on_success; ///< Success callback
    CB_ERROR          _on_error;   ///< Error callback
    const std::string _query_name; ///< Name of the prepared query
    result_impl       _results;    ///< Result data storage

public:
    /**
     * @brief Constructs a QueryPrepared command
     *
     * @param parent Parent transaction
     * @param query_name Name of the prepared query
     * @param params Parameter values for the query
     * @param on_success Callback for successful execution with results
     * @param on_error Callback for execution errors
     */
    QueryPrepared(Transaction *parent, std::string const &query_name,
                  QueryParams &&params, CB_SUCCESS &&on_success, CB_ERROR &&on_error)
        : Transaction(parent)
        , _on_success(std::forward<CB_SUCCESS>(on_success))
        , _on_error(std::forward<CB_ERROR>(on_error))
        , _query_name(query_name) {
        push_query(std::unique_ptr<ISqlQuery>(new ExecuteQuery(
            _query_storage, _query_name, std::move(params),
            [this]() {
                try {
                    _results.row_description() =
                        _query_storage.get(_query_name).row_description;
                    _on_success(*this, resultset(&_results));
                    _parent->results() = std::move(_results);
                } catch (std::exception const &e) {
                    _result = false;
                    _on_error((error::db_error) error::client_error{e.what()});
                }
            },
            [this](auto const &err) { _on_error(err); })));
    }

    /**
     * @brief Handles a data row from the query result
     *
     * Adds a row of data to the result set.
     *
     * @param data Row data
     */
    void
    on_new_data_row(row_data &&data) final {
        _results.rows().push_back(std::move(data));
    }
};

} // namespace qb::pg::detail

#include "./transaction.inl"