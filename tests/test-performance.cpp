#include <gtest/gtest.h>
#include <qb/io/async.h>
#include <thread>
#include <chrono>
#include <vector>
#include <random>
#include <algorithm>
#include "../pgsql.h"

using namespace qb::pg;

class PostgreSQLPerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_ = std::make_unique<tcp::database>("host=localhost port=5432 dbname=postgres user=postgres password=postgres");
        ASSERT_TRUE(db_->connect());
        
        // Create temporary test tables
        db_->execute(R"(
            CREATE TEMP TABLE test_performance (
                id SERIAL PRIMARY KEY,
                value1 TEXT,
                value2 INTEGER,
                value3 TIMESTAMP,
                value4 JSONB,
                value5 TEXT[]
            )
        )");
        
        // Create indexes
        db_->execute("CREATE INDEX idx_value2 ON test_performance(value2)");
        db_->execute("CREATE INDEX idx_value3 ON test_performance(value3)");
    }

    void TearDown() override {
        if (db_) {
            db_->execute("DROP TABLE IF EXISTS test_performance");
            db_.reset();
        }
    }

    std::unique_ptr<tcp::database> db_;
    
    // Helper function to generate random data
    std::string generate_random_string(size_t length) {
        const std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, chars.size() - 1);
        
        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += chars[dis(gen)];
        }
        return result;
    }
};

TEST_F(PostgreSQLPerformanceTest, BulkInsertPerformance) {
    const int num_rows = 10000;
    auto start = std::chrono::steady_clock::now();
    
    // Prepare statement for better performance
    auto stmt = db_->prepare("INSERT INTO test_performance (value1, value2, value3, value4, value5) VALUES ($1, $2, $3, $4, $5)");
    ASSERT_TRUE(stmt);
    
    // Start transaction
    ASSERT_TRUE(db_->begin());
    
    // Insert data in bulk
    for (int i = 0; i < num_rows; ++i) {
        std::string value1 = generate_random_string(50);
        int value2 = i;
        std::string timestamp = "2024-03-20 12:00:00";
        std::string json = "{\"key\": \"" + generate_random_string(20) + "\"}";
        std::string array = "{" + generate_random_string(10) + "," + generate_random_string(10) + "}";
        
        ASSERT_TRUE(stmt->execute(value1, value2, timestamp, json, array));
    }
    
    // Commit transaction
    ASSERT_TRUE(db_->commit());
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "Bulk insert performance: " << duration.count() << "ms for " << num_rows << " rows" << std::endl;
    ASSERT_LT(duration.count(), 5000); // Should complete within 5 seconds
}

TEST_F(PostgreSQLPerformanceTest, QueryPerformance) {
    // Insert test data
    const int num_rows = 10000;
    auto stmt = db_->prepare("INSERT INTO test_performance (value1, value2, value3, value4, value5) VALUES ($1, $2, $3, $4, $5)");
    ASSERT_TRUE(stmt);
    
    ASSERT_TRUE(db_->begin());
    for (int i = 0; i < num_rows; ++i) {
        ASSERT_TRUE(stmt->execute(
            generate_random_string(50),
            i,
            "2024-03-20 12:00:00",
            "{\"key\": \"value\"}",
            "{\"value1\",\"value2\"}"
        ));
    }
    ASSERT_TRUE(db_->commit());
    
    // Test different query types
    std::vector<std::pair<std::string, std::string>> queries = {
        {"Simple SELECT", "SELECT * FROM test_performance LIMIT 100"},
        {"WHERE clause", "SELECT * FROM test_performance WHERE value2 > 5000"},
        {"ORDER BY", "SELECT * FROM test_performance ORDER BY value2 DESC LIMIT 100"},
        {"JOIN", "SELECT a.*, b.* FROM test_performance a JOIN test_performance b ON a.value2 = b.value2 LIMIT 100"},
        {"Aggregation", "SELECT value2, COUNT(*) FROM test_performance GROUP BY value2"},
        {"Complex query", R"(
            SELECT value2, COUNT(*), AVG(EXTRACT(EPOCH FROM value3))
            FROM test_performance
            WHERE value2 > 5000
            GROUP BY value2
            HAVING COUNT(*) > 10
            ORDER BY value2 DESC
            LIMIT 100
        )"}
    };
    
    for (const auto& query : queries) {
        auto start = std::chrono::steady_clock::now();
        auto result = db_->query(query.second);
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << query.first << " performance: " << duration.count() << "ms" << std::endl;
        ASSERT_LT(duration.count(), 1000); // Should complete within 1 second
    }
}

TEST_F(PostgreSQLPerformanceTest, ConcurrentQueryPerformance) {
    // Insert test data
    const int num_rows = 10000;
    auto stmt = db_->prepare("INSERT INTO test_performance (value1, value2, value3, value4, value5) VALUES ($1, $2, $3, $4, $5)");
    ASSERT_TRUE(stmt);
    
    ASSERT_TRUE(db_->begin());
    for (int i = 0; i < num_rows; ++i) {
        ASSERT_TRUE(stmt->execute(
            generate_random_string(50),
            i,
            "2024-03-20 12:00:00",
            "{\"key\": \"value\"}",
            "{\"value1\",\"value2\"}"
        ));
    }
    ASSERT_TRUE(db_->commit());
    
    // Test concurrent queries
    const int num_threads = 5;
    const int queries_per_thread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> total_queries{0};
    
    auto start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([this, queries_per_thread, &total_queries]() {
            auto thread_db = std::make_unique<tcp::database>("host=localhost port=5432 dbname=postgres user=postgres password=postgres");
            ASSERT_TRUE(thread_db->connect());
            
            for (int j = 0; j < queries_per_thread; ++j) {
                auto result = thread_db->query("SELECT * FROM test_performance WHERE value2 = $1", j);
                total_queries++;
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "Concurrent query performance: " << duration.count() << "ms for " 
              << total_queries << " total queries" << std::endl;
    ASSERT_EQ(total_queries, num_threads * queries_per_thread);
    ASSERT_LT(duration.count(), 10000); // Should complete within 10 seconds
}

TEST_F(PostgreSQLPerformanceTest, TransactionPerformance) {
    const int num_transactions = 1000;
    const int rows_per_transaction = 10;
    
    auto start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < num_transactions; ++i) {
        ASSERT_TRUE(db_->begin());
        
        auto stmt = db_->prepare("INSERT INTO test_performance (value1, value2, value3, value4, value5) VALUES ($1, $2, $3, $4, $5)");
        ASSERT_TRUE(stmt);
        
        for (int j = 0; j < rows_per_transaction; ++j) {
            ASSERT_TRUE(stmt->execute(
                generate_random_string(50),
                i * rows_per_transaction + j,
                "2024-03-20 12:00:00",
                "{\"key\": \"value\"}",
                "{\"value1\",\"value2\"}"
            ));
        }
        
        ASSERT_TRUE(db_->commit());
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "Transaction performance: " << duration.count() << "ms for " 
              << num_transactions << " transactions" << std::endl;
    ASSERT_LT(duration.count(), 30000); // Should complete within 30 seconds
}

TEST_F(PostgreSQLPerformanceTest, PreparedStatementReusePerformance) {
    const int num_executions = 10000;
    
    // Prepare statement once
    auto stmt = db_->prepare("INSERT INTO test_performance (value1, value2, value3, value4, value5) VALUES ($1, $2, $3, $4, $5)");
    ASSERT_TRUE(stmt);
    
    auto start = std::chrono::steady_clock::now();
    
    // Start transaction
    ASSERT_TRUE(db_->begin());
    
    // Execute prepared statement multiple times
    for (int i = 0; i < num_executions; ++i) {
        ASSERT_TRUE(stmt->execute(
            generate_random_string(50),
            i,
            "2024-03-20 12:00:00",
            "{\"key\": \"value\"}",
            "{\"value1\",\"value2\"}"
        ));
    }
    
    ASSERT_TRUE(db_->commit());
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "Prepared statement reuse performance: " << duration.count() << "ms for " 
              << num_executions << " executions" << std::endl;
    ASSERT_LT(duration.count(), 5000); // Should complete within 5 seconds
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 