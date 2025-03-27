/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2025 qb - isndev (cpp.actor). All rights reserved.
 *
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
 *         limitations under the License.
 */

#include "pgsql.h"

namespace qb::allocator {

template <>
pipe<char> &
pipe<char>::put<qb::pg::detail::message>(const qb::pg::detail::message &msg) {
    const auto data_range = msg.buffer();

    return put(&*data_range.first, data_range.second - data_range.first);
}

} // namespace qb::allocator

template class qb::pg::detail::Database<qb::io::transport::tcp>;
#ifdef QB_IO_WITH_SSL
template class qb::pg::detail::Database<qb::io::transport::stcp>;
#endif