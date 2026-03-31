/**
 * @file transaction.cpp
 * @brief Implementation of PostgreSQL transaction management
 *
 * This file contains the implementation of the Transaction class methods for
 * managing PostgreSQL database transactions. It implements the core functionality for:
 *
 * - Transaction state management
 * - Query and sub-transaction queuing
 * - Command execution
 * - Result handling and error reporting
 * - Asynchronous operation coordination
 *
 * The implementation supports PostgreSQL's transaction features including
 * nested transactions via savepoints and reusable prepared statements.
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

#include <memory>
#include <optional>
#include <utility>

#include "./transaction.h"
#include "./transaction.inl"

namespace qb::pg::detail {

Transaction::Transaction(Transaction *parent) noexcept
    : _parent(parent)
    , _query_storage(parent->_query_storage)
    , _error{"unknown error"} {}

Transaction::Transaction(PreparedQueryStorage &storage) noexcept
    : _parent(nullptr)
    , _query_storage(storage)
    , _error{"unknown error"} {}

Transaction::~Transaction() {
    while (!_sub_commands.empty()) {
        pop_transaction();
    }
    while (!_queries.empty()) {
        pop_query();
    }
}

void
Transaction::on_before_pop() {}

void
Transaction::result(bool value) {
    _result = value;
}

bool
Transaction::result() const {
    return _result;
}

Transaction *
Transaction::parent() const {
    return _parent;
}

void
Transaction::push_transaction(std::unique_ptr<Transaction> cmd) {
    _sub_commands.push(std::move(cmd));
    on_new_command();
}

std::unique_ptr<Transaction>
Transaction::pop_transaction() {
    auto ret = std::move(_sub_commands.front());
    _sub_commands.pop();
    ret->on_before_pop();
    on_sub_command_status(ret->_result);
    return ret;
}

Transaction *
Transaction::next_transaction() {
    return _sub_commands.empty() ? nullptr : _sub_commands.front().get();
}

void
Transaction::push_query(std::unique_ptr<ISqlQuery> qry) {
    _queries.push(std::move(qry));
}

ISqlQuery *
Transaction::next_query() {
    return _queries.empty() ? nullptr : _queries.front().get();
}

std::unique_ptr<ISqlQuery>
Transaction::pop_query() {
    auto ret = std::move(_queries.front());
    _queries.pop();
    return ret;
}

void
Transaction::on_sub_command_status(bool status) {
    _result &= status;
    if (_parent) {
        _parent->on_sub_command_status(status);
    }
}

void
Transaction::on_new_command() {}

void
Transaction::on_new_row_description(row_description_type &&) {}

void
Transaction::on_new_data_row(row_data &&) {}

void
Transaction::on_command_complete(const std::string &tag) {
    _results.set_command_tag(tag);
}

bool
Transaction::has_error() const {
    return _error.sqlstate != sqlstate::unknown_code;
}

error::db_error const &
Transaction::error() const {
    return _error;
}

result_impl &
Transaction::results() {
    return _results;
}

Transaction::status
Transaction::await() {
    // `push_transaction` → `on_new_command` → `process_if_query_ready` can finish the whole
    // operation (including errors) synchronously before `execute()`/`prepare()` returns.
    // If we always reset `_result`/`_error` here, `await()` then sees an empty queue, runs
    // zero `run_once()` iterations, and incorrectly reports success (prepared-statement
    // client validation, etc.). When the queue is already empty at entry, snapshot outcome
    // before resetting if that snapshot is not a clean success.
    results() = {};

    const bool empty_at_entry = _sub_commands.empty() && _queries.empty();
    const bool pre_success    = _result && (_error.sqlstate == sqlstate::unknown_code);
    std::optional<error::db_error> pre_failure_error;
    if (empty_at_entry && !pre_success)
        pre_failure_error = _error;

    _error  = error::db_error{"unknown error"};
    _result = true;

    while (!_sub_commands.empty() || !_queries.empty())
        qb::io::async::run_once();

    if (empty_at_entry && pre_failure_error.has_value()) {
        _result = false;
        _error  = std::move(*pre_failure_error);
    }

    status out{std::move(results()), std::move(_error), _result};

    // Neutral baseline for the next fluent chain; `status` owns the moved error/results.
    _error  = error::db_error{"unknown error"};
    _result = true;

    return out;
}

} // namespace qb::pg::detail