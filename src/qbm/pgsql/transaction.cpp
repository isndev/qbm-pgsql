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
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 * @ingroup Pgsql
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
Transaction::fail_all_pending(error::db_error const &err) {
    _result = false;
    // Swap the queues out before draining so an on_error callback that enqueues
    // new work (or otherwise mutates this node) cannot make the loops re-enter
    // or invalidate their own iteration.
    std::queue<std::unique_ptr<ISqlQuery>>   queries;
    std::queue<std::unique_ptr<Transaction>> subs;
    queries.swap(_queries);
    subs.swap(_sub_commands);

    while (!queries.empty()) {
        auto q = std::move(queries.front());
        queries.pop();
        if (q) {
            try {
                q->on_error(err);
            } catch (...) {
                // A failing user error-callback must not abort the drain.
            }
        }
    }
    while (!subs.empty()) {
        auto sub = std::move(subs.front());
        subs.pop();
        if (sub)
            sub->fail_all_pending(err);
    }
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
    // zero loop iterations, and incorrectly reports success (prepared-statement
    // client validation, etc.). When the queue is already empty at entry, snapshot outcome
    // before resetting if that snapshot is not a clean success.
    results() = {};

    const bool                     empty_at_entry = _sub_commands.empty() && _queries.empty();
    const bool                     pre_success    = _result && (_error.sqlstate == sqlstate::unknown_code);
    std::optional<error::db_error> pre_failure_error;
    if (empty_at_entry && !pre_success)
        pre_failure_error = _error;

    _error  = error::db_error{"unknown error"};
    _result = true;

    // Drive libev with `listener::current.run(EVRUN_ONCE)` — not `async::run_once()`:
    // fluent `.await()` may run from inside a coroutine body (under `run_ready()`);
    // `async::*` entry points reject that nesting to prevent re-entrant blocking pumps.
    while (!_sub_commands.empty() || !_queries.empty())
        qb::io::async::listener::current.run(EVRUN_ONCE);

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