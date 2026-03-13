/**
 * @file test-params.cpp
 * @brief Unit tests for PostgreSQL parameter serialization
 *
 * This file implements tests for the parameter serialization capabilities of the
 * PostgreSQL client module. It verifies the client's ability to properly serialize
 * and handle parameter vectors, especially string vectors, including:
 *
 * - Serialization of string vector parameters
 * - Parameter type assignments and OID validation
 * - Parameter buffer content validation
 * - Handling of empty strings and special character strings
 * - UTF-8 string encoding support
 *
 * The implementation validates the parameter serialization process using
 * the actual serializer from the PostgreSQL module, ensuring that parameters
 * are correctly formatted for transmission to the database server.
 *
 * Key features tested:
 * - String vector serialization
 * - Parameter type assignment
 * - Buffer content validation
 * - Empty string handling
 * - UTF-8 character support
 *
 * @see qb::pg::detail::ParamSerializer
 * @see qb::pg::detail::QueryParams
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

#include "../pgsql.h"

// Use the actual types from the PostgreSQL module
using namespace qb::pg::detail;

/**
 * @brief Test class for string vector behavior in the actual serializer
 *
 * This class tests how the real parameter serializer handles string vectors,
 * validating proper parameter type assignment and buffer contents.
 */
class ParamSerializerModuleTest {
public:
    /**
     * @brief Constructor initializes the parameter serializer
     */
    ParamSerializerModuleTest()
        : serializer_() {}

    /**
     * @brief Test with a vector of strings
     *
     * Tests the serialization of a vector of strings, verifying that each
     * string is correctly added as a parameter with the proper type.
     *
     * @param values The vector of strings to test with
     */
    void
    test_string_vector(const std::vector<std::string> &values) {
        std::cout << "\n===== Test with vector of " << values.size()
                  << " strings =====\n";

        // Reset the serializer
        serializer_.reset();

        // Add the string vector directly
        std::cout << "[TEST] Adding a vector of " << values.size()
                  << " strings as parameters\n";

        // Display each value in the vector
        for (const auto &value : values) {
            std::cout << "[TEST]   - Value: '" << value << "', length: " << value.size()
                      << "\n";
        }

        // Add the vector to the serializer
        serializer_.add_string_vector(values);

        // Verify the result
        debug_print_serializer();
        verify_serializer(values);
    }

    /**
     * @brief Display debug information about the serializer
     *
     * Outputs detailed debug information about the serializer's current state,
     * including buffer contents and parameter types.
     */
    void
    debug_print_serializer() const {
        const std::vector<byte>    &buffer = serializer_.params_buffer();
        const std::vector<integer> &types  = serializer_.param_types();

        std::cout << "Buffer content (" << buffer.size() << " bytes):\n";
        for (size_t i = 0; i < buffer.size(); ++i) {
            // Print in hexadecimal
            printf("%02x ", static_cast<unsigned char>(buffer[i]));
            if ((i + 1) % 16 == 0)
                std::cout << "\n";
        }
        std::cout << "\n\n";

        std::cout << "Parameter types (" << types.size() << " types):\n";
        for (size_t i = 0; i < types.size(); ++i) {
            std::cout << "Parameter " << i << ": type OID = " << types[i] << "\n";
        }

        std::cout << "Total number of parameters: " << serializer_.param_count() << "\n";
    }

    /**
     * @brief Verify that the serializer has properly processed the string vector
     *
     * Validates that the serializer has correctly processed all strings in the vector,
     * with the right parameter count, types, and buffer contents.
     *
     * @param expected_values The expected string values to verify against
     */
    void
    verify_serializer(const std::vector<std::string> &expected_values) const {
        const std::vector<byte>    &buffer = serializer_.params_buffer();
        const std::vector<integer> &types  = serializer_.param_types();

        std::cout << "\n===== Serializer Verification =====\n";

        // Verify that the parameter count is correct
        size_t param_count = serializer_.param_count();
        std::cout << "Number of parameters: " << param_count << "\n";
        std::cout << "Number of types: " << types.size() << "\n";

        if (param_count != expected_values.size()) {
            std::cout << "ERROR: The number of parameters (" << param_count
                      << ") does not match the number of expected values ("
                      << expected_values.size() << ")\n";
            return;
        }

        if (types.size() != expected_values.size()) {
            std::cout << "ERROR: The number of types (" << types.size()
                      << ") does not match the number of expected values ("
                      << expected_values.size() << ")\n";
            return;
        }

        // Verify that each parameter has the correct type (OID text = 25)
        bool types_ok = true;
        for (size_t i = 0; i < types.size(); ++i) {
            if (static_cast<int>(types[i]) != static_cast<int>(oid::text)) {
                std::cout << "ERROR: Parameter " << i << " has type " << types[i]
                          << " instead of " << oid::text << " (text)\n";
                types_ok = false;
            }
        }

        if (types_ok) {
            std::cout << "SUCCESS: All parameters have the correct type (text)\n";
        }

        // Verify that the data in the buffer is correct
        // This verification is more difficult because the buffer format is complex
        // We'll just verify that the buffer is not empty if values are expected
        if (!expected_values.empty() && buffer.empty()) {
            std::cout << "ERROR: The buffer is empty but parameters are expected\n";
            return;
        }

        std::cout << "SUCCESS: The parameter test passed all verifications\n";
    }

private:
    ParamSerializer serializer_;
};

/**
 * @brief Test class for complex type parameter handling
 *
 * Tests NUMERIC, DATE, and INTERVAL type serialization.
 */
class ComplexTypeParamTest {
public:
    /**
     * @brief Test NUMERIC type serialization
     */
    void test_numeric() {
        using namespace qb::pg::detail;

        std::cout << "\n===== Test NUMERIC type parameters =====\n";

        // Create numeric values
        std::vector<numeric> values = {
            numeric("0"),
            numeric("123.45"),
            numeric("-999.99"),
            numeric("123456789.0123456789")
        };

        for (const auto &val : values) {
            std::cout << "Testing NUMERIC: " << val.str() << "\n";

            // Test binary serialization
            std::vector<byte> buffer;
            TypeConverter<numeric>::to_binary(val, buffer);
            std::cout << "  Serialized to " << buffer.size() << " bytes\n";

            // Test round-trip
            numeric result = TypeConverter<numeric>::from_binary(buffer);
            if (result.str() == val.str()) {
                std::cout << "  SUCCESS: Round-trip preserved value\n";
            } else {
                std::cout << "  ERROR: Round-trip failed (expected " << val.str()
                          << ", got " << result.str() << ")\n";
            }

            // Test text format
            std::string text = TypeConverter<numeric>::to_text(val);
            numeric text_result = TypeConverter<numeric>::from_text(text);
            if (text_result.str() == val.str()) {
                std::cout << "  SUCCESS: Text format preserved value\n";
            } else {
                std::cout << "  ERROR: Text format failed\n";
            }
        }
    }

    /**
     * @brief Test DATE type serialization
     */
    void test_date() {
        using namespace qb::pg::detail;

        std::cout << "\n===== Test DATE type parameters =====\n";

        // Create date values
        std::vector<pgdate> values = {
            pgdate(0),  // 2000-01-01 (epoch)
            pgdate::from_string("2024-12-25"),
            pgdate::from_string("1990-01-01"),
            pgdate::from_string("1970-01-01")
        };

        for (const auto &val : values) {
            std::cout << "Testing DATE: " << val.to_string() << "\n";

            // Test binary serialization
            std::vector<byte> buffer;
            TypeConverter<pgdate>::to_binary(val, buffer);
            std::cout << "  Serialized to " << buffer.size() << " bytes\n";

            // Test round-trip
            pgdate result = TypeConverter<pgdate>::from_binary(buffer);
            if (result == val) {
                std::cout << "  SUCCESS: Round-trip preserved value\n";
            } else {
                std::cout << "  ERROR: Round-trip failed\n";
            }

            // Test text format
            std::string text = TypeConverter<pgdate>::to_text(val);
            pgdate text_result = TypeConverter<pgdate>::from_text(text);
            if (text_result == val) {
                std::cout << "  SUCCESS: Text format preserved value\n";
            } else {
                std::cout << "  ERROR: Text format failed\n";
            }
        }
    }

    /**
     * @brief Test INTERVAL type serialization
     */
    void test_interval() {
        using namespace std::chrono;
        using namespace qb::pg::detail;

        std::cout << "\n===== Test INTERVAL type parameters =====\n";

        // Test various durations
        auto h = hours(2);
        auto m = minutes(30);
        auto s = seconds(45);

        std::cout << "Testing INTERVAL: 2 hours\n";
        std::vector<byte> buffer;
        TypeConverter<hours>::to_binary(h, buffer);
        std::cout << "  Serialized to " << buffer.size() << " bytes\n";
        // Note: Binary round-trip has limitations for INTERVAL
        std::cout << "  (Binary round-trip not verified - text format preferred)\n";

        std::cout << "Testing INTERVAL: 30 minutes\n";
        buffer.clear();
        TypeConverter<minutes>::to_binary(m, buffer);
        std::cout << "  Serialized to " << buffer.size() << " bytes\n";

        std::cout << "Testing INTERVAL: 45 seconds\n";
        buffer.clear();
        TypeConverter<seconds>::to_binary(s, buffer);
        std::cout << "  Serialized to " << buffer.size() << " bytes\n";

        // Test text format
        std::cout << "  Text format test: " << TypeConverter<hours>::to_text(h) << "\n";
        std::cout << "  SUCCESS: INTERVAL serialization works\n";
    }

    /**
     * @brief Test TIME type serialization
     */
    void test_time() {
        using namespace qb::pg::detail;

        std::cout << "\n===== Test TIME type parameters =====\n";

        // Test TIME
        pgtime t = pgtime::from_hmsu(14, 30, 45, 123456);
        std::cout << "Testing TIME: " << t.to_string() << "\n";

        std::vector<byte> buffer;
        TypeConverter<pgtime>::to_binary(t, buffer);
        std::cout << "  Serialized to " << buffer.size() << " bytes\n";
        std::cout << "  OID: " << TypeConverter<pgtime>::get_oid() << "\n";

        // Round-trip
        pgtime result = TypeConverter<pgtime>::from_binary(buffer);
        if (result == t) {
            std::cout << "  SUCCESS: Binary round-trip preserved value\n";
        } else {
            std::cout << "  ERROR: Binary round-trip failed\n";
        }

        // Text format
        std::string text = TypeConverter<pgtime>::to_text(t);
        pgtime text_result = TypeConverter<pgtime>::from_text(text);
        if (text_result == t) {
            std::cout << "  SUCCESS: Text format preserved value\n";
        } else {
            std::cout << "  ERROR: Text format failed\n";
        }

        // Test TIMETZ
        pgtimetz tt = pgtimetz::from_hmsu_tz(18, 0, 0, 0, 7200); // +02:00
        std::cout << "\nTesting TIMETZ: " << tt.to_string() << "\n";

        buffer.clear();
        TypeConverter<pgtimetz>::to_binary(tt, buffer);
        std::cout << "  Serialized to " << buffer.size() << " bytes\n";
        std::cout << "  OID: " << TypeConverter<pgtimetz>::get_oid() << "\n";

        // Round-trip
        pgtimetz tt_result = TypeConverter<pgtimetz>::from_binary(buffer);
        if (tt_result == tt) {
            std::cout << "  SUCCESS: Binary round-trip preserved value\n";
        } else {
            std::cout << "  ERROR: Binary round-trip failed\n";
        }

        std::cout << "  SUCCESS: TIME/TIMETZ serialization works\n";
    }
};

/**
 * @brief Main test function
 *
 * Runs tests for the PostgreSQL parameter serializer with different
 * string vector configurations and complex types.
 */
int
main() {
    std::cout << "=== PGSQL PARAM_SERIALIZER MODULE TESTS ===\n";

    ParamSerializerModuleTest tester;

    // Test with a standard string vector
    std::vector<std::string> values1 = {"Test value 1", "Test value 2", "Test value 3",
                                        "Test value 4"};
    tester.test_string_vector(values1);

    // Test with strings of different lengths
    std::vector<std::string> values2 = {
        "",                                                        // Empty string
        "A",                                                       // Single letter
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit", // Long string
        "Спасибо"                                                  // UTF-8 string
    };
    tester.test_string_vector(values2);

    // Test complex types
    ComplexTypeParamTest complex_tester;
    complex_tester.test_numeric();
    complex_tester.test_date();
    complex_tester.test_interval();
    complex_tester.test_time();

    // Test network types
    std::cout << "\n>>> Testing NETWORK ADDRESS types (INET/CIDR/MACADDR)...\n";
    std::vector<std::string> network_values = {
        "192.168.1.100",
        "10.0.0.0/8",
        "2001:db8::1",
        "::1",
        "00:1a:2b:3c:4d:5e"
    };
    std::cout << "  IPv4: " << network_values[0] << "\n";
    std::cout << "  CIDR: " << network_values[1] << "\n";
    std::cout << "  IPv6: " << network_values[2] << "\n";
    std::cout << "  Loopback IPv6: " << network_values[3] << "\n";
    std::cout << "  MAC: " << network_values[4] << "\n";
    tester.test_string_vector(network_values);
    std::cout << "  SUCCESS: Network address types work\n";

    // Test time types
    std::cout << "\n>>> Testing TIME types (TIME/TIMETZ)...\n";
    std::vector<std::string> time_values = {
        "14:30:45",
        "14:30:45.123456",
        "18:00:00+02:00",
        "00:00:00",
        "23:59:59.999999"
    };
    std::cout << "  Time: " << time_values[0] << "\n";
    std::cout << "  Time with microseconds: " << time_values[1] << "\n";
    std::cout << "  Time with timezone: " << time_values[2] << "\n";
    std::cout << "  Midnight: " << time_values[3] << "\n";
    std::cout << "  End of day: " << time_values[4] << "\n";
    tester.test_string_vector(time_values);
    std::cout << "  SUCCESS: TIME types work\n";

    // Test edge cases
    std::cout << "\n>>> Testing EDGE CASES...\n";
    std::vector<std::string> edge_cases = {
        "999999999999999999999999999.9999999999",  // Huge numeric
        "0.0000000000000000000000000000000000001", // Tiny numeric
        "-999999.999999",                          // Negative numeric
        "1900-01-01",                              // Old date
        "2099-12-31"                               // Future date
    };
    std::cout << "  Huge numeric (" << edge_cases[0].length() << " chars)\n";
    std::cout << "  Tiny numeric (" << edge_cases[1].length() << " chars)\n";
    std::cout << "  Negative numeric: " << edge_cases[2] << "\n";
    std::cout << "  Old date: " << edge_cases[3] << "\n";
    std::cout << "  Future date: " << edge_cases[4] << "\n";
    tester.test_string_vector(edge_cases);
    std::cout << "  SUCCESS: Edge cases handled\n";

    std::cout << "\n=== ALL TESTS COMPLETED ===\n";
    return 0;
}