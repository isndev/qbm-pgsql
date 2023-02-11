/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
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

#ifndef QBM_PGSQL_DETAIL_TRANSACTION_H
#define QBM_PGSQL_DETAIL_TRANSACTION_H
#include "queries.h"
#include <queue>

namespace qb::pg::detail {
using namespace qb::pg;

class Transaction {
protected:
    Transaction *_parent;
    std::queue<Transaction *> _sub_commands;
    std::queue<ISqlQuery *> _queries;
    PreparedQueryStorage &_query_storage;
    bool _result = true;

    Transaction() = delete;
    Transaction(Transaction const &) = delete;
    Transaction(Transaction &&) = delete;
    Transaction(Transaction *parent) noexcept;
    Transaction(PreparedQueryStorage &storage) noexcept;

public:
    virtual ~Transaction();

    void result(bool value);
    bool result() const;
    Transaction *parent() const;

    void push_transaction(Transaction *cmd);
    Transaction *pop_transaction();
    Transaction *next_transaction();

    void push_query(ISqlQuery *qry);
    ISqlQuery *next_query();
    ISqlQuery *pop_query();

    virtual void on_sub_command_status(bool status);
    virtual void on_new_command();
    virtual void on_new_row_description(row_description_type &&);
    virtual void on_new_data_row(row_data &&);

    template <typename CB_SUCCESS, typename CB_ERROR>
    Transaction &begin(CB_SUCCESS &&on_success, CB_ERROR &&on_error,
                       transaction_mode mode = {});
    template <typename CB_SUCCESS, typename CB_ERROR,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS> &&
                                        std::is_function_v<CB_ERROR>>>
    Transaction &savepoint(std::string name, CB_SUCCESS &&on_success,
                           CB_ERROR &&on_error);
    template <typename CB_SUCCESS>
    Transaction &savepoint(std::string name, CB_SUCCESS &&on_success);

    template <typename CB_SUCCESS, typename CB_ERROR,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS> &&
                                        std::is_function_v<CB_ERROR>>>
    Transaction &execute(std::string expr, CB_SUCCESS &&on_success,
                         CB_ERROR &&on_error);
    template <typename CB_SUCCESS,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS>>>
    Transaction &execute(std::string expr, CB_SUCCESS &&on_success);
    Transaction &execute(std::string expr);

    template <typename CB_SUCCESS, typename CB_ERROR,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS> &&
                                        std::is_function_v<CB_ERROR>>>
    Transaction &prepare(std::string query_name, std::string expr,
                         type_oid_sequence &&types, CB_SUCCESS &&on_success,
                         CB_ERROR &&on_error);
    template <typename CB_SUCCESS,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS>>>
    Transaction &prepare(std::string query_name, std::string expr,
                         type_oid_sequence &&types, CB_SUCCESS &&on_success);
    Transaction &prepare(std::string query_name, std::string expr,
                         type_oid_sequence &&types = {});

    template <typename CB_SUCCESS, typename CB_ERROR,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS> &&
                                        std::is_function_v<CB_ERROR>>>
    Transaction &execute(std::string query_name, QueryParams &&params,
                         CB_SUCCESS &&on_success, CB_ERROR &&on_error);
    template <typename CB_SUCCESS,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS>>>
    Transaction &execute(std::string query_name, QueryParams &&params,
                         CB_SUCCESS &&on_success);
    template <typename CB_SUCCESS,
              typename = std::enable_if<std::is_function_v<CB_SUCCESS>>>
    Transaction &execute(std::string query_name, CB_SUCCESS &&on_success,
                         QueryParams &&params);
    Transaction &execute(std::string query_name, QueryParams &&params);

    template <typename CB_SUCCESS>
    Transaction &then(CB_SUCCESS &&on_success);
    template <typename CB_SUCCESS>
    Transaction &success(CB_SUCCESS &&on_success);
    template <typename CB_ERROR>
    Transaction &error(CB_ERROR &&on_success);
};

} // namespace qb::pg::detail

#include "transaction.inl"

#endif // QBM_PGSQL_DETAIL_TRANSACTION_H
