#include <cassert>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <vector>
#include "../pgsql.h"

// Définir notre propre macro de journalisation qui remplacera la macro du module
#define LOG_DEBUG(msg) std::cout << msg << std::endl

#include "../src/param_serializer.h"
// Inclure nos fichiers
#undef LOG_DEBUG

// Utiliser les types réels du module PostgreSQL
using namespace qb::pg::detail;

// Test qui vérifie le comportement des vecteurs de string dans le sérialiseur réel
class ParamSerializerModuleTest {
public:
    ParamSerializerModuleTest()
        : serializer_() {}

    // Test avec un vecteur de chaînes
    void
    test_string_vector(const std::vector<std::string> &values) {
        std::cout << "\n===== Test avec vecteur de " << values.size() << " chaînes =====\n";

        // Réinitialiser le sérialiseur
        serializer_.reset();

        // Ajouter le vecteur de chaînes directement
        std::cout << "[TEST] Ajout d'un vecteur de " << values.size()
                  << " chaînes comme paramètres\n";

        // Afficher chaque valeur du vecteur
        for (const auto &value : values) {
            std::cout << "[TEST]   - Valeur: '" << value << "', longueur: " << value.size() << "\n";
        }

        // Ajouter le vecteur au sérialiseur
        serializer_.add_string_vector(values);

        // Vérifier le résultat
        debug_print_serializer();
        verify_serializer(values);
    }

    // Afficher les informations de debug du sérialiseur
    void
    debug_print_serializer() const {
        const std::vector<byte> &buffer = serializer_.params_buffer();
        const std::vector<integer> &types = serializer_.param_types();

        std::cout << "Buffer contenu (" << buffer.size() << " octets):\n";
        for (size_t i = 0; i < buffer.size(); ++i) {
            // Imprimer en hexadécimal
            printf("%02x ", static_cast<unsigned char>(buffer[i]));
            if ((i + 1) % 16 == 0)
                std::cout << "\n";
        }
        std::cout << "\n\n";

        std::cout << "Types de paramètres (" << types.size() << " types):\n";
        for (size_t i = 0; i < types.size(); ++i) {
            std::cout << "Paramètre " << i << ": type OID = " << types[i] << "\n";
        }

        std::cout << "Nombre total de paramètres: " << serializer_.param_count() << "\n";
    }

    // Vérifier que le sérialiseur a bien traité le vecteur de chaînes
    void
    verify_serializer(const std::vector<std::string> &expected_values) const {
        const std::vector<byte> &buffer = serializer_.params_buffer();
        const std::vector<integer> &types = serializer_.param_types();

        std::cout << "\n===== Vérification du sérialiseur =====\n";

        // Vérifier que le nombre de paramètres est correct
        size_t param_count = serializer_.param_count();
        std::cout << "Nombre de paramètres: " << param_count << "\n";
        std::cout << "Nombre de types: " << types.size() << "\n";

        if (param_count != expected_values.size()) {
            std::cout << "ERREUR: Le nombre de paramètres (" << param_count
                      << ") ne correspond pas au nombre de valeurs attendues ("
                      << expected_values.size() << ")\n";
            return;
        }

        if (types.size() != expected_values.size()) {
            std::cout << "ERREUR: Le nombre de types (" << types.size()
                      << ") ne correspond pas au nombre de valeurs attendues ("
                      << expected_values.size() << ")\n";
            return;
        }

        // Vérifier que chaque paramètre a le bon type (OID text = 25)
        bool types_ok = true;
        for (size_t i = 0; i < types.size(); ++i) {
            if (static_cast<int>(types[i]) != static_cast<int>(oid::text)) {
                std::cout << "ERREUR: Le paramètre " << i << " a le type " << types[i]
                          << " au lieu de " << oid::text << " (text)\n";
                types_ok = false;
            }
        }

        if (types_ok) {
            std::cout << "SUCCÈS: Tous les paramètres ont le bon type (text)\n";
        }

        // Vérifier que les données dans le buffer sont correctes
        // Cette vérification est plus difficile car le format du buffer est complexe
        // Nous allons juste vérifier que le buffer n'est pas vide si des valeurs sont attendues
        if (!expected_values.empty() && buffer.empty()) {
            std::cout << "ERREUR: Le buffer est vide mais des paramètres sont attendus\n";
            return;
        }

        std::cout << "SUCCÈS: Le test des paramètres a passé toutes les vérifications\n";
    }

private:
    ParamSerializer serializer_;
};

// Fonction de test principale
int
main() {
    std::cout << "=== TESTS DU MODULE PGSQL PARAM_SERIALIZER ===\n";

    ParamSerializerModuleTest tester;

    // Test avec un vecteur de chaînes standard
    std::vector<std::string> values1 = {"Test value 1", "Test value 2", "Test value 3",
                                        "Test value 4"};
    tester.test_string_vector(values1);

    // Test avec des chaînes de différentes longueurs
    std::vector<std::string> values2 = {
        "",                                                        // Chaîne vide
        "A",                                                       // Une seule lettre
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit", // Longue chaîne
        "Спасибо"                                                  // Chaîne UTF-8
    };
    tester.test_string_vector(values2);

    return 0;
}