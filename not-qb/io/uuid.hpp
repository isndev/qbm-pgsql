/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#ifndef LIB_PG_ASYNC_INCLUDE_TIP_DB_PG_IO_UUID_HPP_
#define LIB_PG_ASYNC_INCLUDE_TIP_DB_PG_IO_UUID_HPP_

#include "../protocol_io_traits.h"
#include <qb/uuid.h>

#include <iterator>

namespace qb {
namespace pg {
namespace io {

template <>
struct protocol_parser<qb::uuid, BINARY_DATA_FORMAT> : detail::parser_base<qb::uuid> {

    using uuid = qb::uuid;
    using base_type = detail::parser_base<uuid>;
    using value_type = base_type::value_type;

    protocol_parser(value_type &v)
        : base_type(v) {}

    size_t
    size() const {
        return 16;
    }

    template <typename InputIterator>
    InputIterator
    operator()(InputIterator begin, InputIterator end) {
        typedef std::iterator_traits<InputIterator> iter_traits;
        typedef typename iter_traits::value_type iter_value_type;
        static_assert(std::is_same<iter_value_type, byte>::type::value,
                      "Input iterator must be over a char container");
        assert((end - begin) >= (decltype(end - begin))size() &&
               "Buffer size is insufficient");
        end = begin + size();
        ::std::copy(begin, end, base_type::value.begin());
        return begin;
    }
};

template <>
struct protocol_formatter<qb::uuid, BINARY_DATA_FORMAT>
    : detail::formatter_base<qb::uuid> {

    using uuid = qb::uuid;
    using base_type = detail::formatter_base<uuid>;
    using value_type = base_type::value_type;

    protocol_formatter(value_type const &val)
        : base_type(val) {}

    size_t
    size() const {
        return 16;
    }

    bool
    operator()(::std::vector<byte> &buffer) {
        if (buffer.capacity() - buffer.size() < size()) {
            buffer.reserve(buffer.size() + size());
        }

        ::std::copy(base_type::value.begin(), base_type::value.end(),
                    ::std::back_inserter(buffer));
        return true;
    }
};

namespace traits {

template <>
struct has_parser<qb::uuid, BINARY_DATA_FORMAT> : std::true_type {};
template <>
struct has_formatter<qb::uuid, BINARY_DATA_FORMAT> : std::true_type {};

//@{
template <>
struct pgcpp_data_mapping<oids::type::uuid>
    : detail::data_mapping_base<oids::type::uuid, qb::uuid> {};
template <>
struct cpppg_data_mapping<qb::uuid>
    : detail::data_mapping_base<oids::type::uuid, qb::uuid> {};
//@}

template <>
struct is_nullable<qb::uuid> : ::std::true_type {};

template <>
struct nullable_traits<qb::uuid> {
    using uuid = qb::uuid;
    inline static bool
    is_null(uuid const &val) {
        return val.is_nil();
    }
    inline static void
    set_null(uuid &val) {
        val = uuid{};
    }
};

} // namespace traits
} // namespace io
} // namespace pg
} // namespace qb

#endif /* LIB_PG_ASYNC_INCLUDE_TIP_DB_PG_IO_UUID_HPP_ */
