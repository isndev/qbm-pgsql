/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#ifndef LIB_PG_ASYNC_INCLUDE_TIP_DB_PG_IO_VECTOR_HPP_
#define LIB_PG_ASYNC_INCLUDE_TIP_DB_PG_IO_VECTOR_HPP_

#include "../protocol_io_traits.h"
#include "container_to_array.hpp"

#include <vector>

namespace qb {
namespace pg {
namespace io {

template <typename T>
struct protocol_parser<std::vector<T>, TEXT_DATA_FORMAT>
    : detail::text_container_parser<protocol_parser<std::vector<T>, TEXT_DATA_FORMAT>,
                                    std::vector<T>> {

    typedef detail::text_container_parser<
        protocol_parser<std::vector<T>, TEXT_DATA_FORMAT>, std::vector<T>>
        base_type;
    typedef typename base_type::value_type value_type;

    protocol_parser(value_type &v)
        : base_type(v) {}

    typename base_type::token_iterator
    tokens_end(typename base_type::tokens_list const &tokens) {
        return tokens.end();
    }

    template <typename InputIterator>
    InputIterator
    operator()(InputIterator begin, InputIterator end) {
        base_type::value.clear();
        return base_type::parse(begin, end, std::back_inserter(base_type::value));
    }
};

namespace traits {

template <typename T>
struct has_parser<std::vector<T>, TEXT_DATA_FORMAT> : std::true_type {};

template <typename T>
struct cpppg_data_mapping<std::vector<T>>
    : detail::data_mapping_base<oids::type::text, std::vector<T>> {};

} // namespace traits

} // namespace io
} // namespace pg
} // namespace qb

#endif /* LIB_PG_ASYNC_INCLUDE_TIP_DB_PG_IO_VECTOR_HPP_ */
