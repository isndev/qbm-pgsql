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
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
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
        push_query(_result ? std::unique_ptr<ISqlQuery>(new CommitQuery([]() {}, [this](auto const &err) { _on_error(err); }))
                           : std::unique_ptr<ISqlQuery>(new RollbackQuery(
                                 [this]() { _on_error((error::db_error) error::query_error("rollback processed due to a query failure")); },
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
    Begin(Transaction *parent, End<CB_ERROR> *end, transaction_mode mode, CB_SUCCESS &&on_success)
        : Transaction(parent)
        , _end(end)
        , _mode(mode)
        , _on_success(std::forward<CB_SUCCESS>(on_success)) {
        push_query(std::unique_ptr<ISqlQuery>(new BeginQuery(
            mode, parent->get_timeout(),
            [this]() {
                try {
                    _on_success(*this);
                } catch (std::exception const &e) {
                    _result = false;
                    _end->get_error_callback()((error::db_error) error::client_error{e.what()});
                }
            },
            [this](auto &&err) { _end->get_error_callback()(err); })));
    }

    /**
     * @brief Finalizes the begin/end pair when this command leaves the queue
     *
     * Replaces destructor-driven commit scheduling for predictable async semantics.
     */
    void
    on_before_pop() override {
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
        push_query(should_release ? std::unique_ptr<ISqlQuery>(new ReleaseSavePointQuery(
                                        _name, []() {}, [this](auto const &err) { _on_error(err); }))
                                  : std::unique_ptr<ISqlQuery>(new RollbackSavePointQuery(
                                        _name,
                                        [this]() {
                                            _on_error((error::db_error) error::query_error("savepoint rollback processed due to a "
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
                    _end->get_error_callback()((error::db_error) error::client_error{e.what()});
                }
            },
            [this](auto const &err) {
                _result = false;        // Mark explicitly as failed on error
                _end->force_rollback(); // Force rollback on SQL error
                _end->get_error_callback()(err);
            })));
    }

    /**
     * @brief Finalizes the savepoint/end pair when this command leaves the queue
     */
    void
    on_before_pop() override {
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
    Query(Transaction *parent, std::string &&expr, CB_SUCCESS &&on_success, CB_ERROR &&on_error)
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
    ResultQuery(Transaction *parent, std::string &&expr, CB_SUCCESS &&on_success, CB_ERROR &&on_error)
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
     * @param data Row data
     */
    void
    on_new_data_row(row_data &&data) final {
        _results.rows().push_back(std::move(data));
    }

    /**
     * @brief Stores the CommandComplete tag for rows_affected() reporting
     *
     * @param tag CommandComplete tag string
     */
    void
    on_command_complete(const std::string &tag) final {
        _results.set_command_tag(tag);
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
            _on_error((parent()->error()));
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
    Prepare(Transaction *parent, PreparedQuery &&query, CB_SUCCESS &&on_success, CB_ERROR &&on_error)
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
    ExecutePrepared(Transaction *parent, std::string &&query_name, QueryParams &&params, CB_SUCCESS &&on_success, CB_ERROR &&on_error)
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
    QueryPrepared(Transaction *parent, std::string const &query_name, QueryParams &&params, CB_SUCCESS &&on_success, CB_ERROR &&on_error)
        : Transaction(parent)
        , _on_success(std::forward<CB_SUCCESS>(on_success))
        , _on_error(std::forward<CB_ERROR>(on_error))
        , _query_name(query_name) {
        push_query(std::unique_ptr<ISqlQuery>(new ExecuteQuery(
            _query_storage, _query_name, std::move(params),
            [this]() {
                try {
                    _results.row_description() = _query_storage.get(_query_name).row_description;
                    sync_field_format_codes_with_extended_query_bind(_results.row_description());
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
     * @brief Handles a data row from the query result
     *
     * @param data Row data
     */
    void
    on_new_data_row(row_data &&data) final {
        _results.rows().push_back(std::move(data));
    }

    /**
     * @brief Stores the CommandComplete tag for rows_affected() reporting
     *
     * @param tag CommandComplete tag string
     */
    void
    on_command_complete(const std::string &tag) final {
        _results.set_command_tag(tag);
    }
};

} // namespace qb::pg::detail

// ---------------------------------------------------------------------------
// Template + inline definitions for qb::pg::detail::Transaction, merged here
// from the retired transaction.inl and transaction_coro.inl (3.0).
//
// They live at the tail of commands.h rather than transaction.h because
// commands.h is the header that CLOSES the cycle: transaction.h (included at
// :26 above) only declares Transaction, while the bodies below need the
// complete command types defined above. This is byte-for-byte the position the
// `#include "./transaction.inl"` that used to occupy this line spliced them
// into, so the preprocessed token stream of commands.h is unchanged.
//
// The include block below is the UNION of the two retired files' blocks.
// transaction_coro.inl's seven includes used to be spliced INSIDE
// `namespace qb::pg::detail` and were no-ops only because transaction.inl's own
// block had already pulled the same headers in first. That is a live trap, not
// a style point: deleting <fstream> from this block was measured to reparse
// <fstream> inside the namespace and emit 20 errors led by
// "no template named 'basic_streambuf'; did you mean '::std::basic_streambuf'?".
// Hoisting them here makes that unreachable.
//
// Not carried over: each merged file's own `#pragma once` and transaction.inl's
// self-include of commands.h. Both were no-ops at this point.
// See dev/analysis/TEMPLATE-LINKAGE-AUDIT-3.0.md.
// ---------------------------------------------------------------------------

#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "./pg_awaiter.h"
#include "./pg_notify_sql.h"
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
template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::begin(CB_SUCCESS &&on_success, CB_ERROR &&on_error, transaction_mode mode) {
    if (_parent) {
        on_error((error::db_error) error::query_error("already in transaction"));
    } else {
        auto end = new End<CB_ERROR>(this, std::forward<CB_ERROR>(on_error));
        push_transaction(std::unique_ptr<Transaction>(new Begin<CB_SUCCESS, CB_ERROR>(this, end, mode, std::forward<CB_SUCCESS>(on_success))));
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
    return begin(std::forward<CB_SUCCESS>(on_success), [](error::db_error const &) {}, mode);
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
template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::savepoint(std::string_view name, CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    auto end = new EndSavePoint<CB_ERROR>(this, std::string(name), std::forward<CB_ERROR>(on_error));
    push_transaction(std::unique_ptr<Transaction>(new SavePoint<CB_SUCCESS, CB_ERROR>(this, end, std::forward<CB_SUCCESS>(on_success))));
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
template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::execute(std::string_view expr, CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &, resultset>) {
        push_transaction(std::unique_ptr<Transaction>(new ResultQuery<CB_SUCCESS, CB_ERROR>(
            this, std::string(expr), std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error))));
    } else if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &>) {
        push_transaction(std::unique_ptr<Transaction>(
            new Query<CB_SUCCESS, CB_ERROR>(this, std::string(expr), std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error))));
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
template <typename CB_SUCCESS>
Transaction &
Transaction::execute(std::string_view expr, CB_SUCCESS &&on_success) {
    return execute(std::string(expr), std::forward<CB_SUCCESS>(on_success), [](error::db_error const &) {});
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
template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::prepare(std::string_view query_name, std::string_view expr, type_oid_sequence &&types, CB_SUCCESS &&on_success,
                     CB_ERROR &&on_error) {
    PreparedQuery query{std::string(query_name), std::string(expr), std::move(types), {}};

    push_transaction(std::unique_ptr<Transaction>(
        new Prepare<CB_SUCCESS, CB_ERROR>(this, std::move(query), std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error))));
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
template <typename CB_SUCCESS>
Transaction &
Transaction::prepare(std::string_view query_name, std::string_view expr, type_oid_sequence &&types, CB_SUCCESS &&on_success) {
    return prepare(query_name, expr, std::move(types), std::forward<CB_SUCCESS>(on_success), [](error::db_error const &) {});
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
template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::execute(std::string_view query_name, QueryParams &&params, CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &, resultset>) {
        push_transaction(std::unique_ptr<Transaction>(new QueryPrepared<CB_SUCCESS, CB_ERROR>(
            this, std::string(query_name), std::move(params), std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error))));
    } else if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &>) {
        push_transaction(std::unique_ptr<Transaction>(new ExecutePrepared<CB_SUCCESS, CB_ERROR>(
            this, std::string(query_name), std::move(params), std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error))));
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
template <typename CB_SUCCESS>
Transaction &
Transaction::execute(std::string_view query_name, QueryParams &&params, CB_SUCCESS &&on_success) {
    return execute(query_name, std::move(params), std::forward<CB_SUCCESS>(on_success), [](error::db_error const &) {});
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
template <typename CB_SUCCESS>
Transaction &
Transaction::execute(std::string_view query_name, CB_SUCCESS &&on_success, QueryParams &&params) {
    return execute(query_name, std::move(params), std::forward<CB_SUCCESS>(on_success), [](error::db_error const &) {});
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
    push_transaction(std::unique_ptr<Transaction>(new Then<CB_SUCCESS>(this, std::forward<CB_SUCCESS>(on_success))));
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
    push_transaction(std::unique_ptr<Transaction>(new Then<CB_SUCCESS>(this, std::forward<CB_SUCCESS>(on_success))));
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
    push_transaction(std::unique_ptr<Transaction>(new Error<CB_ERROR>(this, std::forward<CB_ERROR>(on_error))));
    return *this;
}

/**
 * @brief Prepares a SQL query from a file with success and error callbacks
 *
 * This method reads the SQL query from a file and prepares it with the specified
 * parameter types. If the file cannot be read, the error callback is invoked
 * and an exception is thrown.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam CB_ERROR Type of error callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param query_name Name to assign to the prepared statement
 * @param file_path Path to the file containing the SQL query
 * @param types Sequence of PostgreSQL OIDs for parameter types
 * @param on_success Callback invoked when preparation succeeds
 * @param on_error Callback invoked if preparation fails (called before exception is thrown)
 * @return Reference to this transaction for method chaining
 * @throws error::query_error If file doesn't exist, can't be opened, or there's an error reading it
 */
template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::prepare_file(std::string_view query_name, const std::filesystem::path &file_path, type_oid_sequence &&types,
                          CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    try {
        // Check if file exists
        if (!std::filesystem::exists(file_path))
            throw error::query_error("SQL file not found: " + file_path.string());

        // Open and read the file
        std::ifstream file(file_path);
        if (!file.is_open())
            throw error::query_error("Cannot open SQL file: " + file_path.string());

        // Read the entire file content into a string
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string sql_query = buffer.str();

        // Call the regular prepare method with the file content
        return prepare(query_name, sql_query, std::move(types), std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error));
    } catch (const std::exception &e) {
        auto err = error::query_error("Error reading SQL file: " + std::string(e.what()));
        on_error(err);
        throw err;
    }
}

/**
 * @brief Prepares a SQL query from a file with only success callback
 *
 * Simplified version that uses an empty error callback.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param query_name Name to assign to the prepared statement
 * @param file_path Path to the file containing the SQL query
 * @param types Sequence of PostgreSQL OIDs for parameter types
 * @param on_success Callback invoked when preparation succeeds
 * @return Reference to this transaction for method chaining
 */
template <typename CB_SUCCESS>
Transaction &
Transaction::prepare_file(std::string_view query_name, const std::filesystem::path &file_path, type_oid_sequence &&types,
                          CB_SUCCESS &&on_success) {
    return prepare_file(query_name, file_path, std::move(types), std::forward<CB_SUCCESS>(on_success), [](error::db_error const &) {});
}

/**
 * @brief Executes a SQL query from a file with success and error callbacks
 *
 * This method reads the SQL query from a file and executes it.
 * If the file cannot be read, the error callback is invoked
 * and an exception is thrown.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam CB_ERROR Type of error callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param file_path Path to the file containing the SQL query
 * @param on_success Callback invoked when execution succeeds
 * @param on_error Callback invoked if execution fails (called before exception is thrown)
 * @return Reference to this transaction for method chaining
 * @throws error::query_error If file doesn't exist, can't be opened, or there's an error reading it
 */
template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::execute_file(const std::filesystem::path &file_path, CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    try {
        // Check if file exists
        if (!std::filesystem::exists(file_path))
            throw error::query_error("SQL file not found: " + file_path.string());

        // Open and read the file
        std::ifstream file(file_path);
        if (!file.is_open())
            throw error::query_error("Cannot open SQL file: " + file_path.string());

        // Read the entire file content into a string
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string sql_query = buffer.str();

        // Call the regular execute method with the file content
        return execute(sql_query, std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error));
    } catch (const std::exception &e) {
        auto err = error::query_error("Error reading SQL file: " + std::string(e.what()));
        on_error(err);
        throw err;
    }
}

/**
 * @brief Executes a SQL query from a file with only success callback
 *
 * Simplified version that uses an empty error callback.
 *
 * @tparam CB_SUCCESS Type of success callback function
 * @tparam Dummy SFINAE enabler (not used in implementation)
 * @param file_path Path to the file containing the SQL query
 * @param on_success Callback invoked when execution succeeds
 * @return Reference to this transaction for method chaining
 * @throws error::query_error If file doesn't exist, can't be opened, or there's an error reading it
 */
template <typename CB_SUCCESS>
Transaction &
Transaction::execute_file(const std::filesystem::path &file_path, CB_SUCCESS &&on_success) {
    return execute_file(file_path, std::forward<CB_SUCCESS>(on_success), [](error::db_error const &) {});
}

template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::notify(std::string_view channel, std::string_view payload, CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    try {
        std::string sql = build_notify_sql(channel, payload);
        return execute(std::string_view(sql), std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error));
    } catch (error::db_error const &e) {
        on_error(e);
        throw;
    }
}

template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::notify(std::string_view channel, CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    try {
        std::string sql = build_notify_sql(channel, {});
        return execute(std::string_view(sql), std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error));
    } catch (error::db_error const &e) {
        on_error(e);
        throw;
    }
}

template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::listen(std::string_view channel, CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    try {
        std::string sql = build_listen_sql(channel);
        return execute(std::string_view(sql), std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error));
    } catch (error::db_error const &e) {
        on_error(e);
        throw;
    }
}

template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::unlisten(std::string_view channel, CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    try {
        std::string sql = build_unlisten_sql(channel);
        return execute(std::string_view(sql), std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error));
    } catch (error::db_error const &e) {
        on_error(e);
        throw;
    }
}

template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::unlisten_all(CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    return execute(std::string_view(build_unlisten_all_sql()), std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error));
}

// --- No-callback overloads: same names as the callback API, return awaiters ---
// Merged from the retired transaction_coro.inl. These sit INSIDE
// namespace qb::pg::detail on purpose -- the anonymous namespace below is
// qb::pg::detail::{anonymous}, and the Transaction:: bodies are unqualified.
// The include block that used to head this file has been hoisted out of the
// namespace, into the block at the top of this merge site.

namespace {

/** Append `; SET LOCAL statement_timeout = N` (ms) when @p timeout_ms &gt; 0. */
inline void
pg_append_set_local_statement_timeout(std::string &sql, qb::duration timeout) {
    const auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
    if (timeout_ms <= 0)
        return;
    sql += "; SET LOCAL statement_timeout = ";
    sql += std::to_string(timeout_ms);
}

[[nodiscard]] inline bool
pg_savepoint_name_ok(std::string_view name) noexcept {
    if (name.empty() || name.size() > 63)
        return false;
    for (unsigned char const c : name) {
        if (std::isalnum(c) != 0 || c == '_')
            continue;
        return false;
    }
    return true;
}

[[nodiscard]] inline pg_reply_awaiter<resultset>
pg_fail_resultset(error::db_error err) {
    return pg_reply_awaiter<resultset>{[e = std::move(err)](pg_coro_complete<resultset> complete) mutable {
        complete(::qb::pg::Reply<resultset>::failure(std::move(e)));
    }};
}

[[nodiscard]] inline pg_reply_awaiter<void>
pg_fail_void(error::db_error err) {
    return pg_reply_awaiter<void>{[e = std::move(err)](pg_coro_complete<void> complete) mutable {
        complete(::qb::pg::Reply<void>::failure(std::move(e)));
    }};
}

/** Awaiter that resolves immediately with a failure (any Reply<T>). */
template <typename T>
[[nodiscard]] inline pg_reply_awaiter<T>
pg_fail(error::db_error err) {
    return pg_reply_awaiter<T>{[e = std::move(err)](pg_coro_complete<T> complete) mutable {
        complete(::qb::pg::Reply<T>::failure(std::move(e)));
    }};
}

/** Connection-down error used to fail a query/execute/prepare submitted on a closed handle. */
[[nodiscard]] inline error::connection_error
pg_not_connected_error() {
    return error::connection_error("connection is not established; query rejected (the handle is disconnected)");
}

/** Simple query that returns no meaningful rowset: completion → Reply<void>. */
[[nodiscard]] inline pg_reply_awaiter<void>
pg_execute_void_sql(Transaction *self, std::string sql) {
    return pg_reply_awaiter<void>{[self, sql = std::move(sql)](pg_coro_complete<void> complete) mutable {
        self->execute(
            std::string_view(sql), [complete](Transaction &, resultset) mutable { complete(::qb::pg::Reply<void>::success()); },
            [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<void>::failure(e)); });
    }};
}

template <typename Build>
[[nodiscard]] inline pg_reply_awaiter<void>
pg_try_build_and_execute_void_sql(Transaction *self, Build &&build) {
    std::string sql;
    try {
        sql = std::forward<Build>(build)();
    } catch (error::db_error const &e) {
        return pg_fail_void(e);
    }
    return pg_execute_void_sql(self, std::move(sql));
}

} // namespace

inline pg_reply_awaiter<resultset>
Transaction::execute(std::string_view expr) {
    if (!is_connection_usable())
        return pg_fail<resultset>(pg_not_connected_error());
    return pg_reply_awaiter<resultset>{[this, sql = std::string(expr)](pg_coro_complete<resultset> complete) {
        this->execute(
            std::string_view(sql),
            [complete](Transaction &, resultset rs) mutable { complete(::qb::pg::Reply<resultset>::success(rs.deep_snapshot())); },
            [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<resultset>::failure(e)); });
    }};
}

inline pg_reply_awaiter<PreparedQuery>
Transaction::prepare(std::string_view query_name, std::string_view expr, type_oid_sequence types) {
    if (!is_connection_usable())
        return pg_fail<PreparedQuery>(pg_not_connected_error());
    return pg_reply_awaiter<PreparedQuery>{[this, qn = std::string(query_name), ex = std::string(expr),
                                            t = std::move(types)](pg_coro_complete<PreparedQuery> complete) mutable {
        this->prepare(
            std::string_view(qn), std::string_view(ex), std::move(t),
            [complete](Transaction &, PreparedQuery const &pq) mutable {
                complete(::qb::pg::Reply<PreparedQuery>::success(PreparedQuery{pq}));
            },
            [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<PreparedQuery>::failure(e)); });
    }};
}

inline pg_reply_awaiter<resultset>
Transaction::execute(std::string_view query_name, QueryParams &&params) {
    if (!is_connection_usable())
        return pg_fail<resultset>(pg_not_connected_error());
    return pg_reply_awaiter<resultset>{[this, qn = std::string(query_name),
                                        p = std::move(params)](pg_coro_complete<resultset> complete) mutable {
        this->execute(
            std::string_view(qn), std::move(p),
            [complete](Transaction &, resultset rs) mutable { complete(::qb::pg::Reply<resultset>::success(rs.deep_snapshot())); },
            [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<resultset>::failure(e)); });
    }};
}

inline pg_reply_awaiter<resultset>
Transaction::begin(transaction_mode mode) {
    std::string sql = "BEGIN ";
    sql += to_string(mode);
    pg_append_set_local_statement_timeout(sql, get_timeout());
    return execute(std::string_view(sql));
}

inline pg_reply_awaiter<resultset>
Transaction::begin() {
    return begin(transaction_mode{});
}

inline pg_reply_awaiter<resultset>
Transaction::commit() {
    return execute("COMMIT");
}

inline pg_reply_awaiter<resultset>
Transaction::rollback() {
    return execute("ROLLBACK");
}

inline pg_reply_awaiter<resultset>
Transaction::savepoint(std::string_view name) {
    if (!pg_savepoint_name_ok(name))
        return pg_fail_resultset(error::client_error{"invalid savepoint name (use non-empty alphanumeric/underscore, max 63)"});
    // Quote the identifier exactly like the callback SavePointQuery path (queries.h) so a name means
    // the SAME savepoint through either API: unquoted, PostgreSQL case-folds "MyPoint" to mypoint and
    // rejects a digit-leading name outright, while the quoted callback path preserves it — mixing the
    // APIs on one name would then silently miss. Quoting also keeps injection impossible on its own
    // (the validator above is belt-and-suspenders).
    std::string sql = "SAVEPOINT " + pg_quote_identifier(std::string(name));
    return execute(std::string_view(sql));
}

inline pg_reply_awaiter<resultset>
Transaction::rollback_savepoint(std::string_view name) {
    if (!pg_savepoint_name_ok(name))
        return pg_fail_resultset(error::client_error{"invalid savepoint name (use non-empty alphanumeric/underscore, max 63)"});
    std::string sql = "ROLLBACK TO SAVEPOINT " + pg_quote_identifier(std::string(name));
    return execute(std::string_view(sql));
}

inline pg_reply_awaiter<resultset>
Transaction::release_savepoint(std::string_view name) {
    if (!pg_savepoint_name_ok(name))
        return pg_fail_resultset(error::client_error{"invalid savepoint name (use non-empty alphanumeric/underscore, max 63)"});
    std::string sql = "RELEASE SAVEPOINT " + pg_quote_identifier(std::string(name));
    return execute(std::string_view(sql));
}

inline pg_reply_awaiter<resultset>
Transaction::execute_file(const std::filesystem::path &file_path) {
    return pg_reply_awaiter<resultset>{[this, path = file_path](pg_coro_complete<resultset> complete) mutable {
        try {
            if (!std::filesystem::exists(path)) {
                complete(::qb::pg::Reply<resultset>::failure(error::query_error("SQL file not found: " + path.string())));
                return;
            }
            std::ifstream file(path);
            if (!file.is_open()) {
                complete(::qb::pg::Reply<resultset>::failure(error::query_error("Cannot open SQL file: " + path.string())));
                return;
            }
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string sql_query = buffer.str();
            this->execute(
                std::string_view(sql_query),
                [complete](Transaction &, resultset rs) mutable { complete(::qb::pg::Reply<resultset>::success(rs.deep_snapshot())); },
                [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<resultset>::failure(e)); });
        } catch (std::exception const &e) {
            complete(::qb::pg::Reply<resultset>::failure(error::query_error("Error reading SQL file: " + std::string(e.what()))));
        }
    }};
}

inline pg_reply_awaiter<PreparedQuery>
Transaction::prepare_file(std::string_view query_name, const std::filesystem::path &file_path, type_oid_sequence types) {
    return pg_reply_awaiter<PreparedQuery>{[this, qn = std::string(query_name), path = file_path,
                                            t = std::move(types)](pg_coro_complete<PreparedQuery> complete) mutable {
        try {
            if (!std::filesystem::exists(path)) {
                complete(::qb::pg::Reply<PreparedQuery>::failure(error::query_error("SQL file not found: " + path.string())));
                return;
            }
            std::ifstream file(path);
            if (!file.is_open()) {
                complete(::qb::pg::Reply<PreparedQuery>::failure(error::query_error("Cannot open SQL file: " + path.string())));
                return;
            }
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string sql_query = buffer.str();
            this->prepare(
                std::string_view(qn), std::string_view(sql_query), std::move(t),
                [complete](Transaction &, PreparedQuery const &pq) mutable {
                    complete(::qb::pg::Reply<PreparedQuery>::success(PreparedQuery{pq}));
                },
                [complete](error::db_error const &e) mutable { complete(::qb::pg::Reply<PreparedQuery>::failure(e)); });
        } catch (std::exception const &e) {
            complete(::qb::pg::Reply<PreparedQuery>::failure(error::query_error("Error reading SQL file: " + std::string(e.what()))));
        }
    }};
}

inline pg_reply_awaiter<void>
Transaction::notify(std::string_view channel, std::string_view payload) {
    return pg_try_build_and_execute_void_sql(this, [channel, payload] { return build_notify_sql(channel, payload); });
}

inline pg_reply_awaiter<void>
Transaction::listen(std::string_view channel) {
    return pg_try_build_and_execute_void_sql(this, [channel] { return build_listen_sql(channel); });
}

inline pg_reply_awaiter<void>
Transaction::unlisten(std::string_view channel) {
    return pg_try_build_and_execute_void_sql(this, [channel] { return build_unlisten_sql(channel); });
}

inline pg_reply_awaiter<void>
Transaction::unlisten_all() {
    return pg_execute_void_sql(this, std::string(build_unlisten_all_sql()));
}

} // namespace qb::pg::detail
