/**
 * @file queries.h
 * @brief PostgreSQL query representation and management
 *
 * This file defines the data structures and classes for representing
 * and managing SQL queries for the PostgreSQL client, including:
 *
 * - Storage for prepared queries
 * - Parameter binding for prepared statements
 * - SQL query execution with callbacks
 * - Various query types (BEGIN, COMMIT, ROLLBACK, etc.)
 *
 * The implementation follows the PostgreSQL protocol for preparing
 * and executing queries, supporting both simple and prepared statements.
 *
 * @see qb::pg::detail::Transaction
 * @see qb::pg::detail::Database
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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

#include <iomanip>
#include <iostream>
#include <list> // P2-1: LRU cache
#include <string_view>
#include <type_traits>
#include <vector>
#include <qb/io.h>
#include <qb/system/container/unordered_map.h>
#include <qb/utility/branch_hints.h>

#include "./common.h"
#include "./error.h"
#include "./param_serializer.h"
#include "./protocol.h"
#include "./type_mapping.h"

namespace qb::pg::detail {
using namespace qb::pg;

/**
 * @brief Structure for storing a prepared query definition
 *
 * Contains all the information needed to execute a prepared statement,
 * including its name, SQL expression, parameter types, and result description.
 */
struct PreparedQuery {
    std::string          name;            ///< Name of the prepared query
    std::string          expression;      ///< SQL expression
    std::vector<oid>     param_types;     ///< Types of parameters (was type_oid_sequence)
    row_description_type row_description; ///< Description of result columns
};

/**
 * @brief Storage for prepared queries with LRU eviction (P2-1)
 *
 * Provides a central repository for all prepared statements in the
 * database session, allowing them to be referenced by name.
 * Implements bounded capacity with LRU eviction to prevent unbounded growth.
 */
class PreparedStorage {
    // LRU cache implementation
    struct LruEntry {
        std::string                              name;     ///< Query name (key)
        PreparedQuery                            query;    ///< The prepared query
        mutable std::list<std::string>::iterator lru_iter; ///< Iterator in LRU list (mutable for get())
    };

    qb::unordered_map<std::string, LruEntry> _prepared_queries; ///< Map of queries
    std::list<std::string>                   _lru_list;         ///< LRU order list
    size_t                                   _max_size{100};    ///< Max capacity
    size_t                                   _evicted_count{0}; ///< Stats: evicted

public:
    /**
     * @brief Default constructor with default max size
     */
    PreparedStorage() = default;

    /**
     * @brief Construct with custom max size
     * @param max_size Maximum number of prepared queries to keep
     */
    explicit PreparedStorage(size_t max_size)
        : _max_size(max_size > 0 ? max_size : 100) {}

    /**
     * @brief Set maximum cache size (applies to future insertions)
     * @param max_size New maximum size
     */
    void
    set_max_size(size_t max_size) {
        _max_size = max_size > 0 ? max_size : 100;
        evict_if_needed(); // Evict immediately if over capacity
    }

    /**
     * @brief Get current maximum cache size
     * @return size_t Max size
     */
    size_t
    max_size() const {
        return _max_size;
    }

    /**
     * @brief Get current number of cached queries
     * @return size_t Current size
     */
    size_t
    size() const {
        return _prepared_queries.size();
    }

    /**
     * @brief Get total number of evicted queries (stats)
     * @return size_t Eviction count
     */
    size_t
    evicted_count() const {
        return _evicted_count;
    }

    /**
     * @brief Checks if a prepared query exists
     *
     * @param name Name of the prepared query
     * @return bool True if the query exists, false otherwise
     */
    bool
    has(std::string_view name) const {
        return _prepared_queries.find(std::string(name)) != _prepared_queries.cend();
    }

    /**
     * @brief Adds a prepared query to storage
     *
     * Implements LRU eviction if over capacity.
     * Updates LRU order on access.
     *
     * @param query Prepared query to add
     * @return const PreparedQuery& Reference to the stored query
     */
    const PreparedQuery &
    push(PreparedQuery &&query) {
        std::string key = query.name;

        // Check if already exists - update it and move to front
        auto it = _prepared_queries.find(key);
        if (it != _prepared_queries.end()) {
            // Move to front (most recently used)
            _lru_list.erase(it->second.lru_iter);
            _lru_list.push_front(key);
            it->second.lru_iter = _lru_list.begin();
            it->second.query    = std::move(query);
            return it->second.query;
        }

        // Evict if at capacity
        evict_if_needed();

        // Add to front of LRU list
        _lru_list.push_front(key);

        // Store in map with LRU iterator
        LruEntry entry{key, std::move(query), _lru_list.begin()};
        auto     result = _prepared_queries.emplace(std::move(key), std::move(entry));

        return result.first->second.query;
    }

    /**
     * @brief Retrieves a prepared query by name
     *
     * Updates LRU order on access (marks as recently used).
     *
     * @param name Name of the prepared query
     * @return PreparedQuery const& Reference to the prepared query
     * @throws std::out_of_range If the query doesn't exist
     */
    PreparedQuery const &
    get(std::string_view name) const {
        std::string key(name);
        auto        it = _prepared_queries.find(key);
        if (it == _prepared_queries.end()) {
            throw std::out_of_range("Prepared query not found: " + key);
        }

        // Move to front (most recently used) - need to cast away const
        auto &mutable_this = const_cast<PreparedStorage &>(*this);
        mutable_this._lru_list.erase(it->second.lru_iter);
        mutable_this._lru_list.push_front(key);
        it->second.lru_iter = mutable_this._lru_list.begin();

        return it->second.query;
    }

    /**
     * @brief Clear all prepared queries
     */
    void
    clear() {
        _prepared_queries.clear();
        _lru_list.clear();
    }

private:
    /**
     * @brief Evict least recently used items if over capacity
     */
    void
    evict_if_needed() {
        while (_prepared_queries.size() >= _max_size && !_lru_list.empty()) {
            // Get least recently used (back of list)
            std::string &lru_name = _lru_list.back();
            _prepared_queries.erase(lru_name);
            _lru_list.pop_back();
            ++_evicted_count;
        }
    }
};

// Maintain backward compatibility
using PreparedQueryStorage = PreparedStorage;

/**
 * @brief Class for managing query parameters
 *
 * Encapsulates parameters for prepared statements, handling
 * type conversion and binary encoding according to PostgreSQL protocol.
 */
class QueryParams {
    std::vector<byte>    _params;      ///< Serialized parameters
    std::vector<integer> _param_types; ///< OIDs for parameter types

public:
    /**
     * @brief Constructs an empty parameter set
     */
    QueryParams() = default;

    /**
     * @brief Constructs parameter set from variadic arguments
     *
     * Uses template argument deduction to convert various parameter types
     * to their PostgreSQL binary representation.
     *
     * The single-argument case is constrained to exclude QueryParams itself so
     * this forwarding constructor does not hijack the copy/move constructors
     * (a `QueryParams b(a)` from a non-const lvalue would otherwise bind here and
     * try to *serialize* `a` instead of copying it).
     *
     * @tparam T Parameter types
     * @param args Parameter values
     */
    template <typename... T,
              std::enable_if_t<!(sizeof...(T) == 1 && std::conjunction_v<std::is_same<std::decay_t<T>, QueryParams>...>), int> = 0>
    QueryParams(T &&...args) {
        if constexpr (sizeof...(T) > 0) {
            // Do not use format_codes_buffer, no longer used

            // Serialize parameters directly
            ParamSerializer serializer;
            serializer.serialize_params(std::forward<T>(args)...);

            // GCC -O2 emits a spurious -Warray-bounds on these small-vector copies
            // (a known GCC-14 middle-end false positive; clang/MSVC are clean).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
            _params      = serializer.params_buffer();
            _param_types = serializer.param_types();
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        }
    }

    /**
     * @brief Gets the serialized parameters
     *
     * @return std::vector<byte>& Reference to the serialized parameters
     */
    std::vector<byte> &
    get() {
        return _params;
    }

    /**
     * @brief Gets the serialized parameters (const version)
     *
     * @return const std::vector<byte>& Const reference to the serialized parameters
     */
    const std::vector<byte> &
    get() const {
        return _params;
    }

    /**
     * @brief Gets the parameter types
     *
     * @return const std::vector<integer>& Const reference to parameter OIDs
     */
    const std::vector<integer> &
    param_types() const {
        return _param_types;
    }

    /**
     * @brief Gets the number of parameters
     *
     * @return smallint The number of parameters
     */
    smallint
    param_count() const {
        if (_params.size() >= sizeof(smallint)) {
            // Extract the number of parameters from the buffer
            smallint count;
            std::memcpy(&count, _params.data(), sizeof(smallint));
            return ntohs(count); // Convert from network byte order to host byte order
        }
        return 0;
    }

    /**
     * @brief Checks if the parameter set is empty
     *
     * @return bool True if there are no parameters, false otherwise
     */
    bool
    empty() const {
        return _params.empty();
    }
};

/**
 * @brief Interface for SQL queries
 *
 * Base class for all SQL query implementations, providing a common
 * interface for getting the query message and handling callbacks.
 */
class ISqlQuery {
public:
    ISqlQuery() = default;

    virtual ~ISqlQuery() = default;

    /**
     * @brief Checks if the query is valid
     *
     * @return bool True if the query is valid, false otherwise
     */
    virtual bool
    is_valid() const {
        return true;
    }

    /**
     * @brief Gets the PostgreSQL protocol message for the query
     *
     * @return message Message to send to the server
     */
    virtual message get() const = 0;

    /**
     * @brief Called when the query succeeds
     */
    virtual void on_success() const = 0;

    /**
     * @brief Called when the query fails
     *
     * @param err Error information
     */
    virtual void on_error(error::db_error const &err) const = 0;
};

/**
 * @brief Base implementation of SQL query with callbacks
 *
 * Provides a base implementation for SQL queries with success and error callbacks.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class SqlQuery : public ISqlQuery {
    CB_SUCCESS _on_success; ///< Success callback
    CB_ERROR   _on_error;   ///< Error callback

public:
    /**
     * @brief Constructs a SQL query with callbacks
     *
     * @param success Success callback
     * @param error Error callback
     */
    SqlQuery(CB_SUCCESS &&success, CB_ERROR &&error)
        : _on_success(std::forward<CB_SUCCESS>(success))
        , _on_error(std::forward<CB_ERROR>(error)) {}

    virtual ~SqlQuery() = default;

    /**
     * @brief Calls the success callback
     */
    void
    on_success() const final {
        _on_success();
    }

    /**
     * @brief Calls the error callback
     *
     * @param err Error information
     */
    void
    on_error(error::db_error const &err) const final {
        _on_error(err);
    }
};

/**
 * @brief Query for beginning a transaction
 *
 * Creates a BEGIN statement with optional transaction mode (isolation level, etc.).
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class BeginQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    transaction_mode _mode;                   ///< Transaction mode
    int              _statement_timeout_ms{}; ///< If &gt; 0, append SET LOCAL statement_timeout (ms)

public:
    /**
     * @brief Constructs a BEGIN query
     *
     * @param mode Transaction mode
     * @param statement_timeout If positive, same round-trip runs
     *        `SET LOCAL statement_timeout = N` (milliseconds) after BEGIN
     * @param success Success callback
     * @param error Error callback
     */
    BeginQuery(transaction_mode mode, qb::duration statement_timeout, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success), std::forward<CB_ERROR>(error))
        , _mode(mode)
        , _statement_timeout_ms(statement_timeout > qb::duration::zero()
                                    ? static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(statement_timeout).count())
                                    : 0) {}

    /**
     * @brief Creates the BEGIN message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        std::string sql = "BEGIN ";
        sql += to_string(_mode);
        if (_statement_timeout_ms > 0) {
            sql += "; SET LOCAL statement_timeout = ";
            sql += std::to_string(_statement_timeout_ms);
        }
        LOG_DEBUG("[pgsql] Send BEGIN: \"" << sql << "\"");
        message m(query_tag);
        m.write(sql);
        return m;
    }
};

/**
 * @brief Query for committing a transaction
 *
 * Creates a COMMIT statement to finalize a transaction.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class CommitQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
public:
    /**
     * @brief Constructs a COMMIT query
     *
     * @param success Success callback
     * @param error Error callback
     */
    CommitQuery(CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success), std::forward<CB_ERROR>(error)) {}

    /**
     * @brief Creates the COMMIT message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        LOG_DEBUG("[pgsql] Send COMMIT");
        message m(query_tag);
        m.write("commit");
        return m;
    }
};

/**
 * @brief Query for rolling back a transaction
 *
 * Creates a ROLLBACK statement to abort a transaction.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class RollbackQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
public:
    /**
     * @brief Constructs a ROLLBACK query
     *
     * @param success Success callback
     * @param error Error callback
     */
    RollbackQuery(CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success), std::forward<CB_ERROR>(error)) {}

    /**
     * @brief Creates the ROLLBACK message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        LOG_DEBUG("[pgsql] Send ROLLBACK");
        message m(query_tag);
        m.write("rollback");
        return m;
    }
};

/**
 * @brief Query for creating a savepoint
 *
 * Creates a SAVEPOINT statement within a transaction.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class SavePointQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    std::string _name; ///< Savepoint name (owned copy — never a dangling reference)

public:
    /**
     * @brief Constructs a SAVEPOINT query
     *
     * @param name Savepoint name
     * @param success Success callback
     * @param error Error callback
     */
    SavePointQuery(std::string const &name, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success), std::forward<CB_ERROR>(error))
        , _name(name) {}

    /**
     * @brief Creates the SAVEPOINT message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        LOG_DEBUG("[pgsql] Send SAVEPOINT " << _name);
        message m(query_tag);
        m.write("savepoint " + _name);
        return m;
    }
};

/**
 * @brief Query for releasing a savepoint
 *
 * Creates a RELEASE SAVEPOINT statement within a transaction.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class ReleaseSavePointQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    std::string _name; ///< Savepoint name (owned copy — never a dangling reference)

public:
    /**
     * @brief Constructs a RELEASE SAVEPOINT query
     *
     * @param name Savepoint name
     * @param success Success callback
     * @param error Error callback
     */
    ReleaseSavePointQuery(std::string const &name, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success), std::forward<CB_ERROR>(error))
        , _name(name) {}

    /**
     * @brief Creates the RELEASE SAVEPOINT message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        LOG_DEBUG("[pgsql] Send RELEASE SAVEPOINT " << _name);
        message m(query_tag);
        m.write("release savepoint " + _name);
        return m;
    }
};

/**
 * @brief Query for rolling back to a savepoint
 *
 * Creates a ROLLBACK TO SAVEPOINT statement within a transaction.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class RollbackSavePointQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    std::string _name; ///< Savepoint name (owned copy — never a dangling reference)

public:
    /**
     * @brief Constructs a ROLLBACK TO SAVEPOINT query
     *
     * @param name Savepoint name
     * @param success Success callback
     * @param error Error callback
     */
    RollbackSavePointQuery(std::string const &name, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success), std::forward<CB_ERROR>(error))
        , _name(name) {}

    /**
     * @brief Creates the ROLLBACK TO SAVEPOINT message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        LOG_DEBUG("[pgsql] Send ROLLBACK TO SAVEPOINT " << _name);
        message m(query_tag);
        m.write("rollback to savepoint " + _name);
        return m;
    }
};

/**
 * @brief Query for executing a simple SQL statement
 *
 * Creates a query for direct execution of SQL expressions.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class SimpleQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    const std::string _expression; ///< SQL expression

public:
    /**
     * @brief Constructs a simple query
     *
     * @param expr SQL expression
     * @param success Success callback
     * @param error Error callback
     */
    SimpleQuery(std::string &&expr, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success), std::forward<CB_ERROR>(error))
        , _expression(std::move(expr)) {}

    /**
     * @brief Creates the query message
     *
     * @return message Message to send to the server
     */
    message
    get() const final {
        LOG_DEBUG("[pgsql] Send QUERY \"" << _expression << "\"");
        message m(query_tag);
        m.write(_expression);

        return m;
    }
};

/**
 * @brief Query for preparing a statement
 *
 * Creates a query for preparing a named statement according to the PostgreSQL protocol.
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class ParseQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    PreparedQuery const &_query; ///< Prepared query definition

public:
    ParseQuery(PreparedQuery const &query, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success), std::forward<CB_ERROR>(error))
        , _query(query) {}

    bool
    is_valid() const final {
        // The Parse message encodes the parameter-type count as an int16, but get()
        // writes every OID entry. More than 32767 declared types would truncate the
        // count while still emitting all entries, desynchronizing the wire stream.
        // Reject here (like ExecuteQuery's missing-statement check) so the failure goes
        // through the normal on_error path instead of corrupting the connection. This is
        // the Parse-side twin of the Bind guard ParamSerializer::ensure_param_count_fits().
        if (qb::likely(_query.param_types.size() <= static_cast<std::size_t>(std::numeric_limits<smallint>::max())))
            return true;
        LOG_CRIT("[pgsql] PARSE rejected: " << _query.param_types.size() << " parameter types exceed protocol max 32767");
        return false;
    }

    [[nodiscard]] message
    get() const final {
        LOG_DEBUG("[pgsql] Send PARSE QUERY \"" << _query.expression << "\"");
        message cmd(parse_tag);
        cmd.write(_query.name);
        cmd.write(_query.expression);
        cmd.write((smallint) _query.param_types.size());
        for (auto oid_val : _query.param_types) {
            cmd.write(static_cast<integer>(oid_val));
        }

        message describe(describe_tag);
        describe.write('S');
        describe.write(_query.name);
        cmd.pack(describe);
        cmd.pack(message(sync_tag));

        return cmd;
    }
};

/**
 * @brief Prepared statement execution
 *
 * @tparam CB_SUCCESS Type of success callback
 * @tparam CB_ERROR Type of error callback
 */
template <typename CB_SUCCESS, typename CB_ERROR>
class ExecuteQuery final : public SqlQuery<CB_SUCCESS, CB_ERROR> {
    const PreparedStorage &_storage;    ///< Prepared statement storage
    std::string            _query_name; ///< Query name to execute
    QueryParams            _params;     ///< Query parameters

public:
    /**
     * @brief Constructs an execute query
     *
     * @param storage Prepared statement storage
     * @param query_name Query name to execute
     * @param params Query parameters
     * @param success Success callback
     * @param error Error callback
     */
    ExecuteQuery(const PreparedStorage &storage, std::string_view query_name, QueryParams &&params, CB_SUCCESS &&success, CB_ERROR &&error)
        : SqlQuery<CB_SUCCESS, CB_ERROR>(std::forward<CB_SUCCESS>(success), std::forward<CB_ERROR>(error))
        , _storage(storage)
        , _query_name(query_name)
        , _params(std::move(params)) {}

    bool
    is_valid() const final {
        if (qb::likely(_storage.has(_query_name)))
            return true;
        LOG_CRIT("[pgsql] Error prepared query " << _query_name << " not registered");
        return false;
    }

    message
    get() const final {
        const auto &query = _storage.get(_query_name);
        message     cmd(bind_tag);

        // Exact format expected by PostgreSQL for a Bind message:
        // 1. Portal name (empty = unnamed)
        cmd.write("");

        // 2. Prepared statement name
        cmd.write(query.name);

        // 3. Parameter format codes (PostgreSQL Bind message)
        // 0 = no parameters or all default (text); 1 = single code applies to every
        // parameter. We send binary (1) for all parameters when there are any.
        const smallint param_count = _params.param_count();
        if (param_count == 0) {
            cmd.write(static_cast<smallint>(0));
        } else {
            cmd.write(static_cast<smallint>(1));
            cmd.write(static_cast<smallint>(1)); // binary
        }

        // 4. Total number of parameters
        cmd.write(param_count);

        // 5. Parameter values
        if (!_params.empty() && param_count > 0) {
            // Skip the count in the parameters buffer
            const std::vector<byte> &param_buffer = _params.get();
            if (param_buffer.size() > sizeof(smallint)) {
                const byte *data      = param_buffer.data() + sizeof(smallint);
                size_t      data_size = param_buffer.size() - sizeof(smallint);

                if (data_size == 0) {
                    LOG_WARN("[pgsql] Bind: param_count=" << param_count << " but serialized payload is empty");
                }
                // Copy the raw data
                auto out = cmd.output();
                std::copy(data, data + data_size, out);
            } else {
                LOG_WARN("[pgsql] Bind: param_count=" << param_count << " but parameter buffer missing payload");
            }
        }

        // 6. Result-column format codes (one per column, or 0 = default all text).
        // Binary for scalars; text for string-like OIDs so DataRow matches decoders.
        const auto    &rd   = query.row_description;
        const smallint ncol = static_cast<smallint>(rd.size());
        cmd.write(ncol);
        for (auto const &fd : rd) {
            const smallint fmt = type_oid_prefers_binary_result_format(fd.type_oid) ? 1 : 0;
            cmd.write(fmt);
        }

        // 7. Execute message (empty portal, no row limit)
        message execute(execute_tag);
        execute.write("");
        execute.write(0);
        cmd.pack(execute);

        // 8. Sync message
        cmd.pack(message(sync_tag));
        return cmd;
    }
};

} // namespace qb::pg::detail
