/**
 * @file resultset.cpp
 * @brief Implementation of PostgreSQL result set handling interface
 *
 * This file contains the implementation of classes defined in resultset.h,
 * providing functionality for accessing and manipulating PostgreSQL query results.
 *
 * Implementation details include:
 * - Container interface operations for result sets
 * - Iterator implementations for traversing rows and fields
 * - Field data access and conversion
 * - Result set navigation utilities
 *
 * @see resultset.h
 * @see result_impl.h
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

#include "./resultset.h"
#include <algorithm>
#include <assert.h>
#include "./protocol.h"
#include "./result_impl.h"

namespace qb {
namespace pg {

const resultset::size_type resultset::npos =
    std::numeric_limits<resultset::size_type>::max();
const resultset::row::size_type resultset::row::npos =
    std::numeric_limits<resultset::row::size_type>::max();

//----------------------------------------------------------------------------
// resultset::row implementation - Container-like interface for database rows
//----------------------------------------------------------------------------
resultset::row::size_type
resultset::row::size() const {
    // Return the number of columns in the result set
    return result_->columns_size();
}

bool
resultset::row::empty() const {
    // A row in a valid result set is never empty as it always contains column values
    // (even if they're NULL values)
    return false;
}

resultset::row::const_iterator
resultset::row::begin() const {
    return const_iterator(result_, row_index_, 0);
}

resultset::row::const_iterator
resultset::row::end() const {
    return const_iterator(result_, row_index_, size());
}

resultset::row::const_reverse_iterator
resultset::row::rbegin() const {
    return const_reverse_iterator(const_iterator(result_, row_index_, size() - 1));
}

resultset::row::const_reverse_iterator
resultset::row::rend() const {
    return const_reverse_iterator(const_iterator(result_, row_index_, npos));
}

resultset::row::reference
resultset::row::operator[](size_type col_index) const {
    // TODO check the index
    return reference(result_, row_index_, col_index);
}

resultset::row::reference
resultset::row::operator[](std::string const &name) const {
    size_type col_index = result_->index_of_name(name);
    return (*this)[col_index];
}

//----------------------------------------------------------------------------
// resultset::const_row_iterator implementation - Bidirectional iterator for rows
//----------------------------------------------------------------------------
int
resultset::const_row_iterator::compare(const_row_iterator const &rhs) const {
    if (!(*this) && !rhs) // invalid iterators are equal
        return 0;
    assert(result_ == rhs.result_ &&
           "Cannot compare iterators in different result sets");
    if (row_index_ != rhs.row_index_)
        return (row_index_ < rhs.row_index_) ? -1 : 1;
    return 0;
}

resultset::const_row_iterator &
resultset::const_row_iterator::advance(difference_type distance) {
    if (*this) {
        // movement is defined only for valid iterators
        difference_type target = distance + row_index_;
        if (target < 0 || static_cast<resultset::size_type>(target) > result_->size()) {
            // invalidate the iterator if target position is out of range
            row_index_ = npos;
        } else {
            row_index_ = target;
        }
    } else if (result_) {
        if (distance == 1) {
            // When a non-valid iterator that belongs to a result set
            // is incremented it is moved to the beginning of sequence.
            // This supports the rend() iterator moving to the beginning
            // of sequence when incremented.
            row_index_ = 0;
        } else if (distance == -1) {
            // When a non-valid iterator that belongs to a result set
            // is decremented it is moved to the end of sequence.
            // This supports the end() iterator moving to the end
            // of sequence when decremented.
            row_index_ = result_->size() - 1;
        }
    }
    return *this;
}

//----------------------------------------------------------------------------
// resultset::field implementation - Database field value access
//----------------------------------------------------------------------------
std::string const &
resultset::field::name() const {
    assert(result_ && "Cannot get field name not bound to result set");
    return result_->field_name(field_index_);
}

field_description const &
resultset::field::description() const {
    assert(result_ && "Cannot get field description not bound to result set");
    return result_->field(field_index_);
}

bool
resultset::field::is_null() const {
    return result_->is_null(row_index_, field_index_);
}

field_buffer
resultset::field::input_buffer() const {
    return result_->at(row_index_, field_index_);
}

//----------------------------------------------------------------------------
// resultset::const_field_iterator implementation - Bidirectional iterator for fields
//----------------------------------------------------------------------------
int
resultset::const_field_iterator::compare(const_field_iterator const &rhs) const {
    if (!(*this) && !rhs) // invalid iterators are equal
        return 0;
    assert(result_ == rhs.result_ &&
           "Cannot compare iterators in different result sets");
    assert(row_index_ == rhs.row_index_ &&
           "Cannot compare iterators in different data rows");
    if (field_index_ != rhs.field_index_)
        return field_index_ < rhs.field_index_ ? -1 : 1;
    return 0;
}

resultset::const_field_iterator &
resultset::const_field_iterator::advance(difference_type distance) {
    if (*this) {
        // movement is defined only for valid iterators
        difference_type target = distance + field_index_;
        if (target < 0 || target > result_->columns_size()) {
            // invalidate the iterator if target position is out of range
            field_index_ = row::npos;
        } else {
            field_index_ = target;
        }
    } else if (result_) {
        if (distance == 1) {
            // When a non-valid iterator that belongs to a result set
            // is incremented it is moved to the beginning of sequence.
            // This supports the rend() iterator moving to the beginning
            // of sequence when incremented.
            field_index_ = 0;
        } else if (distance == -1) {
            // When a non-valid iterator that belongs to a result set
            // is decremented it is moved to the end of sequence.
            // This supports the end() iterator moving to the end
            // of sequence when decremented.
            field_index_ = result_->columns_size() - 1;
        }
    }
    return *this;
}

//----------------------------------------------------------------------------
// resultset implementation - Core functionality for PostgreSQL result set handling
//----------------------------------------------------------------------------
resultset::resultset()
    : pimpl_(new detail::result_impl) {}

resultset::resultset(result_impl_ptr impl)
    : pimpl_(impl) {}

resultset::size_type
resultset::size() const {
    return pimpl_->size();
}

bool
resultset::empty() const {
    return pimpl_->empty();
}

resultset::const_iterator
resultset::begin() const {
    return const_iterator(this, 0);
}

resultset::const_iterator
resultset::end() const {
    return const_iterator(this, size());
}

resultset::const_reverse_iterator
resultset::rbegin() const {
    return const_reverse_iterator(const_iterator(this, size() - 1));
}

resultset::const_reverse_iterator
resultset::rend() const {
    return const_reverse_iterator(const_iterator(this, npos));
}

resultset::reference
resultset::front() const {
    assert(size() > 0 && "Cannot get row in an empty resultset");
    // Return the first row in the result set
    return row(this, 0);
}

resultset::reference
resultset::back() const {
    assert(size() > 0 && "Cannot get row in an empty resultset");
    // Return the last row in the result set
    return row(this, size() - 1);
}

resultset::reference
resultset::operator[](size_type index) const {
    assert(index < size() && "Index is out of bounds");
    // Access a row by index without bounds checking (checked by assertion in debug mode)
    return row(this, index);
}

resultset::row::size_type
resultset::columns_size() const {
    // Return the number of columns in the result set
    return pimpl_->row_description().size();
}

row_description_type const &
resultset::row_description() const {
    // Return the metadata about the columns in the result set
    return pimpl_->row_description();
}

resultset::size_type
resultset::index_of_name(std::string const &name) const {
    row_description_type const &descriptions = pimpl_->row_description();
    // Find the field description with the matching name
    auto f =
        std::find_if(descriptions.begin(), descriptions.end(),
                     [name](field_description const &fd) { return fd.name == name; });
    if (f != descriptions.end()) {
        // Return the index (position) of the field in the result set
        return f - descriptions.begin();
    }
    // Return npos if no field with the given name exists
    return npos;
}

field_description const &
resultset::field(size_type col_index) const {
    // Get field description by index with range checking via std::vector::at
    return pimpl_->row_description().at(col_index);
}

field_description const &
resultset::field(std::string const &name) const {
    // Linear search for the field description by name
    for (field_description const &fd : pimpl_->row_description()) {
        if (fd.name == name)
            return fd;
    }
    // Throw an exception if no field with the given name exists
    throw std::runtime_error("No field with name '" + name + "' found in result set");
}

std::string const &
resultset::field_name(size_type index) const {
    // Get the name of a field by its index
    return field(index).name;
}

field_buffer
resultset::at(size_type r, row::size_type c) const {
    // Get the value buffer at the specified row and column
    return pimpl_->at(r, c);
}

bool
resultset::is_null(size_type r, row::size_type c) const {
    // Check if the value at the specified row and column is NULL
    return pimpl_->is_null(r, c);
}

qb::json
resultset::json() const {
    qb::json result = qb::json::array();

    for (const auto row : *this) {
        qb::json row_obj = qb::json::object();
        for (const auto field : row) {
            auto opt = field.as<std::optional<std::string>>();
            row_obj[field.name()] = opt;
        }
        result.push_back(row_obj);
    }
    
    return result;
}

} // namespace pg
} // namespace qb
