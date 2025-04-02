/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#include "result_impl.h"
#include <exception>
#include <iostream>
#include <sstream>
#include <string>

namespace qb {
namespace pg {
namespace detail {

size_t
result_impl::size() const {
    return rows_.size();
}

bool
result_impl::empty() const {
    return rows_.empty();
}

void
result_impl::check_row_index(uinteger row) const {
    if (row >= rows_.size()) {
        std::ostringstream out;
        out << "Row index " << row << " is out of bounds [0.." << rows_.size() << ")";
        throw std::out_of_range(out.str().c_str());
    }
}

field_buffer
result_impl::at(uinteger row, usmallint col) const {
    check_row_index(row);
    row_data const &rd = rows_[row];
    return rd.field_data(col);
}

bool
result_impl::is_null(uinteger row, usmallint col) const {
    check_row_index(row);
    row_data const &rd = rows_[row];
    return rd.is_null(col);
}

row_data::data_buffer_bounds
result_impl::buffer_bounds(uinteger row, usmallint col) const {
    check_row_index(row);
    row_data const &rd = rows_[row];
    return rd.field_buffer_bounds(col);
}

} /* namespace detail */
} /* namespace pg */
} /* namespace qb */
