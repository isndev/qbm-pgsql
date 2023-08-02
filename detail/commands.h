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

#ifndef QBM_PGSQL_DETAIL_COMMANDS_H
#define QBM_PGSQL_DETAIL_COMMANDS_H
#include "../not-qb/result_impl.h"
#include "../not-qb/resultset.h"
#include "transaction.h"

namespace qb::pg::detail {
using namespace qb::pg;

template <typename CB_ERROR>
class End final : public Transaction {
    CB_ERROR _on_error;

public:
    End(Transaction *parent, CB_ERROR &&on_error)
        : Transaction(parent)
        , _on_error(std::forward<CB_ERROR>(on_error)) {}

    CB_ERROR &
    get_error_callback() {
        return _on_error;
    }

    void
    on_end_transaction() {
        push_query(_result ? (ISqlQuery *)new CommitQuery(
                                 []() {
                                     // commited
                                 },
                                 [this](auto const &err) { _on_error(err); })
                           : (ISqlQuery *)new RollbackQuery(
                                 [this]() {
                                     _on_error((error::db_error)error::query_error(
                                         "rollback processed due to a query failure"));
                                 },
                                 [this](auto const &err) { _on_error(err); }));
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class Begin final : public Transaction {
    End<CB_ERROR> *_end;
    transaction_mode _mode;
    CB_SUCCESS _on_success;

public:
    Begin(Transaction *parent, End<CB_ERROR> *end, transaction_mode mode,
          CB_SUCCESS &&on_success)
        : Transaction(parent)
        , _end(end)
        , _mode(mode)
        , _on_success(std::forward<CB_SUCCESS>(on_success)) {
        push_query(new BeginQuery(
            mode,
            [this]() {
                try {
                    _on_success(*this);
                } catch (std::exception const &e) {
                    _result = false;
                    _end->get_error_callback()(
                        (error::db_error)error::client_error{e.what()});
                }
            },
            [this](auto const &err) { _end->get_error_callback()(err); }));
    }

    ~Begin() {
        _end->result(_result);
        _end->on_end_transaction();
    }

    void
    on_sub_command_status(bool status) final {
        _result &= status;
    }
};

template <typename CB_ERROR>
class EndSavePoint final : public Transaction {
    const std::string _name;
    CB_ERROR _on_error;

public:
    EndSavePoint(Transaction *parent, std::string &&name, CB_ERROR &&on_error)
        : Transaction(parent)
        , _name(std::move(name))
        , _on_error(std::forward<CB_ERROR>(on_error)) {}

    std::string const &
    get_name() {
        return _name;
    }

    CB_ERROR &
    get_error_callback() {
        return _on_error;
    }

    void
    on_end_savepoint() {
        push_query(_result ? (ISqlQuery *)new ReleaseSavePointQuery(
                                 _name,
                                 [this]() {
                                     // commited
                                 },
                                 [this](auto const &err) { _on_error(err); })
                           : (ISqlQuery *)new RollbackSavePointQuery(
                                 _name,
                                 [this]() {
                                     _on_error((error::db_error)error::query_error(
                                         "savepoint rollback processed due to a "
                                         "query failure"));
                                 },
                                 [this](auto const &err) { _on_error(err); }));
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class SavePoint final : public Transaction {
    EndSavePoint<CB_ERROR> *_end;
    CB_SUCCESS _on_success;

public:
    SavePoint(Transaction *parent, EndSavePoint<CB_ERROR> *end, CB_SUCCESS &&on_success)
        : Transaction(parent)
        , _end(end)
        , _on_success(std::forward<CB_SUCCESS>(on_success)) {
        push_query(new SavePointQuery(
            _end->get_name(),
            [this]() {
                try {
                    _on_success(*this);
                } catch (std::exception const &e) {
                    _result = false;
                    _end->get_error_callback()(
                        (error::db_error)error::client_error{e.what()});
                }
            },
            [this](auto const &err) { _end->get_error_callback()(err); }));
    }

    ~SavePoint() {
        _end->result(_result);
        _end->on_end_savepoint();
    }

    void
    on_sub_command_status(bool status) final {
        _result &= status;
        _parent->on_sub_command_status(status);
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class Query final : public Transaction {
    CB_SUCCESS _on_success;
    CB_ERROR _on_error;

public:
    Query(Transaction *parent, std::string &&expr, CB_SUCCESS &&on_success,
          CB_ERROR &&on_error)
        : Transaction(parent)
        , _on_success(std::forward<CB_SUCCESS>(on_success))
        , _on_error(std::forward<CB_ERROR>(on_error)) {
        push_query(new SimpleQuery(
            std::move(expr),
            [this]() {
                try {
                    _on_success(*this);
                } catch (std::exception const &e) {
                    _result = false;
                    _on_error((error::db_error)error::client_error{e.what()});
                }
            },
            [this](auto const &err) { _on_error(err); }));
    }

//    void
//    on_sub_command_status(bool status) final {
//        _parent->on_sub_command_status(status);
//    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class ResultQuery final : public Transaction {
    CB_SUCCESS _on_success;
    CB_ERROR _on_error;
    result_impl _results;

public:
    ResultQuery(Transaction *parent, std::string &&expr, CB_SUCCESS &&on_success,
                CB_ERROR &&on_error)
        : Transaction(parent)
        , _on_success(std::forward<CB_SUCCESS>(on_success))
        , _on_error(std::forward<CB_ERROR>(on_error)) {
        push_query(new SimpleQuery(
            std::move(expr),
            [this]() {
                try {
                    _on_success(*this, resultset(&_results));
                } catch (std::exception const &e) {
                    _result = false;
                    _on_error((error::db_error)error::client_error{e.what()});
                }
            },
            [this](auto const &err) { _on_error(err); }));
    }

    void
    on_new_row_description(row_description_type &&desc) final {
        _results.row_description() = std::move(desc);
    };
    void
    on_new_data_row(row_data &&data) final {
        _results.rows().push_back(std::move(data));
    }
};

template <typename CB_SUCCESS>
class Then final : public Transaction {
    CB_SUCCESS _on_success;

public:
    Then(Transaction *parent, CB_SUCCESS &&on_success)
        : Transaction(parent)
        , _on_success(std::forward<CB_SUCCESS>(on_success)) {}

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

template <typename CB_SUCCESS, typename CB_ERROR>
class Prepare final : public Transaction {
    CB_SUCCESS _on_success;
    CB_ERROR _on_error;
    PreparedQuery _query;

public:
    Prepare(Transaction *parent, PreparedQuery &&query, CB_SUCCESS &&on_success,
            CB_ERROR &&on_error)
        : Transaction(parent)
        , _on_success(std::forward<CB_SUCCESS>(on_success))
        , _on_error(std::forward<CB_ERROR>(on_error))
        , _query(std::move(query)) {
        push_query(new ParseQuery(
            _query,
            [this]() {
                try {
                    _on_success(*this, _query_storage.push(std::move(_query)));
                } catch (std::exception const &e) {
                    _result = false;
                    _on_error((error::db_error)error::client_error{e.what()});
                }
            },
            [this](auto const &err) { _on_error(err); }));
    }

    void
    on_new_row_description(row_description_type &&desc) {
        _query.row_description = std::move(desc);
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class ExecutePrepared final : public Transaction {
    CB_SUCCESS _on_success;
    CB_ERROR _on_error;
    const std::string _query_name;

public:
    ExecutePrepared(Transaction *parent, std::string const &query_name,
                    std::vector<byte> &&params, CB_SUCCESS &&on_success,
                    CB_ERROR &&on_error)
        : Transaction(parent)
        , _on_success(std::forward<CB_SUCCESS>(on_success))
        , _on_error(std::forward<CB_ERROR>(on_error))
        , _query_name(query_name) {
        push_query(new BindExecQuery(
            _query_storage, _query_name, std::move(params),
            [this]() {
                try {
                    _on_success(*this);
                } catch (std::exception const &e) {
                    _result = false;
                    _on_error((error::db_error)error::client_error{e.what()});
                }
            },
            [this](auto const &err) { _on_error(err); }));
    }
};

template <typename CB_SUCCESS, typename CB_ERROR>
class QueryPrepared final : public Transaction {
    CB_SUCCESS _on_success;
    CB_ERROR _on_error;
    const std::string _query_name;
    result_impl _results;

public:
    QueryPrepared(Transaction *parent, std::string const &query_name,
                  std::vector<byte> &&params, CB_SUCCESS &&on_success,
                  CB_ERROR &&on_error)
        : Transaction(parent)
        , _on_success(std::forward<CB_SUCCESS>(on_success))
        , _on_error(std::forward<CB_ERROR>(on_error))
        , _query_name(query_name) {
        push_query(new BindExecQuery(
            _query_storage, _query_name, std::move(params),
            [this]() {
                try {
                    _results.row_description() =
                        _query_storage.get(_query_name).row_description;
                    _on_success(*this, resultset(&_results));
                } catch (std::exception const &e) {
                    _result = false;
                    _on_error((error::db_error)error::client_error{e.what()});
                }
            },
            [this](auto const &err) { _on_error(err); }));
    }

    void
    on_new_data_row(row_data &&data) final {
        _results.rows().push_back(std::move(data));
    }
};

} // namespace qb::pg::detail

#endif // QBM_PGSQL_DETAIL_COMMANDS_H
