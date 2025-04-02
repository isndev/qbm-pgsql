/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#ifndef QBM_PGSQL_NOT_QB_RESULT_IMPL_H
#define QBM_PGSQL_NOT_QB_RESULT_IMPL_H

#include "common.h"
#include "protocol.h"
#include <vector>

namespace qb {
namespace pg {
namespace detail {

class result_impl {
public:
    typedef std::vector<row_data> row_set_type;

public:
    result_impl() = default;
    result_impl(result_impl &) = default;
    result_impl(result_impl &&) = default;
    result_impl &operator=(result_impl &) = default;
    result_impl &operator=(result_impl &&) = default;

    row_description_type &
    row_description() {
        return row_description_;
    }
    row_description_type const &
    row_description() const {
        return row_description_;
    }

    row_set_type &
    rows() {
        return rows_;
    }

    size_t size() const;

    bool empty() const;

    field_buffer at(uinteger row, usmallint col) const;

    row_data::data_buffer_bounds buffer_bounds(uinteger row, usmallint col) const;

    bool is_null(uinteger row, usmallint col) const;

private:
    void check_row_index(uinteger row) const;
    row_description_type row_description_;
    row_set_type rows_;
};

} /* namespace detail */
} /* namespace pg */
} /* namespace qb */

#endif /* QBM_PGSQL_NOT_QB_RESULT_IMPL_H */
