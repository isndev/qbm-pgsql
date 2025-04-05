/**
 * @file pgsql.cpp
 * @brief Implements initialization of Postgres module
 */

#include "pgsql.h"
#include "detail/transaction.h"
#include "detail/field_reader.h"
#include "detail/tuple_converter.h"
#include "not-qb/protocol.h"

namespace qb::pg {

/**
 * Initialize configuration defaults and type database.
 * 
 * Load static database of types, etc.
 * called once, at startup. 
 */
void init() {
    // Initialisation par défaut de la configuration
    // Chargement des bases de données de types
    // Les bases de données statiques sont chargées au démarrage
    
    // Initialiser le lecteur de champs
    detail::initialize_field_reader();
}

} // namespace qb::pg

namespace qb::allocator {
    template <>
    pipe<char> &
    pipe<char>::put<qb::pg::detail::message>(const qb::pg::detail::message &msg) {
        auto buffer_range = msg.buffer();
        // Écrire les données du buffer dans le pipe
        if (buffer_range.first != buffer_range.second) {
            const char* data = &(*buffer_range.first);
            std::size_t size = std::distance(buffer_range.first, buffer_range.second);
            put(data, size);
        }
        return *this;
    }
} // namespace qb::allocator

// Implémentation des templates pour Database
template class qb::pg::detail::Database<qb::io::transport::tcp>;
#ifdef QB_IO_WITH_SSL
template class qb::pg::detail::Database<qb::io::transport::stcp>;
#endif