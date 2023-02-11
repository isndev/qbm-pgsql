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

#include "commands.h"
#include "transaction.h"

#ifndef QBM_PGSQL_DETAIL_TRANSACTION_INL
#    define QBM_PGSQL_DETAIL_TRANSACTION_INL

namespace qb::pg::detail {

template <typename CB_SUCCESS, typename CB_ERROR>
Transaction &
Transaction::begin(CB_SUCCESS &&on_success, CB_ERROR &&on_error, transaction_mode mode) {
    if (_parent) {
        on_error((error::db_error)error::query_error("already in transaction"));
    } else {
        auto end = new End<CB_ERROR>(this, std::forward<CB_ERROR>(on_error));
        push_transaction(new Begin<CB_SUCCESS, CB_ERROR>(
            this, end, mode, std::forward<CB_SUCCESS>(on_success)));
        push_transaction(end);
    }
    return *this;
}
template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::savepoint(std::string name, CB_SUCCESS &&on_success,
                       CB_ERROR &&on_error) {
    auto end = new EndSavePoint<CB_ERROR>(this, std::move(name),
                                          std::forward<CB_ERROR>(on_error));
    push_transaction(new SavePoint<CB_SUCCESS, CB_ERROR>(
        this, end, std::forward<CB_SUCCESS>(on_success)));
    push_transaction(end);
    return *this;
}

template <typename CB_SUCCESS>
Transaction &
Transaction::savepoint(std::string name, CB_SUCCESS &&on_success) {
    return savepoint(std::move(name), std::forward<CB_SUCCESS>(on_success),
                     [](error::db_error) {});
}

template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::execute(std::string expr, CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &, resultset>) {
        push_transaction(new ResultQuery<CB_SUCCESS, CB_ERROR>(
            this, std::move(expr), std::forward<CB_SUCCESS>(on_success),
            std::forward<CB_ERROR>(on_error)));
    } else if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &>) {
        push_transaction(new Query<CB_SUCCESS, CB_ERROR>(
            this, std::move(expr), std::forward<CB_SUCCESS>(on_success),
            std::forward<CB_ERROR>(on_error)));
    } else
        static_assert("execute call_back requires -> [](qb::pg::transaction &tr, "
                      "(optional) qb::pg::results res)");

    return *this;
}
template <typename CB_SUCCESS, typename>
Transaction &
Transaction::execute(std::string expr, CB_SUCCESS &&on_success) {
    return execute(std::move(expr), std::forward<CB_SUCCESS>(on_success),
                   [](error::db_error const &) {});
}

template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::prepare(std::string query_name, std::string expr,
                     type_oid_sequence &&types, CB_SUCCESS &&on_success,
                     CB_ERROR &&on_error) {
    PreparedQuery query{std::move(query_name), std::move(expr), std::move(types)};

    push_transaction(new Prepare<CB_SUCCESS, CB_ERROR>(
        this, std::move(query), std::forward<CB_SUCCESS>(on_success),
        std::forward<CB_ERROR>(on_error)));
    return *this;
}
template <typename CB_SUCCESS, typename>
Transaction &
Transaction::prepare(std::string query_name, std::string expr,
                     type_oid_sequence &&types, CB_SUCCESS &&on_success) {
    return prepare(std::move(query_name), std::move(expr), types,
                   std::forward<CB_SUCCESS>(on_success), [](error::db_error const &) {});
}

template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::execute(std::string query_name, QueryParams &&params,
                     CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &, resultset>) {
        push_transaction(new QueryPrepared<CB_SUCCESS, CB_ERROR>(
            this, std::move(query_name), std::move(params.get()),
            std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error)));
    } else if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &>) {
        push_transaction(new ExecutePrepared<CB_SUCCESS, CB_ERROR>(
            this, std::move(query_name), std::move(params.get()),
            std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error)));
    } else
        static_assert("execute call_back requires -> [](qb::pg::transaction &tr, "
                      "(optional) qb::pg::results res)");
    return *this;
}
template <typename CB_SUCCESS, typename>
Transaction &
Transaction::execute(std::string query_name, QueryParams &&params,
                     CB_SUCCESS &&on_success) {
    return execute(std::move(query_name), std::move(params), std::forward<CB_SUCCESS>(on_success),
                   [](error::db_error const &) {});
}

template <typename CB_SUCCESS, typename>
Transaction &
Transaction::execute(std::string query_name, CB_SUCCESS &&on_success,
                     QueryParams &&params) {
    return execute(std::move(query_name), std::move(params),
                   std::forward<CB_SUCCESS>(on_success), [](error::db_error const &) {});
}

template <typename CB_SUCCESS>
Transaction &
Transaction::then(CB_SUCCESS &&on_success) {
    push_transaction(new Then<CB_SUCCESS>(this, std::forward<CB_SUCCESS>(on_success)));
    return *this;
}

} // namespace qb::pg::detail

#endif // !QBM_PGSQL_DETAIL_TRANSACTION_INL