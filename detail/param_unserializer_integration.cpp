/**
 * @file param_unserializer_integration.cpp
 * @brief Intégration du deserializer moderne avec le code existant
 * 
 * Ce fichier fournit l'intégration entre notre système moderne
 * de désérialisation et le code existant sans introduire de conflits.
 */

#include "param_unserializer.h"
#include "tuple_converter.h"
#include "../not-qb/resultset.h"

namespace qb::pg::detail {

// Wrapper pour convertir une resultset::row en tuples
template <typename Tuple>
void tuple_conversion_wrapper(const resultset::row& row, Tuple& tuple) {
    direct_row_to_tuple(row, tuple);
}

// Explicitization des templates pour les types communs
// Cela garantit que les templates seront générés lors de la compilation
template void tuple_conversion_wrapper<std::tuple<int>>(
    const resultset::row&, std::tuple<int>&);
    
template void tuple_conversion_wrapper<std::tuple<std::string>>(
    const resultset::row&, std::tuple<std::string>&);
    
template void tuple_conversion_wrapper<std::tuple<int, std::string>>(
    const resultset::row&, std::tuple<int, std::string>&);
    
template void tuple_conversion_wrapper<std::tuple<std::string, int>>(
    const resultset::row&, std::tuple<std::string, int>&);
    
template void tuple_conversion_wrapper<std::tuple<int, int>>(
    const resultset::row&, std::tuple<int, int>&);
    
template void tuple_conversion_wrapper<std::tuple<std::string, std::string>>(
    const resultset::row&, std::tuple<std::string, std::string>&);
    
template void tuple_conversion_wrapper<std::tuple<int, int, int>>(
    const resultset::row&, std::tuple<int, int, int>&);
    
template void tuple_conversion_wrapper<std::tuple<std::string, std::string, std::string>>(
    const resultset::row&, std::tuple<std::string, std::string, std::string>&);

/**
 * @brief Initialise le système de désérialisation
 * 
 * Cette fonction est appelée lors de l'initialisation du module
 * pour s'assurer que notre désérialiseur moderne est prêt à être utilisé.
 */
void initialize_param_unserializer() {
    // Initialisation du ParamUnserializer
    // (Aucune initialisation spécifique nécessaire pour le moment)
}

} // namespace qb::pg::detail

namespace qb::pg {

// Méthodes d'extension pour la classe resultset::row utilisant notre deserializer moderne

template<typename... T>
void resultset::row::to(std::tuple<T...>& tuple) const {
    // Utiliser directement notre convertisseur moderne, sans passer par protocol_parser
    detail::direct_row_to_tuple(*this, tuple);
}

template<typename... T>
void resultset::row::to(std::tuple<T&...> tuple) const {
    // Utiliser directement notre convertisseur moderne, sans passer par protocol_parser
    detail::direct_row_to_tuple(*this, tuple);
}

// Instanciation explicite pour les types de tuples courants
// Cela garantit que le compilateur génère le code pour ces cas courants
template void resultset::row::to(std::tuple<int>& tuple) const;
template void resultset::row::to(std::tuple<std::string>& tuple) const;
template void resultset::row::to(std::tuple<int, std::string>& tuple) const;
template void resultset::row::to(std::tuple<std::string, int>& tuple) const;
template void resultset::row::to(std::tuple<int, int>& tuple) const;
template void resultset::row::to(std::tuple<std::string, std::string>& tuple) const;
template void resultset::row::to(std::tuple<int, int, int>& tuple) const;
template void resultset::row::to(std::tuple<std::string, std::string, std::string>& tuple) const;
template void resultset::row::to(std::tuple<int, std::string, int>& tuple) const;
template void resultset::row::to(std::tuple<std::string, int, std::string>& tuple) const;

// Instanciation explicite pour les types de tuples de référence courants
template void resultset::row::to(std::tuple<int&>& tuple) const;
template void resultset::row::to(std::tuple<std::string&>& tuple) const;
template void resultset::row::to(std::tuple<int&, std::string&>& tuple) const;
template void resultset::row::to(std::tuple<std::string&, int&>& tuple) const;
template void resultset::row::to(std::tuple<int&, int&>& tuple) const;
template void resultset::row::to(std::tuple<std::string&, std::string&>& tuple) const;
template void resultset::row::to(std::tuple<int&, int&, int&>& tuple) const;
template void resultset::row::to(std::tuple<std::string&, std::string&, std::string&>& tuple) const;
template void resultset::row::to(std::tuple<int&, std::string&, int&>& tuple) const;
template void resultset::row::to(std::tuple<std::string&, int&, std::string&>& tuple) const;

} // namespace qb::pg 