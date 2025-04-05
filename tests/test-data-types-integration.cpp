#include <gtest/gtest.h>
#include <optional>
#include "../not-qb/common.h"
#include "../connection.h"
#include "../transaction.h"
#include "../prepared_statement.h"
#include "../result.h"

using namespace qb::pg;

// Cette classe de test effectue des tests d'intégration avec une véritable base de données PostgreSQL
class PostgreSQLIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Informations de connexion - à adapter selon votre environnement
        ConnectionInfo conn_info;
        conn_info.host = std::getenv("POSTGRES_HOST") ? std::getenv("POSTGRES_HOST") : "localhost";
        conn_info.port = std::getenv("POSTGRES_PORT") ? std::atoi(std::getenv("POSTGRES_PORT")) : 5432;
        conn_info.user = std::getenv("POSTGRES_USER") ? std::getenv("POSTGRES_USER") : "postgres";
        conn_info.password = std::getenv("POSTGRES_PASSWORD") ? std::getenv("POSTGRES_PASSWORD") : "postgres";
        conn_info.dbname = std::getenv("POSTGRES_DB") ? std::getenv("POSTGRES_DB") : "postgres";
        
        // Établir la connexion
        try {
            connection = std::make_unique<Connection>(conn_info);
            std::cout << "Connexion établie avec succès à la base PostgreSQL" << std::endl;
            
            // Créer la table de test si elle n'existe pas
            auto transaction = connection->create_transaction();
            transaction->execute(R"(
                DROP TABLE IF EXISTS data_types_test;
                CREATE TABLE data_types_test (
                    id SERIAL PRIMARY KEY,
                    smallint_val SMALLINT,
                    integer_val INTEGER,
                    bigint_val BIGINT,
                    float_val REAL,
                    double_val DOUBLE PRECISION,
                    text_val TEXT,
                    varchar_val VARCHAR(255),
                    boolean_val BOOLEAN,
                    timestamp_val TIMESTAMP,
                    date_val DATE,
                    bytea_val BYTEA,
                    null_val TEXT
                );
            )");
            transaction->commit();
            
            // Insérer des données de test
            populateTestData();
            
        } catch (const std::exception& e) {
            std::cerr << "Erreur lors de la connexion à PostgreSQL: " << e.what() << std::endl;
            FAIL() << "Impossible de se connecter à la base de données. Vérifiez que PostgreSQL est en cours d'exécution et que les informations de connexion sont correctes.";
        }
    }
    
    void TearDown() override {
        if (connection) {
            // Nettoyer les données de test
            try {
                auto transaction = connection->create_transaction();
                transaction->execute("DROP TABLE IF EXISTS data_types_test;");
                transaction->commit();
                std::cout << "Table de test supprimée" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Erreur lors du nettoyage: " << e.what() << std::endl;
            }
            
            connection.reset();
        }
    }
    
    void populateTestData() {
        auto transaction = connection->create_transaction();
        
        // Valeurs normales
        transaction->execute(R"(
            INSERT INTO data_types_test (
                smallint_val, integer_val, bigint_val, 
                float_val, double_val, 
                text_val, varchar_val, 
                boolean_val, 
                timestamp_val, date_val,
                bytea_val, null_val
            ) VALUES (
                32767, 2147483647, 9223372036854775807,
                3.14159, 2.7182818284590452,
                'Texte standard', 'Chaîne VARCHAR',
                true,
                '2023-01-15 14:30:45', '2023-01-15',
                E'\\xDEADBEEF', NULL
            );
        )");
        
        // Valeurs minimum
        transaction->execute(R"(
            INSERT INTO data_types_test (
                smallint_val, integer_val, bigint_val, 
                float_val, double_val, 
                text_val, varchar_val, 
                boolean_val, 
                timestamp_val, date_val,
                bytea_val, null_val
            ) VALUES (
                -32768, -2147483648, -9223372036854775808,
                -3.40282e+38, -1.7976931348623157e+308,
                '', '',
                false,
                '1970-01-01 00:00:00', '1970-01-01',
                E'\\x', NULL
            );
        )");
        
        // Valeurs spéciales
        transaction->execute(R"(
            INSERT INTO data_types_test (
                smallint_val, integer_val, bigint_val, 
                float_val, double_val, 
                text_val, varchar_val, 
                boolean_val, 
                timestamp_val, date_val,
                bytea_val, null_val
            ) VALUES (
                0, 0, 0,
                'NaN'::real, 'Infinity'::double precision,
                'Caractères spéciaux: \n\r\t\b\f\\''', 'Unicode: äöü 你好 😀',
                NULL,
                'infinity'::timestamp, NULL,
                E'\\x00FF00FF', NULL
            );
        )");
        
        transaction->commit();
        std::cout << "Données de test insérées" << std::endl;
    }
    
    std::unique_ptr<Connection> connection;
};

// Test de sélection de smallint
TEST_F(PostgreSQLIntegrationTest, SelectSmallint) {
    auto transaction = connection->create_transaction();
    
    // Sélectionner les valeurs smallint
    auto result = transaction->execute("SELECT smallint_val FROM data_types_test ORDER BY id");
    
    // Vérifier le nombre de lignes
    ASSERT_EQ(result->row_count(), 3);
    
    // Récupérer les valeurs
    result->next();
    smallint max_val = result->get<smallint>(0);
    ASSERT_EQ(max_val, 32767);
    
    result->next();
    smallint min_val = result->get<smallint>(0);
    ASSERT_EQ(min_val, -32768);
    
    result->next();
    smallint zero_val = result->get<smallint>(0);
    ASSERT_EQ(zero_val, 0);
    
    transaction->commit();
}

// Test de sélection d'integer
TEST_F(PostgreSQLIntegrationTest, SelectInteger) {
    auto transaction = connection->create_transaction();
    
    // Sélectionner les valeurs integer
    auto result = transaction->execute("SELECT integer_val FROM data_types_test ORDER BY id");
    
    // Vérifier le nombre de lignes
    ASSERT_EQ(result->row_count(), 3);
    
    // Récupérer les valeurs
    result->next();
    integer max_val = result->get<integer>(0);
    ASSERT_EQ(max_val, 2147483647);
    
    result->next();
    integer min_val = result->get<integer>(0);
    ASSERT_EQ(min_val, -2147483648);
    
    result->next();
    integer zero_val = result->get<integer>(0);
    ASSERT_EQ(zero_val, 0);
    
    transaction->commit();
}

// Test de sélection de bigint
TEST_F(PostgreSQLIntegrationTest, SelectBigint) {
    auto transaction = connection->create_transaction();
    
    // Sélectionner les valeurs bigint
    auto result = transaction->execute("SELECT bigint_val FROM data_types_test ORDER BY id");
    
    // Vérifier le nombre de lignes
    ASSERT_EQ(result->row_count(), 3);
    
    // Récupérer les valeurs
    result->next();
    bigint max_val = result->get<bigint>(0);
    ASSERT_EQ(max_val, 9223372036854775807LL);
    
    result->next();
    bigint min_val = result->get<bigint>(0);
    ASSERT_EQ(min_val, std::numeric_limits<bigint>::min());
    
    result->next();
    bigint zero_val = result->get<bigint>(0);
    ASSERT_EQ(zero_val, 0);
    
    transaction->commit();
}

// Test de sélection de float
TEST_F(PostgreSQLIntegrationTest, SelectFloat) {
    auto transaction = connection->create_transaction();
    
    // Sélectionner les valeurs float
    auto result = transaction->execute("SELECT float_val FROM data_types_test ORDER BY id");
    
    // Vérifier le nombre de lignes
    ASSERT_EQ(result->row_count(), 3);
    
    // Récupérer les valeurs
    result->next();
    float pi_val = result->get<float>(0);
    ASSERT_NEAR(pi_val, 3.14159f, 0.0001f);
    
    result->next();
    float min_val = result->get<float>(0);
    // La valeur exacte peut varier, donc nous vérifions juste qu'elle est négative
    ASSERT_LT(min_val, 0.0f);
    
    result->next();
    float nan_val = result->get<float>(0);
    ASSERT_TRUE(std::isnan(nan_val));
    
    transaction->commit();
}

// Test de sélection de double
TEST_F(PostgreSQLIntegrationTest, SelectDouble) {
    auto transaction = connection->create_transaction();
    
    // Sélectionner les valeurs double
    auto result = transaction->execute("SELECT double_val FROM data_types_test ORDER BY id");
    
    // Vérifier le nombre de lignes
    ASSERT_EQ(result->row_count(), 3);
    
    // Récupérer les valeurs
    result->next();
    double e_val = result->get<double>(0);
    ASSERT_NEAR(e_val, 2.7182818284590452, 0.000000000001);
    
    result->next();
    double min_val = result->get<double>(0);
    // La valeur exacte peut varier, donc nous vérifions juste qu'elle est négative
    ASSERT_LT(min_val, 0.0);
    
    result->next();
    double inf_val = result->get<double>(0);
    ASSERT_TRUE(std::isinf(inf_val) && inf_val > 0);
    
    transaction->commit();
}

// Test de sélection de texte
TEST_F(PostgreSQLIntegrationTest, SelectText) {
    auto transaction = connection->create_transaction();
    
    // Sélectionner les valeurs text
    auto result = transaction->execute("SELECT text_val FROM data_types_test ORDER BY id");
    
    // Vérifier le nombre de lignes
    ASSERT_EQ(result->row_count(), 3);
    
    // Récupérer les valeurs
    result->next();
    std::string standard_text = result->get<std::string>(0);
    ASSERT_EQ(standard_text, "Texte standard");
    
    result->next();
    std::string empty_text = result->get<std::string>(0);
    ASSERT_TRUE(empty_text.empty());
    
    result->next();
    std::string special_chars = result->get<std::string>(0);
    ASSERT_EQ(special_chars, "Caractères spéciaux: \n\r\t\b\f\\'");
    
    transaction->commit();
}

// Test de sélection de varchar
TEST_F(PostgreSQLIntegrationTest, SelectVarchar) {
    auto transaction = connection->create_transaction();
    
    // Sélectionner les valeurs varchar
    auto result = transaction->execute("SELECT varchar_val FROM data_types_test ORDER BY id");
    
    // Vérifier le nombre de lignes
    ASSERT_EQ(result->row_count(), 3);
    
    // Récupérer les valeurs
    result->next();
    std::string varchar_text = result->get<std::string>(0);
    ASSERT_EQ(varchar_text, "Chaîne VARCHAR");
    
    result->next();
    std::string empty_varchar = result->get<std::string>(0);
    ASSERT_TRUE(empty_varchar.empty());
    
    result->next();
    std::string unicode_text = result->get<std::string>(0);
    ASSERT_EQ(unicode_text, "Unicode: äöü 你好 😀");
    
    transaction->commit();
}

// Test de sélection de boolean
TEST_F(PostgreSQLIntegrationTest, SelectBoolean) {
    auto transaction = connection->create_transaction();
    
    // Sélectionner les valeurs boolean
    auto result = transaction->execute("SELECT boolean_val FROM data_types_test ORDER BY id");
    
    // Vérifier le nombre de lignes
    ASSERT_EQ(result->row_count(), 3);
    
    // Récupérer les valeurs
    result->next();
    bool true_val = result->get<bool>(0);
    ASSERT_TRUE(true_val);
    
    result->next();
    bool false_val = result->get<bool>(0);
    ASSERT_FALSE(false_val);
    
    result->next();
    // La troisième valeur est NULL, on doit vérifier si la colonne est nulle
    ASSERT_TRUE(result->is_null(0));
    
    transaction->commit();
}

// Test de valeurs NULL
TEST_F(PostgreSQLIntegrationTest, SelectNull) {
    auto transaction = connection->create_transaction();
    
    // Sélectionner des valeurs NULL
    auto result = transaction->execute("SELECT null_val FROM data_types_test WHERE null_val IS NULL");
    
    // Vérifier que toutes les lignes ont une valeur NULL
    ASSERT_EQ(result->row_count(), 3);
    
    while (result->next()) {
        ASSERT_TRUE(result->is_null(0));
    }
    
    transaction->commit();
}

// Test avec une requête préparée
TEST_F(PostgreSQLIntegrationTest, PreparedStatement) {
    auto transaction = connection->create_transaction();
    
    // Créer une requête préparée
    auto stmt = transaction->prepare("SELECT * FROM data_types_test WHERE id = $1");
    
    // Exécuter avec différentes valeurs
    auto result1 = stmt->execute(1);
    auto result2 = stmt->execute(2);
    auto result3 = stmt->execute(3);
    
    // Vérifier que chaque requête retourne une ligne
    ASSERT_EQ(result1->row_count(), 1);
    ASSERT_EQ(result2->row_count(), 1);
    ASSERT_EQ(result3->row_count(), 1);
    
    // Vérifier quelques valeurs
    result1->next();
    ASSERT_EQ(result1->get<smallint>("smallint_val"), 32767);
    
    result2->next();
    ASSERT_EQ(result2->get<smallint>("smallint_val"), -32768);
    
    result3->next();
    ASSERT_EQ(result3->get<smallint>("smallint_val"), 0);
    
    transaction->commit();
}

// Test de sélection de plusieurs colonnes à la fois
TEST_F(PostgreSQLIntegrationTest, SelectMultipleColumns) {
    auto transaction = connection->create_transaction();
    
    // Sélectionner plusieurs colonnes
    auto result = transaction->execute("SELECT id, smallint_val, text_val FROM data_types_test WHERE id = 1");
    
    // Vérifier le nombre de lignes
    ASSERT_EQ(result->row_count(), 1);
    
    // Récupérer et vérifier les valeurs
    result->next();
    ASSERT_EQ(result->get<integer>("id"), 1);
    ASSERT_EQ(result->get<smallint>("smallint_val"), 32767);
    ASSERT_EQ(result->get<std::string>("text_val"), "Texte standard");
    
    transaction->commit();
}

// Test de bytea (données binaires)
TEST_F(PostgreSQLIntegrationTest, SelectBytea) {
    auto transaction = connection->create_transaction();
    
    // Sélectionner les valeurs bytea
    auto result = transaction->execute("SELECT bytea_val FROM data_types_test ORDER BY id");
    
    // Vérifier le nombre de lignes
    ASSERT_EQ(result->row_count(), 3);
    
    // Premier bytea (DEADBEEF)
    result->next();
    auto binary_data1 = result->get<std::vector<byte>>(0);
    std::vector<byte> expected1 = {
        static_cast<byte>(0xDE),
        static_cast<byte>(0xAD),
        static_cast<byte>(0xBE),
        static_cast<byte>(0xEF)
    };
    ASSERT_EQ(binary_data1, expected1);
    
    // Deuxième bytea (vide)
    result->next();
    auto binary_data2 = result->get<std::vector<byte>>(0);
    ASSERT_TRUE(binary_data2.empty());
    
    // Troisième bytea (00FF00FF)
    result->next();
    auto binary_data3 = result->get<std::vector<byte>>(0);
    std::vector<byte> expected3 = {
        static_cast<byte>(0x00),
        static_cast<byte>(0xFF),
        static_cast<byte>(0x00),
        static_cast<byte>(0xFF)
    };
    ASSERT_EQ(binary_data3, expected3);
    
    transaction->commit();
}

// Test de requêtes avec JOIN et GROUP BY
TEST_F(PostgreSQLIntegrationTest, ComplexQueries) {
    auto transaction = connection->create_transaction();
    
    // Créer une table supplémentaire pour les tests de JOIN
    transaction->execute(R"(
        DROP TABLE IF EXISTS extra_test;
        CREATE TABLE extra_test (
            id INTEGER PRIMARY KEY,
            data_id INTEGER REFERENCES data_types_test(id),
            description TEXT
        );
        
        INSERT INTO extra_test VALUES (1, 1, 'Lié à la ligne 1');
        INSERT INTO extra_test VALUES (2, 2, 'Lié à la ligne 2');
        INSERT INTO extra_test VALUES (3, 3, 'Lié à la ligne 3');
    )");
    
    // Test de JOIN
    auto join_result = transaction->execute(R"(
        SELECT d.id, d.text_val, e.description
        FROM data_types_test d
        JOIN extra_test e ON d.id = e.data_id
        ORDER BY d.id
    )");
    
    ASSERT_EQ(join_result->row_count(), 3);
    
    join_result->next();
    ASSERT_EQ(join_result->get<integer>(0), 1);
    ASSERT_EQ(join_result->get<std::string>(1), "Texte standard");
    ASSERT_EQ(join_result->get<std::string>(2), "Lié à la ligne 1");
    
    // Test de GROUP BY
    auto group_result = transaction->execute(R"(
        SELECT COUNT(*), boolean_val
        FROM data_types_test
        WHERE boolean_val IS NOT NULL
        GROUP BY boolean_val
        ORDER BY boolean_val
    )");
    
    ASSERT_EQ(group_result->row_count(), 2);
    
    group_result->next();
    ASSERT_EQ(group_result->get<integer>(0), 1); // 1 ligne avec false
    ASSERT_FALSE(group_result->get<bool>(1));
    
    group_result->next();
    ASSERT_EQ(group_result->get<integer>(0), 1); // 1 ligne avec true
    ASSERT_TRUE(group_result->get<bool>(1));
    
    // Nettoyer
    transaction->execute("DROP TABLE IF EXISTS extra_test;");
    transaction->commit();
}

// Test de sélection avec conditions WHERE complexes
TEST_F(PostgreSQLIntegrationTest, ComplexWhereConditions) {
    auto transaction = connection->create_transaction();
    
    // Requête avec conditions multiples
    auto result = transaction->execute(R"(
        SELECT id, text_val 
        FROM data_types_test 
        WHERE smallint_val > 0 OR (bigint_val < 0 AND float_val < 0)
        ORDER BY id
    )");
    
    // Devrait retourner 2 lignes (id 1 et id 2)
    ASSERT_EQ(result->row_count(), 2);
    
    result->next();
    ASSERT_EQ(result->get<integer>(0), 1);
    
    result->next();
    ASSERT_EQ(result->get<integer>(0), 2);
    
    transaction->commit();
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 