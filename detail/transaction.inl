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

#include "commands.h"
#include "transaction.h"
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#ifndef QBM_PGSQL_DETAIL_TRANSACTION_INL
#    define QBM_PGSQL_DETAIL_TRANSACTION_INL

namespace qb::pg::detail {

template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::begin(CB_SUCCESS &&on_success, CB_ERROR &&on_error, transaction_mode mode) {
    if (_parent) {
        on_error((error::db_error)error::query_error("already in transaction"));
    } else {
        auto end = new End<CB_ERROR>(this, std::forward<CB_ERROR>(on_error));
        push_transaction(std::unique_ptr<Transaction>(new Begin<CB_SUCCESS, CB_ERROR>(
            this, end, mode, std::forward<CB_SUCCESS>(on_success))));
        push_transaction(std::unique_ptr<Transaction>(end));
    }
    return *this;
}

template <typename CB_SUCCESS>
Transaction &
Transaction::begin(CB_SUCCESS &&on_success, transaction_mode mode) {
    return begin(std::forward<CB_SUCCESS>(on_success), [](error::db_error const &) {}, mode);
}

template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::savepoint(std::string_view name, CB_SUCCESS &&on_success,
                       CB_ERROR &&on_error) {
    auto end = new EndSavePoint<CB_ERROR>(this, std::string(name), std::forward<CB_ERROR>(on_error));
    push_transaction(std::unique_ptr<Transaction>(new SavePoint<CB_SUCCESS, CB_ERROR>(
        this, end, std::forward<CB_SUCCESS>(on_success))));
    push_transaction(std::unique_ptr<Transaction>(end));
    return *this;
}

template <typename CB_SUCCESS>
Transaction &
Transaction::savepoint(std::string_view name, CB_SUCCESS &&on_success) {
    return savepoint(name, std::forward<CB_SUCCESS>(on_success),
                     [](error::db_error) {});
}

template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::execute(std::string_view expr, CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &, resultset>) {
        push_transaction(std::unique_ptr<Transaction>(new ResultQuery<CB_SUCCESS, CB_ERROR>(
            this, std::string(expr), std::forward<CB_SUCCESS>(on_success),
            std::forward<CB_ERROR>(on_error))));
    } else if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &>) {
        push_transaction(std::unique_ptr<Transaction>(new Query<CB_SUCCESS, CB_ERROR>(
            this, std::string(expr), std::forward<CB_SUCCESS>(on_success),
            std::forward<CB_ERROR>(on_error))));
    } else
        static_assert("execute call_back requires -> [](qb::pg::transaction &tr, "
                      "(optional) qb::pg::results res)");

    return *this;
}

template <typename CB_SUCCESS, typename>
Transaction &
Transaction::execute(std::string_view expr, CB_SUCCESS &&on_success) {
    return execute(std::string(expr), std::forward<CB_SUCCESS>(on_success),
                   [](error::db_error const &) {});
}

template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::prepare(std::string_view query_name, std::string_view expr,
                     type_oid_sequence &&types, CB_SUCCESS &&on_success,
                     CB_ERROR &&on_error) {
    PreparedQuery query{std::string(query_name), std::string(expr), std::move(types), {}};

    push_transaction(std::unique_ptr<Transaction>(new Prepare<CB_SUCCESS, CB_ERROR>(
        this, std::move(query), std::forward<CB_SUCCESS>(on_success),
        std::forward<CB_ERROR>(on_error))));
    return *this;
}

template <typename CB_SUCCESS, typename>
Transaction &
Transaction::prepare(std::string_view query_name, std::string_view expr,
                     type_oid_sequence &&types, CB_SUCCESS &&on_success) {
    return prepare(query_name, expr, types, std::forward<CB_SUCCESS>(on_success),
                   [](error::db_error const &) {});
}

template <typename CB_SUCCESS, typename CB_ERROR, typename>
Transaction &
Transaction::execute(std::string_view query_name, QueryParams &&params,
                     CB_SUCCESS &&on_success, CB_ERROR &&on_error) {
    if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &, resultset>) {
        push_transaction(std::unique_ptr<Transaction>(new QueryPrepared<CB_SUCCESS, CB_ERROR>(
            this, std::string(query_name), std::move(params),
            std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error))));
    } else if constexpr (std::is_invocable_v<CB_SUCCESS, Transaction &>) {
        push_transaction(std::unique_ptr<Transaction>(new ExecutePrepared<CB_SUCCESS, CB_ERROR>(
            this, std::string(query_name), std::move(params),
            std::forward<CB_SUCCESS>(on_success), std::forward<CB_ERROR>(on_error))));
    } else
        static_assert("execute call_back requires -> [](qb::pg::transaction &tr, "
                      "(optional) qb::pg::results res)");
    return *this;
}

template <typename CB_SUCCESS, typename>
Transaction &
Transaction::execute(std::string_view query_name, QueryParams &&params,
                     CB_SUCCESS &&on_success) {
    return execute(query_name, std::move(params), std::forward<CB_SUCCESS>(on_success),
                   [](error::db_error const &) {});
}

template <typename CB_SUCCESS, typename>
Transaction &
Transaction::execute(std::string_view query_name, CB_SUCCESS &&on_success,
                     QueryParams &&params) {
    return execute(query_name, std::move(params), std::forward<CB_SUCCESS>(on_success),
                   [](error::db_error const &) {});
}

template <typename CB_SUCCESS>
Transaction &
Transaction::then(CB_SUCCESS &&on_success) {
    push_transaction(std::unique_ptr<Transaction>(new Then<CB_SUCCESS>(this, std::forward<CB_SUCCESS>(on_success))));
    return *this;
}

template <typename CB_SUCCESS>
Transaction &
Transaction::success(CB_SUCCESS &&on_success) {
    push_transaction(std::unique_ptr<Transaction>(new Then<CB_SUCCESS>(this, std::forward<CB_SUCCESS>(on_success))));
    return *this;
}

template <typename CB_ERROR>
Transaction &
Transaction::error(CB_ERROR &&on_error) {
    push_transaction(std::unique_ptr<Transaction>(new Error<CB_ERROR>(this, std::forward<CB_ERROR>(on_error))));
    return *this;
}

} // namespace qb::pg::detail

#endif // !QBM_PGSQL_DETAIL_TRANSACTION_INL