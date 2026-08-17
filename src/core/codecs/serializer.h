#ifndef MOD_DICT_SERIALIZER_H
#define MOD_DICT_SERIALIZER_H

#include "codec_base.h"
#include "../mod_value.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

class ModDict;
class ElasticPool;

namespace Serializer {

// ── Без интернирования (базовый формат) ──────────────────────────────────────
void serialize_value  (std::vector<uint8_t>& buf, const ModValue& val);
void serialize_pyobject(std::vector<uint8_t>& buf, PyObject* obj);
void serialize_dict   (std::vector<uint8_t>& buf, const ModDict*  dict);

ModValue deserialize_value(const uint8_t*& ptr, const uint8_t* end, ElasticPool* pool);
void     deserialize_dict (const uint8_t*  data, size_t len, ModDict* dict);

// ── С интернированием строк (компактный формат) ───────────────────────────────
using StringTable = std::vector<std::string>;
using StringIndex = std::unordered_map<std::string, uint32_t>;

// Сбор всех строк из дерева dict'а
void collect_strings(const ModDict*  dict,  StringIndex& idx, StringTable& table);
void collect_strings_val(const ModValue& val, StringIndex& idx, StringTable& table);

// Сериализация с таблицей строк
void serialize_value_i(std::vector<uint8_t>& buf, const ModValue& val, const StringIndex& si);
void serialize_dict_i (std::vector<uint8_t>& buf, const ModDict*  dict, const StringIndex& si);

// Десериализация с таблицей строк
ModValue deserialize_value_i(const uint8_t*& ptr, const uint8_t* end,
                              const StringTable& st, ElasticPool* pool);
void     deserialize_dict_i (const uint8_t*& ptr, const uint8_t* end,
                              const StringTable& st, ModDict* dict);

// Точки входа (автоматически выбирают формат)
std::vector<uint8_t> serialize_interned(const ModDict* dict);
void                 deserialize_interned(const uint8_t* data, size_t len, ModDict* dict);

// ── WKB geometry deserialize backend preference ───────────────────────────────
// Controls which library a serialized shapely/geoalchemy2 geometry reconstructs
// into on read. name must be "shapely" or "geoalchemy2"; raises (Python
// exception set, returns false) if the name is invalid or that library isn't
// importable. Passing nullptr clears the preference (back to auto-detect).
bool        set_geo_backend(const char* name);
const char* get_geo_backend();  // nullptr if unset

// ── DB row / rowset deserialize backend preference ───────────────────────────
// What a serialized sqlalchemy Row (fetchone/first/one, or each element of
// fetchall/mappings().all()) turns into on read:
//   "row"   (default) sqlalchemy.engine.Row — raises ImportError/RuntimeError
//           if sqlalchemy isn't importable or Row can't be constructed
//   "dict"  {column: value}
//   "tuple" positional values, names dropped
//   "list"  positional values, mutable
// And what a serialized ROWSET (a list of Rows/RowMappings) turns into:
//   "list"  (default) list of whatever set_row_backend() yields per row
//   "tuple" same, as a tuple
//   "dict"  {pk: row} keyed by the primary key found in the row metadata
//           (composite pk -> tuple key); raises if the rowset carries no pk
//   "mod_dict" a ModDict keyed the same way (rows as dicts regardless of
//           set_row_backend — a ModDict row must be a dict)
// Both raise (Python exception set, return false) on an unknown name.
bool        set_row_backend(const char* name);
const char* get_row_backend();
bool        set_rowset_backend(const char* name);
const char* get_rowset_backend();

} // namespace Serializer

#endif
