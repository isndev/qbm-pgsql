/**
 * @file param_unserializer.cpp
 * @brief Implementation of binary data reading functions
 */

#include "param_unserializer.h"
#include <cstring>
#include <stdexcept>
#include <arpa/inet.h>  // for ntohs, ntohl

namespace qb::pg::detail {

// Conversion des nombres en endianness réseau
smallint ParamUnserializer::read_smallint(const std::vector<byte>& buffer) {
    if (buffer.size() < sizeof(smallint)) {
        throw std::runtime_error("Buffer too small for smallint");
    }
    
    uint16_t value;
    std::memcpy(&value, buffer.data(), sizeof(value));
    return ntohs(value);
}

integer ParamUnserializer::read_integer(const std::vector<byte>& buffer) {
    if (buffer.size() < sizeof(integer)) {
        throw std::runtime_error("Buffer too small for integer");
    }
    
    uint32_t value;
    std::memcpy(&value, buffer.data(), sizeof(value));
    return ntohl(value);
}

bigint ParamUnserializer::read_bigint(const std::vector<byte>& buffer) {
    if (buffer.size() < sizeof(bigint)) {
        throw std::runtime_error("Buffer too small for bigint");
    }
    
    // Manual byte swap for 64-bit integers (ntohll is not standard)
    union {
        int64_t i;
        byte b[8];
    } src, dst;
    
    std::memcpy(src.b, buffer.data(), sizeof(src.b));
    
    dst.b[0] = src.b[7];
    dst.b[1] = src.b[6];
    dst.b[2] = src.b[5];
    dst.b[3] = src.b[4];
    dst.b[4] = src.b[3];
    dst.b[5] = src.b[2];
    dst.b[6] = src.b[1];
    dst.b[7] = src.b[0];
    
    return dst.i;
}

float ParamUnserializer::read_float(const std::vector<byte>& buffer) {
    if (buffer.size() < sizeof(float)) {
        throw std::runtime_error("Buffer too small for float");
    }
    
    // Pour les floats, une fois l'integer lu en bon endianness, nous pouvons l'interpréter directement
    uint32_t intval;
    std::memcpy(&intval, buffer.data(), sizeof(intval));
    intval = ntohl(intval);
    
    float result;
    std::memcpy(&result, &intval, sizeof(result));
    return result;
}

double ParamUnserializer::read_double(const std::vector<byte>& buffer) {
    if (buffer.size() < sizeof(double)) {
        throw std::runtime_error("Buffer too small for double");
    }
    
    // Manual byte swap for 64-bit doubles
    union {
        uint64_t i;
        double d;
        byte b[8];
    } src, dst;
    
    std::memcpy(src.b, buffer.data(), sizeof(src.b));
    
    dst.b[0] = src.b[7];
    dst.b[1] = src.b[6];
    dst.b[2] = src.b[5];
    dst.b[3] = src.b[4];
    dst.b[4] = src.b[3];
    dst.b[5] = src.b[2];
    dst.b[6] = src.b[1];
    dst.b[7] = src.b[0];
    
    return dst.d;
}

std::string ParamUnserializer::read_string(const std::vector<byte>& buffer) {
    // Pour les chaînes de caractères, nous pouvons simplement convertir le tampon en string
    if (buffer.empty()) {
        return "";
    }
    
    // PostgreSQL envoie du texte sans terminateur nul, donc on peut directement utiliser les données
    return std::string(buffer.begin(), buffer.end());
}

} // namespace qb::pg::detail 