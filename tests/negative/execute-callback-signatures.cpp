// Compile-fail control for Transaction::execute callback signatures.
// Both calls must be rejected: neither callback accepts a Transaction&.
#include <qbm/pgsql/pgsql.h>

void
invalid_execute_callbacks(qb::pg::transaction &tr) {
    tr.execute("SELECT 1", [] {}, qb::pg::discard_error);
    tr.execute("named", qb::pg::detail::QueryParams{}, [] {}, qb::pg::discard_error);
}
