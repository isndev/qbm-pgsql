/**
 * @file param_unserializer.cpp
 * @brief Implementation of binary data reading functions
 */

#include "param_unserializer.h"
#include <cstring>
#include <netinet/in.h>  // pour ntohl, ntohs
#include <stdexcept>
#include <iostream>

namespace qb::pg::detail {

// Conversion des nombres en endianness réseau
smallint ParamUnserializer::read_smallint(const std::vector<byte>& buffer) {
    // Vérifier la taille minimale du buffer pour un smallint
    if (buffer.size() < sizeof(smallint)) {
        throw std::runtime_error("Buffer too small for smallint");
    }

    // Conversion en network byte order (big-endian)
    union {
        smallint value;
        byte bytes[sizeof(smallint)];
    } data;

    std::memcpy(data.bytes, buffer.data(), sizeof(smallint));
    return ntohs(data.value);
}

integer ParamUnserializer::read_integer(const std::vector<byte>& buffer) {
    // Vérifier la taille minimale du buffer pour un integer
    if (buffer.size() < sizeof(integer)) {
        throw std::runtime_error("Buffer too small for integer");
    }

    // Conversion en network byte order (big-endian)
    union {
        integer value;
        byte bytes[sizeof(integer)];
    } data;

    std::memcpy(data.bytes, buffer.data(), sizeof(integer));
    return ntohl(data.value);
}

bigint ParamUnserializer::read_bigint(const std::vector<byte>& buffer) {
    // Vérifier la taille minimale du buffer pour un bigint
    if (buffer.size() < sizeof(bigint)) {
        throw std::runtime_error("Buffer too small for bigint");
    }

    // Manual byte swapping for 64-bit integers (network byte order)
    union {
        bigint i;
        byte b[8];
    } src, dst;

    std::memcpy(src.b, buffer.data(), sizeof(bigint));
    
    // Conversion big-endian vers l'ordre de l'hôte
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
    // Vérifier la taille minimale du buffer pour un float
    if (buffer.size() < sizeof(float)) {
        throw std::runtime_error("Buffer too small for float");
    }

    // Pour les floats, une fois l'entier lu en bon endianness, nous pouvons l'interpréter directement
    union {
        uint32_t i;
        float f;
        byte b[4];
    } src, dst;

    std::memcpy(src.b, buffer.data(), sizeof(float));
    
    // Conversion big-endian vers l'ordre de l'hôte
    dst.b[0] = src.b[3];
    dst.b[1] = src.b[2];
    dst.b[2] = src.b[1];
    dst.b[3] = src.b[0];

    return dst.f;
}

double ParamUnserializer::read_double(const std::vector<byte>& buffer) {
    // Vérifier la taille minimale du buffer pour un double
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
    // Un buffer vide est une chaîne vide
    if (buffer.empty()) {
        return "";
    }

    // Détection automatique du format - uniquement pour les buffers de taille raisonnable
    // pour éviter de confondre les grands buffers de données avec le format binaire
    if (buffer.size() >= 4 && buffer.size() <= 1024 * 1024 &&
        (buffer[0] == 0 || buffer[1] == 0 || buffer[2] == 0)) {
        // C'est probablement un format binaire avec un préfixe de longueur de 4 octets
        try {
            return read_binary_string(buffer);
        } catch (const std::exception&) {
            // Si la lecture binaire échoue, essayer en tant que texte
            return read_text_string(buffer);
        }
    } else {
        // Format texte - utiliser directement les données
        return read_text_string(buffer);
    }
}

std::string ParamUnserializer::read_text_string(const std::vector<byte>& buffer) {
    // En format TEXT, nous prenons simplement le contenu tel quel
    return std::string(reinterpret_cast<const char*>(buffer.data()), buffer.size());
}

std::string ParamUnserializer::read_binary_string(const std::vector<byte>& buffer) {
    // Le format binaire a un préfixe de longueur de 4 octets
    if (buffer.size() < 4) {
        throw std::runtime_error("Buffer too small for binary string");
    }

    // Lire la longueur (les 4 premiers octets)
    integer length = read_integer(std::vector<byte>(buffer.begin(), buffer.begin() + 4));

    // Vérifier que la longueur est cohérente
    if (length < 0) {
        // Une longueur négative indique NULL
        return "";
    }

    if (static_cast<size_t>(length) + 4 > buffer.size()) {
        throw std::runtime_error("String length exceeds buffer size");
    }

    // Extraire la chaîne
    return std::string(reinterpret_cast<const char*>(buffer.data() + 4), length);
}

bool ParamUnserializer::read_bool(const std::vector<byte>& buffer) {
    // Format texte ("true"/"false")
    if (buffer.size() >= 4 && 
        (buffer[0] == 't' || buffer[0] == 'T' || 
         buffer[0] == 'f' || buffer[0] == 'F' ||
         buffer[0] == '1' || buffer[0] == '0')) {
        
        std::string text = read_text_string(buffer);
        return (text == "true" || text == "t" || text == "1" || text == "y" || text == "yes");
    }
    
    // Format binaire (1 octet)
    if (buffer.size() >= 1) {
        return (buffer[0] != 0);
    }
    
    throw std::runtime_error("Invalid boolean format");
}

std::vector<byte> ParamUnserializer::read_bytea(const std::vector<byte>& buffer) {
    // Détection automatique du format
    if (buffer.size() >= 4 && (buffer[0] == 0 || buffer[1] == 0)) {
        // Format binaire avec préfixe de longueur
        if (buffer.size() < 4) {
            throw std::runtime_error("Buffer too small for binary bytea");
        }

        // Lire la longueur
        integer length = read_integer(std::vector<byte>(buffer.begin(), buffer.begin() + 4));

        // Vérifier la longueur
        if (length < 0) {
            // NULL
            return {};
        }

        if (static_cast<size_t>(length) + 4 > buffer.size()) {
            throw std::runtime_error("Bytea length exceeds buffer size");
        }

        // Extraire les données
        return std::vector<byte>(buffer.begin() + 4, buffer.begin() + 4 + length);
    } else {
        // Format texte (représentation hexadécimale)
        std::string hex_string = read_text_string(buffer);
        std::vector<byte> result;
        
        // Ignorer le préfixe "\\x" si présent
        size_t start_pos = 0;
        if (hex_string.size() >= 2 && hex_string.substr(0, 2) == "\\x") {
            start_pos = 2;
        }
        
        // Convertir chaque paire d'octets hex
        for (size_t i = start_pos; i < hex_string.size(); i += 2) {
            if (i + 1 >= hex_string.size()) {
                break;
            }
            
            byte value = 0;
            for (int j = 0; j < 2; ++j) {
                char c = hex_string[i + j];
                byte nibble;
                
                if (c >= '0' && c <= '9') {
                    nibble = c - '0';
                } else if (c >= 'a' && c <= 'f') {
                    nibble = c - 'a' + 10;
                } else if (c >= 'A' && c <= 'F') {
                    nibble = c - 'A' + 10;
                } else {
                    throw std::runtime_error("Invalid hex character in bytea");
                }
                
                value = (value << 4) | nibble;
            }
            
            result.push_back(value);
        }
        
        return result;
    }
}

} // namespace qb::pg::detail 