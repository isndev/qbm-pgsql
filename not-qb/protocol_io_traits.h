/**
 * @file protocol_io_traits.h
 * @brief Traits minimaux pour le protocole PostgreSQL
 */

#ifndef QBM_PGSQL_NOT_QB_PROTOCOL_IO_TRAITS_H
#define QBM_PGSQL_NOT_QB_PROTOCOL_IO_TRAITS_H

#include "common.h"
#include <string>
#include <optional>
#include <type_traits>
#include <limits>
#include <algorithm>
#include "../detail/param_unserializer.h"

namespace qb {
namespace pg {

namespace io {

/**
 * @brief Traits pour les entrées/sorties du protocole PostgreSQL
 */
namespace traits {

/**
 * @brief Vérifie si un type a un parser pour un format donné
 */
template <typename T, protocol_data_format F>
struct has_parser : std::false_type {};

/**
 * @brief Vérifie si un type est nullable
 */
template <typename T>
struct is_nullable : std::false_type {};

/**
 * @brief Spécialisation pour std::optional
 */
template <typename T>
struct is_nullable<std::optional<T>> : std::true_type {};

/**
 * @brief Traits pour les types nullables
 */
template <typename T>
struct nullable_traits {
    static void set_null(T& val) {
        val = T{};
    }
};

/**
 * @brief Spécialisation pour std::optional
 */
template <typename T>
struct nullable_traits<std::optional<T>> {
    static void set_null(std::optional<T>& val) {
        val = std::nullopt;
    }
};

// Spécialisations pour les types de base
template <> struct has_parser<smallint, TEXT_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<smallint, BINARY_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<integer, TEXT_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<integer, BINARY_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<bigint, TEXT_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<bigint, BINARY_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<float, TEXT_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<float, BINARY_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<double, TEXT_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<double, BINARY_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<bool, TEXT_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<bool, BINARY_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<std::string, TEXT_DATA_FORMAT> : std::true_type {};
template <> struct has_parser<std::string, BINARY_DATA_FORMAT> : std::true_type {};

// Spécialisations pour std::optional
template <typename T>
struct has_parser<std::optional<T>, TEXT_DATA_FORMAT> : has_parser<T, TEXT_DATA_FORMAT> {};

template <typename T>
struct has_parser<std::optional<T>, BINARY_DATA_FORMAT> : has_parser<T, BINARY_DATA_FORMAT> {};

// Spécialisations pour les tuples
template <typename... Args>
struct has_parser<std::tuple<Args...>, TEXT_DATA_FORMAT> : std::true_type {};

template <typename... Args>
struct has_parser<std::tuple<Args...>, BINARY_DATA_FORMAT> : std::true_type {};

/**
 * @brief Lecteur de données de base qui utilise le ParamUnserializer
 */
template <typename T>
struct binary_reader {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, T& value) {
        if (std::distance(begin, end) < sizeof(T))
            return begin;
        
        std::vector<byte> buffer(begin, begin + sizeof(T));
        static detail::ParamUnserializer unserializer;
        
        // Cette méthode ne sera pas vraiment utilisée
        // Voir les spécialisations ci-dessous
        return begin + sizeof(T);
    }
};

/**
 * @brief Spécialisation pour smallint
 */
template <>
struct binary_reader<smallint> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, smallint& value) {
        if (std::distance(begin, end) < sizeof(smallint))
            return begin;
        
        std::vector<byte> buffer(begin, begin + sizeof(smallint));
        static detail::ParamUnserializer unserializer;
        value = unserializer.read_smallint(buffer);
        return begin + sizeof(smallint);
    }
};

/**
 * @brief Spécialisation pour integer
 */
template <>
struct binary_reader<integer> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, integer& value) {
        if (std::distance(begin, end) < sizeof(integer))
            return begin;
        
        std::vector<byte> buffer(begin, begin + sizeof(integer));
        static detail::ParamUnserializer unserializer;
        value = unserializer.read_integer(buffer);
        return begin + sizeof(integer);
    }
};

/**
 * @brief Spécialisation pour bigint
 */
template <>
struct binary_reader<bigint> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, bigint& value) {
        if (std::distance(begin, end) < sizeof(bigint))
            return begin;
        
        std::vector<byte> buffer(begin, begin + sizeof(bigint));
        static detail::ParamUnserializer unserializer;
        value = unserializer.read_bigint(buffer);
        return begin + sizeof(bigint);
    }
};

/**
 * @brief Spécialisation pour float
 */
template <>
struct binary_reader<float> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, float& value) {
        if (std::distance(begin, end) < sizeof(float))
            return begin;
        
        std::vector<byte> buffer(begin, begin + sizeof(float));
        static detail::ParamUnserializer unserializer;
        value = unserializer.read_float(buffer);
        return begin + sizeof(float);
    }
};

/**
 * @brief Spécialisation pour double
 */
template <>
struct binary_reader<double> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, double& value) {
        if (std::distance(begin, end) < sizeof(double))
            return begin;
        
        std::vector<byte> buffer(begin, begin + sizeof(double));
        static detail::ParamUnserializer unserializer;
        value = unserializer.read_double(buffer);
        return begin + sizeof(double);
    }
};

/**
 * @brief Spécialisation pour bool
 */
template <>
struct binary_reader<bool> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, bool& value) {
        if (begin == end)
            return begin;
        
        value = (*begin != 0);
        return begin + 1;
    }
};

/**
 * @brief Spécialisation pour std::string
 */
template <>
struct binary_reader<std::string> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, std::string& value) {
        if (begin == end) {
            value.clear();
            return begin;
        }
        
        std::vector<byte> buffer(begin, end);
        static detail::ParamUnserializer unserializer;
        value = unserializer.read_string(buffer);
        return end;
    }
};

/**
 * @brief Lecteur de données textuelles
 */
template <typename T>
struct text_reader {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, T& value) {
        // Version par défaut - ne fait rien
        return begin;
    }
};

/**
 * @brief Spécialisation pour smallint
 */
template <>
struct text_reader<smallint> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, smallint& value) {
        // Pour le format TEXT, nous cherchons une chaîne terminée par \0
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        try {
            value = std::stoi(text_value);
            return null_terminator + 1; // Sauter le \0
        } catch (const std::exception&) {
            return begin;
        }
    }
};

/**
 * @brief Spécialisation pour integer
 */
template <>
struct text_reader<integer> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, integer& value) {
        // Pour le format TEXT, nous cherchons une chaîne terminée par \0
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        try {
            value = std::stoi(text_value);
            return null_terminator + 1; // Sauter le \0
        } catch (const std::exception&) {
            return begin;
        }
    }
};

/**
 * @brief Spécialisation pour bigint
 */
template <>
struct text_reader<bigint> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, bigint& value) {
        // Pour le format TEXT, nous cherchons une chaîne terminée par \0
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        try {
            value = std::stoll(text_value);
            return null_terminator + 1; // Sauter le \0
        } catch (const std::exception&) {
            return begin;
        }
    }
};

/**
 * @brief Spécialisation pour float
 */
template <>
struct text_reader<float> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, float& value) {
        // Pour le format TEXT, nous cherchons une chaîne terminée par \0
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        try {
            value = std::stof(text_value);
            return null_terminator + 1; // Sauter le \0
        } catch (const std::exception&) {
            return begin;
        }
    }
};

/**
 * @brief Spécialisation pour double
 */
template <>
struct text_reader<double> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, double& value) {
        // Pour le format TEXT, nous cherchons une chaîne terminée par \0
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        try {
            value = std::stod(text_value);
            return null_terminator + 1; // Sauter le \0
        } catch (const std::exception&) {
            return begin;
        }
    }
};

/**
 * @brief Spécialisation pour bool
 */
template <>
struct text_reader<bool> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, bool& value) {
        // Pour le format TEXT, nous cherchons une chaîne terminée par \0
        InputIterator null_terminator = std::find(begin, end, '\0');
        if (null_terminator == end)
            return begin;
        
        std::string text_value(begin, null_terminator);
        
        // PostgreSQL utilise 'true'/'false' ou 't'/'f'
        value = (text_value == "true" || text_value == "t" || text_value == "1" || text_value == "yes" || text_value == "y");
        return null_terminator + 1; // Sauter le \0
    }
};

/**
 * @brief Spécialisation pour std::string
 */
template <>
struct text_reader<std::string> {
    template <typename InputIterator>
    static InputIterator read(InputIterator begin, InputIterator end, std::string& value) {
        // Pour le format TEXT, nous cherchons une chaîne terminée par \0
        InputIterator null_terminator = std::find(begin, end, '\0');
        
        value.assign(begin, null_terminator);
        
        if (null_terminator == end)
            return end;
        
        return null_terminator + 1; // Sauter le \0
    }
};

} // namespace traits

/**
 * @brief Fonction de lecture principale pour le protocole PostgreSQL
 * 
 * Cette fonction est un point d'entrée qui sélectionne la bonne implémentation
 * en fonction du format et du type.
 * 
 * @tparam F Format de données (TEXT ou BINARY)
 * @tparam T Type à lire
 * @tparam InputIterator Type d'itérateur d'entrée
 * @param begin Itérateur de début
 * @param end Itérateur de fin
 * @param value Valeur à remplir
 * @return InputIterator Nouvel itérateur de début (après la lecture)
 */
template <protocol_data_format F, typename T, typename InputIterator>
InputIterator protocol_read(InputIterator begin, InputIterator end, T &value) {
    if (begin == end)
        return begin;
        
    if constexpr (F == BINARY_DATA_FORMAT) {
        return traits::binary_reader<T>::read(begin, end, value);
    } else { // TEXT_DATA_FORMAT
        return traits::text_reader<T>::read(begin, end, value);
    }
}

} // namespace io
} // namespace pg
} // namespace qb

#endif // QBM_PGSQL_NOT_QB_PROTOCOL_IO_TRAITS_H
