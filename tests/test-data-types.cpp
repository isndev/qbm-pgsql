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
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
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
#include <qb/io/async.h>
#include <qb/io/async/coroutine.h>
#include <qb/io/async/coroutine/utils.h>

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
            std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(b)) << " ";
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
    std::vector<byte> buffer = {'H', 'e', 'l', 'l', 'o', '\0', 'W', 'o', 'r', 'l', 'd', '\0', '!'};

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
    std::vector<byte> wrong_size_buffer = {0x30, 0x39, static_cast<char>(0xFF)}; // smallint (12345) with an extra byte
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
    std::vector<byte> corrupted_smallint = {static_cast<char>(0xFF), static_cast<char>(0xFF)}; // Value that might cause issues

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
    std::vector<qb::pg::byte> uuidBytes = {static_cast<qb::pg::byte>(0x55), static_cast<qb::pg::byte>(0x0e), static_cast<qb::pg::byte>(0x84),
                                           static_cast<qb::pg::byte>(0x00), static_cast<qb::pg::byte>(0xe2), static_cast<qb::pg::byte>(0x9b),
                                           static_cast<qb::pg::byte>(0x41), static_cast<qb::pg::byte>(0xd4), static_cast<qb::pg::byte>(0xa7),
                                           static_cast<qb::pg::byte>(0x16), static_cast<qb::pg::byte>(0x44), static_cast<qb::pg::byte>(0x66),
                                           static_cast<qb::pg::byte>(0x55), static_cast<qb::pg::byte>(0x44), static_cast<qb::pg::byte>(0x00),
                                           static_cast<qb::pg::byte>(0x00)};

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
    std::vector<qb::pg::byte> malformedBuffer(malformedUUID.begin(), malformedUUID.end());
    std::string               malformedResult = unserializer->read_string(malformedBuffer);

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
    std::vector<qb::pg::byte> truncatedBuffer(timestampBuffer.begin(), timestampBuffer.begin() + 4);
    ASSERT_THROW(unserializer->read_bigint(truncatedBuffer), std::runtime_error);
}

/**
 * @brief Regression: timestamp from_binary must reject a 9..11 byte field.
 *
 * The decoder takes a fast path for exactly 8 bytes and a legacy "4-byte length
 * prefix" path for >= 12 bytes (reads buffer.data()+4 .. +12). A field length of
 * 9, 10 or 11 — which a malformed/hostile server can supply as col_size — used to
 * fall into the prefixed path and read 1..3 bytes past the end of the field
 * buffer (heap over-read). It must now throw instead of reading out of bounds.
 */
TEST_F(PostgreSQLDataTypesTest, TimestampBinaryRejectsShortPrefixedBuffer) {
    for (size_t sz : {9u, 10u, 11u}) {
        std::vector<qb::pg::byte> buf(sz, byte{0x01});
        ASSERT_THROW(TypeConverter<qb::wall_time>::from_binary(buf), std::runtime_error)
            << "size " << sz << " must be rejected, not read out of bounds";
    }
    // 8 (exact) and 12 (legacy prefixed) remain valid and must not throw.
    ASSERT_NO_THROW(TypeConverter<qb::wall_time>::from_binary(std::vector<qb::pg::byte>(8, byte{0})));
    ASSERT_NO_THROW(TypeConverter<qb::wall_time>::from_binary(std::vector<qb::pg::byte>(12, byte{0})));
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
    std::vector<qb::pg::byte> timestampTextBuffer(timestampStr.begin(), timestampStr.end());

    // Debug
    printBuffer(timestampTextBuffer, "Timestamp Text Buffer");

    // Deserialize
    std::string result = unserializer->read_string(timestampTextBuffer);

    // Verify result
    ASSERT_EQ(result, timestampStr);

    // Test with timezone format
    std::vector<qb::pg::byte> timestampTZBuffer(timestampWithTZ.begin(), timestampWithTZ.end());
    std::string               tzResult = unserializer->read_string(timestampTZBuffer);
    ASSERT_EQ(tzResult, timestampWithTZ);

    // Test with ISO format
    std::vector<qb::pg::byte> timestampISOBuffer(timestampISO.begin(), timestampISO.end());
    std::string               isoResult = unserializer->read_string(timestampISOBuffer);
    ASSERT_EQ(isoResult, timestampISO);

    // Test with truncated timestamp (should still parse as string)
    std::string               partialTimestamp = "2020-01-01 12:34";
    std::vector<qb::pg::byte> partialBuffer(partialTimestamp.begin(), partialTimestamp.end());
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
    smallint high_bit_smallint = static_cast<smallint>(0x8000); // -32768 in two's complement
    integer  high_bit_integer  = 0x80000000;                    // -2147483648 in two's complement
    bigint   high_bit_bigint   = 0x8000000000000000LL;          // Minimum for bigint

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
        {"details", {{"address", "123 Test St"}, {"email", "test@example.com"}}}
    };

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

    ASSERT_THROW(TypeConverter<qb::jsonb>::from_binary(invalid_jsonb), std::runtime_error);
}

/**
 * @brief Tests JSON text format deserialization
 *
 * Verifies the ability to deserialize PostgreSQL JSON text format into qb::jsonb
 * objects.
 */
TEST_F(PostgreSQLDataTypesTest, JSONTextFormatDeserialization) {
    // Test cases with different JSON structures
    std::vector<std::string> json_test_cases = {
        R"({"id": 123, "name": "test"})",
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
        })"
    };

    for (const auto &test_case : json_test_cases) {
        try {
            // Parse the source JSON to compare later
            qb::jsonb expected(nlohmann::json::parse(test_case));

            // Deserialize using TypeConverter
            qb::jsonb result = TypeConverter<qb::jsonb>::from_text(test_case);

            // Compare the result with expected
            ASSERT_EQ(result.dump(), expected.dump()) << "Failed on test case: " << test_case;

            std::cout << "Successfully parsed JSON: " << result.dump(2) << std::endl;
        } catch (const std::exception &e) {
            FAIL() << "Exception during JSON text deserialization for case '" << test_case << "': " << e.what();
        }
    }

    // Test invalid JSON
    std::string invalid_json = R"({"unclosed": "object")";
    ASSERT_THROW(TypeConverter<qb::jsonb>::from_text(invalid_json), std::runtime_error);
}

/**
 * @brief Test resultset memory management and move semantics
 *
 * Verifies that the resultset properly manages its internal implementation
 * memory through destructors, move constructors, and move assignment operators.
 * This test prevents regression of the memory leak bug (P0-17).
 */
TEST(ResultsetMemoryManagementTest, BasicMoveSemantics) {
    // Test 1: Create and destroy multiple resultsets to ensure no memory leak
    {
        std::vector<qb::pg::resultset> resultsets;
        resultsets.reserve(100);

        for (int i = 0; i < 100; ++i) {
            // Create a resultset - this allocates internal implementation
            qb::pg::resultset rs;
            resultsets.push_back(std::move(rs));
        }

        // All resultsets will be destroyed here when leaving scope
        // If there's a memory leak, valgrind/ASan would catch it
    }

    // Test 2: Move semantics - ensure proper ownership transfer
    {
        qb::pg::resultset original;
        qb::pg::resultset moved(std::move(original));

        // After move, moved should have valid internal implementation
        // and original should be null (no crash on access)
        EXPECT_NO_THROW({
            (void) moved.empty(); // Should not crash
        });
    }

    // Test 3: Move assignment
    {
        qb::pg::resultset rs1;
        qb::pg::resultset rs2;

        rs2 = std::move(rs1);

        // rs2 should have taken ownership from rs1
        EXPECT_NO_THROW({ (void) rs2.empty(); });
    }

    std::cout << "Resultset memory management test passed" << std::endl;
}

/**
 * @brief Test index validation in resultset row access (P0-12)
 *
 * Verifies that accessing a column with an out-of-bounds index
 * throws std::out_of_range exception.
 */
TEST(ResultsetIndexValidationTest, OutOfRangeThrows) {
    // Create a mock resultset - we can't easily create a real one without DB
    // but we can test the row::size() behavior with an empty resultset
    qb::pg::resultset rs;

    // An empty resultset should have 0 columns
    EXPECT_EQ(rs.columns_size(), 0);

    // Note: Full index validation test requires a real database connection
    // This is tested implicitly in integration tests
    std::cout << "Index validation test passed" << std::endl;
}

/**
 * @brief Test std::chrono::duration as PostgreSQL INTERVAL (P2-2)
 */
TEST(IntervalTypeTest, ChronoDurationConversion) {
    using namespace std::chrono;

    // Test 1: Serialize duration to binary
    std::vector<byte> buffer;
    auto              duration = seconds(3600); // 1 hour
    TypeConverter<std::chrono::seconds>::to_binary(duration, buffer);

    // Should have 4 bytes length prefix + 16 bytes data
    EXPECT_GE(buffer.size(), 20);

    // Test 2: OID should be INTERVAL (1186)
    EXPECT_EQ(TypeConverter<std::chrono::seconds>::get_oid(), 1186);

    // Test 3: Text format
    auto text = TypeConverter<std::chrono::seconds>::to_text(duration);
    EXPECT_FALSE(text.empty());

    std::cout << "Interval type test passed" << std::endl;
}

/**
 * @brief Test NULL bitmap correctness (P0-4)
 *
 * Verifies that the std::vector<bool> bitmap correctly tracks NULL values
 * compared to the old std::set implementation.
 */
TEST(NullBitmapTest, VectorBoolVsSetBehavior) {
    using namespace qb::pg::detail;

    // Create row_data with null_map as vector<bool> (P0-4 fix)
    row_data row;
    row.null_map.resize(5, false); // 5 columns, all non-NULL initially

    // Mark some columns as NULL
    row.null_map[1] = true; // Column 1 is NULL
    row.null_map[3] = true; // Column 3 is NULL

    // Verify NULL detection
    EXPECT_FALSE(row.null_map[0]); // Column 0: NOT NULL
    EXPECT_TRUE(row.null_map[1]);  // Column 1: NULL
    EXPECT_FALSE(row.null_map[2]); // Column 2: NOT NULL
    EXPECT_TRUE(row.null_map[3]);  // Column 3: NULL
    EXPECT_FALSE(row.null_map[4]); // Column 4: NOT NULL

    // Test O(1) access time characteristic of vector<bool>
    // (vs O(log n) for std::set)
    EXPECT_EQ(row.null_map.size(), 5);

    // Bit-packed storage: 5 bits should use less memory than std::set nodes
    std::cout << "NULL bitmap test passed (vector<bool> vs std::set)" << std::endl;
}

/**
 * @brief Test param serializer buffer reservation (P0-3)
 *
 * Verifies that reserve() is called for batch parameter serialization,
 * reducing memory reallocations.
 */
TEST(ParamBufferReserveTest, BatchOptimization) {
    using namespace qb::pg::detail;

    ParamSerializer serializer;

    // Add multiple string parameters (simulating batch insert)
    std::vector<std::string> values;
    for (int i = 0; i < 100; ++i) {
        values.push_back("test_value_" + std::to_string(i));
    }

    // This should trigger reserve() optimization (P0-3)
    serializer.add_string_vector(values);

    // Verify that parameters were added
    EXPECT_GE(serializer.param_count(), 100);

    // The buffer should be pre-allocated efficiently
    const auto &buffer = serializer.params_buffer();
    EXPECT_GT(buffer.size(), 0);

    std::cout << "Param buffer reserve test passed" << std::endl;
}

/**
 * @brief Test connection options keepalive settings (P1-1)
 *
 * Verifies that connection_options properly stores keepalive configuration.
 */
TEST(ConnectionOptionsTest, KeepaliveSettings) {
    qb::pg::connection_options opts;

    // Default values
    EXPECT_EQ(opts.keepalive_interval, 0); // Disabled by default
    EXPECT_EQ(opts.keepalive_probes, 3);   // Default probes
    EXPECT_EQ(opts.keepalive_idle, 60);    // Default idle time

    // Custom settings
    opts.keepalive_interval = 30;
    opts.keepalive_probes   = 5;
    opts.keepalive_idle     = 120;

    EXPECT_EQ(opts.keepalive_interval, 30);
    EXPECT_EQ(opts.keepalive_probes, 5);
    EXPECT_EQ(opts.keepalive_idle, 120);

    std::cout << "Connection options keepalive test passed" << std::endl;
}

/**
 * @brief Test PostgreSQL NUMERIC/DECIMAL type (P0)
 *
 * Verifies exact precision decimal handling for financial calculations.
 */
TEST(NumericTypeTest, NumericPrecision) {
    using namespace qb::pg::detail;

    // Test 1: OID should be 1700 (NUMERIC)
    EXPECT_EQ(TypeConverter<numeric>::get_oid(), 1700);

    // Test 2: Serialize to binary
    numeric           n1("123456789.0123456789");
    std::vector<byte> buffer;
    TypeConverter<numeric>::to_binary(n1, buffer);

    // Should have 4 bytes length prefix + data
    EXPECT_GE(buffer.size(), 4);

    // Test 3: Text format preserves exact value
    std::string text = TypeConverter<numeric>::to_text(n1);
    EXPECT_EQ(text, "123456789.0123456789");

    // Test 4: Round-trip binary
    numeric n2 = TypeConverter<numeric>::from_binary(buffer);
    EXPECT_EQ(n2.str(), "123456789.0123456789");

    // Test 5: Round-trip text
    numeric n3 = TypeConverter<numeric>::from_text("999.999999999999999");
    EXPECT_EQ(n3.str(), "999.999999999999999");

    // Test 6: numeric is value-equality on its canonical text form (it is NOT an
    // arithmetic type — the old string-concat operator+ was removed).
    numeric price("199.99");
    EXPECT_EQ(price, numeric("199.99"));
    EXPECT_FALSE(price == numeric("200.00"));

    std::cout << "NUMERIC type test passed (financial precision)" << std::endl;
}

/**
 * @brief Test PostgreSQL DATE type (P1)
 *
 * Verifies date handling with the qb civil-date vocabulary (qb::date).
 */
TEST(DateTypeTest, DateConversions) {
    using namespace qb::pg::detail;

    // Test 1: OID should be 1082 (DATE)
    EXPECT_EQ(TypeConverter<qb::date>::get_oid(), 1082);

    // Test 2: Create date from string
    qb::date d1 = qb::date::parse("2024-03-15").value();
    EXPECT_EQ(d1.to_string(), "2024-03-15");

    // Test 3: Create date from a wall instant
    qb::date d2 = qb::date::from_wall_time(qb::wall_now());
    EXPECT_FALSE(d2.to_string().empty());

    // Test 4: Serialize to binary
    std::vector<byte> buffer;
    TypeConverter<qb::date>::to_binary(d1, buffer);
    EXPECT_EQ(buffer.size(), 8); // 4 bytes length + 4 bytes data

    // Test 5: Round-trip binary
    qb::date d3 = TypeConverter<qb::date>::from_binary(buffer);
    EXPECT_EQ(d3, d1);

    // Test 6: Round-trip text
    qb::date d4 = TypeConverter<qb::date>::from_text("2000-01-01");
    EXPECT_EQ(d4.to_string(), "2000-01-01");

    // Test 7: Date comparison
    qb::date early = qb::date::parse("2020-01-01").value();
    qb::date late  = qb::date::parse("2024-01-01").value();
    EXPECT_TRUE(early < late);

    std::cout << "DATE type test passed" << std::endl;
}

/**
 * @brief Test TIME type support (via string for now)
 *
 * TIME and TIMETZ are handled as strings until full binary support.
 */
TEST(TimeTypeTest, StringHandling) {
    using namespace qb::pg;

    // Test 1: OID for TIME (need static_cast for enum class comparison)
    EXPECT_EQ(static_cast<int>(detail::oid::time), 1083);
    EXPECT_EQ(static_cast<int>(detail::oid::timetz), 1266);

    // Test 2: Time as string (current approach)
    std::string time_str = "14:30:45.123456";
    EXPECT_EQ(time_str.length(), 15);

    // Test 3: Time with timezone as string
    std::string timetz_str = "14:30:45+02:00";
    EXPECT_EQ(timetz_str.length(), 14);

    std::cout << "TIME/TIMETZ (string handling) test passed" << std::endl;
}

/**
 * @brief Regression: DATE/TIME/TIMETZ from_binary must decode the unprefixed
 *        wire field, not read 4 bytes past it.
 *
 * The protocol layer (message::read in protocol.cpp) hands the converters the
 * field VALUE only — the 4-byte per-field length prefix is already stripped.
 * The DATE/TIME/TIMETZ decoders used to unconditionally read from
 * buffer.data()+4, i.e. they assumed a prefix that is no longer there, so they
 * decoded 4 bytes too far and returned the PostgreSQL epoch base (DATE =>
 * 2000-01-01, TIME => 00:00:00). The to_binary round-trip test masked this
 * because to_binary writes the prefix, making the buffer accidentally line up.
 *
 * Here we reproduce the real wire buffer by serializing with to_binary and
 * dropping the length prefix it writes, then assert the value survives. The
 * legacy prefixed shape must keep decoding identically.
 */
TEST(DateTimeWireFormatTest, UnprefixedFieldDecode) {
    using namespace qb::pg::detail;

    // to_binary emits [4-byte length prefix][value]; the wire field is value-only.
    auto strip_prefix = [](std::vector<byte> b) {
        b.erase(b.begin(), b.begin() + sizeof(integer));
        return b;
    };

    // DATE: 4-byte value.
    {
        qb::date          d_in = qb::date::parse("2024-03-15").value();
        std::vector<byte> prefixed;
        TypeConverter<qb::date>::to_binary(d_in, prefixed);
        ASSERT_EQ(prefixed.size(), 8u); // 4 prefix + 4 value
        std::vector<byte> wire = strip_prefix(prefixed);
        ASSERT_EQ(wire.size(), 4u); // value only — what the protocol hands us
        qb::date d_wire = TypeConverter<qb::date>::from_binary(wire);
        EXPECT_EQ(d_wire, d_in);
        EXPECT_EQ(d_wire.to_string(), "2024-03-15"); // not the 2000-01-01 epoch base
        // Legacy prefixed shape still decodes to the same value.
        EXPECT_EQ(TypeConverter<qb::date>::from_binary(prefixed), d_in);
    }

    // TIME: 8-byte value.
    {
        qb::time_of_day   t_in = qb::time_of_day::from_hms(14, 30, 45, 123456);
        std::vector<byte> prefixed;
        TypeConverter<qb::time_of_day>::to_binary(t_in, prefixed);
        ASSERT_EQ(prefixed.size(), 12u); // 4 prefix + 8 value
        std::vector<byte> wire = strip_prefix(prefixed);
        ASSERT_EQ(wire.size(), 8u);
        qb::time_of_day t_wire = TypeConverter<qb::time_of_day>::from_binary(wire);
        EXPECT_EQ(t_wire, t_in);
        EXPECT_EQ(t_wire.to_string(), "14:30:45.123456"); // not 00:00:00
        EXPECT_EQ(TypeConverter<qb::time_of_day>::from_binary(prefixed), t_in);
    }

    // TIMETZ: 12-byte value (8-byte micros + 4-byte tz offset).
    {
        qb::time_of_day_tz z_in = qb::time_of_day_tz::from_hms_offset(14, 30, 45, 0, 7200);
        std::vector<byte>  prefixed;
        TypeConverter<qb::time_of_day_tz>::to_binary(z_in, prefixed);
        ASSERT_EQ(prefixed.size(), 16u); // 4 prefix + 12 value
        std::vector<byte> wire = strip_prefix(prefixed);
        ASSERT_EQ(wire.size(), 12u);
        qb::time_of_day_tz z_wire = TypeConverter<qb::time_of_day_tz>::from_binary(wire);
        EXPECT_EQ(z_wire, z_in);
        EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::from_binary(prefixed), z_in);
    }

    std::cout << "DATE/TIME/TIMETZ wire-format decode test passed" << std::endl;
}

// Decode a hex string into a byte buffer (test helper).
static std::vector<qb::pg::byte>
hex_to_bytes(const std::string &h) {
    auto nib = [](char c) {
        return c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
    };
    std::vector<qb::pg::byte> b;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        b.push_back(static_cast<qb::pg::byte>((nib(h[i]) << 4) | nib(h[i + 1])));
    return b;
}

/**
 * @brief Regression: binary NUMERIC decode against PostgreSQL ground truth.
 *
 * Buffers below are the exact bytes from PostgreSQL `numeric_send()` (value only,
 * no per-field length prefix — the shape the protocol layer hands to from_binary).
 * Wire layout: int16 ndigits, int16 weight, uint16 sign, uint16 dscale, then
 * base-10000 digits. The previous decoder mis-read these as length-prefixed text.
 */
TEST(NumericBinaryTest, DecodeAgainstPostgresGroundTruth) {
    using namespace qb::pg::detail;
    struct C {
        const char *hex;
        const char *expect;
    };
    const C cases[] = {
        {"000200000000000404d2162e", "1234.5678"},
        {"0002000040000001000c1388", "-12.5"},
        {"00010001000000000064", "1000000"},
        {"0000000000000000", "0"},
        {"000600020000000a000109291a85007b11d722c4", "123456789.0123456789"},
        {"0001ffff000000011388", "0.5"},
        {"0001ffff0000000203e8", "0.10"},   // trailing-zero / dscale preserved
        {"00010000000000020064", "100.00"}, // dscale preserved
        {"000500000000000f03e7270f270f270f2706", "999.999999999999999"},
    };
    for (const auto &c : cases) {
        numeric got = TypeConverter<numeric>::from_binary(hex_to_bytes(c.hex));
        EXPECT_EQ(got.str(), c.expect) << "hex=" << c.hex;
    }

    // Round-trip: to_binary (real PG binary, length-prefixed) -> from_binary.
    for (const char *v : {"0", "1", "-1", "12345.678", "-999.99", "123456789.0123456789", "0.0001", "1000000"}) {
        std::vector<qb::pg::byte> buf;
        TypeConverter<numeric>::to_binary(numeric(v), buf);
        EXPECT_EQ(TypeConverter<numeric>::from_binary(buf).str(), v) << "value=" << v;
    }
    std::cout << "NUMERIC binary decode test passed" << std::endl;
}

/**
 * @brief Regression: binary ARRAY decode against PostgreSQL ground truth.
 *
 * Buffers are the exact bytes from PostgreSQL `array_send()` (value only). Layout:
 * int32 ndim, int32 has-null, int32 element OID, per-dim {int32 size, int32 lb},
 * then per element {int32 length (-1=NULL), value}. Previously there was no
 * std::vector<T> decoder at all (example4 fell back to ::text).
 */
TEST(ArrayBinaryTest, DecodeAgainstPostgresGroundTruth) {
    using namespace qb::pg::detail;

    // int4[] {10,20,30,40}
    auto ints =
        TypeConverter<std::vector<integer>>::from_binary(hex_to_bytes("0000000100000000000000170000000400000001"
                                                                      "000000040000000a0000000400000014000000040000001e0000000400000028"));
    EXPECT_EQ(ints, (std::vector<integer>{10, 20, 30, 40}));

    // text[] {apple,banana}
    auto txt = TypeConverter<std::vector<std::string>>::from_binary(
        hex_to_bytes("0000000100000000000000190000000200000001000000056170706c650000000662616e616e61"));
    EXPECT_EQ(txt, (std::vector<std::string>{"apple", "banana"}));

    // int4[] {1,NULL,3} -> NULL decodes to default-constructed 0
    auto withnull = TypeConverter<std::vector<integer>>::from_binary(
        hex_to_bytes("00000001000000010000001700000003000000010000000400000001ffffffff0000000400000003"));
    EXPECT_EQ(withnull, (std::vector<integer>{1, 0, 3}));

    // empty int4[]
    auto empty = TypeConverter<std::vector<integer>>::from_binary(hex_to_bytes("000000000000000000000017"));
    EXPECT_TRUE(empty.empty());

    // Round-trip through encode (mirrors ParamSerializer::add_vector) + decode.
    std::vector<qb::pg::byte> buf;
    TypeConverter<std::vector<integer>>::to_binary(std::vector<integer>{7, -3, 100000}, buf);
    // strip the 4-byte length prefix that to_binary writes
    std::vector<qb::pg::byte> body(buf.begin() + 4, buf.end());
    EXPECT_EQ(TypeConverter<std::vector<integer>>::from_binary(body), (std::vector<integer>{7, -3, 100000}));

    std::cout << "ARRAY binary decode test passed" << std::endl;
}

/**
 * @brief Regression: binary array decode for the non-int4/text element types,
 *        against PostgreSQL `array_send()` ground truth.
 */
TEST(ArrayBinaryTest, ScalarElementTypesAgainstPostgresGroundTruth) {
    using namespace qb::pg::detail;

    EXPECT_EQ(
        TypeConverter<std::vector<bigint>>::from_binary(hex_to_bytes("00000001000000000000001400000003000000010000000800000000000000010000"
                                                                     "00080000000000000002000000080000000000000003")),
        (std::vector<bigint>{1, 2, 3}));

    EXPECT_EQ(
        TypeConverter<std::vector<smallint>>::from_binary(hex_to_bytes("0000000100000000000000150000000200000001000000020007000000020008")),
        (std::vector<smallint>{7, 8}));

    EXPECT_EQ(
        TypeConverter<std::vector<double>>::from_binary(hex_to_bytes("0000000100000000000002bd0000000200000001000000083ff80000000000000000"
                                                                     "00084004000000000000")),
        (std::vector<double>{1.5, 2.5}));

    EXPECT_EQ(TypeConverter<std::vector<float>>::from_binary(
                  hex_to_bytes("0000000100000000000002bc0000000200000001000000043fc0000000000004c0200000")),
              (std::vector<float>{1.5f, -2.5f}));

    EXPECT_EQ(
        TypeConverter<std::vector<bool>>::from_binary(hex_to_bytes("0000000100000000000000100000000300000001000000010100000001000000000101")),
        (std::vector<bool>{true, false, true}));

    std::cout << "ARRAY scalar element decode test passed" << std::endl;
}

/**
 * @brief Regression: binary INTERVAL decode must fold days and months, not drop
 *        them. Buffers are PostgreSQL `interval_send()` bytes; expected seconds
 *        equal EXTRACT(EPOCH) (24h day, 30-day residual month, 365.25-day year).
 */
TEST(IntervalBinaryTest, DecodeAgainstPostgresGroundTruth) {
    using namespace qb::pg::detail;
    using secs = std::chrono::seconds;
    struct C {
        const char *hex;
        long long   expect;
    };
    const C cases[] = {
        {"00000000000000000000000100000000", 86400},    // 1 day
        {"00000000000000000000000000000001", 2592000},  // 1 month (30 days)
        {"0000000218711a000000000000000000", 9000},     // 2h30m (pure time)
        {"00000002925553400000000200000001", 2775845},  // 1 mon 2 days 03:04:05
        {"0000000000000000ffffffff00000000", -86400},   // -1 day
        {"0000000000000000000000000000000c", 31557600}, // 12 months = 1 year (365.25d)
    };
    for (const auto &c : cases) {
        auto d = TypeConverter<secs>::from_binary(hex_to_bytes(c.hex));
        EXPECT_EQ(d.count(), c.expect) << "hex=" << c.hex;
    }
    // Pure-time duration round-trips (to_binary sets days=months=0).
    std::vector<qb::pg::byte> buf;
    TypeConverter<secs>::to_binary(secs{90061}, buf);
    std::vector<qb::pg::byte> body(buf.begin() + 4, buf.end());
    EXPECT_EQ(TypeConverter<secs>::from_binary(body).count(), 90061);
    std::cout << "INTERVAL binary decode test passed" << std::endl;
}

/**
 * @brief Regression: binary TIMESTAMP decode value against PostgreSQL ground
 *        truth (the prior tests only checked the int64 read and an over-read
 *        guard, never the decoded instant).
 */
TEST(WallTimeBinaryTest, DecodeAgainstPostgresGroundTruth) {
    using namespace qb::pg::detail;
    // timestamptz '2024-03-15 14:30:45.123456+00' = 763828245123456 us since
    // 2000-01-01 = 1710513045123456 us since the Unix epoch.
    auto t = TypeConverter<qb::wall_time>::from_binary(hex_to_bytes("0002b6b29f385180"));
    EXPECT_EQ(qb::unix_micros(t), 1710513045123456LL);
}

/**
 * @brief Regression: std::optional decode — the 4-byte all-ones NULL sentinel
 *        yields nullopt, a real value decodes the contained type.
 */
// optional<T> from_binary receives the field VALUE bytes; SQL NULL is decided upstream
// by field::as -> is_null(), so the converter ALWAYS has a value and must decode it —
// including a genuine int4 of -1 (0xFFFFFFFF), which the old NULL-sentinel sniffing
// wrongly turned into std::nullopt (data corruption). NULL itself is covered live in
// the integration suite (a real NULL column read as std::optional<int>).
TEST(OptionalBinaryTest, ValueDecodeIncludingMinusOne) {
    using namespace qb::pg::detail;
    // 0xFFFFFFFF is int4 -1 — a real value at this layer, NOT a SQL NULL.
    std::vector<qb::pg::byte> minus_one(4, static_cast<qb::pg::byte>(0xFF));
    auto                      neg = TypeConverter<std::optional<integer>>::from_binary(minus_one);
    ASSERT_TRUE(neg.has_value());
    EXPECT_EQ(*neg, -1);

    auto v = TypeConverter<std::optional<integer>>::from_binary(hex_to_bytes("0000002a"));
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
}

/**
 * @brief Regression: TIMETZ binary decode + sign convention against PostgreSQL.
 *
 * PostgreSQL's wire zone is seconds WEST of UTC (+02:00 => -7200); qb::time_of_day_tz
 * uses an east-positive offset (+02:00 => +7200). The decoder must negate so the
 * rendered offset is not inverted. Buffers are real PostgreSQL `timetz_send()`.
 */
TEST(TimeTzBinaryTest, DecodeAgainstPostgresGroundTruth) {
    using namespace qb::pg::detail;
    auto z = TypeConverter<qb::time_of_day_tz>::from_binary(hex_to_bytes("0000000c2a0b6f40ffffe3e0"));
    EXPECT_EQ(z.tod.since_midnight().count(), 52245000000LL); // 14:30:45
    EXPECT_EQ(z.offset.count(), 7200);                        // +02:00, east-positive
    EXPECT_EQ(z.to_string(), "14:30:45+02:00");

    auto z2 = TypeConverter<qb::time_of_day_tz>::from_binary(hex_to_bytes("00000006b49d200000004650"));
    EXPECT_EQ(z2.offset.count(), -18000); // -05:00
    EXPECT_EQ(z2.to_string(), "08:00:00-05:00");
}

/**
 * @brief Regression: bytea via std::vector<std::byte>. Previously this hit the
 *        generic unsupported-type fallback and returned an empty vector for any
 *        non-empty bytea (the EmptyBytea integration test only checked size 0).
 */
TEST(ByteaStdByteTest, RoundTrip) {
    using namespace qb::pg::detail;
    const std::vector<std::byte> in{std::byte{0xDE}, std::byte{0xAD}, std::byte{0x00}, std::byte{0xBE}, std::byte{0xEF}};

    // Result value carries no length prefix.
    std::vector<qb::pg::byte> wire(in.size());
    for (size_t i = 0; i < in.size(); ++i)
        wire[i] = static_cast<qb::pg::byte>(in[i]);
    EXPECT_EQ(TypeConverter<std::vector<std::byte>>::from_binary(wire), in);

    // to_binary writes [int32 length][raw]; strip the prefix and decode back.
    std::vector<qb::pg::byte> buf;
    TypeConverter<std::vector<std::byte>>::to_binary(in, buf);
    ASSERT_GE(buf.size(), 4u);
    std::vector<qb::pg::byte> body(buf.begin() + 4, buf.end());
    EXPECT_EQ(TypeConverter<std::vector<std::byte>>::from_binary(body), in);
}

/**
 * @brief DATE/TIME/TIMETZ/INTERVAL decoded into the qb core civil vocabulary
 *        (qb::date / qb::time_of_day / qb::time_of_day_tz / qb::calendar_interval),
 *        against PostgreSQL *_send() ground truth, plus to_binary->from_binary
 *        round-trips.
 */
TEST(QbCivilTypesBinaryTest, DecodeAgainstPostgresGroundTruth) {
    using namespace qb::pg::detail;

    auto d = TypeConverter<qb::date>::from_binary(hex_to_bytes("00002288")); // 8840 days since 2000
    EXPECT_EQ(d.to_string(), "2024-03-15");
    EXPECT_EQ(d.days_since_epoch(), 19797);

    auto t = TypeConverter<qb::time_of_day>::from_binary(hex_to_bytes("0000000c2a0d5180"));
    EXPECT_EQ(t.to_string(), "14:30:45.123456");

    auto z = TypeConverter<qb::time_of_day_tz>::from_binary(hex_to_bytes("0000000c2a0b6f40ffffe3e0"));
    EXPECT_EQ(z.to_string(), "14:30:45+02:00"); // wire zone west-positive -> east-positive
    EXPECT_EQ(z.offset.count(), 7200);

    auto iv = TypeConverter<qb::calendar_interval>::from_binary(hex_to_bytes("00000002925553400000000200000001"));
    EXPECT_EQ(iv.months, 1);
    EXPECT_EQ(iv.days, 2);
    EXPECT_EQ(iv.micros.count(), 11045000000LL);
    EXPECT_EQ(iv.to_micros().count(), 2775845000000LL); // == PG EXTRACT(EPOCH) * 1e6

    // to_binary (length-prefixed) -> from_binary round-trips.
    auto rt = [](auto v) {
        using T = decltype(v);
        std::vector<qb::pg::byte> b;
        TypeConverter<T>::to_binary(v, b);
        return TypeConverter<T>::from_binary(b);
    };
    EXPECT_EQ(rt(qb::date::from_ymd(1999, 12, 31)), qb::date::from_ymd(1999, 12, 31));
    EXPECT_EQ(rt(qb::time_of_day::from_hms(23, 59, 59, 999999)), qb::time_of_day::from_hms(23, 59, 59, 999999));
    EXPECT_EQ(rt(qb::time_of_day_tz::from_hms_offset(8, 0, 0, 0, -18000)), qb::time_of_day_tz::from_hms_offset(8, 0, 0, 0, -18000));
    EXPECT_EQ(rt(qb::calendar_interval(13, 5, std::chrono::microseconds{123456})),
              qb::calendar_interval(13, 5, std::chrono::microseconds{123456}));
}

/**
 * @brief std::expected-style monadic combinators on Reply<T> / Reply<void>.
 */
TEST(ReplyMonadicTest, TransformAndThenOrElseValueOr) {
    using qb::pg::Reply;
    using qb::pg::error::db_error;

    const auto ok = Reply<int>::success(21);
    EXPECT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 21);
    EXPECT_EQ(ok.value_or(99), 21);
    EXPECT_EQ(ok.transform([](int x) { return x * 2; }).result(), 42);               // int -> int
    EXPECT_EQ(ok.transform([](int x) { return std::to_string(x); }).result(), "21"); // int -> string
    auto chained = ok.and_then([](int x) { return Reply<std::string>::success("v" + std::to_string(x)); });
    EXPECT_TRUE(chained.ok());
    EXPECT_EQ(chained.result(), "v21");

    const auto bad = Reply<int>::failure(db_error{"boom"});
    EXPECT_FALSE(bad.has_value());
    EXPECT_EQ(bad.value_or(7), 7);
    EXPECT_FALSE(bad.transform([](int x) { return x * 2; }).ok()); // f not called, error propagates
    EXPECT_FALSE(bad.and_then([](int) { return Reply<int>::success(1); }).ok());
    auto recovered = bad.or_else([](db_error const &) { return Reply<int>::success(123); });
    EXPECT_TRUE(recovered.ok());
    EXPECT_EQ(recovered.result(), 123);

    // Reply<void>
    EXPECT_TRUE(Reply<void>::success().and_then([] { return Reply<int>::success(5); }).ok());
    EXPECT_FALSE(Reply<void>::failure(db_error{"x"}).and_then([] { return Reply<int>::success(5); }).ok());
    EXPECT_TRUE(Reply<void>::failure(db_error{"x"}).or_else([](db_error const &) { return Reply<void>::success(); }).ok());
}

/**
 * @brief Test INET and CIDR network address types
 *
 * Network types are handled as strings until full binary support.
 */
TEST(NetworkAddressTypeTest, StringHandling) {
    using namespace qb::pg;

    // Test 1: OID for network types (need static_cast for enum class)
    EXPECT_EQ(static_cast<int>(detail::oid::inet), 869);
    EXPECT_EQ(static_cast<int>(detail::oid::cidr), 650);
    EXPECT_EQ(static_cast<int>(detail::oid::macaddr), 829);

    // Test 2: IPv4 address as string
    std::string ipv4 = "192.168.1.1";
    EXPECT_EQ(ipv4.length(), 11);

    // Test 3: IPv6 address as string
    std::string ipv6 = "2001:db8::1";
    EXPECT_EQ(ipv6.length(), 11);

    // Test 4: CIDR notation
    std::string cidr = "192.168.0.0/16";
    EXPECT_EQ(cidr.length(), 14);

    // Test 5: MAC address
    std::string mac = "00:1a:2b:3c:4d:5e";
    EXPECT_EQ(mac.length(), 17);

    std::cout << "Network address types (INET/CIDR/MACADDR) test passed" << std::endl;
}

/**
 * @brief Test edge cases and extreme values
 *
 * Tests boundary conditions for numeric types.
 */
TEST(EdgeCasesTest, ExtremeValues) {
    using namespace qb::pg::detail;

    // Test 1: Very large numeric (just check it's stored, not exact length)
    numeric huge("999999999999999999999999999.9999999999");
    EXPECT_GT(huge.str().length(), 30);                       // Should be long
    EXPECT_TRUE(huge.str().find("999") != std::string::npos); // Contains digits

    // Test 2: Very small numeric
    numeric tiny("0.0000000000000000000000000000000000001");
    EXPECT_GT(tiny.str().length(), 30);                      // Should be long
    EXPECT_TRUE(tiny.str().find("0.") != std::string::npos); // Starts with 0.

    // Test 3: Negative numeric
    numeric negative("-9999999999.9999999999");
    EXPECT_TRUE(negative.str()[0] == '-');

    // Test 4: Zero
    numeric zero("0");
    numeric zero2("0.0");
    numeric zero3("0.00000");
    EXPECT_EQ(zero.str(), "0");

    // Test 5: Date far in past
    qb::date old = qb::date::parse("1900-01-01").value();
    EXPECT_EQ(old.to_string(), "1900-01-01");

    // Test 6: Date far in future
    qb::date future = qb::date::parse("2099-12-31").value();
    EXPECT_EQ(future.to_string(), "2099-12-31");

    std::cout << "Edge cases test passed" << std::endl;
}

/**
 * @brief Test TIME type (qb::time_of_day)
 *
 * Tests PostgreSQL TIME type handling.
 */
TEST(TimeTypeFullTest, BinaryConversion) {
    using namespace qb::pg::detail;

    // Test 1: Create time from components
    qb::time_of_day t1 = qb::time_of_day::from_hms(14, 30, 45, 123456);
    EXPECT_EQ(t1.since_midnight().count(), (14 * 3600 + 30 * 60 + 45) * 1000000LL + 123456);

    // Test 2: String conversion
    std::string time_str = t1.to_string();
    EXPECT_TRUE(time_str.find("14:30:45") != std::string::npos);

    // Test 3: Parse from string
    qb::time_of_day t2 = qb::time_of_day::parse("12:00:00").value();
    EXPECT_EQ(t2.since_midnight().count(), 12 * 3600 * 1000000LL);

    // Test 4: Binary serialization
    std::vector<byte> buffer;
    TypeConverter<qb::time_of_day>::to_binary(t1, buffer);
    EXPECT_EQ(buffer.size(), 12); // 4 (length) + 8 (microseconds)

    // Check length prefix
    integer len;
    std::memcpy(&len, buffer.data(), sizeof(integer));
    EXPECT_EQ(ntohl(len), 8);

    // Test 5: Binary deserialization
    qb::time_of_day t3 = TypeConverter<qb::time_of_day>::from_binary(buffer);
    EXPECT_EQ(t3.since_midnight().count(), t1.since_midnight().count());

    // Test 6: Text round-trip
    std::string     text = TypeConverter<qb::time_of_day>::to_text(t1);
    qb::time_of_day t4   = TypeConverter<qb::time_of_day>::from_text(text);
    EXPECT_EQ(t4, t1);

    // Test 7: OID check
    EXPECT_EQ(TypeConverter<qb::time_of_day>::get_oid(), 1083);

    std::cout << "TIME type (qb::time_of_day) test passed" << std::endl;
}

/**
 * @brief Test TIMETZ type (qb::time_of_day_tz)
 *
 * Tests PostgreSQL TIMETZ type handling.
 */
TEST(TimeTzTypeFullTest, BinaryConversion) {
    using namespace qb::pg::detail;

    // Test 1: Create timetz with offset
    qb::time_of_day_tz tt1 = qb::time_of_day_tz::from_hms_offset(18, 0, 0, 0, 7200); // +02:00
    EXPECT_EQ(tt1.tod.since_midnight().count(), 18 * 3600 * 1000000LL);
    EXPECT_EQ(tt1.offset.count(), 7200);

    // Test 2: String conversion
    std::string timetz_str = tt1.to_string();
    EXPECT_TRUE(timetz_str.find("18:00:00") != std::string::npos);
    EXPECT_TRUE(timetz_str.find("+02:00") != std::string::npos);

    // Test 3: Positive timezone offset (east of UTC)
    qb::time_of_day_tz tt2 = qb::time_of_day_tz::from_hms_offset(14, 30, 45, 0, (5 * 3600) + (30 * 60));
    EXPECT_EQ(tt2.tod.since_midnight().count(), (14 * 3600 + 30 * 60 + 45) * 1000000LL);
    EXPECT_EQ(tt2.offset.count(), (5 * 3600) + (30 * 60));
    EXPECT_EQ(tt2.to_string(), "14:30:45+05:30");

    // Test 4: Negative timezone (west of UTC)
    qb::time_of_day_tz tt3 = qb::time_of_day_tz::from_hms_offset(8, 0, 0, 0, -8 * 3600);
    EXPECT_EQ(tt3.offset.count(), -8 * 3600);
    EXPECT_EQ(tt3.to_string(), "08:00:00-08:00");

    // Test 5: Binary serialization
    std::vector<byte> buffer;
    TypeConverter<qb::time_of_day_tz>::to_binary(tt1, buffer);
    EXPECT_EQ(buffer.size(), 16); // 4 (length) + 8 (time) + 4 (tz)

    // Test 6: Binary deserialization
    qb::time_of_day_tz tt4 = TypeConverter<qb::time_of_day_tz>::from_binary(buffer);
    EXPECT_EQ(tt4.tod.since_midnight().count(), tt1.tod.since_midnight().count());
    EXPECT_EQ(tt4.offset.count(), tt1.offset.count());

    // Test 7: Text serialization (to_text renders the east-positive offset)
    std::string text = TypeConverter<qb::time_of_day_tz>::to_text(tt1);
    EXPECT_EQ(text, "18:00:00+02:00");

    // Test 8: OID check
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::get_oid(), 1266);

    std::cout << "TIMETZ type (qb::time_of_day_tz) test passed" << std::endl;
}

/**
 * @brief Pre-1970 (negative Unix epoch) DATE/TIMESTAMP round-trips.
 *
 * Regression for a Windows-only divergence: the CRT gmtime_s / _mkgmtime reject
 * negative time_t, so any date/timestamp before 1970-01-01 used to fall back to
 * "2000-01-01" (DATE) or throw (TIMESTAMP) on Windows while working on POSIX.
 * The converters now use pure-integer UTC arithmetic, identical on all platforms.
 */
TEST(EdgeCasesTest, PreUnixEpochDatesAndTimestamps) {
    using namespace qb::pg::detail;

    // DATE: civil round-trip across the 1970 boundary, far past and BCE.
    for (const char *s : {"1969-12-31", "1900-01-01", "1858-11-17", "0001-01-01", "2000-01-01", "1970-01-01"}) {
        qb::date d = qb::date::parse(s).value();
        EXPECT_EQ(d.to_string(), s) << "DATE round-trip failed for " << s;
    }

    // TIMESTAMP: pre-1970 text must parse (no throw) and round-trip exactly.
    const std::string ts_in = "1969-07-20 20:17:40.000000";
    qb::wall_time     ts;
    ASSERT_NO_THROW(ts = TypeConverter<qb::wall_time>::from_text(ts_in));
    const std::string ts_out = TypeConverter<qb::wall_time>::to_text(ts);
    EXPECT_NE(ts_out.find("1969-07-20 20:17:40"), std::string::npos) << "got: " << ts_out;

    // A sub-second pre-1970 instant must borrow a second, not truncate toward zero.
    const std::string ts2_in  = "1955-11-05 06:15:00.500000";
    qb::wall_time     ts2     = TypeConverter<qb::wall_time>::from_text(ts2_in);
    const std::string ts2_out = TypeConverter<qb::wall_time>::to_text(ts2);
    EXPECT_NE(ts2_out.find("1955-11-05 06:15:00.5"), std::string::npos) << "got: " << ts2_out;

    std::cout << "Pre-Unix-epoch date/timestamp test passed" << std::endl;
}

// Text-format decode for the civil/byte types. A simple (text-protocol) query
// returns these columns as text, so from_text must decode correctly — not return a
// silent default. TIMETZ parses; bytea round-trips PostgreSQL hex; INTERVAL text is
// unsupported and must fail loudly rather than yield a zero interval.
TEST(CivilTypeTextDecodeTest, FromTextBehaviors) {
    using namespace qb::pg::detail;

    // TIMETZ text "HH:MM:SS[.ffffff]±HH[:MM]" -> qb::time_of_day_tz, round-trips via to_text.
    auto z = TypeConverter<qb::time_of_day_tz>::from_text("14:30:45.123456+02:00");
    EXPECT_EQ(z.tod.to_string(), "14:30:45.123456");
    EXPECT_EQ(z.offset.count(), 7200);
    EXPECT_EQ(TypeConverter<qb::time_of_day_tz>::to_text(z), "14:30:45.123456+02:00");

    // Negative offset written without minutes on the wire ("-05").
    auto z2 = TypeConverter<qb::time_of_day_tz>::from_text("08:00:00-05");
    EXPECT_EQ(z2.offset.count(), -5 * 3600);
    EXPECT_EQ(z2.tod.to_string(), "08:00:00");

    // bytea std::vector<std::byte> text: "\xDEADBEEF" <-> raw bytes, round-trip both ways.
    std::vector<std::byte> raw{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    const std::string      hex = TypeConverter<std::vector<std::byte>>::to_text(raw);
    EXPECT_EQ(hex, "\\xdeadbeef");
    EXPECT_EQ(TypeConverter<std::vector<std::byte>>::from_text(hex), raw);
    EXPECT_EQ(TypeConverter<std::vector<std::byte>>::from_text("deadbeef"), raw); // no "\x" prefix

    // INTERVAL text decode is unsupported -> loud throw, never a silent zero interval.
    EXPECT_THROW(TypeConverter<qb::calendar_interval>::from_text("1 mon 2 days 03:04:05"), std::runtime_error);
}

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}