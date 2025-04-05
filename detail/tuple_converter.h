/**
 * @file tuple_converter.h
 * @brief Convertisseur de tuples PostgreSQL en C++
 * 
 * Fournit des utilitaires pour convertir les lignes PostgreSQL en tuples C++
 * en s'intégrant avec FieldReader.
 */

#pragma once
#ifndef QBM_PGSQL_DETAIL_TUPLE_CONVERTER_H
#define QBM_PGSQL_DETAIL_TUPLE_CONVERTER_H

#include <tuple>
#include <utility>
#include "field_reader.h"
#include "../not-qb/resultset.h"

namespace qb::pg::detail {

/**
 * @brief Utilitaire pour convertir des tuples
 */
template<typename FromTuple, typename ToTuple, std::size_t... Indices>
void convert_tuple_impl(const FromTuple& from, ToTuple& to, std::index_sequence<Indices...>) {
    // Expansion de pack pour convertir chaque élément
    ((std::get<Indices>(to) = std::get<Indices>(from)), ...);
}

/**
 * @brief Convertit un tuple source en tuple destination
 * 
 * @tparam FromTuple Type du tuple source
 * @tparam ToTuple Type du tuple destination
 * @param from Tuple source
 * @param to Tuple destination
 */
template<typename FromTuple, typename ToTuple>
void convert_tuple(const FromTuple& from, ToTuple& to) {
    constexpr std::size_t size = std::min(
        std::tuple_size_v<FromTuple>,
        std::tuple_size_v<ToTuple>
    );
    
    convert_tuple_impl(from, to, std::make_index_sequence<size>{});
}

/**
 * @brief Convertir une ligne PostgreSQL en tuple C++
 * 
 * @tparam Ts Types des éléments du tuple
 * @param row Ligne PostgreSQL
 * @param tuple Tuple destination
 * @return true si la conversion a réussi
 */
template <typename... Ts>
bool row_to_tuple(const resultset::row& row, std::tuple<Ts...>& tuple) {
    // Vérifier que le nombre de colonnes correspond
    if (row.size() < sizeof...(Ts)) {
        return false; // Pas assez de colonnes
    }
    
    return convert_tuple(row, tuple);
}

/**
 * @brief Convertir une ligne PostgreSQL en tuple de références
 * 
 * @tparam Ts Types des éléments du tuple
 * @param row Ligne PostgreSQL
 * @param refs Tuple de références
 * @return true si la conversion a réussi
 */
template <typename... Ts>
bool row_to_refs(const resultset::row& row, std::tuple<Ts&...> refs) {
    // Créer un tuple temporaire et le convertir
    std::tuple<std::remove_reference_t<Ts>...> temp;
    
    if (!row_to_tuple(row, temp)) {
        return false;
    }
    
    // Assigner les valeurs aux références
    (void)std::initializer_list<int>{
        (std::get<Ts&>(refs) = std::get<std::remove_reference_t<Ts>>(temp), 0)...
    };
    
    return true;
}

/**
 * @brief Convertir des champs PostgreSQL individuels en valeurs
 * 
 * @tparam Ts Types des valeurs à extraire
 * @param row Ligne PostgreSQL
 * @param values Valeurs destination
 * @return true si la conversion a réussi
 */
template <typename... Ts>
bool row_to_values(const resultset::row& row, Ts&... values) {
    // Vérifier que le nombre de colonnes correspond
    if (row.size() < sizeof...(Ts)) {
        return false; // Pas assez de colonnes
    }
    
    // Convertir chaque valeur en utilisant une séquence d'index
    std::size_t index = 0;
    return (read_field(row[index++], values) && ...);
}

} // namespace qb::pg::detail

#endif // QBM_PGSQL_DETAIL_TUPLE_CONVERTER_H 