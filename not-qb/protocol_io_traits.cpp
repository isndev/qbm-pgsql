/**
 * @file protocol_io_traits.cpp
 * @brief Implémentation des traits d'E/S du protocole PostgreSQL
 */

#include "protocol_io_traits.h"
#include "protocol.h"
#include "../detail/param_unserializer.h"
#include <vector>
#include <string>
#include <cstring>
#include <stdexcept>
#include <boost/endian/conversion.hpp>

namespace qb {
namespace pg {
namespace io {

// Fonctions d'aide pour la conversion
namespace {

template<typename T>
T convert_from_binary(const char* data, size_t size) {
    if (size < sizeof(T)) {
        throw std::runtime_error("Buffer too small for value");
    }
    
    T value;
    std::memcpy(&value, data, sizeof(T));
    
    // Convert from network byte order if needed
    if constexpr (sizeof(T) == 2) {
        value = boost::endian::big_to_native(static_cast<uint16_t>(value));
    } else if constexpr (sizeof(T) == 4) {
        value = boost::endian::big_to_native(static_cast<uint32_t>(value));
    } else if constexpr (sizeof(T) == 8) {
        // Manual byte swap for 64-bit values (not standardized)
        union {
            uint64_t i;
            T value;
            char b[8];
        } src, dst;
        
        src.value = value;
        
        dst.b[0] = src.b[7];
        dst.b[1] = src.b[6];
        dst.b[2] = src.b[5];
        dst.b[3] = src.b[4];
        dst.b[4] = src.b[3];
        dst.b[5] = src.b[2];
        dst.b[6] = src.b[1];
        dst.b[7] = src.b[0];
        
        value = dst.value;
    }
    
    return value;
}

template<typename T>
T convert_from_text(const char* data, size_t size) {
    if (size == 0) {
        if constexpr (std::is_same_v<T, std::string>) {
            return "";
        } else if constexpr (std::is_integral_v<T>) {
            return 0;
        } else if constexpr (std::is_floating_point_v<T>) {
            return 0.0;
        } else if constexpr (std::is_same_v<T, bool>) {
            return false;
        } else {
            throw std::runtime_error("Unsupported type conversion from text");
        }
    }
    
    // Convert string representation to value
    if constexpr (std::is_same_v<T, std::string>) {
        return std::string(data, size);
    } else if constexpr (std::is_integral_v<T>) {
        return static_cast<T>(std::stoll(std::string(data, size)));
    } else if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(std::stod(std::string(data, size)));
    } else if constexpr (std::is_same_v<T, bool>) {
        std::string s(data, size);
        return (s == "t" || s == "true" || s == "1");
    } else {
        throw std::runtime_error("Unsupported type conversion from text");
    }
}

// Copie les données entre deux itérateurs dans un vecteur
template<typename InputIterator>
std::vector<char> copy_to_vector(InputIterator begin, InputIterator end) {
    std::vector<char> result;
    std::copy(begin, end, std::back_inserter(result));
    return result;
}

} // namespace

// Implémentations spécialisées pour les types courants - format BINARY
template<>
detail::message::const_iterator protocol_read<BINARY_DATA_FORMAT, smallint>(
    detail::message::const_iterator begin, detail::message::const_iterator end, smallint& value) {
    
    if (std::distance(begin, end) < sizeof(smallint)) {
        return begin;
    }
    
    // Extraire les données dans un vecteur temporaire
    auto data = copy_to_vector(begin, begin + sizeof(smallint));
    
    try {
        // Convertir les données binaires en smallint
        value = convert_from_binary<smallint>(data.data(), data.size());
        return begin + sizeof(smallint);
    } catch (const std::exception&) {
        return begin;
    }
}

template<>
detail::message::const_iterator protocol_read<BINARY_DATA_FORMAT, integer>(
    detail::message::const_iterator begin, detail::message::const_iterator end, integer& value) {
    
    if (std::distance(begin, end) < sizeof(integer)) {
        return begin;
    }
    
    // Extraire les données dans un vecteur temporaire
    auto data = copy_to_vector(begin, begin + sizeof(integer));
    
    try {
        // Convertir les données binaires en integer
        value = convert_from_binary<integer>(data.data(), data.size());
        return begin + sizeof(integer);
    } catch (const std::exception&) {
        return begin;
    }
}

template<>
detail::message::const_iterator protocol_read<BINARY_DATA_FORMAT, bigint>(
    detail::message::const_iterator begin, detail::message::const_iterator end, bigint& value) {
    
    if (std::distance(begin, end) < sizeof(bigint)) {
        return begin;
    }
    
    // Extraire les données dans un vecteur temporaire
    auto data = copy_to_vector(begin, begin + sizeof(bigint));
    
    try {
        // Convertir les données binaires en bigint
        value = convert_from_binary<bigint>(data.data(), data.size());
        return begin + sizeof(bigint);
    } catch (const std::exception&) {
        return begin;
    }
}

template<>
detail::message::const_iterator protocol_read<BINARY_DATA_FORMAT, float>(
    detail::message::const_iterator begin, detail::message::const_iterator end, float& value) {
    
    if (std::distance(begin, end) < sizeof(float)) {
        return begin;
    }
    
    // Extraire les données dans un vecteur temporaire
    auto data = copy_to_vector(begin, begin + sizeof(float));
    
    try {
        // Convertir les données binaires en float
        value = convert_from_binary<float>(data.data(), data.size());
        return begin + sizeof(float);
    } catch (const std::exception&) {
        return begin;
    }
}

template<>
detail::message::const_iterator protocol_read<BINARY_DATA_FORMAT, double>(
    detail::message::const_iterator begin, detail::message::const_iterator end, double& value) {
    
    if (std::distance(begin, end) < sizeof(double)) {
        return begin;
    }
    
    // Extraire les données dans un vecteur temporaire
    auto data = copy_to_vector(begin, begin + sizeof(double));
    
    try {
        // Convertir les données binaires en double
        value = convert_from_binary<double>(data.data(), data.size());
        return begin + sizeof(double);
    } catch (const std::exception&) {
        return begin;
    }
}

template<>
detail::message::const_iterator protocol_read<BINARY_DATA_FORMAT, bool>(
    detail::message::const_iterator begin, detail::message::const_iterator end, bool& value) {
    
    if (begin == end) {
        return begin;
    }
    
    value = (*begin != 0);
    return begin + 1;
}

template<>
detail::message::const_iterator protocol_read<BINARY_DATA_FORMAT, std::string>(
    detail::message::const_iterator begin, detail::message::const_iterator end, std::string& value) {
    
    value.clear();
    if (begin == end) {
        return begin;
    }
    
    // Copier toutes les données du buffer
    std::copy(begin, end, std::back_inserter(value));
    return end;
}

// Implémentations spécialisées pour les types courants - format TEXT
template<>
detail::message::const_iterator protocol_read<TEXT_DATA_FORMAT, std::string>(
    detail::message::const_iterator begin, detail::message::const_iterator end, std::string& value) {
    
    value.clear();
    if (begin == end) {
        return begin;
    }
    
    // Copier toutes les données du buffer
    std::copy(begin, end, std::back_inserter(value));
    return end;
}

template<>
detail::message::const_iterator protocol_read<TEXT_DATA_FORMAT, smallint>(
    detail::message::const_iterator begin, detail::message::const_iterator end, smallint& value) {
    
    if (begin == end) {
        return begin;
    }
    
    try {
        // Extraire les données dans un vecteur temporaire
        auto data = copy_to_vector(begin, end);
        
        // Convertir le texte en smallint
        value = convert_from_text<smallint>(data.data(), data.size());
        return end;
    } catch (const std::exception&) {
        return begin;
    }
}

template<>
detail::message::const_iterator protocol_read<TEXT_DATA_FORMAT, integer>(
    detail::message::const_iterator begin, detail::message::const_iterator end, integer& value) {
    
    if (begin == end) {
        return begin;
    }
    
    try {
        // Extraire les données dans un vecteur temporaire
        auto data = copy_to_vector(begin, end);
        
        // Convertir le texte en integer
        value = convert_from_text<integer>(data.data(), data.size());
        return end;
    } catch (const std::exception&) {
        return begin;
    }
}

template<>
detail::message::const_iterator protocol_read<TEXT_DATA_FORMAT, bigint>(
    detail::message::const_iterator begin, detail::message::const_iterator end, bigint& value) {
    
    if (begin == end) {
        return begin;
    }
    
    try {
        // Extraire les données dans un vecteur temporaire
        auto data = copy_to_vector(begin, end);
        
        // Convertir le texte en bigint
        value = convert_from_text<bigint>(data.data(), data.size());
        return end;
    } catch (const std::exception&) {
        return begin;
    }
}

template<>
detail::message::const_iterator protocol_read<TEXT_DATA_FORMAT, float>(
    detail::message::const_iterator begin, detail::message::const_iterator end, float& value) {
    
    if (begin == end) {
        return begin;
    }
    
    try {
        // Extraire les données dans un vecteur temporaire
        auto data = copy_to_vector(begin, end);
        
        // Convertir le texte en float
        value = convert_from_text<float>(data.data(), data.size());
        return end;
    } catch (const std::exception&) {
        return begin;
    }
}

template<>
detail::message::const_iterator protocol_read<TEXT_DATA_FORMAT, double>(
    detail::message::const_iterator begin, detail::message::const_iterator end, double& value) {
    
    if (begin == end) {
        return begin;
    }
    
    try {
        // Extraire les données dans un vecteur temporaire
        auto data = copy_to_vector(begin, end);
        
        // Convertir le texte en double
        value = convert_from_text<double>(data.data(), data.size());
        return end;
    } catch (const std::exception&) {
        return begin;
    }
}

template<>
detail::message::const_iterator protocol_read<TEXT_DATA_FORMAT, bool>(
    detail::message::const_iterator begin, detail::message::const_iterator end, bool& value) {
    
    if (begin == end) {
        return begin;
    }
    
    try {
        // Extraire les données dans un vecteur temporaire
        auto data = copy_to_vector(begin, end);
        
        // Convertir le texte en bool
        value = convert_from_text<bool>(data.data(), data.size());
        return end;
    } catch (const std::exception&) {
        return begin;
    }
}

} // namespace io
} // namespace pg
} // namespace qb 