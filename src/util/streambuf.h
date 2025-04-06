/**
 * @file streambuf.h
 * @brief Custom streambuf implementation for iterator-based buffers
 *
 * This file provides a specialized streambuf implementation that works with
 * iterator ranges from containers. It allows creating stream buffers directly
 * from container iterators, facilitating streaming operations on container data
 * without copying the underlying data.
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

#include <streambuf>
#include <vector>

namespace qb {
namespace util {

/**
 * @brief Stream buffer implementation based on container iterators
 *
 * This class implements a streambuf that operates directly on data referenced
 * by container iterators. It allows streaming operations (like reading) on
 * container data without having to copy the data to a separate buffer.
 *
 * @tparam charT The character type of the stream
 * @tparam container The container type that holds the data
 * @tparam traits Character traits for the stream
 */
template <typename charT, typename container = std::vector<charT>,
          typename traits = std::char_traits<charT>>
class basic_input_iterator_buffer : public std::basic_streambuf<charT, traits> {
public:
    typedef charT                                   char_type;      ///< Character type
    typedef container                               container_type; ///< Container type
    typedef typename container_type::const_iterator const_iterator; ///< Iterator type
    typedef std::ios_base                           ios_base;       ///< Base IO class type

    typedef std::basic_streambuf<charT, traits> base;     ///< Base streambuf type
    typedef typename base::pos_type             pos_type; ///< Position type
    typedef typename base::off_type             off_type; ///< Offset type

public:
    /**
     * @brief Constructor from iterator range
     *
     * Creates a stream buffer from the range defined by the iterators [s, e).
     * The data is not copied; the buffer operates directly on the original data.
     *
     * @param s Iterator pointing to the start of the range
     * @param e Iterator pointing to the end of the range
     */
    basic_input_iterator_buffer(const_iterator s, const_iterator e)
        : base()
        , s_(s)
        , start_(const_cast<char_type *>(&*s))
        , count_(e - s) {
        base::setg(start_, start_, start_ + count_);
    }

    /**
     * @brief Move constructor
     *
     * Transfers ownership of the buffer from rhs to this object.
     *
     * @param rhs The buffer to move from
     */
    basic_input_iterator_buffer(basic_input_iterator_buffer &&rhs)
        : base()
        , s_(rhs.s_)
        , start_(rhs.start_)
        , count_(rhs.count_) {
        auto n = rhs.gptr();
        base::setg(start_, n, start_ + count_);
    }

    /**
     * @brief Get iterator to the beginning of the buffer
     *
     * @return const_iterator Iterator pointing to the first element
     */
    const_iterator
    begin() const {
        return s_;
    }

    /**
     * @brief Get iterator to the end of the buffer
     *
     * @return const_iterator Iterator pointing past the last element
     */
    const_iterator
    end() const {
        return s_ + count_;
    }

    /**
     * @brief Check if the buffer is empty
     *
     * @return true if the buffer contains no elements, false otherwise
     */
    bool
    empty() const {
        return count_ == 0;
    }

    /**
     * @brief Convert the buffer contents to a vector
     *
     * Creates a new vector containing a copy of all the data in the buffer.
     *
     * @return A vector containing all the bytes from the buffer
     */
    std::vector<charT>
    to_vector() const {
        if (empty()) {
            return std::vector<charT>();
        }
        return std::vector<charT>(begin(), end());
    }

protected:
    /**
     * @brief Seek to a position relative to a direction
     *
     * Implementation of the seekoff method required by std::basic_streambuf.
     * Allows seeking relative to beginning, current position or end of the buffer.
     *
     * @param off Offset to seek by
     * @param way Direction to seek from (beg, cur, end)
     * @param which I/O modes to consider when seeking
     * @return The new absolute position, or -1 on failure
     */
    pos_type
    seekoff(off_type off, ios_base::seekdir way,
            ios_base::openmode which = ios_base::in | ios_base::out) {
        (void) which;
        char_type *tgt(nullptr);
        switch (way) {
            case ios_base::beg: tgt = start_ + off; break;
            case ios_base::cur: tgt = base::gptr() + off; break;
            case ios_base::end: tgt = start_ + count_ - 1 + off; break;
            default: break;
        }
        if (!tgt) return -1;
        if (tgt < start_ || start_ + count_ < tgt) return -1;
        base::setg(start_, tgt, start_ + count_);
        return tgt - start_;
    }

    /**
     * @brief Seek to an absolute position
     *
     * Implementation of the seekpos method required by std::basic_streambuf.
     * Allows seeking to an absolute position within the buffer.
     *
     * @param pos Absolute position to seek to
     * @param which I/O modes to consider when seeking
     * @return The new position, or -1 on failure
     */
    pos_type
    seekpos(pos_type pos, ios_base::openmode which = ios_base::in | ios_base::out) {
        (void) which;
        char_type *tgt = start_ + pos;
        if (tgt < start_ || start_ + count_ < tgt) return -1;
        base::setg(start_, tgt, start_ + count_);
        return tgt - start_;
    }

private:
    const_iterator s_;     ///< Iterator to the start of the range
    char_type     *start_; ///< Pointer to the start of the buffer
    size_t         count_; ///< Number of elements in the buffer
};

/**
 * @brief Typedef for a character-based input iterator buffer
 *
 * A specialization of basic_input_iterator_buffer for char type,
 * which is the most common use case.
 */
typedef basic_input_iterator_buffer<char> input_iterator_buffer;

} // namespace util
} // namespace qb
