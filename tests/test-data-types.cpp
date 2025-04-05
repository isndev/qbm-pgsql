#include <algorithm>
#include <arpa/inet.h> // For htonl, htons
#include <cstring>
#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>
#include "../pgsql.h"
#include "../src/common.h"
#include "../src/param_unserializer.h"

using namespace qb::pg;
using namespace qb::pg::detail;

class PostgreSQLDataTypesTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        unserializer = std::make_unique<ParamUnserializer>();
    }

    void
    TearDown() override {
        unserializer.reset();
    }

    // Fonction d'aide pour remplir un vecteur avec des données binaires (entier)
    template <typename T>
    std::vector<qb::pg::byte>
    createBinaryBuffer(T value) {
        std::vector<qb::pg::byte> buffer;

        // Conversion en ordre réseau si nécessaire
        if constexpr (sizeof(T) == 2) {
            value = htons(value);
        } else if constexpr (sizeof(T) == 4) {
            value = htonl(value);
        } else if constexpr (sizeof(T) == 8) {
            // Swap manuel pour les valeurs 64 bits
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

        // Copier la valeur dans le buffer
        buffer.resize(sizeof(T));
        std::memcpy(buffer.data(), &value, sizeof(T));

        return buffer;
    }

    // Fonction d'aide pour créer un buffer binaire au format PostgreSQL (avec longueur)
    std::vector<qb::pg::byte>
    createPgBinaryString(const std::string &value) {
        std::vector<qb::pg::byte> buffer;

        // Ajouter la longueur (int32) en ordre réseau
        integer len = static_cast<integer>(value.size());
        auto lenBuffer = createBinaryBuffer(len);
        buffer.insert(buffer.end(), lenBuffer.begin(), lenBuffer.end());

        // Ajouter les données
        buffer.insert(buffer.end(), value.begin(), value.end());

        return buffer;
    }

    // Fonction d'aide pour afficher un buffer en hexadécimal
    void
    printBuffer(const std::vector<qb::pg::byte> &buffer, const std::string &label) {
        std::cout << label << " (size: " << buffer.size() << "): ";
        for (const auto &b : buffer) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(static_cast<unsigned char>(b)) << " ";
        }
        std::cout << std::dec << std::endl;
    }

    // Fonction d'aide pour générer une chaîne aléatoire
    std::string
    generateRandomString(size_t length) {
        static const char charset[] = "0123456789"
                                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                      "abcdefghijklmnopqrstuvwxyz"
                                      "!@#$%^&*()_+=-[]{}|;:,.<>?/";

        std::string result;
        result.resize(length);

        std::random_device rd;
        std::mt19937 generator(rd());
        std::uniform_int_distribution<int> distribution(0, sizeof(charset) - 2);

        for (size_t i = 0; i < length; ++i) {
            result[i] = charset[distribution(generator)];
        }

        return result;
    }

    // Fonction d'aide pour générer un buffer avec des données corrompues
    std::vector<qb::pg::byte>
    createCorruptedBuffer(size_t size) {
        std::vector<qb::pg::byte> buffer(size);

        std::random_device rd;
        std::mt19937 generator(rd());
        std::uniform_int_distribution<int> distribution(0, 255);

        for (size_t i = 0; i < size; ++i) {
            buffer[i] = static_cast<byte>(distribution(generator));
        }

        return buffer;
    }

    // Fonction d'aide pour créer un buffer avec une taille incorrecte
    template <typename T>
    std::vector<qb::pg::byte>
    createWrongSizeBuffer(T value, size_t wrong_size) {
        auto buffer = createBinaryBuffer(value);

        // Redimensionner pour avoir une taille incorrecte
        buffer.resize(wrong_size);

        // Remplir avec des données aléatoires si nécessaire
        if (wrong_size > sizeof(T)) {
            std::random_device rd;
            std::mt19937 generator(rd());
            std::uniform_int_distribution<int> distribution(0, 255);

            for (size_t i = sizeof(T); i < wrong_size; ++i) {
                buffer[i] = static_cast<byte>(distribution(generator));
            }
        }

        return buffer;
    }

    std::unique_ptr<ParamUnserializer> unserializer;
};

// Test avec un buffer vide
TEST_F(PostgreSQLDataTypesTest, EmptyBuffer) {
    // Créer un buffer vide
    std::vector<byte> buffer;

    // Désérialiser avec ParamUnserializer
    std::string result = unserializer->read_string(buffer);

    // Vérifier que le résultat est une chaîne vide
    std::cout << "Result: '" << result << "'" << std::endl;
    ASSERT_TRUE(result.empty());
}

// Test de désérialisation d'un smallint
TEST_F(PostgreSQLDataTypesTest, SmallintDeserialization) {
    // Créer un buffer binaire simulant un smallint
    smallint value = 12345;
    auto buffer = createBinaryBuffer(value);

    // Afficher le buffer pour debug
    printBuffer(buffer, "Smallint Buffer");

    // Désérialiser
    smallint result = unserializer->read_smallint(buffer);

    // Vérifier le résultat
    ASSERT_EQ(result, value);
}

// Test de désérialisation d'un integer
TEST_F(PostgreSQLDataTypesTest, IntegerDeserialization) {
    // Créer un buffer binaire simulant un integer
    integer value = 987654321;
    auto buffer = createBinaryBuffer(value);

    // Afficher le buffer pour debug
    printBuffer(buffer, "Integer Buffer");

    // Désérialiser
    integer result = unserializer->read_integer(buffer);

    // Vérifier le résultat
    ASSERT_EQ(result, value);
}

// Test de désérialisation d'un bigint
TEST_F(PostgreSQLDataTypesTest, BigintDeserialization) {
    // Créer un buffer binaire simulant un bigint
    bigint value = 9223372036854775807LL; // INT64_MAX
    auto buffer = createBinaryBuffer(value);

    // Afficher le buffer pour debug
    printBuffer(buffer, "Bigint Buffer");

    // Désérialiser
    bigint result = unserializer->read_bigint(buffer);

    // Vérifier le résultat
    ASSERT_EQ(result, value);
}

// Test de désérialisation d'un float
TEST_F(PostgreSQLDataTypesTest, FloatDeserialization) {
    // Créer un buffer binaire simulant un float
    float value = 3.14159f;

    // Pour float, on doit créer un buffer contenant les bits du float
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    auto buffer = createBinaryBuffer(bits);

    // Afficher le buffer pour debug
    printBuffer(buffer, "Float Buffer");

    // Désérialiser
    float result = unserializer->read_float(buffer);

    // Vérifier le résultat avec une petite marge d'erreur
    ASSERT_NEAR(result, value, 0.00001f);
}

// Test de désérialisation d'un double
TEST_F(PostgreSQLDataTypesTest, DoubleDeserialization) {
    // Créer un buffer binaire simulant un double
    double value = 2.7182818284590452;

    // Pour double, on doit créer un buffer manuellement avec les bits du double
    std::vector<qb::pg::byte> buffer(sizeof(double));

    // Convertir les bits en ordre réseau
    union {
        uint64_t i;
        double d;
        char b[8];
    } src, dst;

    src.d = value;

    dst.b[0] = src.b[7];
    dst.b[1] = src.b[6];
    dst.b[2] = src.b[5];
    dst.b[3] = src.b[4];
    dst.b[4] = src.b[3];
    dst.b[5] = src.b[2];
    dst.b[6] = src.b[1];
    dst.b[7] = src.b[0];

    std::memcpy(buffer.data(), dst.b, sizeof(double));

    // Afficher le buffer pour debug
    printBuffer(buffer, "Double Buffer");

    // Désérialiser
    double result = unserializer->read_double(buffer);

    // Vérifier le résultat avec une petite marge d'erreur
    ASSERT_NEAR(result, value, 0.0000000000001);
}

// Test de désérialisation d'une chaîne
TEST_F(PostgreSQLDataTypesTest, StringDeserialization) {
    // Créer un buffer contenant une chaîne
    std::string value = "Hello, PostgreSQL!";
    std::vector<byte> buffer(value.begin(), value.end());

    // Afficher le buffer pour debug
    printBuffer(buffer, "String Buffer");

    // Désérialiser
    std::string result = unserializer->read_string(buffer);

    // Vérifier le résultat
    ASSERT_EQ(result, value);
}

// Test de désérialisation d'une chaîne avec caractères spéciaux
TEST_F(PostgreSQLDataTypesTest, StringWithSpecialChars) {
    // Créer un buffer contenant une chaîne avec caractères spéciaux
    std::string value = "Special: \n\r\t\b\f\\\"\'";
    std::vector<byte> buffer(value.begin(), value.end());

    // Afficher le buffer pour debug
    printBuffer(buffer, "Special Chars Buffer");

    // Désérialiser
    std::string result = unserializer->read_string(buffer);

    // Vérifier le résultat
    ASSERT_EQ(result, value);
}

// Test de désérialisation d'une chaîne Unicode
TEST_F(PostgreSQLDataTypesTest, UnicodeStringDeserialization) {
    // Créer un buffer contenant une chaîne Unicode
    std::string value = "Unicode: äöü 你好 😀";
    std::vector<byte> buffer(value.begin(), value.end());

    // Afficher le buffer pour debug
    printBuffer(buffer, "Unicode String Buffer");

    // Désérialiser
    std::string result = unserializer->read_string(buffer);

    // Vérifier le résultat
    ASSERT_EQ(result, value);
}

// Test avec des valeurs limites pour les types numériques
TEST_F(PostgreSQLDataTypesTest, NumericBoundaryValues) {
    // Test avec smallint (MIN et MAX)
    smallint smallint_min = std::numeric_limits<smallint>::min();
    smallint smallint_max = std::numeric_limits<smallint>::max();

    auto buffer_smallint_min = createBinaryBuffer(smallint_min);
    auto buffer_smallint_max = createBinaryBuffer(smallint_max);

    ASSERT_EQ(unserializer->read_smallint(buffer_smallint_min), smallint_min);
    ASSERT_EQ(unserializer->read_smallint(buffer_smallint_max), smallint_max);

    // Test avec integer (MIN et MAX)
    integer integer_min = std::numeric_limits<integer>::min();
    integer integer_max = std::numeric_limits<integer>::max();

    auto buffer_integer_min = createBinaryBuffer(integer_min);
    auto buffer_integer_max = createBinaryBuffer(integer_max);

    ASSERT_EQ(unserializer->read_integer(buffer_integer_min), integer_min);
    ASSERT_EQ(unserializer->read_integer(buffer_integer_max), integer_max);

    // Test avec bigint (MIN et MAX)
    bigint bigint_min = std::numeric_limits<bigint>::min();
    bigint bigint_max = std::numeric_limits<bigint>::max();

    auto buffer_bigint_min = createBinaryBuffer(bigint_min);
    auto buffer_bigint_max = createBinaryBuffer(bigint_max);

    ASSERT_EQ(unserializer->read_bigint(buffer_bigint_min), bigint_min);
    ASSERT_EQ(unserializer->read_bigint(buffer_bigint_max), bigint_max);
}

// Test de buffer malformé ou trop petit
TEST_F(PostgreSQLDataTypesTest, MalformedBuffer) {
    // Créer un buffer plus petit que ce qui est attendu pour les types numériques
    std::vector<byte> small_buffer(1, 0);

    // Vérifier que la désérialisation échoue avec une exception
    ASSERT_THROW(unserializer->read_smallint(small_buffer), std::runtime_error);
    ASSERT_THROW(unserializer->read_integer(small_buffer), std::runtime_error);
    ASSERT_THROW(unserializer->read_bigint(small_buffer), std::runtime_error);
    ASSERT_THROW(unserializer->read_float(small_buffer), std::runtime_error);
    ASSERT_THROW(unserializer->read_double(small_buffer), std::runtime_error);
}

// Test avec NaN et infinité pour les flottants
TEST_F(PostgreSQLDataTypesTest, SpecialFloatingPointValues) {
    // NaN, Infinity, -Infinity
    float nanValue = std::numeric_limits<float>::quiet_NaN();
    float infValue = std::numeric_limits<float>::infinity();
    float negInfValue = -std::numeric_limits<float>::infinity();

    // Créer des buffers
    uint32_t nanBits, infBits, negInfBits;
    std::memcpy(&nanBits, &nanValue, sizeof(nanBits));
    std::memcpy(&infBits, &infValue, sizeof(infBits));
    std::memcpy(&negInfBits, &negInfValue, sizeof(negInfBits));

    auto nanBuffer = createBinaryBuffer(nanBits);
    auto infBuffer = createBinaryBuffer(infBits);
    auto negInfBuffer = createBinaryBuffer(negInfBits);

    // Désérialiser et vérifier
    float nanResult = unserializer->read_float(nanBuffer);
    float infResult = unserializer->read_float(infBuffer);
    float negInfResult = unserializer->read_float(negInfBuffer);

    ASSERT_TRUE(std::isnan(nanResult));
    ASSERT_TRUE(std::isinf(infResult) && infResult > 0);
    ASSERT_TRUE(std::isinf(negInfResult) && negInfResult < 0);
}

// Tests supplémentaires pour renforcer la suite

// Test de désérialisation de très grandes chaînes
TEST_F(PostgreSQLDataTypesTest, LargeStringDeserialization) {
    // Générer une chaîne de grande taille
    size_t length = 100000; // 100KB
    std::string value = generateRandomString(length);

    std::vector<byte> buffer(value.begin(), value.end());

    // Désérialiser (sans afficher le buffer pour éviter de surcharger la sortie)
    std::string result = unserializer->read_string(buffer);

    // Vérifier le résultat
    ASSERT_EQ(result.size(), length);
    ASSERT_EQ(result, value);
}

// Test de désérialisation d'une chaîne avec des caractères nuls
TEST_F(PostgreSQLDataTypesTest, StringWithNullChars) {
    // Créer un buffer contenant une chaîne avec des caractères nuls
    std::vector<byte> buffer = {'H', 'e', 'l', 'l', 'o', '\0', 'W', 'o', 'r', 'l', 'd', '\0', '!'};

    // Afficher le buffer pour debug
    printBuffer(buffer, "String with Null Chars Buffer");

    // Désérialiser
    std::string result = unserializer->read_string(buffer);

    // Vérifier le résultat
    ASSERT_EQ(result.size(), buffer.size());
    for (size_t i = 0; i < buffer.size(); ++i) {
        ASSERT_EQ(result[i], buffer[i]);
    }
}

// Test de désérialisation avec des tailles non standard
TEST_F(PostgreSQLDataTypesTest, NonStandardSizeBuffers) {
    // Créer des buffers avec des tailles un peu plus grandes que ce qui est attendu
    integer value = 12345;

    // Buffer plus grand que sizeof(integer)
    auto oversized_buffer = createWrongSizeBuffer(value, sizeof(integer) + 2);
    printBuffer(oversized_buffer, "Oversized Integer Buffer");

    // La désérialisation devrait réussir car nous prenons les premiers octets
    integer result = unserializer->read_integer(oversized_buffer);
    ASSERT_EQ(result, value);

    // Buffer de taille incorrecte pour smallint
    std::vector<byte> wrong_size_buffer = {
        0x30, 0x39, static_cast<char>(0xFF)}; // smallint (12345) avec un octet supplémentaire
    printBuffer(wrong_size_buffer, "Wrong Size Smallint Buffer");

    // Tester si le parser gère correctement cette situation
    smallint smallint_result = unserializer->read_smallint(wrong_size_buffer);
    ASSERT_EQ(smallint_result, 12345);
}

// Test de désérialisation avec des données corrompues
TEST_F(PostgreSQLDataTypesTest, CorruptedData) {
    // Créer des buffers avec des données corrompues
    std::vector<byte> corrupted_smallint = {
        static_cast<char>(0xFF), static_cast<char>(0xFF)}; // Valeur qui peut causer des problèmes

    // Désérialiser et vérifier
    smallint result = unserializer->read_smallint(corrupted_smallint);
    ASSERT_EQ(result, -1); // La valeur attendue pour 0xFFFF

    // Test avec un buffer corrompu de taille aléatoire
    auto random_corrupted = createCorruptedBuffer(10);
    printBuffer(random_corrupted, "Random Corrupted Buffer");

    // Tester avec les différentes méthodes de désérialisation
    if (random_corrupted.size() >= sizeof(smallint)) {
        smallint smallint_result = unserializer->read_smallint(random_corrupted);
        std::cout << "Corrupted smallint result: " << smallint_result << std::endl;
    }

    if (random_corrupted.size() >= sizeof(integer)) {
        integer integer_result = unserializer->read_integer(random_corrupted);
        std::cout << "Corrupted integer result: " << integer_result << std::endl;
    }

    // La chaîne devrait toujours fonctionner
    std::string string_result = unserializer->read_string(random_corrupted);
    std::cout << "Corrupted string result length: " << string_result.size() << std::endl;
}

// Test de robustesse aux valeurs extrêmes
TEST_F(PostgreSQLDataTypesTest, ExtremeValueRobustness) {
    // Test avec des valeurs extrêmes qui pourraient poser problème

    // 1. Très petites valeurs proches de zéro
    double very_small_double = std::numeric_limits<double>::min() / 2.0;

    union {
        uint64_t i;
        double d;
        char b[8];
    } src, dst;

    src.d = very_small_double;

    dst.b[0] = src.b[7];
    dst.b[1] = src.b[6];
    dst.b[2] = src.b[5];
    dst.b[3] = src.b[4];
    dst.b[4] = src.b[3];
    dst.b[5] = src.b[2];
    dst.b[6] = src.b[1];
    dst.b[7] = src.b[0];

    std::vector<qb::pg::byte> buffer(sizeof(double));
    std::memcpy(buffer.data(), dst.b, sizeof(double));

    // Désérialiser et vérifier
    double result = unserializer->read_double(buffer);
    ASSERT_NEAR(result, very_small_double, std::numeric_limits<double>::min());

    // 2. Valeurs avec précision extrême
    double precise_double = 1.0 + std::numeric_limits<double>::epsilon();

    src.d = precise_double;

    dst.b[0] = src.b[7];
    dst.b[1] = src.b[6];
    dst.b[2] = src.b[5];
    dst.b[3] = src.b[4];
    dst.b[4] = src.b[3];
    dst.b[5] = src.b[2];
    dst.b[6] = src.b[1];
    dst.b[7] = src.b[0];

    std::memcpy(buffer.data(), dst.b, sizeof(double));

    // Désérialiser et vérifier
    result = unserializer->read_double(buffer);
    ASSERT_NEAR(result, precise_double, std::numeric_limits<double>::epsilon());
}

// Test de désérialisation répétée avec la même instance
TEST_F(PostgreSQLDataTypesTest, RepeatedDeserialization) {
    // Créer différents buffers
    auto smallint_buffer = createBinaryBuffer<smallint>(12345);
    auto integer_buffer = createBinaryBuffer<integer>(67890);
    std::string text = "Test string";
    std::vector<byte> string_buffer(text.begin(), text.end());

    // Désérialiser plusieurs fois avec la même instance
    for (int i = 0; i < 100; ++i) {
        smallint smallint_result = unserializer->read_smallint(smallint_buffer);
        ASSERT_EQ(smallint_result, 12345);

        integer integer_result = unserializer->read_integer(integer_buffer);
        ASSERT_EQ(integer_result, 67890);

        std::string string_result = unserializer->read_string(string_buffer);
        ASSERT_EQ(string_result, text);
    }
}

// Test de séquence complexe de désérialisation
TEST_F(PostgreSQLDataTypesTest, ComplexDeserializationSequence) {
    // Simuler une séquence de paramètres telle qu'elle pourrait apparaître dans un protocole réel
    std::vector<std::vector<byte>> buffers;

    // 1. Ajouter un smallint
    buffers.push_back(createBinaryBuffer<smallint>(101));

    // 2. Ajouter un integer
    buffers.push_back(createBinaryBuffer<integer>(20000));

    // 3. Ajouter une chaîne
    std::string text = "Parameter text";
    buffers.push_back(std::vector<byte>(text.begin(), text.end()));

    // 4. Ajouter un bigint
    buffers.push_back(createBinaryBuffer<bigint>(9223372036854775800LL));

    // 5. Ajouter un float
    float float_value = 3.14159f;
    uint32_t float_bits;
    std::memcpy(&float_bits, &float_value, sizeof(float_bits));
    buffers.push_back(createBinaryBuffer(float_bits));

    // Désérialiser la séquence
    smallint r1 = unserializer->read_smallint(buffers[0]);
    integer r2 = unserializer->read_integer(buffers[1]);
    std::string r3 = unserializer->read_string(buffers[2]);
    bigint r4 = unserializer->read_bigint(buffers[3]);
    float r5 = unserializer->read_float(buffers[4]);

    // Vérifier les résultats
    ASSERT_EQ(r1, 101);
    ASSERT_EQ(r2, 20000);
    ASSERT_EQ(r3, text);
    ASSERT_EQ(r4, 9223372036854775800LL);
    ASSERT_NEAR(r5, float_value, 0.00001f);
}

// Test avec des buffers ayant le bit de poids fort activé
TEST_F(PostgreSQLDataTypesTest, HighBitValues) {
    // Créer des valeurs avec le bit de poids fort activé
    smallint high_bit_smallint = 0x8000;           // -32768 en complément à deux
    integer high_bit_integer = 0x80000000;         // -2147483648 en complément à deux
    bigint high_bit_bigint = 0x8000000000000000LL; // Minimum pour bigint

    // Créer les buffers
    auto smallint_buffer = createBinaryBuffer(high_bit_smallint);
    auto integer_buffer = createBinaryBuffer(high_bit_integer);
    auto bigint_buffer = createBinaryBuffer(high_bit_bigint);

    // Afficher les buffers pour debug
    printBuffer(smallint_buffer, "High-bit Smallint Buffer");
    printBuffer(integer_buffer, "High-bit Integer Buffer");
    printBuffer(bigint_buffer, "High-bit Bigint Buffer");

    // Désérialiser
    smallint smallint_result = unserializer->read_smallint(smallint_buffer);
    integer integer_result = unserializer->read_integer(integer_buffer);
    bigint bigint_result = unserializer->read_bigint(bigint_buffer);

    // Vérifier les résultats
    ASSERT_EQ(smallint_result, high_bit_smallint);
    ASSERT_EQ(integer_result, high_bit_integer);
    ASSERT_EQ(bigint_result, high_bit_bigint);
}

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}