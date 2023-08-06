/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#ifndef QBM_PGSQL_NOT_QB_BOOST_DATE_TIME_H
#define QBM_PGSQL_NOT_QB_BOOST_DATE_TIME_H

#include "../protocol_io_traits.h"
#include "../util/endian.h"

#include <boost/date_time.hpp>
#include <boost/spirit/include/karma.hpp>
#include <boost/spirit/include/phoenix.hpp>
#include <boost/spirit/include/qi.hpp>

namespace qb {
namespace pg {
namespace io {

namespace grammar {
namespace parse {

template <typename InputIterator>
struct time_grammar
    : boost::spirit::qi::grammar<InputIterator, boost::posix_time::time_duration()> {
    typedef boost::posix_time::time_duration value_type;
    time_grammar()
        : time_grammar::base_type(time) {
        namespace qi = boost::spirit::qi;
        namespace phx = boost::phoenix;
        using qi::_1;
        using qi::_2;
        using qi::_3;
        using qi::_4;
        using qi::_pass;
        using qi::_val;
        _2digits = qi::uint_parser<std::uint32_t, 10, 2, 2>();
        fractional_part = -('.' >> qi::long_);

        time = (_2digits >> ':' >> _2digits >> ':' >> _2digits >>
                fractional_part)[_pass = (_1 < 24) && (_2 < 60) && (_3 < 60),
                                 _val = phx::construct<value_type>(_1, _2, _3, _4)];
    }
    boost::spirit::qi::rule<InputIterator, value_type()> time;
    boost::spirit::qi::rule<InputIterator, std::uint32_t()> _2digits;
    boost::spirit::qi::rule<InputIterator, std::uint64_t()> fractional_part;
};

template <typename InputIterator>
struct date_grammar
    : boost::spirit::qi::grammar<InputIterator, boost::gregorian::date()> {
    typedef boost::gregorian::date value_type;
    date_grammar()
        : date_grammar::base_type(date) {
        namespace qi = boost::spirit::qi;
        namespace phx = boost::phoenix;
        using qi::_1;
        using qi::_2;
        using qi::_3;
        using qi::_pass;
        using qi::_val;
        _2digits = qi::uint_parser<std::uint32_t, 10, 2, 2>();
        _4digits = qi::uint_parser<std::uint32_t, 10, 4, 4>();
        date = (_4digits >> "-" >> _2digits >> "-" >>
                _2digits)[_pass = (1400 <= _1) && (_1 <= 10000) && (_3 < 32),
                          phx::try_[_val = phx::construct<value_type>(_1, _2, _3)]
                              .catch_all[_pass = false]];
    }
    boost::spirit::qi::rule<InputIterator, value_type()> date;
    boost::spirit::qi::rule<InputIterator, std::int32_t()> _2digits;
    boost::spirit::qi::rule<InputIterator, std::int32_t()> _4digits;
};

template <typename InputIterator, typename DateTime>
struct datetime_grammar;

template <typename InputIterator>
struct datetime_grammar<InputIterator, boost::posix_time::ptime>
    : boost::spirit::qi::grammar<InputIterator, boost::posix_time::ptime()> {
    typedef boost::posix_time::ptime value_type;
    datetime_grammar()
        : datetime_grammar::base_type(datetime) {
        namespace qi = boost::spirit::qi;
        namespace phx = boost::phoenix;
        using qi::_1;
        using qi::_2;
        using qi::_val;

        datetime = (date >> ' ' >> time)[_val = phx::construct<value_type>(_1, _2)];
    }
    boost::spirit::qi::rule<InputIterator, value_type()> datetime;
    date_grammar<InputIterator> date;
    time_grammar<InputIterator> time;
};

} // namespace parse
} // namespace grammar

template <>
struct protocol_parser<boost::posix_time::ptime, TEXT_DATA_FORMAT>
    : detail::parser_base<boost::posix_time::ptime> {
    typedef detail::parser_base<boost::posix_time::ptime> base_type;
    typedef typename base_type::value_type value_type;
    protocol_parser(value_type &v)
        : base_type(v) {}

    size_t size() const;

    template <typename InputIterator>
    InputIterator
    operator()(InputIterator begin, InputIterator end) {
        typedef std::iterator_traits<InputIterator> iter_traits;
        typedef typename iter_traits::value_type iter_value_type;
        static_assert(std::is_same<iter_value_type, byte>::type::value,
                      "Input iterator must be over a char container");
        namespace qi = boost::spirit::qi;
        namespace parse = grammar::parse;

        parse::datetime_grammar<InputIterator, value_type> grammar;
        value_type tmp;
        if (qi::parse(begin, end, grammar, tmp)) {
            std::swap(tmp, base_type::value);
        }
        return begin;
    }
};

template <>
struct protocol_parser<boost::posix_time::ptime, BINARY_DATA_FORMAT>
    : detail::parser_base<boost::posix_time::ptime> {

    using base_type = detail::parser_base<boost::posix_time::ptime>;
    using value_type = base_type::value_type;
    protocol_parser(value_type &v)
        : base_type(v) {}

    size_t
    size() const {
        return sizeof(bigint);
    }

    template <typename InputIterator>
    InputIterator
    operator()(InputIterator begin, InputIterator end) {
        bigint tmp{0};
        auto res = protocol_read<BINARY_DATA_FORMAT>(begin, end, tmp);
        from_int_value(tmp);
        return res;
    }

    void from_int_value(bigint);
};

template <>
struct protocol_formatter<boost::posix_time::ptime, BINARY_DATA_FORMAT>
    : detail::formatter_base<boost::posix_time::ptime> {

    using ptime = boost::posix_time::ptime;
    using time_duration = boost::posix_time::time_duration;
    using base_type = detail::formatter_base<boost::posix_time::ptime>;
    using value_type = base_type::value_type;

    static value_type const pg_epoch;

    protocol_formatter(value_type const &val)
        : base_type(val) {}

    size_t
    size() const {
        return sizeof(bigint);
    }

    bool operator()(std::vector<byte> &buffer);
};

namespace traits {

template <>
struct has_parser<boost::posix_time::ptime, BINARY_DATA_FORMAT> : std::true_type {};
template <>
struct has_formatter<boost::posix_time::ptime, BINARY_DATA_FORMAT> : std::true_type {};

//@{
template <>
struct pgcpp_data_mapping<oids::type::timestamp>
    : detail::data_mapping_base<oids::type::timestamp, boost::posix_time::ptime> {};
template <>
struct cpppg_data_mapping<boost::posix_time::ptime>
    : detail::data_mapping_base<oids::type::timestamp, boost::posix_time::ptime> {};

template <>
struct is_nullable<boost::posix_time::ptime> : ::std::true_type {};

template <>
struct nullable_traits<boost::posix_time::ptime> {
    inline static bool
    is_null(boost::posix_time::ptime const &val) {
        return val.is_not_a_date_time();
    }
    inline static void
    set_null(boost::posix_time::ptime &val) {
        val = boost::posix_time::ptime{};
    }
};

template <>
struct needs_quotes<boost::posix_time::ptime> : ::std::true_type {};
//@}

} // namespace traits

} // namespace io
} // namespace pg
} // namespace qb

#endif /* QBM_PGSQL_NOT_QB_BOOST_DATE_TIME_H */
