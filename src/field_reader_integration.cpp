/**
 * @file field_reader_integration.cpp
 * @brief Field reader integration module
 *
 * This file provides the implementation of the field reader integration components
 * that connect the PostgreSQL binary field reader with the rest of the library.
 * It initializes the static components required for field reading operations.
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

#include "./field_reader.h"

namespace qb::pg::detail {

// Static member definition
ParamUnserializer FieldReader::unserializer;

/**
 * @brief Initializes the field reader module
 *
 * This function is called during the initialization of the PostgreSQL module
 * to set up the field reader components. It ensures that the unserializer
 * is properly configured before any field reading operations take place.
 *
 * Currently, no additional initialization is needed as the unserializer
 * is automatically constructed with appropriate default settings.
 * Future versions may require additional configuration steps.
 */
void
initialize_field_reader() {
    // No initialization needed at this point
}

} // namespace qb::pg::detail