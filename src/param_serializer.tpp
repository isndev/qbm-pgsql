/**
 * @file param_serializer.tpp
 * @brief Template specializations for PostgreSQL parameter serializer traits
 *
 * This file contains the specializations of param_serializer_traits which were
 * originally defined in param_serializer.h. Separating these into a .tpp file
 * helps prevent compilation issues with template specializations.
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

namespace qb::pg::detail {

// Specialization for nullptr_t
template <>
struct ParamSerializer::param_serializer_traits<std::nullptr_t> {
    static void
    add_param(ParamSerializer &serializer, const std::nullptr_t &) {
        serializer.add_null();
    }
};

// Specialization for bool
template <>
struct ParamSerializer::param_serializer_traits<bool> {
    static void
    add_param(ParamSerializer &serializer, const bool &param) {
        serializer.add_bool(param);
    }
};

// Specialization for integral types (excluding bool)
template <typename T>
struct ParamSerializer::param_serializer_traits<
    T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, void>> {
    static void
    add_param(ParamSerializer &serializer, const T &param) {
        if constexpr (sizeof(T) <= 2) {
            serializer.add_smallint(param);
        } else if constexpr (sizeof(T) <= 4) {
            serializer.add_integer(param);
        } else {
            serializer.add_bigint(param);
        }
    }
};

// Specialization for floating-point types
template <typename T>
struct ParamSerializer::param_serializer_traits<T,
                                               std::enable_if_t<std::is_floating_point_v<T>, void>> {
    static void
    add_param(ParamSerializer &serializer, const T &param) {
        if constexpr (sizeof(T) <= 4) {
            serializer.add_float(param);
        } else {
            serializer.add_double(param);
        }
    }
};

// Specialization for std::string
template <>
struct ParamSerializer::param_serializer_traits<std::string> {
    static void
    add_param(ParamSerializer &serializer, const std::string &param) {
        serializer.add_string(param);
    }
};

// Specialization for std::string_view
template <>
struct ParamSerializer::param_serializer_traits<std::string_view> {
    static void
    add_param(ParamSerializer &serializer, const std::string_view &param) {
        serializer.add_string_view(param);
    }
};

// Specialization for const char*
template <>
struct ParamSerializer::param_serializer_traits<const char *> {
    static void
    add_param(ParamSerializer &serializer, const char *param) {
        serializer.add_cstring(param);
    }
};

// Specialization for vector<std::string>
template <>
struct ParamSerializer::param_serializer_traits<std::vector<std::string>> {
    static void
    add_param(ParamSerializer &serializer, const std::vector<std::string> &param) {
        serializer.add_string_vector(param);
    }
};

// Specialization for vector<char>
template <>
struct ParamSerializer::param_serializer_traits<std::vector<char>> {
    static void
    add_param(ParamSerializer &serializer, const std::vector<char> &param) {
        if (!param.empty()) {
            serializer.add_byte_array(reinterpret_cast<const byte *>(param.data()),
                                      param.size());
        } else {
            serializer.add_null();
        }
    }
};

// Specialization for vector<unsigned char>
template <>
struct ParamSerializer::param_serializer_traits<std::vector<unsigned char>> {
    static void
    add_param(ParamSerializer &serializer, const std::vector<unsigned char> &param) {
        if (!param.empty()) {
            serializer.add_byte_array(reinterpret_cast<const byte *>(param.data()),
                                      param.size());
        } else {
            serializer.add_null();
        }
    }
};

// Specialization for std::optional
template <typename T>
struct ParamSerializer::param_serializer_traits<std::optional<T>> {
    static void
    add_param(ParamSerializer &serializer, const std::optional<T> &param) {
        if (param.has_value()) {
            serializer.add_param(*param);
        } else {
            serializer.add_null();
        }
    }
};

// Specialization for vector types (excluding string, char and unsigned char vectors)
template <typename T>
struct ParamSerializer::param_serializer_traits<
    std::vector<T>,
    std::enable_if_t<!std::is_same_v<std::vector<T>, std::vector<std::string>> &&
                         !std::is_same_v<std::vector<T>, std::vector<char>> &&
                         !std::is_same_v<std::vector<T>, std::vector<unsigned char>>,
                     void>> {
    static void
    add_param(ParamSerializer &serializer, const std::vector<T> &param) {
        serializer.add_vector(param);
    }
};

} // namespace qb::pg::detail 