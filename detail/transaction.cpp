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

#include "transaction.h"

namespace qb::pg::detail {

Transaction::Transaction(Transaction *parent) noexcept
    : _parent(parent)
    , _query_storage(parent->_query_storage) {}
Transaction::Transaction(PreparedQueryStorage &storage) noexcept
    : _parent(nullptr)
    , _query_storage(storage) {}

Transaction::~Transaction() {
    while (!_sub_commands.empty())
        delete pop_transaction();
    while (!_queries.empty())
        delete pop_query();
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
Transaction::push_transaction(Transaction *cmd) {
    _sub_commands.push(cmd);
    on_new_command();
}
Transaction *
Transaction::pop_transaction() {
    auto ret = _sub_commands.front();
    _sub_commands.pop();
    on_sub_command_status(ret->_result);
    return ret;
}
Transaction *
Transaction::next_transaction() {
    return _sub_commands.empty() ? nullptr : _sub_commands.front();
}

void
Transaction::push_query(ISqlQuery *qry) {
    _queries.push(qry);
}
ISqlQuery *
Transaction::next_query() {
    return _queries.empty() ? nullptr : _queries.front();
}
ISqlQuery *
Transaction::pop_query() {
    auto ret = _queries.front();
    _queries.pop();
    return ret;
}

void
Transaction::on_sub_command_status(bool status) {
    _result &= status;
    _parent->on_sub_command_status(status);
}
void
Transaction::on_new_command() {}
void
Transaction::on_new_row_description(row_description_type &&) {}
void
Transaction::on_new_data_row(row_data &&) {}

Transaction &
Transaction::execute(std::string expr) {
    return this->execute(
        std::move(expr), [](Transaction &) {}, [](error::db_error const &) {});
}

Transaction &
Transaction::prepare(std::string query_name, std::string expr,
                     type_oid_sequence &&types) {
    return this->prepare(
        std::move(query_name), std::move(expr), std::move(types),
        [](Transaction &, PreparedQuery const &) {}, [](error::db_error const &) {});
}

Transaction &
Transaction::execute(std::string query_name, QueryParams &&params) {
    return this->execute(
        std::move(query_name), std::move(params), [](Transaction &) {},
        [](error::db_error const &) {});
}

} // namespace qb::pg::detail