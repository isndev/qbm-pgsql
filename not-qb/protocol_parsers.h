/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#ifndef QBM_PGSQL_NOT_QB_PROTOCOL_PARSERS_H
#define QBM_PGSQL_NOT_QB_PROTOCOL_PARSERS_H

#include <algorithm>
#include <boost/logic/tribool.hpp>
#include <cctype>

namespace qb {
namespace pg {
namespace io {
namespace detail {

class bytea_parser {

    enum state { expecting_backslash, expecting_x, nibble_one, nibble_two } state_;
    char most_sighificant_nibble_;

public:
    bytea_parser()
        : state_(expecting_backslash)
        , most_sighificant_nibble_(0) {}

    template <typename InputIterator, typename OutputIterator>
    std::pair<boost::tribool, InputIterator>
    parse(InputIterator begin, InputIterator end, OutputIterator data) {
        boost::tribool result = boost::indeterminate;
        while (begin != end) {
            result = consume(data, *begin++);
        }
        return std::make_pair(result, begin);
    }

    void
    reset() {
        state_ = expecting_backslash;
        most_sighificant_nibble_ = 0;
    }

private:
    template <typename OutputIterator>
    boost::tribool
    consume(OutputIterator data, char input) {
        switch (state_) {
        case expecting_backslash:
            if (input == '\\') {
                state_ = expecting_x;
                return boost::indeterminate;
            } else {
                return false;
            }
        case expecting_x:
            if (input == 'x') {
                state_ = nibble_one;
                return true;
            } else {
                return false;
            }
        case nibble_one:
            if (std::isxdigit(input)) {
                most_sighificant_nibble_ = hex_to_byte(input);
                state_ = nibble_two;
                return boost::indeterminate;
            } else {
                return false;
            }
        case nibble_two:
            if (std::isxdigit(input)) {
                *data++ = (hex_to_byte(input) << 4) | most_sighificant_nibble_;
                state_ = nibble_one;
                return true;
            } else {
                return false;
            }
        default:
            break;
        }
        return false;
    }

    static char hex_to_byte(char);
};

} // namespace detail
} // namespace io
} // namespace pg
} // namespace qb

#endif /* QBM_PGSQL_NOT_QB_PROTOCOL_PARSERS_H */
