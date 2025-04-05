/**
 * @file meta_helpers.h
 * @brief Template metaprogramming utilities
 *
 * This file provides a collection of template metaprogramming helpers
 * that simplify common compile-time operations. These include type selection
 * from variadic parameter packs, index sequence generation, and parameter pack
 * expansion utilities.
 *
 * These utilities are particularly useful for template metaprogramming
 * where compile-time type selection and manipulation are needed.
 *
 * @author zmij
 * @copyright Originally from project: https://github.com/zmij/pg_async.git
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

#pragma once

namespace qb {
namespace util {

/**
 * @brief Metafunction for calculating Nth type in variadic template parameters
 *
 * Provides a way to access the Nth type in a parameter pack at compile time.
 * This is a recursive template that unwraps the parameter pack until the
 * desired type is reached.
 *
 * @tparam num Index of the type to select (0-based)
 * @tparam T Parameter pack of types to select from
 */
template <size_t num, typename... T>
struct nth_type;

/**
 * @brief Recursive case for nth_type template
 *
 * Continues the recursion by decrementing the index and
 * removing the first type from the parameter pack.
 *
 * @tparam num Current index
 * @tparam T Current head type
 * @tparam Y Remaining types in the parameter pack
 */
template <size_t num, typename T, typename... Y>
struct nth_type<num, T, Y...> : nth_type<num - 1, Y...> {};

/**
 * @brief Base case for nth_type template
 *
 * When the index reaches 0, this specialization is selected,
 * which defines the target type as the first type in the current pack.
 *
 * @tparam T The selected type (when index is 0)
 * @tparam Y Remaining types (not used in this specialization)
 */
template <typename T, typename... Y>
struct nth_type<0, T, Y...> {
    typedef T type; ///< The type at the specified index
};

/**
 * @brief A tuple of indexes used for template parameter pack expansion
 *
 * This is commonly used for tuple manipulation, function argument unpacking,
 * and other compile-time sequence operations.
 *
 * @tparam Indexes A parameter pack of size_t indices
 */
template <size_t... Indexes>
struct indexes_tuple {
    enum { size = sizeof...(Indexes) }; ///< Number of indices in the tuple
};

/**
 * @brief Generator for a sequence of indices
 *
 * Builds a compile-time sequence of indices from 0 to num-1.
 * This is implemented using recursive template instantiation.
 *
 * @tparam num The number of indices to generate
 * @tparam tp The current indices tuple (default is empty)
 */
template <size_t num, typename tp = indexes_tuple<>>
struct index_builder;

/**
 * @brief Recursive case for index_builder
 *
 * Adds the next index to the tuple and continues the recursion.
 *
 * @tparam num Current count of remaining indices to add
 * @tparam Indexes Current sequence of indices
 */
template <size_t num, size_t... Indexes>
struct index_builder<num, indexes_tuple<Indexes...>>
    : index_builder<num - 1, indexes_tuple<Indexes..., sizeof...(Indexes)>> {};

/**
 * @brief Base case for index_builder
 *
 * When num reaches 0, the recursion stops and the final tuple type is defined.
 *
 * @tparam Indexes The complete sequence of indices
 */
template <size_t... Indexes>
struct index_builder<0, indexes_tuple<Indexes...>> {
    typedef indexes_tuple<Indexes...> type; ///< The final tuple type with all indices
    enum { size = sizeof...(Indexes) };     ///< Size of the index sequence
};

/**
 * @brief Utility for parameter pack expansion
 *
 * This struct provides a constructor that takes a variable number of
 * arguments, allowing for the expansion of a parameter pack in contexts
 * where direct expansion is not possible.
 *
 * It's particularly useful for calling a function on each element of a
 * parameter pack without having to define a separate function for this purpose.
 */
struct expand {
    /**
     * @brief Constructor that expands a parameter pack
     *
     * Takes an arbitrary number of arguments of any type and does nothing with them.
     * This is used purely for the side effect of expanding the parameter pack.
     *
     * @tparam U Types of parameters in the pack
     * @param ... Parameters to expand (not used)
     */
    template <typename... U>
    expand(U const &...) {}
};

} // namespace util
} // namespace qb
