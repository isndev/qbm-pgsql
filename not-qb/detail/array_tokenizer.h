/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#ifndef LIB_PG_ASYNC_INCLUDE_TIP_DB_PG_DETAIL_ARRAY_TOKENIZER_HPP_
#define LIB_PG_ASYNC_INCLUDE_TIP_DB_PG_DETAIL_ARRAY_TOKENIZER_HPP_

#include "tokenizer_base.h"

namespace qb {
namespace pg {
namespace io {
namespace detail {

template <typename InputIterator>
class array_tokenizer {
public:
    typedef InputIterator iterator_type;
    typedef tokenizer_base<InputIterator, '{', '}'> tokenizer_type;

public:
    template <typename OutputIterator>
    array_tokenizer(iterator_type &begin, iterator_type end, OutputIterator out) {
        tokenizer_type(begin, end, out);
    }
};

} // namespace detail
} // namespace io
} // namespace pg
} // namespace qb

#endif