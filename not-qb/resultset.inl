/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#ifndef QBM_PGSQL_NOT_QB_RESULTSET_INL
#define QBM_PGSQL_NOT_QB_RESULTSET_INL

#include "resultset.h"
#include "util/meta_helpers.h"

namespace qb {
namespace pg {

namespace detail {

// Extensions pour la conversion de tuples
namespace resultset_ext {

// Conversion de row vers tuple (implémentation)
template <typename Tuple, std::size_t... I>
inline void 
row_to_impl(resultset::row const& row, Tuple& t, std::index_sequence<I...>) {
    ((std::get<I>(t) = row[I].template as<typename std::tuple_element<I, Tuple>::type>()), ...);
}

// Conversion de row vers tuple 
template <typename... Ts>
inline void 
row_to(resultset::row const& row, std::tuple<Ts...>& t) {
    row_to_impl(row, t, std::index_sequence_for<Ts...>{});
}

// Conversion de row vers tuple de références
template <typename... Ts>
inline void 
row_to(resultset::row const& row, std::tuple<Ts&...> t) {
    row_to_impl(t, row, std::index_sequence_for<Ts...>{});
}

// Version pour la tuple de références
template <typename Tuple, std::size_t... Is>
inline void
row_to_impl(Tuple& t, resultset::row const& row, std::index_sequence<Is...>) {
    ((std::get<Is>(t) = row[Is].template as<std::tuple_element_t<Is, std::remove_reference_t<Tuple>>>()), ...);
}

// Conversion de row vers des paramètres multiples
template <typename... Ts>
inline void 
row_to_multi(resultset::row const& row, Ts&... args) {
    std::tuple<Ts&...> tuple_refs(args...);
    row_to(row, tuple_refs);
}

} // namespace resultset_ext

template <size_t Index, typename T>
struct nth_field {
    enum { index = Index };
    typedef T type;

    nth_field(resultset::row const &r)
        : row(r) {}

    T
    value() {
        return row[index].template as<T>();
    }

    bool
    to(T &val) {
        return row[index].to(val);
    }

    resultset::row row;
};

template <typename IndexTuple, typename... T>
struct row_data_extractor_base;

template <size_t... Indexes, typename... T>
struct row_data_extractor_base<util::indexes_tuple<Indexes...>, T...> {
    static constexpr ::std::size_t size = sizeof...(T);

    static void
    get_tuple(resultset::row const &row, std::tuple<T...> &val) {
        resultset_ext::row_to(row, val);
    }

    static void
    get_values(resultset::row const &row, T &...val) {
        resultset_ext::row_to_multi(row, val...);
    }
};

template <typename... T>
struct row_data_extractor
    : row_data_extractor_base<typename util::index_builder<sizeof...(T)>::type, T...> {};

template <typename IndexTuple, typename... T>
struct field_by_name_extractor;

template <::std::size_t... Indexes, typename... T>
struct field_by_name_extractor<util::indexes_tuple<Indexes...>, T...> {
    static constexpr ::std::size_t size = sizeof...(T);

    static void
    get_tuple(resultset::row const &row,
              ::std::initializer_list<::std::string> const &names,
              ::std::tuple<T...> &val) {
        if (names.size() < size)
            throw error::db_error{"Not enough names in row data extraction"};
        ::std::tuple<T...> tmp(row[*(names.begin() + Indexes)].template as<T>()...);
        tmp.swap(val);
    }

    static void
    get_values(resultset::row const &row,
               ::std::initializer_list<::std::string> const &names, T &...val) {
        util::expand{row[*(names.begin() + Indexes)].to(val)...};
    }
};

template <typename... T>
struct row_data_by_name_extractor
    : field_by_name_extractor<typename util::index_builder<sizeof...(T)>::type, T...> {};

} // namespace detail

template <typename... T>
void
resultset::row::to(std::tuple<T...> &val) const {
    detail::row_data_extractor<T...>::get_tuple(*this, val);
}

template <typename... T>
void
resultset::row::to(std::tuple<T &...> val) const {
    detail::resultset_ext::row_to(*this, val);
}

template <typename... T>
void
resultset::row::to(T &...val) const {
    detail::row_data_extractor<T...>::get_values(*this, val...);
}

template <typename... T>
void
resultset::row::to(::std::initializer_list<::std::string> const &names,
                   ::std::tuple<T...> &val) const {
    detail::row_data_by_name_extractor<T...>::get_tuple(*this, names, val);
}

template <typename... T>
void
resultset::row::to(::std::initializer_list<::std::string> const &names,
                   ::std::tuple<T &...> val) const {
    std::tuple<T...> non_ref;
    detail::row_data_by_name_extractor<T...>::get_tuple(*this, names, non_ref);
    val = non_ref;
}

template <typename... T>
void
resultset::row::to(::std::initializer_list<::std::string> const &names,
                   T &...val) const {
    detail::row_data_by_name_extractor<T...>::get_values(*this, names, val...);
}

} // namespace pg
} // namespace qb

#endif /* QBM_PGSQL_NOT_QB_RESULTSET_INL */
