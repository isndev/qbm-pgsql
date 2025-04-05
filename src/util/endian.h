/**
 * @file endian.h
 * @brief Endian conversion utilities
 *
 * This file provides utilities for endian conversions and byte swapping operations.
 * It defines functions to convert between different byte orders (big-endian, little-endian)
 * and the system's native endianness. The implementation uses both intrinsic byte swap
 * operations when available and manual bit manipulation as a fallback.
 *
 * The utilities allow for type-safe conversions of various integral types and
 * provide optimized implementations for different sizes of data types.
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

#include <boost/cstdint.hpp>
#include <boost/endian/detail/intrinsic.hpp>
#include <boost/predef/other/endian.h>
#include <type_traits>

namespace qb {
namespace util {
namespace endian {

/**
 * @brief Enumeration for endian byte orders
 *
 * Defines the possible byte orders for endian conversions and
 * automatically determines the system's native endianness.
 */
enum order {
    big,    ///< Big-endian (most significant byte first)
    little, ///< Little-endian (least significant byte first)
#ifdef BOOST_BIG_ENDIAN
    native = big ///< Native byte order of the system (big-endian on this system)
#else
    native = little ///< Native byte order of the system (little-endian on this system)
#endif
};

namespace detail {
/**
 * @brief Template struct for byte swapping operations
 *
 * Provides specialized implementations for byte swapping operations
 * based on the size of the type and whether it's signed or unsigned.
 *
 * @tparam Size Size of the type in bytes
 * @tparam sign Whether the type is signed
 */
template <size_t Size, bool sign>
struct bytes;

/**
 * @brief Specialization for signed 8-bit integers
 *
 * No byte swapping needed for single-byte values.
 */
template <>
struct bytes<sizeof(int8_t), true> {
    /**
     * @brief Reverse byte order for int8_t
     *
     * For 8-bit values, this is a no-op.
     *
     * @param x Value to reverse
     * @return The same value (no change for single-byte values)
     */
    static inline int8_t
    reverse(int8_t x) {
        return x;
    }
};

/**
 * @brief Specialization for unsigned 8-bit integers
 *
 * No byte swapping needed for single-byte values.
 */
template <>
struct bytes<sizeof(int8_t), false> {
    /**
     * @brief Reverse byte order for uint8_t
     *
     * For 8-bit values, this is a no-op.
     *
     * @param x Value to reverse
     * @return The same value (no change for single-byte values)
     */
    static inline uint8_t
    reverse(uint8_t x) {
        return x;
    }
};

/**
 * @brief Specialization for signed 16-bit integers
 *
 * Implements byte swapping for 16-bit signed integers.
 */
template <>
struct bytes<sizeof(int16_t), true> {
    /**
     * @brief Reverse byte order for int16_t
     *
     * Swaps the bytes of a 16-bit signed integer. Uses intrinsic operations
     * if available, or manual bit manipulation otherwise.
     *
     * @param x Value to reverse
     * @return Byte-swapped value
     */
    static inline int16_t
    reverse(int16_t x) {
#ifdef BOOST_ENDIAN_NO_INTRINSICS
        return (static_cast<uint16_t>(x) << 8) | (static_cast<uint16_t>(x) >> 8);
#else
        return BOOST_ENDIAN_INTRINSIC_BYTE_SWAP_2(static_cast<uint16_t>(x));
#endif
    }
};

/**
 * @brief Specialization for unsigned 16-bit integers
 *
 * Implements byte swapping for 16-bit unsigned integers.
 */
template <>
struct bytes<sizeof(int16_t), false> {
    /**
     * @brief Reverse byte order for uint16_t
     *
     * Swaps the bytes of a 16-bit unsigned integer. Uses intrinsic operations
     * if available, or manual bit manipulation otherwise.
     *
     * @param x Value to reverse
     * @return Byte-swapped value
     */
    static inline uint16_t
    reverse(uint16_t x) {
#ifdef BOOST_ENDIAN_NO_INTRINSICS
        return (x << 8) | (x >> 8);
#else
        return BOOST_ENDIAN_INTRINSIC_BYTE_SWAP_2(x);
#endif
    }
};

/**
 * @brief Specialization for signed 32-bit integers
 *
 * Implements byte swapping for 32-bit signed integers.
 */
template <>
struct bytes<sizeof(int32_t), true> {
    /**
     * @brief Reverse byte order for int32_t
     *
     * Swaps the bytes of a 32-bit signed integer. Uses intrinsic operations
     * if available, or manual bit manipulation otherwise.
     *
     * @param x Value to reverse
     * @return Byte-swapped value
     */
    static inline int32_t
    reverse(int32_t x) {
#ifdef BOOST_ENDIAN_NO_INTRINSICS
        uint32_t step16;
        step16 = static_cast<uint32_t>(x) << 16 | static_cast<uint32_t>(x) >> 16;
        return ((static_cast<uint32_t>(step16) << 8) & 0xff00ff00) |
               ((static_cast<uint32_t>(step16) >> 8) & 0x00ff00ff);
#else
        return BOOST_ENDIAN_INTRINSIC_BYTE_SWAP_4(static_cast<uint32_t>(x));
#endif
    }
};

/**
 * @brief Specialization for unsigned 32-bit integers
 *
 * Implements byte swapping for 32-bit unsigned integers.
 */
template <>
struct bytes<sizeof(int32_t), false> {
    /**
     * @brief Reverse byte order for uint32_t
     *
     * Swaps the bytes of a 32-bit unsigned integer. Uses intrinsic operations
     * if available, or manual bit manipulation otherwise.
     *
     * @param x Value to reverse
     * @return Byte-swapped value
     */
    static inline uint32_t
    reverse(uint32_t x) {
#ifdef BOOST_ENDIAN_NO_INTRINSICS
        uint32_t step16 = x << 16 | x >> 16;
        return ((step16 << 8) & 0xff00ff00) | ((step16 >> 8) & 0x00ff00ff);
#else
        return BOOST_ENDIAN_INTRINSIC_BYTE_SWAP_4(static_cast<uint32_t>(x));
#endif
    }
};

/**
 * @brief Specialization for signed 64-bit integers
 *
 * Implements byte swapping for 64-bit signed integers.
 */
template <>
struct bytes<sizeof(int64_t), true> {
    /**
     * @brief Reverse byte order for int64_t
     *
     * Swaps the bytes of a 64-bit signed integer. Uses intrinsic operations
     * if available, or manual bit manipulation otherwise.
     *
     * @param x Value to reverse
     * @return Byte-swapped value
     */
    static inline int64_t
    reverse(int64_t x) {
#ifdef BOOST_ENDIAN_NO_INTRINSICS
        uint64_t step32, step16;
        step32 = static_cast<uint64_t>(x) << 32 | static_cast<uint64_t>(x) >> 32;
        step16 = (step32 & 0x0000FFFF0000FFFFULL) << 16 | (step32 & 0xFFFF0000FFFF0000ULL) >> 16;
        return static_cast<int64_t>((step16 & 0x00FF00FF00FF00FFULL) << 8 |
                                    (step16 & 0xFF00FF00FF00FF00ULL) >> 8);
#else
        return BOOST_ENDIAN_INTRINSIC_BYTE_SWAP_8(static_cast<uint64_t>(x));
#endif
    }
};

/**
 * @brief Specialization for unsigned 64-bit integers
 *
 * Implements byte swapping for 64-bit unsigned integers.
 */
template <>
struct bytes<sizeof(int64_t), false> {
    /**
     * @brief Reverse byte order for uint64_t
     *
     * Swaps the bytes of a 64-bit unsigned integer. Uses intrinsic operations
     * if available, or manual bit manipulation otherwise.
     *
     * @param x Value to reverse
     * @return Byte-swapped value
     */
    static inline uint64_t
    reverse(uint64_t x) {
#ifdef BOOST_ENDIAN_NO_INTRINSICS
        uint64_t step32, step16;
        step32 = x << 32 | x >> 32;
        step16 = (step32 & 0x0000FFFF0000FFFFULL) << 16 | (step32 & 0xFFFF0000FFFF0000ULL) >> 16;
        return (step16 & 0x00FF00FF00FF00FFULL) << 8 | (step16 & 0xFF00FF00FF00FF00ULL) >> 8;
#else
        return BOOST_ENDIAN_INTRINSIC_BYTE_SWAP_8(static_cast<uint64_t>(x));
#endif
    }
};
} // namespace detail

/**
 * @brief Generic function to reverse the byte order of any supported type
 *
 * Calls the appropriate specialized implementation based on the type's size
 * and whether it's signed or unsigned.
 *
 * @tparam T Type of value to reverse
 * @param x Value to reverse
 * @return Byte-swapped value
 */
template <typename T>
inline T
endian_reverse(T x) {
    return detail::bytes<sizeof(T), std::is_signed<T>::value>::reverse(x);
}

/**
 * @brief Convert from big-endian to native byte order
 *
 * On big-endian systems, this is a no-op. On little-endian systems,
 * it reverses the byte order.
 *
 * @tparam T Type of value to convert
 * @param x Value in big-endian byte order
 * @return Value in native byte order
 */
template <typename T>
inline T
big_to_native(T x) {
#ifdef BOOST_BIG_ENDIAN
    return x;
#else
    return endian_reverse(x);
#endif
}

/**
 * @brief Convert from native byte order to big-endian
 *
 * On big-endian systems, this is a no-op. On little-endian systems,
 * it reverses the byte order.
 *
 * @tparam T Type of value to convert
 * @param x Value in native byte order
 * @return Value in big-endian byte order
 */
template <typename T>
inline T
native_to_big(T x) {
#ifdef BOOST_BIG_ENDIAN
    return x;
#else
    return endian_reverse(x);
#endif
}

/**
 * @brief Convert from little-endian to native byte order
 *
 * On little-endian systems, this is a no-op. On big-endian systems,
 * it reverses the byte order.
 *
 * @tparam T Type of value to convert
 * @param x Value in little-endian byte order
 * @return Value in native byte order
 */
template <typename T>
inline T
little_to_native(T x) {
#ifdef BOOST_LITTLE_ENDIAN
    return x;
#else
    return endian_reverse(x);
#endif
}

/**
 * @brief Convert from native byte order to little-endian
 *
 * On little-endian systems, this is a no-op. On big-endian systems,
 * it reverses the byte order.
 *
 * @tparam T Type of value to convert
 * @param x Value in native byte order
 * @return Value in little-endian byte order
 */
template <typename T>
inline T
native_to_little(T x) {
#ifdef BOOST_LITTLE_ENDIAN
    return x;
#else
    return endian_reverse(x);
#endif
}

} // namespace endian
} // namespace util
} // namespace qb
