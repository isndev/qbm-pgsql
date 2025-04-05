/**
 * @file data_iterator.h
 * @brief Définition des itérateurs sur les résultats PostgreSQL
 */

#ifndef QBM_PGSQL_NOT_QB_DETAIL_DATA_ITERATOR_H
#define QBM_PGSQL_NOT_QB_DETAIL_DATA_ITERATOR_H

#include <iterator>
#include <cstddef>

namespace qb {
namespace pg {

// Forward declaration
class resultset;

namespace detail {

/**
 * @brief Classe de base pour les itérateurs sur les résultats PostgreSQL
 * 
 * Cette classe fournit un itérateur bidirectionnel pour parcourir les données
 * des résultats PostgreSQL (lignes et champs).
 * 
 * @tparam Derived Type dérivé de cet itérateur
 * @tparam T Type de valeur sur lequel itérer
 */
template <typename Derived, typename T>
class data_iterator {
public:
    typedef std::ptrdiff_t difference_type;
    typedef T value_type;
    typedef T reference;
    typedef Derived pointer;
    typedef std::bidirectional_iterator_tag iterator_category;

    // Constructeurs
    data_iterator() = default;
    
    data_iterator(const data_iterator& other) = default;
    
    data_iterator(data_iterator&& other) = default;
    
    // Constructeur pour les itérateurs de lignes
    data_iterator(const resultset* result, std::size_t row_index)
        : result_(result), row_index_(row_index), field_index_(0) {}
    
    // Constructeur pour les itérateurs de champs
    data_iterator(const resultset* result, std::size_t row_index, std::size_t field_index)
        : result_(result), row_index_(row_index), field_index_(field_index) {}
    
    // Destructeur
    ~data_iterator() = default;
    
    // Opérateurs d'assignation
    data_iterator& operator=(const data_iterator&) = default;
    data_iterator& operator=(data_iterator&&) = default;

    value_type operator*() const;

    Derived& operator++() {
        static_cast<Derived*>(this)->advance(1);
        return *static_cast<Derived*>(this);
    }

    Derived operator++(int) {
        Derived tmp = *static_cast<Derived*>(this);
        ++(*this);
        return tmp;
    }

    Derived& operator--() {
        static_cast<Derived*>(this)->advance(-1);
        return *static_cast<Derived*>(this);
    }

    Derived operator--(int) {
        Derived tmp = *static_cast<Derived*>(this);
        --(*this);
        return tmp;
    }

    bool operator==(const Derived& rhs) const {
        return static_cast<const Derived*>(this)->compare(rhs) == 0;
    }

    bool operator!=(const Derived& rhs) const {
        return !(*this == rhs);
    }

    // Opérateur de conversion booléenne
    explicit operator bool() const {
        return result_ != nullptr;
    }

protected:
    const resultset* result_ = nullptr;
    std::size_t row_index_ = 0;
    std::size_t field_index_ = 0;
    
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
};

} // namespace detail
} // namespace pg
} // namespace qb

#endif // QBM_PGSQL_NOT_QB_DETAIL_DATA_ITERATOR_H 