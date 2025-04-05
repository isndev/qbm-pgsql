#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../pgsql.h"

using namespace qb::pg;

class PerformanceTest : public ::testing::Test {
protected:
    void
    SetUp() override {
        // Configuration pour la connexion à la base de données de test
        std::string connection_string =
            "host=localhost port=5432 dbname=postgres user=postgres password=postgres";

        // Création de la connexion
        db_ = std::make_shared<database<qb::io::transport::tcp>>(connection_string);

        // Se connecter à la base de données
        bool connected = false;
        db_->connect([&connected](auto) { connected = true; }, [](auto) {});

        // Attendre la connexion
        for (int i = 0; i < 100 && !connected; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Vérifier que la connexion a réussi
        ASSERT_TRUE(connected) << "La connexion à PostgreSQL a échoué";

        // Créer la table de test pour les performances
        bool table_created = false;
        auto transaction = db_->create_transaction();
        transaction->execute(
            "DROP TABLE IF EXISTS test_performance",
            [&table_created](auto) { table_created = true; }, [](auto) {});

        // Attendre la suppression de la table
        for (int i = 0; i < 100 && !table_created; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        table_created = false;
        transaction->execute(
            "CREATE TABLE IF NOT EXISTS test_performance ("
            "  id SERIAL PRIMARY KEY,"
            "  value1 TEXT NOT NULL,"
            "  value2 INTEGER NOT NULL,"
            "  value3 FLOAT NOT NULL,"
            "  value4 BOOLEAN NOT NULL,"
            "  value5 TEXT NOT NULL"
            ")",
            [&table_created](auto) { table_created = true; }, [](auto) {});

        // Attendre la création de la table
        for (int i = 0; i < 100 && !table_created; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void
    TearDown() override {
        if (db_) {
            // Supprimer la table de test
            bool cleaned = false;
            auto transaction = db_->create_transaction();
            transaction->execute(
                "DROP TABLE IF EXISTS test_performance", [&cleaned](auto) { cleaned = true; },
                [](auto) {});

            // Attendre la suppression
            for (int i = 0; i < 100 && !cleaned; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Fermer la connexion
            db_->disconnect();
        }
    }

    std::shared_ptr<database<qb::io::transport::tcp>> db_;
};

TEST_F(PerformanceTest, SimpleInsertPerformance) {
    const int num_inserts = 100;

    // Préparer une requête pour l'insertion
    bool prepare_success = false;
    auto transaction = db_->create_transaction();
    transaction->prepare(
        "insert_stmt",
        "INSERT INTO test_performance (value1, value2, value3, value4, value5) VALUES ($1, $2, $3, "
        "$4, $5)",
        {}, [&prepare_success](auto) { prepare_success = true; }, [](auto) {});

    // Attendre la préparation
    for (int i = 0; i < 100 && !prepare_success; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(prepare_success);

    // Démarrer une transaction
    bool begin_success = false;
    transaction->begin([&begin_success](auto) { begin_success = true; }, [](auto) {});

    // Attendre le début de la transaction
    for (int i = 0; i < 100 && !begin_success; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(begin_success);

    // Mesurer le temps d'insertion
    auto start = std::chrono::high_resolution_clock::now();

    // Insérer plusieurs lignes
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> int_dist(1, 1000);
    std::uniform_real_distribution<> float_dist(0.0, 1000.0);
    std::uniform_int_distribution<> bool_dist(0, 1);

    int completed_inserts = 0;
    for (int i = 0; i < num_inserts; ++i) {
        std::string text_value = "Test value " + std::to_string(i);
        int int_value = int_dist(gen);
        float float_value = float_dist(gen);
        bool bool_value = bool_dist(gen) == 1;
        std::string extra_value = "Extra " + std::to_string(i);

        detail::ParamSerializer params;
        params.add_param(text_value);
        params.add_param(int_value);
        params.add_param(float_value);
        params.add_param(bool_value);
        params.add_param(extra_value);

        transaction->execute(
            "insert_stmt", params, [&completed_inserts](auto) { completed_inserts++; },
            [](auto) {});
    }

    // Attendre que toutes les insertions soient terminées
    for (int i = 0; i < 500 && completed_inserts < num_inserts; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(completed_inserts, num_inserts);

    // Commit de la transaction
    bool commit_success = false;
    transaction->commit([&commit_success](auto) { commit_success = true; }, [](auto) {});

    // Attendre le commit
    for (int i = 0; i < 100 && !commit_success; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(commit_success);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "Durée pour " << num_inserts << " insertions: " << duration << " ms" << std::endl;
    std::cout << "Moyenne par insertion: " << (duration / static_cast<double>(num_inserts)) << " ms"
              << std::endl;
}

TEST_F(PerformanceTest, BatchQueryPerformance) {
    const int num_queries = 10;
    const int rows_per_query = 10;

    // Préparer des données pour les tests
    std::vector<std::pair<std::string, std::string>> queries;
    for (int i = 0; i < num_queries; ++i) {
        std::string query =
            "SELECT * FROM generate_series(1, " + std::to_string(rows_per_query) + ")";
        queries.emplace_back("Query " + std::to_string(i), query);
    }

    // Mesurer le temps d'exécution des requêtes en séquence
    auto start = std::chrono::high_resolution_clock::now();

    auto transaction = db_->create_transaction();
    int completed_queries = 0;
    for (const auto &query : queries) {
        transaction->execute(
            query.second, [&completed_queries](auto) { completed_queries++; }, [](auto) {});
    }

    // Attendre que toutes les requêtes soient terminées
    for (int i = 0; i < 100 && completed_queries < num_queries; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(completed_queries, num_queries);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "Durée pour " << num_queries << " requêtes séquentielles: " << duration << " ms"
              << std::endl;
    std::cout << "Moyenne par requête: " << (duration / static_cast<double>(num_queries)) << " ms"
              << std::endl;
}

TEST_F(PerformanceTest, ConcurrentInsertPerformance) {
    const int num_threads = 4;
    const int inserts_per_thread = 25;

    // Insérer des données en utilisant plusieurs threads
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::future<void>> futures;
    for (int t = 0; t < num_threads; ++t) {
        futures.push_back(std::async(std::launch::async, [this, t, inserts_per_thread]() {
            // Créer une nouvelle connexion par thread
            std::string connection_string =
                "host=localhost port=5432 dbname=postgres user=postgres password=postgres";
            auto thread_db = std::make_shared<database<qb::io::transport::tcp>>(connection_string);

            bool connected = false;
            thread_db->connect([&connected](auto) { connected = true; }, [](auto) {});

            // Attendre la connexion
            for (int i = 0; i < 100 && !connected; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            auto transaction = thread_db->create_transaction();

            // Démarrer une transaction
            bool begin_success = false;
            transaction->begin([&begin_success](auto) { begin_success = true; }, [](auto) {});

            // Attendre le début de la transaction
            for (int i = 0; i < 100 && !begin_success; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Préparer une requête pour l'insertion
            bool prepare_success = false;
            transaction->prepare(
                "insert_stmt",
                "INSERT INTO test_performance (value1, value2, value3, value4, value5) VALUES ($1, "
                "$2, $3, $4, $5)",
                {}, [&prepare_success](auto) { prepare_success = true; }, [](auto) {});

            // Attendre la préparation
            for (int i = 0; i < 100 && !prepare_success; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Insérer plusieurs lignes
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> int_dist(1, 1000);
            std::uniform_real_distribution<> float_dist(0.0, 1000.0);
            std::uniform_int_distribution<> bool_dist(0, 1);

            int completed_inserts = 0;
            for (int i = 0; i < inserts_per_thread; ++i) {
                std::string text_value =
                    "Thread " + std::to_string(t) + " Value " + std::to_string(i);
                int int_value = int_dist(gen);
                float float_value = float_dist(gen);
                bool bool_value = bool_dist(gen) == 1;
                std::string extra_value = "Extra " + std::to_string(i);

                detail::ParamSerializer params;
                params.add_param(text_value);
                params.add_param(int_value);
                params.add_param(float_value);
                params.add_param(bool_value);
                params.add_param(extra_value);

                transaction->execute(
                    "insert_stmt", params, [&completed_inserts](auto) { completed_inserts++; },
                    [](auto) {});

                // Pour éviter une surcharge trop rapide
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            // Attendre que toutes les insertions soient terminées
            for (int i = 0; i < 100 && completed_inserts < inserts_per_thread; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Commit de la transaction
            bool commit_success = false;
            transaction->commit([&commit_success](auto) { commit_success = true; }, [](auto) {});

            // Attendre le commit
            for (int i = 0; i < 100 && !commit_success; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Fermer la connexion
            thread_db->disconnect();
        }));
    }

    // Attendre que tous les threads terminent
    for (auto &future : futures) {
        future.wait();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "Durée pour " << num_threads << " threads x " << inserts_per_thread
              << " insertions = " << (num_threads * inserts_per_thread)
              << " insertions totales: " << duration << " ms" << std::endl;
    std::cout << "Moyenne par insertion: "
              << (duration / static_cast<double>(num_threads * inserts_per_thread)) << " ms"
              << std::endl;

    // Vérifier le nombre total d'insertions
    bool query_success = false;
    std::size_t count = 0;

    auto transaction = db_->create_transaction();
    transaction->execute(
        "SELECT COUNT(*) FROM test_performance",
        [&query_success, &count](auto result) {
            query_success = true;
            count = result.template get_value<std::size_t>(0, 0).value_or(0);
        },
        [](auto) {});

    // Attendre la requête
    for (int i = 0; i < 100 && !query_success; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_EQ(count, num_threads * inserts_per_thread);
}

int
main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}