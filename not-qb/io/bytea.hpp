/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#ifndef QBM_PGSQL_NOT_QB_BOOST_BYTEA_H
#define QBM_PGSQL_NOT_QB_BOOST_BYTEA_H

#include "../protocol_io_traits.h"

namespace qb {
namespace pg {
namespace io {

/**
 * @brief Protocol parser specialization for bytea (binary string), text data format
 */
template <>
struct protocol_parser<bytea, TEXT_DATA_FORMAT> : detail::parser_base<bytea> {
    typedef detail::parser_base<bytea> base_type;
    typedef base_type::value_type value_type;
    typedef qb::util::input_iterator_buffer buffer_type;

    protocol_parser(value_type &v)
        : base_type(v) {}

    size_t
    size() const {
        return base_type::value.size();
    }
    bool operator()(std::istream &in);

    bool operator()(buffer_type &buffer);

    template <typename InputIterator>
    InputIterator operator()(InputIterator begin, InputIterator end);
};

/**
 * @brief Protocol parser specialization for bytea (binary string), binary data format
 */
template <>
struct protocol_parser<bytea, BINARY_DATA_FORMAT> : detail::parser_base<bytea> {
    typedef detail::parser_base<bytea> base_type;
    typedef base_type::value_type value_type;

    protocol_parser(value_type &val)
        : base_type(val) {}
    size_t
    size() const {
        return base_type::value.size();
    }
    template <typename InputIterator>
    InputIterator operator()(InputIterator begin, InputIterator end);
};
namespace traits {

template <>
struct has_parser<bytea, TEXT_DATA_FORMAT> : std::true_type {};
template <>
struct has_parser<bytea, BINARY_DATA_FORMAT> : std::true_type {};

//@{
template <>
struct pgcpp_data_mapping<oids::type::bytea>
    : detail::data_mapping_base<oids::type::bytea, bytea> {};
template <>
struct cpppg_data_mapping<bytea>
    : detail::data_mapping_base<oids::type::bytea, bytea> {};
//@}

static_assert(has_parser<bytea, BINARY_DATA_FORMAT>::value,
              "Binary data parser for bool");
static_assert(best_parser<bytea>::value == BINARY_DATA_FORMAT,
              "Best parser for bool is binary");

} // namespace traits

template <typename InputIterator>
InputIterator
protocol_parser<bytea, TEXT_DATA_FORMAT>::operator()(InputIterator begin,
                                                     InputIterator end) {
    typedef InputIterator iterator_type;
    typedef std::iterator_traits<iterator_type> iter_traits;
    typedef typename iter_traits::value_type iter_value_type;
    static_assert(std::is_same<iter_value_type, byte>::type::value,
                  "Input iterator must be over a char container");
    std::vector<byte> data;

    auto result = detail::bytea_parser().parse(begin, end, std::back_inserter(data));
    if (result.first) {
        base_type::value.swap(data);
        return result.second;
    }
    return begin;
}

template <typename InputIterator>
InputIterator
protocol_parser<bytea, BINARY_DATA_FORMAT>::operator()(InputIterator begin,
                                                       InputIterator end) {
    typedef InputIterator iterator_type;
    typedef std::iterator_traits<iterator_type> iter_traits;
    typedef typename iter_traits::value_type iter_value_type;
    static_assert(std::is_same<iter_value_type, byte>::type::value,
                  "Input iterator must be over a char container");

    bytea tmp(begin, end);
    std::swap(base_type::value, tmp);

    return end;
}

} // namespace io
} // namespace pg
} // namespace qb

#endif /* QBM_PGSQL_NOT_QB_BOOST_BYTEA_H */
