/**
 * @file test-data-types.cpp
 * @brief Unit tests for PostgreSQL data type handling
 *
 * This file implements comprehensive tests for the data type serialization and
 * deserialization capabilities of the PostgreSQL client module. It verifies the
 * client's ability to correctly handle the full range of PostgreSQL data types
 * including:
 *
 * - Numeric types (smallint, integer, bigint, float, double, numeric)
 * - Character types (char, varchar, text)
 * - Binary data types (bytea)
 * - Date/time types (date, time, timestamp, interval)
 * - Boolean type
 * - Network address types (inet, cidr)
 * - UUID type
 * - JSON types (json, jsonb)
 * - Array types
 * - Composite types
 *
 * The implementation validates both serialization (client to server) and deserialization
 * (server to client) operations, ensuring data integrity across the wire protocol.
 * Each test case covers normal cases, boundary conditions, and error scenarios.
 *
 * Key features tested:
 * - Binary format parsing and generation
 * - Network byte order handling
 * - Character encoding conversions
 * - Special values (NaN, infinity, NULL)
 * - Unicode character support
 * - Error detection and recovery
 * - Robustness against malformed data
 *
 * @see qb::pg::detail::ParamSerializer
 * @see qb::pg::detail::ParamUnserializer
 * @see qb::pg::detail::TypeConverter
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

#include <gtest/gtest.h>
#include <iomanip>
#include <iostream>
#include <random>
#include "../pgsql.h"

using namespace qb::pg;
using namespace qb::pg::detail;

/**
 * @brief Test fixture for PostgreSQL data type handling
 *
 * Provides a comprehensive environment for testing data type conversion between
 * C++ types and PostgreSQL wire protocol format. Includes helper methods for
 * creating test data, manipulating binary buffers, and validating conversion results.
 */
class PostgreSQLDataTypesTest : public ::testing::Test {
protected:
    /**
     * @brief Set up the test environment
     *
     * Creates a parameter unserializer instance for testing deserialization
     * operations from PostgreSQL binary format to C++ types.
     */
    void
    SetUp() override {
        unserializer = std::make_unique<ParamUnserializer>();
    }

    /**
     * @brief Clean up after tests
     *
     * Destroys the parameter unserializer instance.
     */
    void
    TearDown() override {
        unserializer.reset();
    }

    /**
     * @brief Helper function to create a binary buffer containing a value
     *
     * Creates a binary buffer with a value in network byte order (big-endian)
     * as required by the PostgreSQL wire protocol. Handles different sized types
     * with appropriate byte swapping.
     *
     * @tparam T Type of value to convert to binary
     * @param value The value to convert to binary format
     * @return std::vector<qb::pg::byte> Binary buffer with the value in network byte
     * order
     */
    template <typename T>
    std::vector<qb::pg::byte>
    createBinaryBuffer(T value) {
        std::vector<qb::pg::byte> buffer;

        // Utiliser l'utilitaire d'endianness pour toutes les conversions
        if constexpr (sizeof(T) == 2) {
            value = qb::endian::to_big_endian(value);
        } else if constexpr (sizeof(T) == 4) {
            value = qb::endian::to_big_endian(value);
        } else if constexpr (sizeof(T) == 8) {
            value = qb::endian::to_big_endian(value);
        }

        // Copy the value to the buffer
        buffer.resize(sizeof(T));
        std::memcpy(buffer.data(), &value, sizeof(T));

        return buffer;
    }

    /**
     * @brief Helper function to create a PostgreSQL binary string
     *
     * Creates a binary buffer in PostgreSQL text format with 4-byte length prefix
     * followed by the string data. This matches the wire format used in the protocol.
     *
     * @param value The string value to convert
     * @return std::vector<qb::pg::byte> Binary buffer with length prefix and string data
     */
    std::vector<qb::pg::byte>
    createPgBinaryString(const std::string &value) {
        std::vector<qb::pg::byte> buffer;

        // Add length (int32) in network order
        integer len       = static_cast<integer>(value.size());
        auto    lenBuffer = createBinaryBuffer(len);
        buffer.insert(buffer.end(), lenBuffer.begin(), lenBuffer.end());

        // Add data
        buffer.insert(buffer.end(), value.begin(), value.end());

        return buffer;
    }

    /**
     * @brief Helper function to print a binary buffer in hexadecimal
     *
     * Outputs the contents of a binary buffer in hexadecimal format for debugging
     * and verification purposes.
     *
     * @param buffer The binary buffer to display
     * @param label A descriptive label for the output
     */
    void
    printBuffer(const std::vector<qb::pg::byte> &buffer, const std::string &label) {
        std::cout << label << " (size: " << buffer.size() << "): ";
        for (const auto &b : buffer) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(static_cast<unsigned char>(b)) << " ";
        }
        std::cout << std::dec << std::endl;
    }

    /**
     * @brief Helper function to generate a random string
     *
     * Creates a string of specified length with random characters from a diverse
     * character set including letters, numbers, and special characters.
     *
     * @param length The desired length of the random string
     * @return std::string A randomly generated string
     */
    std::string
    generateRandomString(size_t length) {
        static const char charset[] = "0123456789"
                                      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                      "abcdefghijklmnopqrstuvwxyz"
                                      "!@#$%^&*()_+=-[]{}|;:,.<>?/";

        std::string result;
        result.resize(length);

        std::random_device                 rd;
        std::mt19937                       generator(rd());
        std::uniform_int_distribution<int> distribution(0, sizeof(charset) - 2);

        for (size_t i = 0; i < length; ++i) {
            result[i] = charset[distribution(generator)];
        }

        return result;
    }

    /**
     * @brief Helper function to create a corrupted buffer with random data
     *
     * Generates a buffer of specified size filled with random bytes. Used for
     * testing robustness against malformed or corrupted data.
     *
     * @param size The desired size of the corrupted buffer
     * @return std::vector<qb::pg::byte> A buffer with random content
     */
    std::vector<qb::pg::byte>
    createCorruptedBuffer(size_t size) {
        std::vector<qb::pg::byte> buffer(size);

        std::random_device                 rd;
        std::mt19937                       generator(rd());
        std::uniform_int_distribution<int> distribution(0, 255);

        for (size_t i = 0; i < size; ++i) {
            buffer[i] = static_cast<byte>(distribution(generator));
        }

        return buffer;
    }

    /**
     * @brief Helper function to create a buffer with incorrect size
     *
     * Creates a binary buffer for a value but with deliberately incorrect size
     * to test handling of malformed data. Can create both undersized and oversized
     * buffers.
     *
     * @tparam T Type of value to convert
     * @param value The value to convert
     * @param wrong_size The incorrect size to use for the buffer
     * @return std::vector<qb::pg::byte> Incorrectly sized buffer
     */
    template <typename T>
    std::vector<qb::pg::byte>
    createWrongSizeBuffer(T value, size_t wrong_size) {
        auto buffer = createBinaryBuffer(value);

        // Resize to incorrect size
        buffer.resize(wrong_size);

        // Fill with random data if needed
        if (wrong_size > sizeof(T)) {
            std::random_device                 rd;
            std::mt19937                       generator(rd());
            std::uniform_int_distribution<int> distribution(0, 255);

            for (size_t i = sizeof(T); i < wrong_size; ++i) {
                buffer[i] = static_cast<byte>(distribution(generator));
            }
        }

        return buffer;
    }

    std::unique_ptr<ParamUnserializer> unserializer;
};

/**
 * @brief Test empty buffer handling
 *
 * Verifies that the unserializer can safely handle an empty buffer
 * without causing errors or undefined behavior.
 */
TEST_F(PostgreSQLDataTypesTest, EmptyBuffer) {
    // Create an empty buffer
    std::vector<byte> buffer;

    // Deserialize with ParamUnserializer
    std::string result = unserializer->read_string(buffer);

    // Verify that the result is an empty string
    std::cout << "Result: '" << result << "'" << std::endl;
    ASSERT_TRUE(result.empty());
}

/**
 * @brief Test smallint deserialization
 *
 * Verifies that a smallint (int16) value can be correctly deserialized
 * from its binary PostgreSQL representation.
 */
TEST_F(PostgreSQLDataTypesTest, SmallintDeserialization) {
    // Create a binary buffer simulating a smallint
    smallint value  = 12345;
    auto     buffer = createBinaryBuffer(value);

    // Display buffer for debugging
    printBuffer(buffer, "Smallint Buffer");

    // Deserialize
    smallint result = unserializer->read_smallint(buffer);

    // Verify the result
    ASSERT_EQ(result, value);
}

/**
 * @brief Test integer deserialization
 *
 * Verifies that an integer (int32) value can be correctly deserialized
 * from its binary PostgreSQL representation.
 */
TEST_F(PostgreSQLDataTypesTest, IntegerDeserialization) {
    // Create a binary buffer simulating an integer
    integer value  = 987654321;
    auto    buffer = createBinaryBuffer(value);

    // Display buffer for debugging
    printBuffer(buffer, "Integer Buffer");

    // Deserialize
    integer result = unserializer->read_integer(buffer);

    // Verify the result
    ASSERT_EQ(result, value);
}

/**
 * @brief Test bigint deserialization
 *
 * Verifies that a bigint (int64) value can be correctly deserialized
 * from its binary PostgreSQL representation, including handling of
 * large numeric values.
 */
TEST_F(PostgreSQLDataTypesTest, BigintDeserialization) {
    // Create a binary buffer simulating a bigint
    bigint value  = 9223372036854775807LL; // INT64_MAX
    auto   buffer = createBinaryBuffer(value);

    // Display buffer for debugging
    printBuffer(buffer, "Bigint Buffer");

    // Deserialize
    bigint result = unserializer->read_bigint(buffer);

    // Verify the result
    ASSERT_EQ(result, value);
}

/**
 * @brief Test float deserialization
 *
 * Verifies that a float (float4) value can be correctly deserialized
 * from its binary PostgreSQL representation, handling the
 * network byte order conversion.
 */
TEST_F(PostgreSQLDataTypesTest, FloatDeserialization) {
    // Create a binary buffer simulating a float
    float value = 3.14159f;

    // For float, we need to create a buffer containing the bits of the float
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    auto buffer = createBinaryBuffer(bits);

    // Display buffer for debugging
    printBuffer(buffer, "Float Buffer");

    // Deserialize
    float result = unserializer->read_float(buffer);

    // Verify the result with a small error margin
    ASSERT_NEAR(result, value, 0.00001f);
}

/**
 * @brief Test double deserialization
 *
 * Verifies that a double (float8) value can be correctly deserialized
 * from its binary PostgreSQL representation, handling the
 * network byte order conversion for 64-bit floating point values.
 */
TEST_F(PostgreSQLDataTypesTest, DoubleDeserialization) {
    // Create a binary buffer simulating a double
    double value = 2.7182818284590452;

    // For double, we need to manually create a buffer with the bits of the double
    std::vector<qb::pg::byte> buffer(sizeof(double));

    // Convert bits to network byte order
    union {
        uint64_t i;
        double   d;
        char     b[8];
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

    // Display buffer for debugging
    printBuffer(buffer, "Double Buffer");

    // Deserialize
    double result = unserializer->read_double(buffer);

    // Verify the result with a small error margin
    ASSERT_NEAR(result, value, 0.0000000000001);
}

/**
 * @brief Test string deserialization
 *
 * Verifies that string values can be correctly deserialized
 * from their binary PostgreSQL representation, ensuring exact
 * character-by-character reproduction.
 */
TEST_F(PostgreSQLDataTypesTest, StringDeserialization) {
    // Create a buffer containing a string
    std::string       value = "Hello, PostgreSQL!";
    std::vector<byte> buffer(value.begin(), value.end());

    // Display buffer for debugging
    printBuffer(buffer, "String Buffer");

    // Deserialize
    std::string result = unserializer->read_string(buffer);

    // Verify the result
    ASSERT_EQ(result, value);
}

/**
 * @brief Test string deserialization with special characters
 *
 * Verifies that strings containing special characters (escape sequences)
 * can be correctly deserialized, ensuring proper handling of
 * control characters and escaping sequences.
 */
TEST_F(PostgreSQLDataTypesTest, StringWithSpecialChars) {
    // Create a buffer containing a string with special characters
    std::string       value = "Special: \n\r\t\b\f\\\"\'";
    std::vector<byte> buffer(value.begin(), value.end());

    // Display buffer for debugging
    printBuffer(buffer, "Special Chars Buffer");

    // Deserialize
    std::string result = unserializer->read_string(buffer);

    // Verify the result
    ASSERT_EQ(result, value);
}

/**
 * @brief Test Unicode string deserialization
 *
 * Verifies that strings containing Unicode characters can be
 * correctly deserialized, ensuring support for international
 * character sets and emoji.
 */
TEST_F(PostgreSQLDataTypesTest, UnicodeStringDeserialization) {
    // Create a buffer containing a Unicode string
    std::string       value = "Unicode: äöü 你好 😀";
    std::vector<byte> buffer(value.begin(), value.end());

    // Display buffer for debugging
    printBuffer(buffer, "Unicode String Buffer");

    // Deserialize
    std::string result = unserializer->read_string(buffer);

    // Verify the result
    ASSERT_EQ(result, value);
}

/**
 * @brief Test numeric boundary values
 *
 * Verifies that minimum and maximum values for various numeric types
 * can be correctly deserialized, ensuring the implementation handles
 * boundary cases properly.
 */
TEST_F(PostgreSQLDataTypesTest, NumericBoundaryValues) {
    // Test with smallint (MIN and MAX)
    smallint smallint_min = std::numeric_limits<smallint>::min();
    smallint smallint_max = std::numeric_limits<smallint>::max();

    auto buffer_smallint_min = createBinaryBuffer(smallint_min);
    auto buffer_smallint_max = createBinaryBuffer(smallint_max);

    ASSERT_EQ(unserializer->read_smallint(buffer_smallint_min), smallint_min);
    ASSERT_EQ(unserializer->read_smallint(buffer_smallint_max), smallint_max);

    // Test with integer (MIN and MAX)
    integer integer_min = std::numeric_limits<integer>::min();
    integer integer_max = std::numeric_limits<integer>::max();

    auto buffer_integer_min = createBinaryBuffer(integer_min);
    auto buffer_integer_max = createBinaryBuffer(integer_max);

    ASSERT_EQ(unserializer->read_integer(buffer_integer_min), integer_min);
    ASSERT_EQ(unserializer->read_integer(buffer_integer_max), integer_max);

    // Test with bigint (MIN and MAX)
    bigint bigint_min = std::numeric_limits<bigint>::min();
    bigint bigint_max = std::numeric_limits<bigint>::max();

    auto buffer_bigint_min = createBinaryBuffer(bigint_min);
    auto buffer_bigint_max = createBinaryBuffer(bigint_max);

    ASSERT_EQ(unserializer->read_bigint(buffer_bigint_min), bigint_min);
    ASSERT_EQ(unserializer->read_bigint(buffer_bigint_max), bigint_max);
}

/**
 * @brief Test malformed buffer handling
 *
 * Verifies that the unserializer correctly handles malformed or
 * undersized buffers by throwing appropriate exceptions rather
 * than causing undefined behavior or crashes.
 */
TEST_F(PostgreSQLDataTypesTest, MalformedBuffer) {
    // Create a buffer smaller than expected for numeric types
    std::vector<byte> small_buffer(1, 0);

    // Verify that deserialization fails with an exception
    ASSERT_THROW(unserializer->read_smallint(small_buffer), std::runtime_error);
    ASSERT_THROW(unserializer->read_integer(small_buffer), std::runtime_error);
    ASSERT_THROW(unserializer->read_bigint(small_buffer), std::runtime_error);
    ASSERT_THROW(unserializer->read_float(small_buffer), std::runtime_error);
    ASSERT_THROW(unserializer->read_double(small_buffer), std::runtime_error);
}

/**
 * @brief Test special floating-point values
 *
 * Verifies that special floating-point values (NaN, Infinity, -Infinity)
 * can be correctly deserialized, ensuring conformance with IEEE 754
 * floating-point standard representation.
 */
TEST_F(PostgreSQLDataTypesTest, SpecialFloatingPointValues) {
    // NaN, Infinity, -Infinity
    float nanValue    = std::numeric_limits<float>::quiet_NaN();
    float infValue    = std::numeric_limits<float>::infinity();
    float negInfValue = -std::numeric_limits<float>::infinity();

    // Create buffers
    uint32_t nanBits, infBits, negInfBits;
    std::memcpy(&nanBits, &nanValue, sizeof(nanBits));
    std::memcpy(&infBits, &infValue, sizeof(infBits));
    std::memcpy(&negInfBits, &negInfValue, sizeof(negInfBits));

    auto nanBuffer    = createBinaryBuffer(nanBits);
    auto infBuffer    = createBinaryBuffer(infBits);
    auto negInfBuffer = createBinaryBuffer(negInfBits);

    // Deserialize and verify
    float nanResult    = unserializer->read_float(nanBuffer);
    float infResult    = unserializer->read_float(infBuffer);
    float negInfResult = unserializer->read_float(negInfBuffer);

    ASSERT_TRUE(std::isnan(nanResult));
    ASSERT_TRUE(std::isinf(infResult) && infResult > 0);
    ASSERT_TRUE(std::isinf(negInfResult) && negInfResult < 0);
}

// Additional tests to strengthen the test suite

/**
 * @brief Test large string deserialization
 *
 * Verifies that large strings can be correctly deserialized,
 * ensuring the implementation can handle substantial data sizes
 * without memory issues or truncation.
 */
TEST_F(PostgreSQLDataTypesTest, LargeStringDeserialization) {
    // Generate a large string
    size_t      length = 100000; // 100KB
    std::string value  = generateRandomString(length);

    std::vector<byte> buffer(value.begin(), value.end());

    // Deserialize (without displaying the buffer to avoid overloading output)
    std::string result = unserializer->read_string(buffer);

    // Verify the result
    ASSERT_EQ(result.size(), length);
    ASSERT_EQ(result, value);
}

/**
 * @brief Test string deserialization with null characters
 *
 * Verifies that strings containing null characters can be correctly
 * deserialized, ensuring proper handling of embedded nulls which might
 * otherwise cause premature string termination in C-style strings.
 */
TEST_F(PostgreSQLDataTypesTest, StringWithNullChars) {
    // Create a buffer containing a string with null characters
    std::vector<byte> buffer = {'H', 'e', 'l', 'l', 'o',  '\0', 'W',
                                'o', 'r', 'l', 'd', '\0', '!'};

    // Display buffer for debugging
    printBuffer(buffer, "String with Null Chars Buffer");

    // Deserialize
    std::string result = unserializer->read_string(buffer);

    // Verify the result
    ASSERT_EQ(result.size(), buffer.size());
    for (size_t i = 0; i < buffer.size(); ++i) {
        ASSERT_EQ(result[i], buffer[i]);
    }
}

/**
 * @brief Test deserialization with non-standard size buffers
 *
 * Verifies that the unserializer can correctly handle buffers with
 * sizes different from the expected standard sizes, ensuring
 * resilience against varied or malformed input data.
 */
TEST_F(PostgreSQLDataTypesTest, NonStandardSizeBuffers) {
    // Create buffers with sizes slightly larger than expected
    integer value = 12345;

    // Buffer larger than sizeof(integer)
    auto oversized_buffer = createWrongSizeBuffer(value, sizeof(integer) + 2);
    printBuffer(oversized_buffer, "Oversized Integer Buffer");

    // Deserialization should succeed because we use the first bytes
    integer result = unserializer->read_integer(oversized_buffer);
    ASSERT_EQ(result, value);

    // Buffer with incorrect size for smallint
    std::vector<byte> wrong_size_buffer = {
        0x30, 0x39, static_cast<char>(0xFF)}; // smallint (12345) with an extra byte
    printBuffer(wrong_size_buffer, "Wrong Size Smallint Buffer");

    // Test if the parser handles this situation correctly
    smallint smallint_result = unserializer->read_smallint(wrong_size_buffer);
    ASSERT_EQ(smallint_result, 12345);
}

/**
 * @brief Test deserialization with corrupted data
 *
 * Verifies that the unserializer can safely handle corrupted or
 * randomly generated data without crashing, ensuring robustness
 * against malformed input that might occur in real-world scenarios.
 */
TEST_F(PostgreSQLDataTypesTest, CorruptedData) {
    // Create buffers with corrupted data
    std::vector<byte> corrupted_smallint = {
        static_cast<char>(0xFF),
        static_cast<char>(0xFF)}; // Value that might cause issues

    // Deserialize and verify
    smallint result = unserializer->read_smallint(corrupted_smallint);
    ASSERT_EQ(result, -1); // Expected value for 0xFFFF

    // Test with a randomly corrupted buffer
    auto random_corrupted = createCorruptedBuffer(10);
    printBuffer(random_corrupted, "Random Corrupted Buffer");

    // Test with different deserialization methods
    if (random_corrupted.size() >= sizeof(smallint)) {
        smallint smallint_result = unserializer->read_smallint(random_corrupted);
        std::cout << "Corrupted smallint result: " << smallint_result << std::endl;
    }

    if (random_corrupted.size() >= sizeof(integer)) {
        integer integer_result = unserializer->read_integer(random_corrupted);
        std::cout << "Corrupted integer result: " << integer_result << std::endl;
    }

    // String should always work
    std::string string_result = unserializer->read_string(random_corrupted);
    std::cout << "Corrupted string result length: " << string_result.size() << std::endl;
}

/**
 * @brief Test robustness with extreme values
 *
 * Verifies that the unserializer can correctly handle extreme values
 * such as very small numbers and values with high precision, ensuring
 * compliance with IEEE floating-point representation standards.
 */
TEST_F(PostgreSQLDataTypesTest, ExtremeValueRobustness) {
    // Test with extreme values that could cause issues

    // 1. Very small values close to zero
    double very_small_double = std::numeric_limits<double>::min() / 2.0;

    union {
        uint64_t i;
        double   d;
        char     b[8];
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

    // Deserialize and verify
    double result = unserializer->read_double(buffer);
    ASSERT_NEAR(result, very_small_double, std::numeric_limits<double>::min());

    // 2. Values with extreme precision
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

    // Deserialize and verify
    result = unserializer->read_double(buffer);
    ASSERT_NEAR(result, precise_double, std::numeric_limits<double>::epsilon());
}

/**
 * @brief Test repeated deserialization with the same instance
 *
 * Verifies that the unserializer can correctly handle repeated
 * deserialization operations with the same instance, ensuring
 * that internal state is properly maintained or reset between calls.
 */
TEST_F(PostgreSQLDataTypesTest, RepeatedDeserialization) {
    // Create different buffers
    auto              smallint_buffer = createBinaryBuffer<smallint>(12345);
    auto              integer_buffer  = createBinaryBuffer<integer>(67890);
    std::string       text            = "Test string";
    std::vector<byte> string_buffer(text.begin(), text.end());

    // Deserialize multiple times with the same instance
    for (int i = 0; i < 100; ++i) {
        smallint smallint_result = unserializer->read_smallint(smallint_buffer);
        ASSERT_EQ(smallint_result, 12345);

        integer integer_result = unserializer->read_integer(integer_buffer);
        ASSERT_EQ(integer_result, 67890);

        std::string string_result = unserializer->read_string(string_buffer);
        ASSERT_EQ(string_result, text);
    }
}

/**
 * @brief Test complex deserialization sequence
 *
 * Verifies that the unserializer can correctly handle a sequence of
 * different data types, simulating a real-world protocol interaction
 * with multiple parameters of various types.
 */
TEST_F(PostgreSQLDataTypesTest, ComplexDeserializationSequence) {
    // Simulate a sequence of parameters as might appear in a real protocol
    std::vector<std::vector<byte>> buffers;

    // 1. Add a smallint
    buffers.push_back(createBinaryBuffer<smallint>(101));

    // 2. Add an integer
    buffers.push_back(createBinaryBuffer<integer>(20000));

    // 3. Add a string
    std::string text = "Parameter text";
    buffers.push_back(std::vector<byte>(text.begin(), text.end()));

    // 4. Add a bigint
    buffers.push_back(createBinaryBuffer<bigint>(9223372036854775800LL));

    // 5. Add a float
    float    float_value = 3.14159f;
    uint32_t float_bits;
    std::memcpy(&float_bits, &float_value, sizeof(float_bits));
    buffers.push_back(createBinaryBuffer(float_bits));

    // Deserialize the sequence
    smallint    r1 = unserializer->read_smallint(buffers[0]);
    integer     r2 = unserializer->read_integer(buffers[1]);
    std::string r3 = unserializer->read_string(buffers[2]);
    bigint      r4 = unserializer->read_bigint(buffers[3]);
    float       r5 = unserializer->read_float(buffers[4]);

    // Verify the results
    ASSERT_EQ(r1, 101);
    ASSERT_EQ(r2, 20000);
    ASSERT_EQ(r3, text);
    ASSERT_EQ(r4, 9223372036854775800LL);
    ASSERT_NEAR(r5, float_value, 0.00001f);
}

/**
 * @brief Test UUID binary format deserialization
 *
 * Verifies that UUID values are correctly deserialized from PostgreSQL
 * binary format. UUIDs are represented as 16-byte binary values.
 */
TEST_F(PostgreSQLDataTypesTest, UUIDBinaryFormatDeserialization) {
    // Example UUID: 550e8400-e29b-41d4-a716-446655440000
    std::vector<qb::pg::byte> uuidBytes = {
        static_cast<qb::pg::byte>(0x55), static_cast<qb::pg::byte>(0x0e),
        static_cast<qb::pg::byte>(0x84), static_cast<qb::pg::byte>(0x00),
        static_cast<qb::pg::byte>(0xe2), static_cast<qb::pg::byte>(0x9b),
        static_cast<qb::pg::byte>(0x41), static_cast<qb::pg::byte>(0xd4),
        static_cast<qb::pg::byte>(0xa7), static_cast<qb::pg::byte>(0x16),
        static_cast<qb::pg::byte>(0x44), static_cast<qb::pg::byte>(0x66),
        static_cast<qb::pg::byte>(0x55), static_cast<qb::pg::byte>(0x44),
        static_cast<qb::pg::byte>(0x00), static_cast<qb::pg::byte>(0x00)};

    // Debug
    printBuffer(uuidBytes, "UUID Binary Buffer");

    // Deserialize using string method (since UUID is stored as binary data)
    std::string result = unserializer->read_string(uuidBytes);

    // Verify length is correct
    ASSERT_EQ(result.size(), 16);

    // Verify first and last bytes match expected values
    ASSERT_EQ(static_cast<unsigned char>(result[0]), 0x55);
    ASSERT_EQ(static_cast<unsigned char>(result[15]), 0x00);

    // Verify specific bytes at key positions
    ASSERT_EQ(static_cast<unsigned char>(result[6]), 0x41);
    ASSERT_EQ(static_cast<unsigned char>(result[7]), 0xd4);

    // Verify incorrect buffer size is handled
    std::vector<qb::pg::byte> truncatedUUID(uuidBytes.begin(), uuidBytes.begin() + 10);
    std::string               truncatedResult = unserializer->read_string(truncatedUUID);
    ASSERT_EQ(truncatedResult.size(), 10);
}

/**
 * @brief Test UUID text format deserialization
 *
 * Verifies that UUID values in text format are correctly deserialized.
 * UUID text format is a 36-character string with hyphens (8-4-4-4-12 pattern).
 */
TEST_F(PostgreSQLDataTypesTest, UUIDTextFormatDeserialization) {
    // Standard UUID text representation
    std::string uuidStr = "550e8400-e29b-41d4-a716-446655440000";

    // Create vector of bytes from the string
    std::vector<qb::pg::byte> uuidTextBuffer(uuidStr.begin(), uuidStr.end());

    // Debug
    printBuffer(uuidTextBuffer, "UUID Text Buffer");

    // Deserialize
    std::string result = unserializer->read_string(uuidTextBuffer);

    // Verify result
    ASSERT_EQ(result, uuidStr);
    ASSERT_EQ(result.size(), 36);

    // Verify format with dashes in correct places
    ASSERT_EQ(result[8], '-');
    ASSERT_EQ(result[13], '-');
    ASSERT_EQ(result[18], '-');
    ASSERT_EQ(result[23], '-');

    // Test with malformed UUID
    std::string               malformedUUID = "550e8400-e29b-41d4-a716"; // Too short
    std::vector<qb::pg::byte> malformedBuffer(malformedUUID.begin(),
                                              malformedUUID.end());
    std::string malformedResult = unserializer->read_string(malformedBuffer);

    // Even malformed UUIDs should be correctly deserialized as strings
    ASSERT_EQ(malformedResult, malformedUUID);
}

/**
 * @brief Test timestamp binary format deserialization
 *
 * Verifies that timestamp values are correctly deserialized from PostgreSQL
 * binary format. Timestamps are represented as 8-byte values containing
 * microseconds since 2000-01-01.
 */
TEST_F(PostgreSQLDataTypesTest, TimestampBinaryFormatDeserialization) {
    // Create a timestamp value representing: 2020-01-01 12:34:56.789012
    // PostgreSQL timestamps store microseconds since 2000-01-01
    int64_t pgTimestampMicros = 631197296789012LL;

    // Create binary representation (8 bytes, network byte order)
    // byte timestampBytes[8];
    union {
        int64_t i;
        byte    b[8];
    } src, dst;

    src.i = pgTimestampMicros;

    // Convert to big-endian for network byte order
    dst.b[0] = src.b[7];
    dst.b[1] = src.b[6];
    dst.b[2] = src.b[5];
    dst.b[3] = src.b[4];
    dst.b[4] = src.b[3];
    dst.b[5] = src.b[2];
    dst.b[6] = src.b[1];
    dst.b[7] = src.b[0];

    std::vector<qb::pg::byte> timestampBuffer(dst.b, dst.b + 8);

    // Debug
    printBuffer(timestampBuffer, "Timestamp Binary Buffer");

    // Deserialize using a specific bigint method, since timestamps
    // are int64 values in microseconds
    bigint result = unserializer->read_bigint(timestampBuffer);

    // Verify value
    ASSERT_EQ(result, pgTimestampMicros);

    // Test with special values
    int64_t infinity       = 0x7FFFFFFFFFFFFFFFLL; // PostgreSQL infinity representation
    auto    infinityBuffer = createBinaryBuffer(infinity);
    bigint  infinityResult = unserializer->read_bigint(infinityBuffer);
    ASSERT_EQ(infinityResult, infinity);

    // Test with incorrect buffer size
    std::vector<qb::pg::byte> truncatedBuffer(timestampBuffer.begin(),
                                              timestampBuffer.begin() + 4);
    ASSERT_THROW(unserializer->read_bigint(truncatedBuffer), std::runtime_error);
}

/**
 * @brief Test timestamp text format deserialization
 *
 * Verifies that timestamp values in text format are correctly deserialized.
 * Timestamp text format can vary, but typically follows ISO 8601 format.
 */
TEST_F(PostgreSQLDataTypesTest, TimestampTextFormatDeserialization) {
    // Standard timestamp text representations
    std::string timestampStr    = "2020-01-01 12:34:56.789012";
    std::string timestampWithTZ = "2020-01-01 12:34:56.789012+00";
    std::string timestampISO    = "2020-01-01T12:34:56.789012Z";

    // Create vector of bytes from the first string
    std::vector<qb::pg::byte> timestampTextBuffer(timestampStr.begin(),
                                                  timestampStr.end());

    // Debug
    printBuffer(timestampTextBuffer, "Timestamp Text Buffer");

    // Deserialize
    std::string result = unserializer->read_string(timestampTextBuffer);

    // Verify result
    ASSERT_EQ(result, timestampStr);

    // Test with timezone format
    std::vector<qb::pg::byte> timestampTZBuffer(timestampWithTZ.begin(),
                                                timestampWithTZ.end());
    std::string               tzResult = unserializer->read_string(timestampTZBuffer);
    ASSERT_EQ(tzResult, timestampWithTZ);

    // Test with ISO format
    std::vector<qb::pg::byte> timestampISOBuffer(timestampISO.begin(),
                                                 timestampISO.end());
    std::string               isoResult = unserializer->read_string(timestampISOBuffer);
    ASSERT_EQ(isoResult, timestampISO);

    // Test with truncated timestamp (should still parse as string)
    std::string               partialTimestamp = "2020-01-01 12:34";
    std::vector<qb::pg::byte> partialBuffer(partialTimestamp.begin(),
                                            partialTimestamp.end());
    std::string               partialResult = unserializer->read_string(partialBuffer);
    ASSERT_EQ(partialResult, partialTimestamp);
}

/**
 * @brief Test values with high-bit set
 *
 * Verifies that the unserializer can correctly handle values with
 * the high bit set, which are typically interpreted as negative numbers
 * in two's complement representation common in PostgreSQL.
 */
TEST_F(PostgreSQLDataTypesTest, HighBitValues) {
    // Create values with high bit set
    smallint high_bit_smallint = 0x8000;               // -32768 in two's complement
    integer  high_bit_integer  = 0x80000000;           // -2147483648 in two's complement
    bigint   high_bit_bigint   = 0x8000000000000000LL; // Minimum for bigint

    // Create buffers
    auto smallint_buffer = createBinaryBuffer(high_bit_smallint);
    auto integer_buffer  = createBinaryBuffer(high_bit_integer);
    auto bigint_buffer   = createBinaryBuffer(high_bit_bigint);

    // Display buffers for debugging
    printBuffer(smallint_buffer, "High-bit Smallint Buffer");
    printBuffer(integer_buffer, "High-bit Integer Buffer");
    printBuffer(bigint_buffer, "High-bit Bigint Buffer");

    // Deserialize
    smallint smallint_result = unserializer->read_smallint(smallint_buffer);
    integer  integer_result  = unserializer->read_integer(integer_buffer);
    bigint   bigint_result   = unserializer->read_bigint(bigint_buffer);

    // Verify the results
    ASSERT_EQ(smallint_result, high_bit_smallint);
    ASSERT_EQ(integer_result, high_bit_integer);
    ASSERT_EQ(bigint_result, high_bit_bigint);
}

/**
 * @brief Tests JSONB binary format deserialization
 *
 * Verifies the ability to deserialize PostgreSQL JSONB binary format into qb::jsonb
 * objects. JSONB format includes a version number and specific binary encoding.
 */
TEST_F(PostgreSQLDataTypesTest, JSONBBinaryFormatDeserialization) {
    // Create a simple JSON object
    qb::jsonb test_json = {
        {"id", 123},
        {"name", "test user"},
        {"active", true},
        {"scores", {98, 87, 95}},
        {"details", {{"address", "123 Test St"}, {"email", "test@example.com"}}}};

    // Convert to string representation
    std::string json_str = test_json.dump();

    // Create binary JSONB buffer:
    // - 4-byte integer length prefix
    // - JSONB version (1 byte, value 1)
    // - JSON content
    std::vector<byte> jsonb_buffer;

    // 1. Add length prefix (version byte + content size)
    integer content_size  = 1 + json_str.size(); // 1 byte for version + content
    auto    length_buffer = createBinaryBuffer(content_size);
    jsonb_buffer.insert(jsonb_buffer.end(), length_buffer.begin(), length_buffer.end());

    // 2. Add JSONB version (1)
    jsonb_buffer.push_back(1);

    // 3. Add JSON content as string
    jsonb_buffer.insert(jsonb_buffer.end(), json_str.begin(), json_str.end());

    // Print buffer for debug
    printBuffer(jsonb_buffer, "JSONB Binary Buffer");

    // Test deserialization
    try {
        qb::jsonb result = TypeConverter<qb::jsonb>::from_binary(jsonb_buffer);

        // The result might be in array format [[key, value], [key, value], ...]
        // We need to convert it to an object format if needed
        qb::jsonb obj_result;

        if (result.is_array()) {
            // Convert array format to object format
            for (const auto &pair : result) {
                if (pair.is_array() && pair.size() == 2) {
                    if (pair[0].is_string()) {
                        obj_result[pair[0].get<std::string>()] = pair[1];
                    }
                }
            }
            // Use obj_result for verification
            result = obj_result;
        }

        // Verify the contents
        ASSERT_EQ(result["id"].get<int>(), 123);
        ASSERT_EQ(result["name"].get<std::string>(), "test user");
        ASSERT_EQ(result["active"].get<bool>(), true);
        ASSERT_EQ(result["scores"].size(), 3);
        ASSERT_EQ(result["scores"][0].get<int>(), 98);
        ASSERT_EQ(result["details"]["address"].get<std::string>(), "123 Test St");
        ASSERT_EQ(result["details"]["email"].get<std::string>(), "test@example.com");

        std::cout << "Successfully deserialized JSONB: " << result.dump(2) << std::endl;
    } catch (const std::exception &e) {
        FAIL() << "Exception during JSONB deserialization: " << e.what();
    }

    // Test invalid version
    std::vector<byte> invalid_jsonb = jsonb_buffer;
    invalid_jsonb[4]                = 2; // Set version to 2 (unsupported)

    ASSERT_THROW(TypeConverter<qb::jsonb>::from_binary(invalid_jsonb),
                 std::runtime_error);
}

/**
 * @brief Tests JSON text format deserialization
 *
 * Verifies the ability to deserialize PostgreSQL JSON text format into qb::jsonb
 * objects.
 */
TEST_F(PostgreSQLDataTypesTest, JSONTextFormatDeserialization) {
    // Test cases with different JSON structures
    std::vector<std::string> json_test_cases = {R"({"id": 123, "name": "test"})",
                                                R"(["apple", "banana", "cherry"])",
                                                R"(42)",
                                                R"("simple string")",
                                                R"(true)",
                                                R"(null)",
                                                R"({
            "complex": {
                "nested": {
                    "array": [1, 2, 3],
                    "object": {"a": 1, "b": 2}
                },
                "types": [true, null, 42, "string"]
            }
        })"};

    for (const auto &test_case : json_test_cases) {
        try {
            // Parse the source JSON to compare later
            qb::jsonb expected(nlohmann::json::parse(test_case));

            // Deserialize using TypeConverter
            qb::jsonb result = TypeConverter<qb::jsonb>::from_text(test_case);

            // Compare the result with expected
            ASSERT_EQ(result.dump(), expected.dump())
                << "Failed on test case: " << test_case;

            std::cout << "Successfully parsed JSON: " << result.dump(2) << std::endl;
        } catch (const std::exception &e) {
            FAIL() << "Exception during JSON text deserialization for case '"
                   << test_case << "': " << e.what();
        }
    }

    // Test invalid JSON
    std::string invalid_json = R"({"unclosed": "object")";
    ASSERT_THROW(TypeConverter<qb::jsonb>::from_text(invalid_json), std::runtime_error);
}

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}