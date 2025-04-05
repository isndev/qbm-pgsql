#include "protocol_io_traits.h"
#include <stdexcept>
#include <array>
#include <regex>
#include <ctime>
#include <netinet/in.h> // For htons, htonl

namespace qb {
namespace pg {
namespace io {

// Helper functions for text format conversion
namespace {
    template <typename T>
    T convert_from_text(const char* data, size_t size) {
        if (!data || size == 0)
            throw std::runtime_error("Empty data for text conversion");
        
        std::string text(data, size);
        
        if constexpr (std::is_same_v<T, smallint>) {
            return static_cast<smallint>(std::stoi(text));
        } else if constexpr (std::is_same_v<T, integer>) {
            return std::stoi(text);
        } else if constexpr (std::is_same_v<T, bigint>) {
            return std::stoll(text);
        } else if constexpr (std::is_same_v<T, float>) {
            return std::stof(text);
        } else if constexpr (std::is_same_v<T, double>) {
            return std::stod(text);
        } else if constexpr (std::is_same_v<T, bool>) {
            return (text == "t" || text == "true" || text == "y" || text == "yes" || text == "1");
        } else {
            throw std::runtime_error("Unsupported type for text conversion");
        }
    }
}

// UUID specializations for binary format
template<>
detail::message::const_iterator protocol_read<pg::protocol_data_format::Binary, qb::uuid>(
    detail::message::const_iterator begin, detail::message::const_iterator end, qb::uuid& value) {
    
    if (std::distance(begin, end) < 16) {
        return begin;
    }
    
    // Create UUID from raw 16 bytes
    std::array<uint8_t, 16> uuid_bytes;
    for (size_t i = 0; i < 16; ++i) {
        uuid_bytes[i] = static_cast<uint8_t>(*(begin + i));
    }
    
    value = qb::uuid(uuid_bytes);
    return begin + 16;
}

// UUID specializations for text format
template<>
detail::message::const_iterator protocol_read<pg::protocol_data_format::Text, qb::uuid>(
    detail::message::const_iterator begin, detail::message::const_iterator end, qb::uuid& value) {
    
    if (begin == end) {
        return begin;
    }
    
    try {
        // Extract data into a temporary vector
        auto data = detail::copy_to_vector(begin, end);
        
        // Parse UUID from string
        auto uuid_opt = qb::uuid::from_string(std::string(data.data(), data.size()));
        if (!uuid_opt) {
            return begin;
        }
        
        value = *uuid_opt;
        return end;
    } catch (const std::exception&) {
        return begin;
    }
}

} // namespace io
} // namespace pg
} // namespace qb 