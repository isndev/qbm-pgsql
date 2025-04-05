/**
 * @file data_iterator.h
 * @brief Definition of iterators for PostgreSQL query results
 *
 * This file defines bidirectional iterators for navigating PostgreSQL query results.
 * It provides a template base class that implements common iterator functionality
 * that can be specialized for different iteration patterns (row iteration and field iteration).
 *
 * The iterators follow the STL iterator requirements and can be used with standard
 * algorithms and range-based for loops to process PostgreSQL result data efficiently.
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

#pragma once

#include <cstddef>
#include <iterator>

namespace qb {
namespace pg {

// Forward declaration
class resultset;

namespace detail {

/**
 * @brief Base class for PostgreSQL result data iterators
 *
 * This template class provides a bidirectional iterator implementation for traversing
 * PostgreSQL result data (rows and fields). It follows the Curiously Recurring Template Pattern
 * (CRTP) to allow derived classes to specialize behavior while sharing common functionality.
 *
 * The iterator provides functionality for:
 * - Moving forward and backward through result data
 * - Accessing result data elements
 * - Comparing iterators for equality
 * - Position tracking within the result set
 *
 * @tparam Derived The derived iterator type that inherits from this base class
 * @tparam T The value type that the iterator will return when dereferenced
 */
template <typename Derived, typename T>
class data_iterator {
public:
    /**
     * @brief Type definitions required by STL iterator concept
     */
    typedef std::ptrdiff_t difference_type;
    typedef T value_type;
    typedef T reference;
    typedef Derived pointer;
    typedef std::bidirectional_iterator_tag iterator_category;

    /**
     * @brief Default constructor
     *
     * Creates an empty iterator that doesn't point to any result data.
     * This is commonly used for end iterators.
     */
    data_iterator() = default;

    /**
     * @brief Copy constructor
     *
     * Creates a new iterator with the same position as the source iterator.
     *
     * @param other The source iterator to copy from
     */
    data_iterator(const data_iterator &other) = default;

    /**
     * @brief Move constructor
     *
     * Transfers ownership of the iterator state from the source iterator.
     *
     * @param other The source iterator to move from
     */
    data_iterator(data_iterator &&other) = default;

    /**
     * @brief Row iterator constructor
     *
     * Creates an iterator positioned at a specific row in the result set.
     * This constructor is primarily used by row iterators.
     *
     * @param result Pointer to the resultset containing the data
     * @param row_index The index of the row to position the iterator at
     */
    data_iterator(const resultset *result, std::size_t row_index)
        : result_(result)
        , row_index_(row_index)
        , field_index_(0) {}

    /**
     * @brief Field iterator constructor
     *
     * Creates an iterator positioned at a specific field in a specific row.
     * This constructor is primarily used by field iterators.
     *
     * @param result Pointer to the resultset containing the data
     * @param row_index The index of the row containing the field
     * @param field_index The index of the field to position the iterator at
     */
    data_iterator(const resultset *result, std::size_t row_index, std::size_t field_index)
        : result_(result)
        , row_index_(row_index)
        , field_index_(field_index) {}

    /**
     * @brief Destructor
     */
    ~data_iterator() = default;

    /**
     * @brief Copy assignment operator
     *
     * @param other The source iterator to copy from
     * @return Reference to this iterator after assignment
     */
    data_iterator &operator=(const data_iterator &) = default;

    /**
     * @brief Move assignment operator
     *
     * @param other The source iterator to move from
     * @return Reference to this iterator after assignment
     */
    data_iterator &operator=(data_iterator &&) = default;

    /**
     * @brief Dereference operator
     *
     * Accesses the data element at the current iterator position.
     * The actual implementation is provided by the derived class.
     *
     * @return The data element at the current position
     */
    value_type operator*() const;

    /**
     * @brief Pre-increment operator
     *
     * Advances the iterator to the next position and returns a reference to the updated iterator.
     * The actual advancement logic is provided by the derived class's advance() method.
     *
     * @return Reference to this iterator after advancement
     */
    Derived &
    operator++() {
        static_cast<Derived *>(this)->advance(1);
        return *static_cast<Derived *>(this);
    }

    /**
     * @brief Post-increment operator
     *
     * Advances the iterator to the next position but returns a copy of the iterator
     * at its original position before advancement.
     *
     * @return Copy of the iterator before advancement
     */
    Derived
    operator++(int) {
        Derived tmp = *static_cast<Derived *>(this);
        ++(*this);
        return tmp;
    }

    /**
     * @brief Pre-decrement operator
     *
     * Moves the iterator to the previous position and returns a reference to the updated iterator.
     * The actual movement logic is provided by the derived class's advance() method.
     *
     * @return Reference to this iterator after movement
     */
    Derived &
    operator--() {
        static_cast<Derived *>(this)->advance(-1);
        return *static_cast<Derived *>(this);
    }

    /**
     * @brief Post-decrement operator
     *
     * Moves the iterator to the previous position but returns a copy of the iterator
     * at its original position before movement.
     *
     * @return Copy of the iterator before movement
     */
    Derived
    operator--(int) {
        Derived tmp = *static_cast<Derived *>(this);
        --(*this);
        return tmp;
    }

    /**
     * @brief Equality comparison operator
     *
     * Compares this iterator with another to check if they point to the same position.
     * The actual comparison logic is provided by the derived class's compare() method.
     *
     * @param rhs The iterator to compare with
     * @return true if the iterators point to the same position, false otherwise
     */
    bool
    operator==(const Derived &rhs) const {
        return static_cast<const Derived *>(this)->compare(rhs) == 0;
    }

    /**
     * @brief Inequality comparison operator
     *
     * Compares this iterator with another to check if they point to different positions.
     *
     * @param rhs The iterator to compare with
     * @return true if the iterators point to different positions, false otherwise
     */
    bool
    operator!=(const Derived &rhs) const {
        return !(*this == rhs);
    }

    /**
     * @brief Boolean conversion operator
     *
     * Allows checking if the iterator is valid (points to actual result data).
     * An iterator is considered valid if it has a non-null result set pointer.
     *
     * @return true if the iterator is valid, false otherwise
     */
    explicit
    operator bool() const {
        return result_ != nullptr;
    }

protected:
    /**
     * @brief Pointer to the result set containing the data
     */
    const resultset *result_ = nullptr;

    /**
     * @brief Current row index position
     */
    std::size_t row_index_ = 0;

    /**
     * @brief Current field index position
     */
    std::size_t field_index_ = 0;

    /**
     * @brief Special value representing an invalid position
     *
     * This constant is used to mark the end of a sequence or an invalid position.
     */
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
};

} // namespace detail
} // namespace pg
} // namespace qb
