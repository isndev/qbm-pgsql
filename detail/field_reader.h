/**
 * @file field_reader.h
 * @brief Lecteur direct de champs PostgreSQL
 * 
 * Un lecteur direct pour convertir les champs PostgreSQL en types C++
 * sans dépendre de protocol_io_traits.
 */

#pragma once
#ifndef QBM_PGSQL_DETAIL_FIELD_READER_H
#define QBM_PGSQL_DETAIL_FIELD_READER_H

#include <string>
#include <optional>
#include <type_traits>
#include "../not-qb/pg_types.h"
#include "param_unserializer.h"

namespace qb::pg {
namespace detail {

/**
 * @brief Lecteur de champs PostgreSQL
 * 
 * Fournit des fonctions pour lire les champs PostgreSQL
 * et les convertir en types C++.
 */
class FieldReader {
public:
    /**
     * @brief Lit un tampon de données et le convertit en valeur
     * 
     * @tparam T Type de la valeur à lire
     * @param is_null Indique si le champ est null
     * @param buffer Tampon contenant les données
     * @param value Référence vers la valeur à remplir
     * @return true si la lecture a réussi
     */
    template<typename T>
    static bool read_buffer(bool is_null, const std::vector<byte>& buffer, T& value) {
        if (is_null) {
            // Pour les types optional, on peut gérer NULL facilement
            if constexpr (std::is_same_v<T, std::optional<typename T::value_type>>) {
                value.reset();
                return true;
            } else {
                return false; // Valeur NULL pour un type non nullable
            }
        }

        // Pour un type simple, utiliser ParamUnserializer
        // pour lire la valeur depuis le tampon binaire
        return read_value(buffer, value);
    }

private:
    // Désérialiseur utilisé pour la lecture des données
    static ParamUnserializer unserializer;

    /**
     * @brief Lit une valeur à partir d'un tampon
     * 
     * @tparam T Type de la valeur
     * @param buffer Tampon contenant les données
     * @param value Valeur à remplir
     * @return true si la lecture a réussi
     */
    template<typename T>
    static bool read_value(const std::vector<byte>& buffer, T& value) {
        // Par défaut, retourne false pour les types non supportés
        return false;
    }

    // Spécialisations pour les types de base
    static bool read_value(const std::vector<byte>& buffer, smallint& value) {
        try {
            value = unserializer.read_smallint(buffer);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    static bool read_value(const std::vector<byte>& buffer, integer& value) {
        try {
            value = unserializer.read_integer(buffer);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    static bool read_value(const std::vector<byte>& buffer, bigint& value) {
        try {
            value = unserializer.read_bigint(buffer);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    static bool read_value(const std::vector<byte>& buffer, float& value) {
        try {
            value = unserializer.read_float(buffer);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    static bool read_value(const std::vector<byte>& buffer, double& value) {
        try {
            value = unserializer.read_double(buffer);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    static bool read_value(const std::vector<byte>& buffer, bool& value) {
        try {
            // Pour un booléen, on a juste un octet avec 0 ou 1
            if (buffer.size() >= 1) {
                value = (buffer[0] != 0);
                return true;
            }
            return false;
        } catch (const std::exception&) {
            return false;
        }
    }

    static bool read_value(const std::vector<byte>& buffer, std::string& value) {
        try {
            value = unserializer.read_string(buffer);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    // Support pour les types optionnels
    template<typename T>
    static bool read_value(const std::vector<byte>& buffer, std::optional<T>& value) {
        T temp_value;
        if (read_value(buffer, temp_value)) {
            value = std::move(temp_value);
            return true;
        }
        value.reset();
        return false;
    }
};

/**
 * @brief Initialise le lecteur de champs
 */
void initialize_field_reader();

} // namespace detail
} // namespace qb::pg

#endif // QBM_PGSQL_DETAIL_FIELD_READER_H 