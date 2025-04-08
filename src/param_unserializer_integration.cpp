/**
 * @file param_unserializer_integration.cpp
 * @brief Integration of the modern deserializer with existing code
 *
 * This file provides the integration between our modern deserialization system
 * and existing code without introducing conflicts. It handles the conversion from
 * PostgreSQL result rows to C++ tuples through explicit template instantiation
 * and specialized wrappers.
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

#include "./param_unserializer.h"
#include "./resultset.h"
#include "./tuple_converter.h"

namespace qb::pg::detail {

/**
 * @brief Wrapper to convert a resultset::row to C++ tuple
 *
 * This template function serves as a bridge between the resultset row data
 * and the tuple converter. It abstracts away the direct conversion details,
 * allowing for easier maintenance and potential future changes to the
 * conversion mechanism.
 *
 * @tparam Tuple The target tuple type to convert into
 * @param row The PostgreSQL result row containing the source data
 * @param tuple The target tuple to store the converted values
 */
template <typename Tuple>
void
tuple_conversion_wrapper(const resultset::row &row, Tuple &tuple) {
    direct_row_to_tuple(row, tuple);
}

// Explicit template instantiations for common types
// This ensures that templates will be generated at compile time

/**
 * @brief Explicit instantiation for single integer tuple
 */
template void tuple_conversion_wrapper<std::tuple<int>>(const resultset::row &,
                                                        std::tuple<int> &);

/**
 * @brief Explicit instantiation for single string tuple
 */
template void
tuple_conversion_wrapper<std::tuple<std::string>>(const resultset::row &,
                                                  std::tuple<std::string> &);

/**
 * @brief Explicit instantiation for int, string tuple
 */
template void
tuple_conversion_wrapper<std::tuple<int, std::string>>(const resultset::row &,
                                                       std::tuple<int, std::string> &);

/**
 * @brief Explicit instantiation for string, int tuple
 */
template void
tuple_conversion_wrapper<std::tuple<std::string, int>>(const resultset::row &,
                                                       std::tuple<std::string, int> &);

/**
 * @brief Explicit instantiation for int, int tuple
 */
template void tuple_conversion_wrapper<std::tuple<int, int>>(const resultset::row &,
                                                             std::tuple<int, int> &);

/**
 * @brief Explicit instantiation for string, string tuple
 */
template void tuple_conversion_wrapper<std::tuple<std::string, std::string>>(
    const resultset::row &, std::tuple<std::string, std::string> &);

/**
 * @brief Explicit instantiation for int, int, int tuple
 */
template void
tuple_conversion_wrapper<std::tuple<int, int, int>>(const resultset::row &,
                                                    std::tuple<int, int, int> &);

/**
 * @brief Explicit instantiation for string, string, string tuple
 */
template void
tuple_conversion_wrapper<std::tuple<std::string, std::string, std::string>>(
    const resultset::row &, std::tuple<std::string, std::string, std::string> &);

/**
 * @brief Initializes the deserialization system
 *
 * This function is called during module initialization to ensure
 * that our modern deserializer is ready to use. Currently it's a placeholder,
 * but allows for future expansion if initialization steps become necessary.
 */
void
initialize_param_unserializer() {
    // ParamUnserializer initialization
    // (No specific initialization required at the moment)
}

} // namespace qb::pg::detail

namespace qb::pg {

// Extension methods for the resultset::row class using our modern deserializer

/**
 * @brief Converts row data into a tuple of values
 *
 * This method allows direct conversion of a row's data into a tuple of values.
 * It uses the modern converter system instead of going through the protocol_parser.
 *
 * @tparam T Parameter pack of the tuple's value types
 * @param tuple The target tuple that will receive the converted values
 */
template <typename... T>
void
resultset::row::to(std::tuple<T...> &tuple) const {
    // Use our modern converter directly, without going through protocol_parser
    detail::direct_row_to_tuple(*this, tuple);
}

/**
 * @brief Converts row data into a tuple of references
 *
 * This overload handles tuples of references, allowing direct assignment
 * to existing variables wrapped in a tuple.
 *
 * @tparam T Parameter pack of the tuple's reference types
 * @param tuple The target tuple of references that will receive the converted values
 */
template <typename... T>
void
resultset::row::to(std::tuple<T &...> tuple) const {
    // Use our modern converter directly, without going through protocol_parser
    detail::direct_row_to_tuple(*this, tuple);
}

// Explicit instantiation for common tuple types
// This ensures the compiler generates code for these common cases

/**
 * @brief Explicit instantiation for single integer tuple
 */
template void resultset::row::to(std::tuple<int> &tuple) const;

/**
 * @brief Explicit instantiation for single string tuple
 */
template void resultset::row::to(std::tuple<std::string> &tuple) const;

/**
 * @brief Explicit instantiation for int, string tuple
 */
template void resultset::row::to(std::tuple<int, std::string> &tuple) const;

/**
 * @brief Explicit instantiation for string, int tuple
 */
template void resultset::row::to(std::tuple<std::string, int> &tuple) const;

/**
 * @brief Explicit instantiation for int, int tuple
 */
template void resultset::row::to(std::tuple<int, int> &tuple) const;

/**
 * @brief Explicit instantiation for string, string tuple
 */
template void resultset::row::to(std::tuple<std::string, std::string> &tuple) const;

/**
 * @brief Explicit instantiation for int, int, int tuple
 */
template void resultset::row::to(std::tuple<int, int, int> &tuple) const;

/**
 * @brief Explicit instantiation for string, string, string tuple
 */
template void
resultset::row::to(std::tuple<std::string, std::string, std::string> &tuple) const;

/**
 * @brief Explicit instantiation for int, string, int tuple
 */
template void resultset::row::to(std::tuple<int, std::string, int> &tuple) const;

/**
 * @brief Explicit instantiation for string, int, string tuple
 */
template void resultset::row::to(std::tuple<std::string, int, std::string> &tuple) const;

// Explicit instantiation for common reference tuple types

/**
 * @brief Explicit instantiation for single integer reference tuple
 */
template void resultset::row::to(std::tuple<int &> &tuple) const;

/**
 * @brief Explicit instantiation for single string reference tuple
 */
template void resultset::row::to(std::tuple<std::string &> &tuple) const;

/**
 * @brief Explicit instantiation for int reference, string reference tuple
 */
template void resultset::row::to(std::tuple<int &, std::string &> &tuple) const;

/**
 * @brief Explicit instantiation for string reference, int reference tuple
 */
template void resultset::row::to(std::tuple<std::string &, int &> &tuple) const;

/**
 * @brief Explicit instantiation for int reference, int reference tuple
 */
template void resultset::row::to(std::tuple<int &, int &> &tuple) const;

/**
 * @brief Explicit instantiation for string reference, string reference tuple
 */
template void resultset::row::to(std::tuple<std::string &, std::string &> &tuple) const;

/**
 * @brief Explicit instantiation for int reference, int reference, int reference tuple
 */
template void resultset::row::to(std::tuple<int &, int &, int &> &tuple) const;

/**
 * @brief Explicit instantiation for string reference, string reference, string reference
 * tuple
 */
template void
resultset::row::to(std::tuple<std::string &, std::string &, std::string &> &tuple) const;

/**
 * @brief Explicit instantiation for int reference, string reference, int reference tuple
 */
template void resultset::row::to(std::tuple<int &, std::string &, int &> &tuple) const;

/**
 * @brief Explicit instantiation for string reference, int reference, string reference
 * tuple
 */
template void
resultset::row::to(std::tuple<std::string &, int &, std::string &> &tuple) const;

} // namespace qb::pg