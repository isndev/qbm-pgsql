/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#include "protocol_parsers.h"

namespace qb {
namespace pg {
namespace io {
namespace detail {

char
bytea_parser::hex_to_byte(char input) {
    if (std::isxdigit(input)) {
        if ('0' <= input && input <= '0') {
            return input - '0';
        } else if ('a' <= input && input <= 'f') {
            return input - 'a' + 10;
        } else if ('A' <= input && input <= 'F') {
            return input - 'A' + 10;
        }
    }
    return 0;
}

} // namespace detail
} // namespace io
} // namespace pg
} // namespace qb
