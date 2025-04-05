/**
 * @file pgsql.cpp
 * @brief PostgreSQL client for the QB Actor Framework
 *
 * This file implements the core functionality of the PostgreSQL client:
 *
 * - Module initialization for PostgreSQL type mappings and configurations
 * - Implementation of pipe allocator specialization for PostgreSQL messages
 * - Explicit template instantiations for different transport types
 *
 * The implementation works in conjunction with pgsql.h to provide a complete
 * asynchronous PostgreSQL client solution for the QB Actor Framework.
 *
 * @see qb::pg::detail::Database
 * @see qb::pg::detail::Transaction
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "./pgsql.h"

namespace qb::pg {

/**
 * @brief Initialize the PostgreSQL module
 *
 * This function performs the following operations:
 * - Sets up default configurations for PostgreSQL connections
 * - Initializes the static database of PostgreSQL types and their mappings
 * - Configures field readers for different data types
 *
 * This function is called once during application startup to prepare
 * the PostgreSQL subsystem for use with the QB Actor Framework.
 */
void
init() {
    // Initialize default configuration
    // Load type databases
    // Static databases are loaded at startup

    // Initialize the field reader for PostgreSQL data types
    detail::initialize_field_reader();
}

} // namespace qb::pg

namespace qb::allocator {
/**
 * @brief Template specialization for pipe allocation with PostgreSQL messages
 *
 * This specialization handles the efficient allocation of PostgreSQL protocol messages
 * in the pipe allocator. It extracts the buffer data from the message and writes
 * it to the pipe.
 *
 * @param msg PostgreSQL message to write to the pipe
 * @return Reference to the pipe after data has been written
 */
template <>
pipe<char> &
pipe<char>::put<qb::pg::detail::message>(const qb::pg::detail::message &msg) {
    auto buffer_range = msg.buffer();
    // Write buffer data to the pipe
    if (buffer_range.first != buffer_range.second) {
        const char *data = &(*buffer_range.first);
        std::size_t size = std::distance(buffer_range.first, buffer_range.second);
        put(data, size);
    }
    return *this;
}
} // namespace qb::allocator

// Explicit template instantiations for Database class with different transport types
template class qb::pg::detail::Database<qb::io::transport::tcp>;

#ifdef QB_IO_WITH_SSL

template class qb::pg::detail::Database<qb::io::transport::stcp>;

#endif