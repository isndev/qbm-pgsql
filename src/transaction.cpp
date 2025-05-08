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

Transaction &
Transaction::execute(std::string_view expr) {
    return this->execute(
        expr, [](Transaction &, auto) {}, [](error::db_error const &) {});
}

Transaction &
Transaction::prepare(std::string_view query_name, std::string_view expr,
                     type_oid_sequence &&types) {
    return this->prepare(
        query_name, expr, std::move(types), [](Transaction &, PreparedQuery const &) {},
        [](error::db_error const &) {});
}

Transaction &
Transaction::prepare_file(std::string_view query_name, 
                         const std::filesystem::path& file_path,
                         type_oid_sequence &&types) {
    return prepare_file(query_name, file_path, std::move(types), 
                       [](Transaction&, PreparedQuery const&) {},
                       [](error::db_error const &) {});
}

Transaction &
Transaction::execute(std::string_view query_name, QueryParams &&params) {
    return this->execute(
        query_name, std::move(params), [](Transaction &, auto) {},
        [](error::db_error const &) {});
}

Transaction &
Transaction::execute_file(const std::filesystem::path& file_path) {
    return execute_file(file_path, 
                       [](Transaction&, auto) {},
                       [](error::db_error const &) {});
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
    results() = {};

    while (!_sub_commands.empty() || !_queries.empty())
        qb::io::async::run_once();

    return {std::move(results()), std::move(_error)};
}

} // namespace qb::pg::detail