/**
 * @file pg_types.cpp
 * @brief Implémentation des types PostgreSQL
 */

#include "pg_types.h"
#include <iostream>
#include <map>
#include <string>

namespace qb {
namespace pg {

namespace {
// Mapping entre oid et string pour faciliter la conversion
const std::map<oid, std::string> OID_TO_STRING{
    {oid::boolean, "boolean"},
    {oid::bytea, "bytea"},
    {oid::char_, "char"},
    {oid::name, "name"},
    {oid::int8, "int8"},
    {oid::int2, "int2"},
    {oid::int2_vector, "int2_vector"},
    {oid::int4, "int4"},
    {oid::regproc, "regproc"},
    {oid::text, "text"},
    {oid::oid_t, "oid"},
    {oid::tid, "tid"},
    {oid::xid, "xid"},
    {oid::cid, "cid"},
    {oid::oid_vector, "oid_vector"},
    {oid::json, "json"},
    {oid::xml, "xml"},
    {oid::pg_node_tree, "pg_node_tree"},
    {oid::pg_ddl_command, "pg_ddl_command"},
    {oid::point, "point"},
    {oid::lseg, "lseg"},
    {oid::path, "path"},
    {oid::box, "box"},
    {oid::polygon, "polygon"},
    {oid::line, "line"},
    {oid::float4, "float4"},
    {oid::float8, "float8"},
    {oid::abstime, "abstime"},
    {oid::reltime, "reltime"},
    {oid::tinterval, "tinterval"},
    {oid::unknown, "unknown"},
    {oid::circle, "circle"},
    {oid::cash, "cash"},
    {oid::macaddr, "macaddr"},
    {oid::inet, "inet"},
    {oid::cidr, "cidr"},
    {oid::int2_array, "int2_array"},
    {oid::int4_array, "int4_array"},
    {oid::text_array, "text_array"},
    {oid::oid_array, "oid_array"},
    {oid::float4_array, "float4_array"},
    {oid::acl_item, "acl_item"},
    {oid::cstring_array, "cstring_array"},
    {oid::bpchar, "bpchar"},
    {oid::varchar, "varchar"},
    {oid::date, "date"},
    {oid::time, "time"},
    {oid::timestamp, "timestamp"},
    {oid::timestamptz, "timestamptz"},
    {oid::interval, "interval"},
    {oid::timetz, "timetz"},
    {oid::bit, "bit"},
    {oid::varbit, "varbit"},
    {oid::numeric, "numeric"},
    {oid::refcursor, "refcursor"},
    {oid::regprocedure, "regprocedure"},
    {oid::regoper, "regoper"},
    {oid::regoperator, "regoperator"},
    {oid::regclass, "regclass"},
    {oid::regtype, "regtype"},
    {oid::regrole, "regrole"},
    {oid::regtypearray, "regtypearray"},
    {oid::uuid, "uuid"},
    {oid::lsn, "lsn"},
    {oid::tsvector, "tsvector"},
    {oid::gtsvector, "gtsvector"},
    {oid::tsquery, "tsquery"},
    {oid::regconfig, "regconfig"},
    {oid::regdictionary, "regdictionary"},
    {oid::jsonb, "jsonb"},
    {oid::int4_range, "int4_range"},
    {oid::record, "record"},
    {oid::record_array, "record_array"},
    {oid::cstring, "cstring"},
    {oid::any, "any"},
    {oid::any_array, "any_array"},
    {oid::void_, "void"},
    {oid::trigger, "trigger"},
    {oid::evttrigger, "evttrigger"},
    {oid::language_handler, "language_handler"},
    {oid::internal, "internal"},
    {oid::opaque, "opaque"},
    {oid::any_element, "any_element"},
    {oid::any_non_array, "any_non_array"},
    {oid::any_enum, "any_enum"},
    {oid::fdw_handler, "fdw_handler"},
    {oid::any_range, "any_range"},
};

// Mapping inverse pour la conversion string vers oid
const std::map<std::string, oid> STRING_TO_OID{
    {"boolean", oid::boolean},
    {"bytea", oid::bytea},
    {"char", oid::char_},
    {"name", oid::name},
    {"int8", oid::int8},
    {"int2", oid::int2},
    {"int2_vector", oid::int2_vector},
    {"int4", oid::int4},
    {"regproc", oid::regproc},
    {"text", oid::text},
    {"oid", oid::oid_t},
    {"tid", oid::tid},
    {"xid", oid::xid},
    {"cid", oid::cid},
    {"oid_vector", oid::oid_vector},
    {"json", oid::json},
    {"xml", oid::xml},
    {"pg_node_tree", oid::pg_node_tree},
    {"pg_ddl_command", oid::pg_ddl_command},
    {"point", oid::point},
    {"lseg", oid::lseg},
    {"path", oid::path},
    {"box", oid::box},
    {"polygon", oid::polygon},
    {"line", oid::line},
    {"float4", oid::float4},
    {"float8", oid::float8},
    {"abstime", oid::abstime},
    {"reltime", oid::reltime},
    {"tinterval", oid::tinterval},
    {"unknown", oid::unknown},
    {"circle", oid::circle},
    {"cash", oid::cash},
    {"macaddr", oid::macaddr},
    {"inet", oid::inet},
    {"cidr", oid::cidr},
    {"int2_array", oid::int2_array},
    {"int4_array", oid::int4_array},
    {"text_array", oid::text_array},
    {"oid_array", oid::oid_array},
    {"float4_array", oid::float4_array},
    {"acl_item", oid::acl_item},
    {"cstring_array", oid::cstring_array},
    {"bpchar", oid::bpchar},
    {"varchar", oid::varchar},
    {"date", oid::date},
    {"time", oid::time},
    {"timestamp", oid::timestamp},
    {"timestamptz", oid::timestamptz},
    {"interval", oid::interval},
    {"timetz", oid::timetz},
    {"bit", oid::bit},
    {"varbit", oid::varbit},
    {"numeric", oid::numeric},
    {"refcursor", oid::refcursor},
    {"regprocedure", oid::regprocedure},
    {"regoper", oid::regoper},
    {"regoperator", oid::regoperator},
    {"regclass", oid::regclass},
    {"regtype", oid::regtype},
    {"regrole", oid::regrole},
    {"regtypearray", oid::regtypearray},
    {"uuid", oid::uuid},
    {"lsn", oid::lsn},
    {"tsvector", oid::tsvector},
    {"gtsvector", oid::gtsvector},
    {"tsquery", oid::tsquery},
    {"regconfig", oid::regconfig},
    {"regdictionary", oid::regdictionary},
    {"jsonb", oid::jsonb},
    {"int4_range", oid::int4_range},
    {"record", oid::record},
    {"record_array", oid::record_array},
    {"cstring", oid::cstring},
    {"any", oid::any},
    {"any_array", oid::any_array},
    {"void", oid::void_},
    {"trigger", oid::trigger},
    {"evttrigger", oid::evttrigger},
    {"language_handler", oid::language_handler},
    {"internal", oid::internal},
    {"opaque", oid::opaque},
    {"any_element", oid::any_element},
    {"any_non_array", oid::any_non_array},
    {"any_enum", oid::any_enum},
    {"fdw_handler", oid::fdw_handler},
    {"any_range", oid::any_range},
};

} // namespace

// Opérateur de sortie pour oid
std::ostream& operator<<(std::ostream& out, oid val) {
    std::ostream::sentry s(out);
    if (s) {
        auto it = OID_TO_STRING.find(val);
        if (it != OID_TO_STRING.end()) {
            out << it->second;
        } else {
            out << "oid_" << static_cast<std::underlying_type_t<oid>>(val);
        }
    }
    return out;
}

// Opérateur d'entrée pour oid
std::istream& operator>>(std::istream& in, oid& val) {
    std::istream::sentry s(in);
    if (s) {
        std::string name;
        if (in >> name) {
            auto it = STRING_TO_OID.find(name);
            if (it != STRING_TO_OID.end()) {
                val = it->second;
            } else {
                in.setstate(std::ios_base::failbit);
            }
        }
    }
    return in;
}

} // namespace pg
} // namespace qb
