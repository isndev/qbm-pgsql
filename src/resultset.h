/**
 * @file resultset.h
 * @brief PostgreSQL result set handling interface
 *
 * This file defines the classes and interfaces for accessing and manipulating
 * PostgreSQL query results. It provides:
 *
 * - Container-like access to query result rows and fields
 * - Bidirectional iteration over result rows and fields
 * - Type-safe conversion of field values to C++ native types
 * - Field access by name or position
 * - Support for tuple-based data extraction
 *
 * The implementation follows standard C++ container patterns to provide
 * a familiar and idiomatic interface for working with database results.
 *
 * Key features:
 * - Standard container interface with iterators
 * - Type conversion with error handling
 * - Support for NULL values through std::optional
 * - Row-to-tuple conversion utilities
 * - Field metadata access
 *
 * @see resultset.inl
 * @see field.h
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

#include <istream>
#include <iterator>
#include <memory>
#include <tuple>
#include "./common.h"
#include "./data_iterator.h"
#include "./error.h"
#include "./field_reader.h"
#include "./protocol_io_traits.h"
#include "./type_converter.h"

namespace qb {
namespace pg {

namespace detail {
class result_impl;
}

/**
 * Result set.
 * Provides access to query results returned from PostgreSQL.
 *
 * This class offers a complete interface for working with result sets:
 * - Random access to rows via indexing operators
 * - Bidirectional iteration through rows and fields
 * - Access to field metadata and type information
 * - Conversion of field values to native C++ types
 * - Support for NULL values through std::optional
 *
 * The resultset follows standard C++ container patterns with iterators,
 * making it intuitive to use with modern C++ idioms including range-based for loops.
 *
 * Usage example:
 * @code
 * void
 * handle_results(transaction_ptr tran, resultset res, bool complete)
 * {
 *     if (complete) { // This is the last callback for the command
 *         // C++11 style iteration
 *         for (resultset::row const& row : res) {
 *             for (resultset::field const& field : row) {
 *                 // Process each field
 *             }
 *         }
 *
 *         // Oldschool iteration
 *        for (resultset::const_iterator row = res.begin(); row != res.end(); ++row) {
 *            // Do something with the row
 *            for (resultset::const_field_iterator f = row.begin(); f != row.end(); ++f) {
 *               // Process each field
 *            }
 *        }
 *     }
 * }
 * @endcode
 */
class resultset {
public:
    //@{
    /** @name Size types definitions */
    typedef uinteger size_type;
    typedef integer difference_type;
    //@}

    class row;

    class field;

    class const_row_iterator;

    /** Shared pointer to internal implementation */
    typedef detail::result_impl *result_impl_ptr;

    //@{
    /** @name Row container concept */
    typedef const_row_iterator const_iterator;
    typedef std::reverse_iterator<const_iterator> const_reverse_iterator;

    typedef row value_type;
    typedef const value_type reference;
    typedef const_iterator pointer;
    //@}

    //@{
    /** @name Field iterators */
    class const_field_iterator;

    typedef std::reverse_iterator<const_field_iterator> const_reverse_field_iterator;
    //@}
private:
    typedef resultset const *result_pointer;

public:
    /** @brief Not-a-position constant */
    static const size_type npos;

public:
    /**
     * @brief Construct an empty resultset
     */
    resultset();

    /**
     * @brief Constructs a resultset with the pointer to internal implementation
     * Used internally by the library
     * @param Shared pointer to result set
     */
    resultset(result_impl_ptr);
    //@{
    /** @name Row-wise container interface */
    size_type size() const; /**< Number of rows */
    bool empty() const;     /**< Is the result set empty */

    const_iterator begin() const; /**< Iterator to the beginning of rows sequence */
    const_iterator end() const;   /**< Iterator past the end of rows sequence. */

    const_reverse_iterator
    rbegin() const; /**< Iterator to the beginning of rows sequence in reverse order */
    const_reverse_iterator
    rend() const; /**< Iterator past the end of of rows sequence in reverse order */

    /**
     * Get the first row in the result set.
     * Will raise an assertion if the result set is empty.
     * @return
     */
    reference front() const;

    /**
     * Get the last row in the result set.
     * Will raise an assertion if the result set is empty.
     * @return
     */
    reference back() const;

    /**
     * Access a row by index.
     * In case of index out-of-range situation will rase an assertion
     * @param index index of the row
     * @return @c row object
     */
    reference operator[](size_type index) const;

    /**
     * Assess a row by index (range checking)
     * In case of index out-of-range will throw an exception
     * @param index index of the row
     * @return @c row object
     */
    reference at(size_type index) const;
    //@}

    //@{
    /** @name Result checking */
    /**
     * Syntactic sugar operator for checking the result set
     * @code
     * void result_callback(resultset r)
     * {
     *         if (r) {
     *             // Do something with the result
     *         }
     * }
     * @endcode
     */
    operator bool() const {
        return !empty();
    }

    /**
     * Syntactic sugar operator for checking the result set
     * @code
     * void result_callback(resultset r)
     * {
     *         if (!r) {
     *             // Handle the situation of an empty result set
     *         }
     * }
     * @endcode
     */
    bool
    operator!() const {
        return *this;
    }
    //@}
public:
    //@{
    /** @name Data access classes */
    /**
     * Represents a data row in the result set.
     *
     * This class provides a view into a row of the result set.
     * It doesn't hold any data except a pointer to the result set
     * and the data row index. Must not outlive the parent result set.
     *
     * Key features:
     * - Container-like interface for fields with iterators
     * - Access to fields by index or name
     * - Conversion of entire row to tuple with type conversion
     * - Lightweight reference object (doesn't own data)
     */
    class row {
    public:
        typedef smallint size_type;
        typedef resultset::difference_type difference_type;

        //@{
        /** @name Field container concept */
        typedef const_field_iterator const_iterator;
        typedef const_reverse_field_iterator const_reverse_iterator;

        typedef class field value_type;
        typedef class field reference;
        typedef const_iterator pointer;
        //@}
        static const size_type npos;

    public:
        size_type
        row_index() const /**< Index of the row in the result set */
        {
            return row_index_;
        }

        //@{
        /** @name Field container interface */
        size_type size() const; /**< Number of fields */
        /**
         * Is row empty.
         * Actually, shouln'd happen in a non-empty result set.
         * @return
         */
        bool empty() const;

        const_iterator begin() const; /**< Iterator to the beginning of field sequence */
        const_iterator end() const;   /**< Iterator past the field sequence */

        const_reverse_iterator rbegin() const;

        const_reverse_iterator rend() const;
        //@}
        //@{
        /** @name Field access */
        /**
         * Get field by it's index.
         * @param index
         * @return
         */
        reference operator[](size_type index) const;

        /**
         * Get field by it's name
         * @param name
         * @return
         */
        reference operator[](std::string const &name) const;
        //@}

        /**
         * Get the row as a tuple of typed values
         * @param target tuple
         */
        template <typename... T>
        void to(std::tuple<T...> &) const;

        template <typename... T>
        void to(std::tuple<T &...>) const;

        template <typename... T>
        void to(T &...) const;

        template <typename... T>
        void to(::std::initializer_list<::std::string> const &names, ::std::tuple<T...> &) const;

        template <typename... T>
        void to(::std::initializer_list<::std::string> const &names, ::std::tuple<T &...>) const;

        template <typename... T>
        void to(::std::initializer_list<::std::string> const &names, T &...val) const;

        /**
         * Get the index of field with name.
         * Shortcut to the resultset's method
         * @param name the field name
         * @return if found, index in the range of [0..columns_size). If not found - npos
         */
        resultset::size_type
        index_of_name(std::string const &name) const {
            return result_->index_of_name(name);
        }

    protected:
        friend class resultset;

        /**
         * Construct a row object in a result set.
         * @param res
         * @param idx
         */
        row(result_pointer res, resultset::size_type idx)
            : result_(res)
            , row_index_(idx) {}

        result_pointer result_;
        resultset::size_type row_index_;
    }; // row
    /**
     * Represents a single field in the result set.
     *
     * This class provides access to field data with type conversion capabilities.
     * Key features:
     * - Type-safe conversion to native C++ types
     * - Support for NULL values
     * - Access to field metadata (name, type, format)
     * - Both static and dynamic type checking
     */
    class field {
    public:
        size_type
        row_index() const /**< @brief Index of owner row */
        {
            return row_index_;
        }

        row::size_type
        field_index() const /**< @brief Index of field in the row */
        {
            return field_index_;
        }

        std::string const &name() const; /**< @brief Name of field */

        field_description const &description() const; /**< @brief Field description */

        bool is_null() const; /**< @brief Is field value null */

        bool empty() const; /**< @brief Is field value empty (not null) */

        /**
         * Parse the value buffer to the type specified by value passed as
         * target. Will throw a value_is_null exception if the field is null and
         * type is not nullable.
         * @tparam T type of target variable
         * @param val Target variable for the field value.
         * @return true if parsing the buffer was a success.
         * @exception tip::db::pg::value_is_null
         */
        template <typename T>
        bool
        to(T &val) const {
            return to_nullable(val, io::traits::is_nullable<T>());
        }

        /**
         * Parse the value buffer to the type specified by value passed as
         * target. std::optional is used as to specify a 'nullable' type
         * concept.
         * @tparam T type of target variable.
         * @param val Target variable for the field value.
         * @return true if parsing the buffer was a success.
         * @exception tip::db::pg::value_is_null
         */
        template <typename T>
        bool
        to(std::optional<T> &val) const {
            if (is_null()) {
                val = std::optional<T>();
                return true;
            } else {
                typename std::decay<T>::type tmp;
                to_impl(tmp, io::traits::has_parser<T, pg::protocol_data_format::Binary>());
                val = std::optional<T>(tmp);
                return true;
            }
            return false;
        }

        /**
         * Cast the field value to the type requested.
         * @tparam T requested data type
         * @return field buffer as requested data type
         * @exception tip::db::pg::value_is_null if the value is null and the
         *     type requested is not 'nullable'.
         */
        template <typename T>
        typename std::decay<T>::type
        as() const {
            using result_type = typename std::decay<T>::type;

            // 1. Check if the value is NULL
            if (is_null()) {
                if constexpr (io::traits::is_nullable<result_type>::value) {
                    result_type val;
                    io::traits::nullable_traits<result_type>::set_null(val);
                    return val;
                } else {
                    throw error::value_is_null(name());
                }
            }

            // 2. Retrieve the data and format
            field_buffer buffer = input_buffer();
            bool is_binary = (description().format_code == pg::protocol_data_format::Binary);
            auto data_vector = buffer.to_vector();

            // 3. Use the TypeConverter to convert according to format
            if (is_binary) {
                return detail::TypeConverter<result_type>::from_binary(data_vector);
            } else {
                // For text format, we first need to read the string
                static detail::ParamUnserializer unserializer;
                std::string text_value = unserializer.read_string(data_vector);
                return detail::TypeConverter<result_type>::from_text(text_value);
            }
        }

    private:
        template <typename T>
        bool
        to_nullable(T &val, std::true_type const &) const {
            typedef io::traits::nullable_traits<T> nullable_traits;

            if (is_null()) {
                nullable_traits::set_null(val);
                return true;
            }
            return to_impl(val, io::traits::has_parser<T, pg::protocol_data_format::Binary>());
        }

        template <typename T>
        bool
        to_nullable(T &val, std::false_type const &) const {
            if (is_null())
                throw error::value_is_null(name());
            return to_impl(val, io::traits::has_parser<T, pg::protocol_data_format::Binary>());
        }

        template <typename T>
        bool
        to_impl(T &val, std::true_type const &) const {
            field_buffer b = input_buffer();
            return true;
        }

        template <typename T>
        bool
        to_impl(T &val, std::false_type const &) const {
            field_description const &fd = description();
            if (fd.format_code == pg::protocol_data_format::Binary) {
                throw error::db_error{
                    "Cannot find pg::protocol_data_format::Binary parser for field " + fd.name};
            }

            field_buffer b = input_buffer();
            return true;
        }

        field_buffer input_buffer() const;

    protected:
        friend class resultset;

        friend class row;

        field(result_pointer res, size_type row, row::size_type col)
            : result_(res)
            , row_index_(row)
            , field_index_(col) {}

        result_pointer result_;
        size_type row_index_;
        row::size_type field_index_;
    }; // field
    //@}
    //@{
    /** @name Data iterators */
    /**
     * Iterator over the rows in a result set
     */
    class const_row_iterator : public detail::data_iterator<const_row_iterator, row> {
        using base_type = detail::data_iterator<const_row_iterator, row>;

    public:
        /**
         * Create a terminating iterator
         */
        const_row_iterator()
            : base_type(nullptr, npos) {}

        int compare(const_row_iterator const &rhs) const;

        const_row_iterator &advance(difference_type);

        bool
        valid() const {
            return result_ && row_index_ <= result_->size();
        }

    private:
        friend class resultset;

        const_row_iterator(result_pointer res, resultset::size_type index)
            : base_type(res, index) {}
    }; // const_row_iterator

    /**
     * Iterator over the fields in a data row
     */
    class const_field_iterator : public detail::data_iterator<const_field_iterator, field> {
        using base_type = detail::data_iterator<const_field_iterator, field>;

    public:
        /**
         * Create a terminating iterator
         */
        const_field_iterator()
            : base_type(nullptr, npos, npos) {}

        int compare(const_field_iterator const &rhs) const;

        const_field_iterator &advance(difference_type distance);
        //@{
        /**
         * The field index is valid when it is equal to the row size
         * for an iterator that is returned by end() function to be valid
         * and movable.
         */
        bool
        valid() const {
            return result_ && row_index_ < result_->size() &&
                   field_index_ <= result_->columns_size();
        }

    private:
        friend class row;

        const_field_iterator(result_pointer res, size_type row, size_type col)
            : base_type(res, row, col) {}
    }; // const_field_iterator
    //@}
public:
    //@{
    /** @name Column-related interface */
    row::size_type columns_size() const; /**< Column count */

    row_description_type const &row_description() const;

    /**
     * Get the index of field with name
     * @param name the field name
     * @return if found, index in the range of [0..columns_size). If not found - npos
     */
    size_type index_of_name(std::string const &name) const;

    /**
     * Get the field description of field by it's index.
     * @param col_index field index, must be in range of [0..columns_size)
     * @return constant reference to the field description
     * @throws out_of_range exception
     */
    field_description const &field(size_type col_index) const;

    /**
     * Get the field description of field by it's name.
     * @param name name of the field. must be present in the result set.
     * @return constant reference to the field description
     * @throws out_of_range exception
     */
    field_description const &field(std::string const &name) const;

    /**
     * Get the name of field by it's index
     * @param col_index field index, must be in range of [0..columns_size)
     * @return the name of the field.
     * @throws out_of_range exception
     */
    std::string const &field_name(size_type col_index) const;
    //@}
private:
    friend class row;

    friend class field;

    typedef const detail::result_impl *const_result_impl_ptr;
    const_result_impl_ptr pimpl_;

    field_buffer at(size_type r, row::size_type c) const;

    bool is_null(size_type r, row::size_type c) const;
}; // resultset

inline resultset::row::difference_type
operator-(resultset::row const &a, resultset::row const &b) {
    return a.row_index() - b.row_index();
}

} // namespace pg
} // namespace qb

#include "resultset.inl"
