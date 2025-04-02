/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#ifndef LIB_PG_ASYNC_INCLUDE_TIP_DB_PG_IO_CONTAINER_TO_ARRAY_HPP_
#define LIB_PG_ASYNC_INCLUDE_TIP_DB_PG_IO_CONTAINER_TO_ARRAY_HPP_

#include "../protocol_io_traits.h"
#include "../detail/array_tokenizer.h"

namespace qb {
namespace pg {
namespace io {
namespace detail {

template <typename Parser, typename Container>
struct text_container_parser : detail::parser_base<Container> {

    typedef Parser parser_type;
    typedef detail::parser_base<Container> base_type;
    typedef typename base_type::value_type value_type;
    typedef typename value_type::value_type element_type;
    typedef std::vector<std::string> tokens_list;
    typedef tokens_list::const_iterator token_iterator;

    text_container_parser(value_type &v)
        : base_type(v) {}

    template <typename InputIterator, typename OutputIterator>
    InputIterator
    parse(InputIterator begin, InputIterator end, OutputIterator out) {
        typedef std::iterator_traits<InputIterator> iter_traits;
        typedef typename iter_traits::value_type iter_value_type;
        static_assert(std::is_same<iter_value_type, byte>::type::value,
                      "Input iterator must be over a char container");

        typedef detail::array_tokenizer<InputIterator> array_tokenizer;

        tokens_list tokens;
        array_tokenizer(begin, end, std::back_inserter(tokens));

        token_iterator tok = tokens.begin();
        token_iterator tok_end = parser().tokens_end(tokens);

        for (; tok != tok_end; ++tok) {
            element_type tmp;
            protocol_read<TEXT_DATA_FORMAT>(tok->begin(), tok->end(), tmp);
            *out++ = std::move(tmp);
        }
        return begin;
    }

    parser_type &
    parser() {
        return static_cast<parser_type &>(*this);
    }
};

} // namespace detail
} // namespace io
} // namespace pg
} // namespace qb

#endif /* LIB_PG_ASYNC_INCLUDE_TIP_DB_PG_IO_CONTAINER_TO_ARRAY_HPP_ */
