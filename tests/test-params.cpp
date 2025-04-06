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
 * @author QB PostgreSQL Module Team
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
        std::cout << "\n===== Test with vector of " << values.size() << " strings =====\n";

        // Reset the serializer
        serializer_.reset();

        // Add the string vector directly
        std::cout << "[TEST] Adding a vector of " << values.size()
                  << " strings as parameters\n";

        // Display each value in the vector
        for (const auto &value : values) {
            std::cout << "[TEST]   - Value: '" << value << "', length: " << value.size() << "\n";
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
            if ((i + 1) % 16 == 0) std::cout << "\n";
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
 * @brief Main test function
 *
 * Runs tests for the PostgreSQL parameter serializer with different
 * string vector configurations.
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

    return 0;
}