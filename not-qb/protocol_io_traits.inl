/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#ifndef QBM_PGSQL_NOT_QB_PROTOCOL_IO_TRAITS_INL
#define QBM_PGSQL_NOT_QB_PROTOCOL_IO_TRAITS_INL

#include "protocol_io_traits.h"
#include "protocol_parsers.h"

#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/range/iterator_range.hpp>

#include "util/endian.h"
#include <algorithm>
#include <cassert>
#include <iterator>

namespace qb {
namespace pg {
namespace io {
namespace detail {

template <typename T>
template <typename InputIterator>
InputIterator
binary_data_parser<T, INTEGRAL>::operator()(InputIterator begin, InputIterator end) {
    typedef std::iterator_traits<InputIterator> iter_traits;
    typedef typename iter_traits::value_type iter_value_type;
    static_assert(std::is_same<iter_value_type, byte>::type::value,
                  "Input iterator must be over a char container");
    assert((end - begin) >= (decltype(end - begin))size() &&
           "Buffer size is insufficient");
    value_type tmp(0);
    char *p = reinterpret_cast<char *>(&tmp);
    char *e = p + size();
    while (p != e && begin != end) {
        *p++ = *begin++;
    }
    tmp = util::endian::big_to_native(tmp);
    std::swap(base_type::value, tmp);
    return begin;
}

} // namespace detail

template <typename T>
template <typename InputIterator>
InputIterator
protocol_parser<T, TEXT_DATA_FORMAT>::operator()(InputIterator begin,
                                                 InputIterator end) {
    typedef std::iterator_traits<InputIterator> iter_traits;
    typedef typename iter_traits::value_type iter_value_type;
    static_assert(std::is_same<iter_value_type, byte>::type::value,
                  "Input iterator must be over a char container");
    boost::iostreams::filtering_istream is(boost::make_iterator_range(begin, end));
    is >> base_type::value;
    begin += size();
    return begin;
}

template <typename InputIterator>
InputIterator
protocol_parser<std::string, TEXT_DATA_FORMAT>::operator()(InputIterator begin,
                                                           InputIterator end) {
    typedef InputIterator iterator_type;
    typedef std::iterator_traits<iterator_type> iter_traits;
    typedef typename iter_traits::value_type iter_value_type;
    static_assert(std::is_same<iter_value_type, byte>::type::value,
                  "Input iterator must be over a char container");

    integer sz = end - begin;

    std::string tmp;
    tmp.reserve(sz);
    for (; begin != end && *begin; ++begin) {
        tmp.push_back(*begin);
    }
    if (begin != end && !*begin)
        ++begin;
    base_type::value.swap(tmp);
    return begin;
}

template <typename InputIterator>
InputIterator
protocol_parser<bool, TEXT_DATA_FORMAT>::operator()(InputIterator begin,
                                                    InputIterator end) {
    typedef InputIterator iterator_type;
    typedef std::iterator_traits<iterator_type> iter_traits;
    typedef typename iter_traits::value_type iter_value_type;
    static_assert(std::is_same<iter_value_type, byte>::type::value,
                  "Input iterator must be over a char container");

    std::string literal;
    iterator_type tmp = protocol_read<TEXT_DATA_FORMAT>(begin, end, literal);
    if (use_literal(literal)) {
        return tmp;
    }
    return begin;
}

template <typename InputIterator>
InputIterator
protocol_parser<bool, BINARY_DATA_FORMAT>::operator()(InputIterator begin,
                                                      InputIterator end) {
    typedef InputIterator iterator_type;
    typedef std::iterator_traits<iterator_type> iter_traits;
    typedef typename iter_traits::value_type iter_value_type;
    static_assert(std::is_same<iter_value_type, byte>::type::value,
                  "Input iterator must be over a char container");

    assert((end - begin) >= (decltype(end - begin))size() &&
           "Buffer size is insufficient");
    base_type::value = *begin++;
    return begin;
}

} // namespace io
} // namespace pg
} // namespace qb

#endif /* QBM_PGSQL_NOT_QB_PROTOCOL_IO_TRAITS_INL */
