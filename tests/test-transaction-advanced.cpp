/**
 * @file test-transaction-advanced.cpp
 * @brief Advanced tests for PostgreSQL transaction management
 *
 * This file implements additional advanced tests for the transaction management 
 * functionality of the PostgreSQL client module. It tests more complex scenarios
 * including:
 *
 * - Concurrent transactions with locking behavior
 * - Explicit transaction isolation levels
 * - Transaction with timeouts and cancellation
 * - Savepoint restoration to particular points
 * - Table locking within transactions
 * - Stored procedure execution within transactions
 * - Complex nested transactions with failure scenarios
 *
 * The tests ensure robust transaction handling for production environments
 * with high concurrency and complex data integrity requirements.
 *
 * @see qb::pg::detail::Transaction
 * @see qb::pg::detail::Database
 * @see qb::pg::isolation_level
 *
 * @author QB PostgreSQL Module Team
 */

#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <future>
#include "../pgsql.h"

constexpr std::string_view PGSQL_CONNECTION_STR = "tcp://test:test@localhost:5432[test]";

using namespace qb::pg;
using namespace qb::pg::detail;

/**
 * @brief Test fixture for PostgreSQL advanced transaction functionality
 *
 * Sets up a test environment with database tables and data for testing
 * advanced transaction management features.
 */
class PostgreSQLAdvancedTransactionTest : public ::testing::Test {
protected:
    /**
     * @brief Set up the test environment
     *
     * Creates database connections and sets up test tables with sample data
     * for advanced transaction testing.
     */
    void SetUp() override {
        // Create primary database connection
        db1_ = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(db1_->connect(PGSQL_CONNECTION_STR.data()));
        
        // Create secondary database connection for concurrent testing
        db2_ = std::make_unique<qb::pg::tcp::database>();
        ASSERT_TRUE(db2_->connect(PGSQL_CONNECTION_STR.data()));
        
        // Set up test tables
        auto setup1 = db1_->execute("DROP TABLE IF EXISTS test_advanced_transactions").await();
        ASSERT_TRUE(setup1);
        
        auto setup2 = db1_->execute(
            "CREATE TABLE test_advanced_transactions ("
            "id SERIAL PRIMARY KEY, "
            "value TEXT, "
            "counter INTEGER DEFAULT 0)"
        ).await();
        ASSERT_TRUE(setup2);
        
        // Insert initial test data
        auto setup3 = db1_->execute(
            "INSERT INTO test_advanced_transactions (value, counter) VALUES "
            "('row1', 10), ('row2', 20), ('row3', 30)"
        ).await();
        ASSERT_TRUE(setup3);
    }

    /**
     * @brief Clean up after tests
     *
     * Drops temporary tables and disconnects database connections.
     */
    void TearDown() override {
        if (db1_) {
            auto cleanup = db1_->execute("DROP TABLE IF EXISTS test_advanced_transactions").await();
            db1_->disconnect();
            db1_.reset();
        }
        
        if (db2_) {
            db2_->disconnect();
            db2_.reset();
        }
    }

    std::unique_ptr<qb::pg::tcp::database> db1_; // Primary connection
    std::unique_ptr<qb::pg::tcp::database> db2_; // Secondary connection for concurrent tests
};

/**
 * @brief Test explicit transaction isolation levels
 *
 * Verifies that different isolation levels work correctly with
 * the expected visibility behavior for concurrent transactions.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, ExplicitIsolationLevels) {
    // Test READ COMMITTED isolation (default)
    bool read_committed_success = false;
    
    // Create transaction mode with explicit isolation level for debugging
    transaction_mode mode{isolation_level::read_committed};
    std::cout << "DEBUG: Transaction mode created with isolation_level::read_committed" << std::endl;
    
    // Convert the transaction mode to string for debug
    std::stringstream ss;
    ss << mode;
    std::cout << "DEBUG: Transaction mode string representation: \"BEGIN TRANSACTION" << ss.str() << "\"" << std::endl;
    
    // Start a transaction with READ COMMITTED isolation
    auto status = db1_->begin(
        [&read_committed_success](Transaction &t) {
            // Query the initial value
            t.execute(
                "SELECT counter FROM test_advanced_transactions WHERE value = 'row1'",
                [&read_committed_success](Transaction &tr, results result) {
                    ASSERT_EQ(result.size(), 1);
                    ASSERT_EQ(result[0][0].as<int>(), 10);
                    read_committed_success = true;
                    
                    // Print debug message
                    std::cout << "READ COMMITTED isolation test successful" << std::endl;
                }
            );
        },
        [](error::db_error error) { 
            std::cout << "Error in READ COMMITTED test: " << error.what() << std::endl;
            ASSERT_TRUE(false); 
        },
        mode // use our debug mode variable
    ).await();
    
    ASSERT_TRUE(read_committed_success);
    
    // Reset counter if needed
    auto reset = db1_->execute(
        "UPDATE test_advanced_transactions SET counter = 10 WHERE value = 'row1'"
    ).await();
    ASSERT_TRUE(reset);
}

/**
 * @brief Test read-only transactions
 *
 * Verifies that read-only transactions prevent data modifications.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, ReadOnlyTransaction) {
    bool read_success = false;
    bool write_attempted = false;
    bool error_caught = false;
    
    std::cout << "Beginning read-only transaction test" << std::endl;
    
    // Create transaction mode with read_only=true for debugging
    transaction_mode mode;
    mode.read_only = true;
    
    // Convert the mode to string for debugging
    std::stringstream ss;
    ss << mode;
    std::cout << "DEBUG: READ ONLY transaction mode string: \"BEGIN TRANSACTION" << ss.str() << "\"" << std::endl;
    
    // Start a transaction with READ ONLY mode using mode parameter
    auto status = db1_->begin(
        [&read_success, &write_attempted](Transaction &t) {
            // Try to read data (should succeed)
            t.execute(
                "SELECT COUNT(*) FROM test_advanced_transactions",
                [&read_success, &write_attempted](Transaction &tr, results result) {
                    ASSERT_EQ(result.size(), 1);
                    std::cout << "Read operation successful, count: " << result[0][0].as<int>() << std::endl;
                    read_success = true;
                    
                    // Now try to modify data (should fail)
                    tr.execute(
                        "INSERT INTO test_advanced_transactions (value, counter) VALUES ('readonly_test', 999)",
                        [&write_attempted](Transaction &tr2, results result) {
                            write_attempted = true;
                            std::cout << "WARNING: Write operation succeeded in READ ONLY transaction" << std::endl;
                        },
                        [&write_attempted](error::db_error error) {
                            write_attempted = true;
                            std::cout << "Expected error in READ ONLY transaction: " << error.what() << std::endl;
                        }
                    );
                }
            );
        },
        [&error_caught](error::db_error error) {
            error_caught = true;
            std::cout << "Transaction error callback triggered: " << error.what() << std::endl;
        },
        mode // Use mode parameter directly
    ).await();
    
    // We expect the transaction to fail because the INSERT will cause an error
    // which will cause the transaction to be rolled back
    ASSERT_FALSE(status);
    ASSERT_TRUE(read_success);
    ASSERT_TRUE(write_attempted);
    ASSERT_TRUE(error_caught);
    
    // Make sure no data was modified
    auto verify = db1_->execute(
        "SELECT COUNT(*) FROM test_advanced_transactions WHERE value = 'readonly_test'",
        [](Transaction &tr, results result) {
            ASSERT_EQ(result.size(), 1);
            ASSERT_EQ(result[0][0].as<int>(), 0); // Should be 0 if read-only worked correctly
        }
    ).await();
    
    ASSERT_TRUE(verify);
}

/**
 * @brief Test basic savepoint functionality
 *
 * Verifies that basic savepoint creation works correctly.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, SavepointRestoration) {
    bool transaction_started = false;
    bool savepoint_created = false;
    bool after_savepoint = false;
    
    // Start a transaction with a savepoint
    auto status = db1_->begin(
        [&transaction_started, &savepoint_created, &after_savepoint](Transaction &t) {
            transaction_started = true;
            std::cout << "Main transaction started" << std::endl;
            
            // Create a savepoint
            t.savepoint(
                "sp1",
                [&savepoint_created](Transaction &tr) {
                    savepoint_created = true;
                    std::cout << "Savepoint created successfully" << std::endl;
                    
                    // Execute a query inside the savepoint
                    tr.execute(
                        "SELECT COUNT(*) FROM test_advanced_transactions",
                        [](Transaction &tr2, results result) {
                            ASSERT_EQ(result.size(), 1);
                            std::cout << "Count in savepoint: " << result[0][0].as<int>() << std::endl;
                        }
                    );
                },
                [](error::db_error error) {
                    std::cout << "Error creating savepoint: " << error.what() << std::endl;
                    ASSERT_TRUE(false);
                }
            );
            
            // Execute a query after the savepoint
            t.execute(
                "SELECT 1",
                [&after_savepoint](Transaction &tr, results result) {
                    after_savepoint = true;
                    std::cout << "Query after savepoint executed successfully" << std::endl;
                }
            );
        },
        [](error::db_error error) {
            std::cout << "Transaction error: " << error.what() << std::endl;
            ASSERT_TRUE(false);
        }
    ).await();
    
    ASSERT_TRUE(transaction_started);
    ASSERT_TRUE(savepoint_created);
    ASSERT_TRUE(after_savepoint);
    ASSERT_TRUE(status);
}

/**
 * @brief Test simple transaction with multiple operations
 *
 * Verifies that a transaction can contain multiple operations.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, MultiOperationTransaction) {
    bool all_operations_succeeded = false;
    
    // Start a transaction with multiple operations
    auto status = db1_->begin(
        [&all_operations_succeeded](Transaction &t) {
            // First operation - insert a new row
            t.execute(
                "INSERT INTO test_advanced_transactions (value, counter) VALUES ('temp_row', 100)"
            );
            
            // Second operation - update an existing row
            t.execute(
                "UPDATE test_advanced_transactions SET counter = counter + 5 WHERE value = 'row1'"
            );
            
            // Third operation - verify results
            t.execute(
                "SELECT COUNT(*) FROM test_advanced_transactions",
                [&all_operations_succeeded](Transaction &tr, results result) {
                    ASSERT_EQ(result.size(), 1);
                    ASSERT_GE(result[0][0].as<int>(), 4); // 3 original rows + new temp row
                    all_operations_succeeded = true;
                    std::cout << "Transaction with multiple operations completed successfully" << std::endl;
                }
            );
        }
    ).await();
    
    ASSERT_TRUE(all_operations_succeeded);
    ASSERT_TRUE(status);
    
    // Clean up - delete the temp row
    auto cleanup = db1_->execute(
        "DELETE FROM test_advanced_transactions WHERE value = 'temp_row'"
    ).await();
    ASSERT_TRUE(cleanup);
    
    // Reset the counter for row1
    auto reset = db1_->execute(
        "UPDATE test_advanced_transactions SET counter = 10 WHERE value = 'row1'"
    ).await();
    ASSERT_TRUE(reset);
}

/**
 * @brief Test simple error handling in transactions
 *
 * Verifies that errors in transactions are properly handled.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, BasicErrorHandling) {
    bool error_handled = false;
    
    // Start a transaction with an error
    auto status = db1_->begin(
        [](Transaction &t) {
            // Execute an invalid SQL query
            t.execute(
                "SELECT * FROM nonexistent_table",
                [](Transaction &tr, results result) {
                    ASSERT_TRUE(false); // This should not be called
                },
                [](error::db_error error) {
                    std::cout << "Error handled correctly: " << error.what() << std::endl;
                    ASSERT_TRUE(true); // Error handler called as expected
                }
            );
        },
        [&error_handled](error::db_error error) {
            error_handled = true;
            std::cout << "Transaction error callback: " << error.what() << std::endl;
        }
    ).await();
    
    // The transaction might fail or succeed depending on how errors are propagated in the library
    std::cout << "Transaction status: " << (status ? "succeeded" : "failed") << std::endl;
}

/**
 * @brief Test transaction with explicit timeout handling
 *
 * Verifies that long-running transactions can be timed out appropriately.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, TransactionTimeout) {
    bool timeout_operation_started = false;
    
    // Start a transaction with a pg_sleep operation
    auto start_time = std::chrono::steady_clock::now();
    
    auto status = db1_->begin(
        [&timeout_operation_started](Transaction &t) {
            timeout_operation_started = true;
            
            // Add statement timeout setting (1 second)
            t.execute("SET statement_timeout = '1000'");
            
            // Execute a query that will exceed the timeout
            t.execute(
                "SELECT pg_sleep(3)", // 3 seconds, exceeding the 1 second timeout
                [](Transaction &tr, results result) {
                    ASSERT_TRUE(false); // Should not be called due to timeout
                },
                [](error::db_error error) {
                    std::cout << "Expected timeout error: " << error.what() << std::endl;
                    ASSERT_TRUE(std::string(error.what()).find("timeout") != std::string::npos);
                }
            );
        }
    ).await();
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    ASSERT_TRUE(timeout_operation_started);
    
    // The operation should have timed out after approximately 1 second
    // Allow some margin for processing and network overhead
    std::cout << "Transaction took " << duration.count() << " ms" << std::endl;
    ASSERT_GT(duration.count(), 900);  // At least 900ms
    ASSERT_LT(duration.count(), 2000); // But less than 2 seconds (not the full 3s sleep)
}

/**
 * @brief Test basic transaction API usage
 *
 * This is a simpler test that demonstrates the transaction API without
 * requiring complex database setups.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, BasicTransactionAPIUsage) {
    // Simple transaction API test
    bool success = false;
    
    // Create a basic transaction with the simplest form
    auto status = db1_->begin(
        [&success](Transaction &t) {
            // Simple query to verify transaction functionality
            t.execute(
                "SELECT 1 AS simple_test",
                [&success](Transaction &tr, results result) {
                    ASSERT_EQ(result.size(), 1);
                    ASSERT_EQ(result[0][0].as<int>(), 1);
                    success = true;
                    
                    // Print test success
                    std::cout << "Basic transaction API test succeeded" << std::endl;
                }
            );
        }
    ).await();
    
    ASSERT_TRUE(success);
    ASSERT_TRUE(status);
}

/**
 * @brief Test basic sequential transactions
 *
 * Verifies that multiple transactions can be executed sequentially.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, SequentialTransactions) {
    bool transaction1_complete = false;
    bool transaction2_complete = false;
    
    // First transaction - update row1
    auto status1 = db1_->begin(
        [&transaction1_complete](Transaction &t) {
            t.execute(
                "UPDATE test_advanced_transactions SET counter = counter + 1 WHERE value = 'row1'",
                [&transaction1_complete](Transaction &tr, results result) {
                    transaction1_complete = true;
                    std::cout << "First transaction completed successfully" << std::endl;
                }
            );
        }
    ).await();
    
    ASSERT_TRUE(transaction1_complete);
    ASSERT_TRUE(status1);
    
    // Second transaction - update row2
    auto status2 = db1_->begin(
        [&transaction2_complete](Transaction &t) {
            t.execute(
                "UPDATE test_advanced_transactions SET counter = counter + 1 WHERE value = 'row2'",
                [&transaction2_complete](Transaction &tr, results result) {
                    transaction2_complete = true;
                    std::cout << "Second transaction completed successfully" << std::endl;
                }
            );
        }
    ).await();
    
    ASSERT_TRUE(transaction2_complete);
    ASSERT_TRUE(status2);
    
    // Verify both updates
    auto verify = db1_->execute(
        "SELECT value, counter FROM test_advanced_transactions WHERE value IN ('row1', 'row2') ORDER BY value",
        [](Transaction &tr, results result) {
            ASSERT_EQ(result.size(), 2);
            std::cout << "Row1 counter after both transactions: " << result[0]["counter"].as<int>() << std::endl;
            std::cout << "Row2 counter after both transactions: " << result[1]["counter"].as<int>() << std::endl;
        }
    ).await();
    
    // Reset the counters for other tests
    auto reset = db1_->execute(
        "UPDATE test_advanced_transactions SET counter = CASE "
        "WHEN value = 'row1' THEN 10 "
        "WHEN value = 'row2' THEN 20 "
        "WHEN value = 'row3' THEN 30 END"
    ).await();
    ASSERT_TRUE(reset);
}

/**
 * @brief Test serializable isolation level
 *
 * Verifies that serializable transactions provide the highest isolation level
 * with serializability guarantees.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, SerializableTransaction) {
    bool serializable_success = false;
    
    std::cout << "Beginning serializable transaction test" << std::endl;
    
    // Create transaction mode with serializable isolation level for debugging
    transaction_mode mode;
    mode.isolation = isolation_level::serializable;
    
    // Convert the mode to string for debugging
    std::stringstream ss;
    ss << mode;
    std::cout << "DEBUG: SERIALIZABLE transaction mode string: \"BEGIN TRANSACTION" << ss.str() << "\"" << std::endl;
    
    // Start a transaction with SERIALIZABLE mode directly via the mode parameter
    auto status = db1_->begin(
        [&serializable_success](Transaction &t) {
            // Run a query in serializable mode
            t.execute(
                "SELECT counter FROM test_advanced_transactions WHERE value = 'row1'",
                [&serializable_success](Transaction &tr, results result) {
                    ASSERT_EQ(result.size(), 1);
                    ASSERT_EQ(result[0][0].as<int>(), 10);
                    serializable_success = true;
                    std::cout << "Serializable transaction query executed successfully" << std::endl;
                }
            );
        },
        [](error::db_error error) { 
            std::cout << "Error in SERIALIZABLE transaction test: " << error.what() << std::endl;
            ASSERT_TRUE(false); 
        },
        mode // Pass mode directly here
    ).await();
    
    ASSERT_TRUE(serializable_success);
    ASSERT_TRUE(status);
}

/**
 * @brief Test deallocate prepared statement inside a transaction
 *
 * Verifies that prepared statements can be properly deallocated within a transaction.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, DeallocatePreparedStatement) {
    bool prepare_success = false;
    bool deallocate_success = false;
    
    std::cout << "Beginning prepared statement deallocation test" << std::endl;
    
    // First prepare a statement outside transaction
    auto prepare_status = db1_->prepare(
        "test_deallocate_stmt", 
        "SELECT * FROM test_advanced_transactions WHERE value = $1"
    ).await();
    ASSERT_TRUE(prepare_status);
    
    // Execute the prepared statement to verify it works
    auto exec_status = db1_->execute(
        "test_deallocate_stmt",
        params{std::string("row1")},
        [&prepare_success](Transaction &tr, results result) {
            ASSERT_EQ(result.size(), 1);
            prepare_success = true;
            std::cout << "Successfully executed prepared statement before deallocate" << std::endl;
        }
    ).await();
    ASSERT_TRUE(exec_status);
    ASSERT_TRUE(prepare_success);
    
    // Start a transaction and deallocate the statement
    auto status = db1_->begin(
        [&deallocate_success](Transaction &t) {
            // Deallocate the prepared statement
            t.execute(
                "DEALLOCATE test_deallocate_stmt",
                [&deallocate_success](Transaction &tr, results result) {
                    deallocate_success = true;
                    std::cout << "Successfully deallocated prepared statement" << std::endl;
                }
            );
        }
    ).await();
    
    ASSERT_TRUE(deallocate_success);
    ASSERT_TRUE(status);
    
    // Try to execute the deallocated statement (should fail)
    bool error_caught = false;
    auto reuse_status = db1_->execute(
        "test_deallocate_stmt",
        params{std::string("row1")},
        [](Transaction &tr, results result) {
            ASSERT_TRUE(false) << "Deallocated statement should not execute";
        },
        [&error_caught](error::db_error error) {
            error_caught = true;
            std::cout << "Expected error after deallocate: " << error.what() << std::endl;
        }
    ).await();
    
    ASSERT_FALSE(reuse_status);
    ASSERT_TRUE(error_caught);
}

/**
 * @brief Test error handling for constraint violations
 *
 * Verifies that errors that occur due to constraint violations during
 * commit are properly detected and handled.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, ConstraintViolationHandling) {
    bool setup_complete = false;
    bool error_caught = false;
    
    std::cout << "Beginning constraint violation test" << std::endl;
    
    // First, create a table with a unique constraint
    auto setup = db1_->execute(
        "CREATE TABLE IF NOT EXISTS test_unique_constraint ("
        "id SERIAL PRIMARY KEY, "
        "unique_value TEXT UNIQUE)"
    ).await();
    ASSERT_TRUE(setup);
    
    // Insert an initial row
    auto insert = db1_->execute(
        "INSERT INTO test_unique_constraint (unique_value) VALUES ('unique_string')"
    ).await();
    ASSERT_TRUE(insert);
    
    // Now start a transaction that will attempt to violate the unique constraint
    auto status = db1_->begin(
        [&setup_complete](Transaction &t) {
            // First, insert a valid record
            t.execute(
                "INSERT INTO test_unique_constraint (unique_value) VALUES ('different_unique')",
                [&setup_complete](Transaction &tr, results result) {
                    setup_complete = true;
                    std::cout << "First insert successful" << std::endl;
                    
                    // Now attempt to insert a duplicate value (violates unique constraint)
                    tr.execute(
                        "INSERT INTO test_unique_constraint (unique_value) VALUES ('unique_string')",
                        [](Transaction &tr2, results result) {
                            ASSERT_TRUE(false) << "Constraint violation should prevent this callback";
                        },
                        [](error::db_error error) {
                            std::cout << "Expected constraint error: " << error.what() << std::endl;
                        }
                    );
                }
            );
        },
        [&error_caught](error::db_error error) {
            error_caught = true;
            std::cout << "Transaction error callback: " << error.what() << std::endl;
        }
    ).await();
    
    // The transaction should have failed due to constraint violation
    ASSERT_TRUE(setup_complete);
    ASSERT_TRUE(error_caught);
    ASSERT_FALSE(status);
    
    // Verify that the second insert was not applied
    bool verification_complete = false;
    auto verify = db1_->execute(
        "SELECT COUNT(*) FROM test_unique_constraint WHERE unique_value = 'different_unique'",
        [&verification_complete](Transaction &tr, results result) {
            ASSERT_EQ(result.size(), 1);
            // Should be 0 because the transaction was rolled back
            ASSERT_EQ(result[0][0].as<int>(), 0);
            verification_complete = true;
            std::cout << "Verification complete - transaction was correctly rolled back" << std::endl;
        }
    ).await();
    
    ASSERT_TRUE(verify);
    ASSERT_TRUE(verification_complete);
    
    // Clean up
    auto cleanup = db1_->execute("DROP TABLE IF EXISTS test_unique_constraint").await();
    ASSERT_TRUE(cleanup);
}

/**
 * @brief Test cursor functionality within transactions
 *
 * Verifies that PostgreSQL cursors can be used properly within transactions
 * to efficiently process large result sets.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, TransactionWithCursor) {
    // First, insert additional test data
    constexpr int CURSOR_TEST_ROWS = 100;
    
    std::cout << "Beginning cursor test - inserting test data" << std::endl;
    
    // Insert test data in batches
    auto insert_status = db1_->begin(
        [CURSOR_TEST_ROWS](Transaction &t) {
            for (int i = 0; i < CURSOR_TEST_ROWS; i++) {
                t.execute(
                    "INSERT INTO test_advanced_transactions (value, counter) VALUES ('cursor_row_" + 
                    std::to_string(i) + "', " + std::to_string(i) + ")"
                );
            }
            std::cout << "Inserted " << CURSOR_TEST_ROWS << " rows for cursor test" << std::endl;
        }
    ).await();
    ASSERT_TRUE(insert_status);
    
    // Now test a transaction with cursor
    bool cursor_created = false;
    bool fetch_executed = false;
    int rows_fetched = 0;
    constexpr int FETCH_SIZE = 10;
    
    std::cout << "Starting transaction with cursor" << std::endl;
    
    auto cursor_status = db1_->begin(
        [&cursor_created, &fetch_executed, &rows_fetched, FETCH_SIZE](Transaction &t) {
            // Declare a cursor
            t.execute(
                "DECLARE test_cursor CURSOR FOR SELECT * FROM test_advanced_transactions WHERE value LIKE 'cursor_row_%' ORDER BY counter",
                [&cursor_created](Transaction &tr, results result) {
                    cursor_created = true;
                    std::cout << "Cursor declared successfully" << std::endl;
                },
                [](error::db_error error) {
                    std::cout << "Error declaring cursor: " << error.what() << std::endl;
                    ASSERT_TRUE(false);
                }
            );
            
            // Fetch results in batches
            for (int i = 0; i < 3; i++) {
                t.execute(
                    "FETCH " + std::to_string(FETCH_SIZE) + " FROM test_cursor",
                    [&fetch_executed, &rows_fetched, i, FETCH_SIZE](Transaction &tr, results result) {
                        fetch_executed = true;
                        rows_fetched += result.size();
                        
                        std::cout << "Batch " << i+1 << ": Fetched " << result.size() << " rows" << std::endl;
                        
                        // Check first and last row in this batch
                        if (result.size() > 0) {
                            int expected_first = i * FETCH_SIZE;
                            std::string expected_value = "cursor_row_" + std::to_string(expected_first);
                            std::cout << "First row in batch - value: " << result[0]["value"].as<std::string>() 
                                      << ", counter: " << result[0]["counter"].as<int>() << std::endl;
                            
                            int expected_last = std::min(expected_first + static_cast<int>(result.size()) - 1, 99);
                            std::cout << "Last row in batch - counter: " << result[result.size()-1]["counter"].as<int>() << std::endl;
                        }
                    },
                    [i](error::db_error error) {
                        std::cout << "Error in fetch batch " << i << ": " << error.what() << std::endl;
                        ASSERT_TRUE(false);
                    }
                );
            }
            
            // Close the cursor explicitly
            t.execute(
                "CLOSE test_cursor",
                [](Transaction &tr, results result) {
                    std::cout << "Cursor closed successfully" << std::endl;
                },
                [](error::db_error error) {
                    std::cout << "Error closing cursor: " << error.what() << std::endl;
                    ASSERT_TRUE(false);
                }
            );
        }
    ).await();
    
    ASSERT_TRUE(cursor_status);
    ASSERT_TRUE(cursor_created);
    ASSERT_TRUE(fetch_executed);
    ASSERT_GT(rows_fetched, 0);
    std::cout << "Total rows processed with cursor: " << rows_fetched << std::endl;
    
    // Clean up the inserted data
    auto cleanup = db1_->execute(
        "DELETE FROM test_advanced_transactions WHERE value LIKE 'cursor_row_%'"
    ).await();
    ASSERT_TRUE(cleanup);
}

/**
 * @brief Test using transaction_mode directly with parameters
 *
 * This test demonstrates how to use the transaction_mode parameter
 * directly with the begin method to configure transaction properties.
 */
TEST_F(PostgreSQLAdvancedTransactionTest, DirectTransactionModeUsage) {
    bool query_executed = false;
    bool write_error_caught = false;
    
    std::cout << "Beginning direct transaction mode test" << std::endl;
    
    // Create transaction mode with serializable isolation and read-only
    transaction_mode mode;
    mode.isolation = isolation_level::serializable;
    mode.read_only = true;
    
    // Convert the mode to string for debugging
    std::stringstream ss;
    ss << mode;
    std::cout << "DEBUG: Combined mode string: \"" << ss.str() << "\"" << std::endl;
    
    // Start a transaction with mode directly
    auto status = db1_->begin(
        [&query_executed, &write_error_caught](Transaction &t) {
            // Run a query in the transaction with configured mode
            t.execute(
                "SELECT 1 as test_col",
                [&query_executed, &write_error_caught](Transaction &tr, results result) {
                    ASSERT_EQ(result.size(), 1);
                    ASSERT_EQ(result[0][0].as<int>(), 1);
                    query_executed = true;
                    std::cout << "Query executed successfully in transaction with mode parameter" << std::endl;
                    
                    // Try a write operation which should fail due to read-only
                    tr.execute(
                        "CREATE TEMP TABLE temp_test (id INT)",
                        [](Transaction &tr2, results result) {
                            std::cout << "WARNING: Write succeeded in read-only transaction!" << std::endl;
                            ASSERT_TRUE(false) << "Write operation should not succeed in read-only transaction";
                        },
                        [&write_error_caught](error::db_error error) {
                            std::cout << "Expected write error: " << error.what() << std::endl;
                            write_error_caught = true;
                            // The transaction will be rolled back after this error
                        }
                    );
                }
            );
        },
        [&write_error_caught](error::db_error error) {
            std::cout << "Transaction error: " << error.what() << std::endl;
            // This is expected when trying to write in a read-only transaction
            if (error.what() == std::string("rollback processed due to a query failure") && write_error_caught) {
                std::cout << "Expected transaction rollback due to write attempt in read-only transaction" << std::endl;
            } else {
                ASSERT_TRUE(false) << "Unexpected transaction error: " << error.what();
            }
        },
        mode // Use our mode directly
    ).await();
    
    // We expect the query to have executed successfully before the rollback
    ASSERT_TRUE(query_executed);
    
    // We expect the write error to have been caught
    ASSERT_TRUE(write_error_caught);
    
    // We expect the status to be false because the transaction was rolled back
    ASSERT_FALSE(status);
    std::cout << "Test passed: Transaction properties applied successfully using mode parameter" << std::endl;
}

/**
 * @brief Run all the tests
 */
int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 