/**
 *  @author zmij
 *  from project: https://github.com/zmij/pg_async.git
 */

#ifndef QBM_PGSQL_NOT_QB_DATATYPE_MAPPING_H
#define QBM_PGSQL_NOT_QB_DATATYPE_MAPPING_H

#include "protocol_io_traits.h"

namespace qb {
namespace pg {
namespace io {

namespace traits {

template <>
struct cpppg_data_mapping<std::string>
    : detail::data_mapping_base<oids::type::text, std::string> {};

//@{
template <>
struct pgcpp_data_mapping<oids::type::boolean>
    : detail::data_mapping_base<oids::type::boolean, bool> {};
template <>
struct cpppg_data_mapping<bool>
    : detail::data_mapping_base<oids::type::boolean, bool> {};
//@}

//@{
/** @name Integral types */
template <>
struct pgcpp_data_mapping<oids::type::int2>
    : detail::data_mapping_base<oids::type::int2, smallint> {};
template <>
struct cpppg_data_mapping<smallint>
    : detail::data_mapping_base<oids::type::int2, smallint> {};

template <>
struct pgcpp_data_mapping<oids::type::int4>
    : detail::data_mapping_base<oids::type::int4, integer> {};
template <>
struct cpppg_data_mapping<integer>
    : detail::data_mapping_base<oids::type::int4, integer> {};

template <>
struct pgcpp_data_mapping<oids::type::int8>
    : detail::data_mapping_base<oids::type::int8, bigint> {};
template <>
struct cpppg_data_mapping<bigint>
    : detail::data_mapping_base<oids::type::int8, bigint> {};
//@}

//@{
/** @name OID types */
template <>
struct pgcpp_data_mapping<oids::type::oid>
    : detail::data_mapping_base<oids::type::oid, integer> {};
template <>
struct pgcpp_data_mapping<oids::type::tid>
    : detail::data_mapping_base<oids::type::tid, integer> {};
template <>
struct pgcpp_data_mapping<oids::type::xid>
    : detail::data_mapping_base<oids::type::xid, integer> {};
template <>
struct pgcpp_data_mapping<oids::type::cid>
    : detail::data_mapping_base<oids::type::cid, integer> {};
//@}

//@{
/** @name Floating-point types */
template <>
struct pgcpp_data_mapping<oids::type::float4>
    : detail::data_mapping_base<oids::type::float4, float> {};
template <>
struct cpppg_data_mapping<float>
    : detail::data_mapping_base<oids::type::float4, float> {};

template <>
struct pgcpp_data_mapping<oids::type::float8>
    : detail::data_mapping_base<oids::type::float8, double> {};
template <>
struct cpppg_data_mapping<double>
    : detail::data_mapping_base<oids::type::float8, double> {};
//@}

//@{
/** @name UUID types */
//template <>
//struct pgcpp_data_mapping<oids::type::uuid>
//    : detail::data_mapping_base<oids::type::uuid, qb::uuid> {};
//template <>
//struct cpppg_data_mapping<qb::uuid>
//    : detail::data_mapping_base<oids::type::uuid, qb::uuid> {};
//@}

} // namespace traits
} // namespace io
} // namespace pg
} // namespace qb

#endif /* QBM_PGSQL_NOT_QB_DATATYPE_MAPPING_H */
