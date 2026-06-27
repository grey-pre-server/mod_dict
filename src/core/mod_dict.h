#ifndef MOD_DICT_MOD_DICT_H
#define MOD_DICT_MOD_DICT_H

#include "mod_value.h"
#include "flat_hash_map.h"
#include <Python.h>
#include <string>
#include <vector>
#include <cstring>

enum class MergeConflict { KEEP_LEFT, KEEP_RIGHT, MERGE, CONCAT };
enum class FilterOp      { EQ, NE, LT, LE, GT, GE };

// ──────────────────────────────────────────────────────────────────────────────
// OuterEntry — one record in the outer FlatHashMap.
//   key_py  = the outer key Python object (Py_INCREF'd)
//   val_py  = scalar PyObject* or deep-copied row dict (Py_INCREF'd)
//   is_row  = true → val_py is a Python dict (the stored row)
//
// Lifecycle is manual:
//   insert new: Py_INCREF key_py + val_py before outer.insert
//   replace:    Py_XDECREF old refs, Py_INCREF new refs, mutate in place
//   erase:      Py_XDECREF refs, then outer.erase
//   ~ModDict:   Py_XDECREF all occupied entries
//
// NO destructor — FlatHashMap resize() memcpy's entries; a destructor on
// OuterEntry would double-Py_DECREF after the copy.
// ──────────────────────────────────────────────────────────────────────────────
struct OuterEntry {
    PyObject* key_py   = nullptr;
    PyObject* val_py   = nullptr;
    bool      is_row   = false;
    bool      is_alias = false;
    uint64_t  orig_hash  = 0;  // set on alias entry: points to original
    uint64_t  alias_hash = 0;  // set on original entry: points to its alias (0 = none)
};

class ModDict {
public:
    FlatHashMap<uint64_t, OuterEntry> outer;

    struct {
        FlatHashMap<std::string, class FieldIndex*> by_field;
    } indices;

    PyObject* on_change_cb = nullptr;
    PyObject* on_merge_cb  = nullptr;
    struct ModDictObject* py_wrapper = nullptr;

    // ──────────────────────────────────────────────────
    // Constructors
    // ──────────────────────────────────────────────────
    ModDict();
    ~ModDict();

    // ──────────────────────────────────────────────────
    // Insert
    // ──────────────────────────────────────────────────
    bool insert(const ModValue& key, const ModValue& value);
    void insert_row(const ModValue& outer_key, PyObject* dict_obj);

    // ──────────────────────────────────────────────────
    // Read
    // ──────────────────────────────────────────────────
    bool   contains(const ModValue& key) const;
    size_t len() const {
        size_t n = 0;
        for (auto& e : outer.occupied()) if (!e.value.is_alias) ++n;
        return n;
    }
    bool   empty() const { return len() == 0; }

    // Full row → Py_INCREF'd dict, O(1).
    PyObject* get_row(uint64_t outer_hash) const;

    // Sub-dict or scalar at path. path = "\x01"-joined segments.
    PyObject* get_subrow(uint64_t outer_hash, const std::string& path) const;

    // Borrowed ref to the stored row dict (for FieldIndex, merges). No Py_INCREF.
    PyObject* get_row_ref(uint64_t outer_hash) const;

    bool remove(const ModValue& key);
    void reindex_row(uint64_t outer_hash);

    // ──────────────────────────────────────────────────
    // Operations
    // ──────────────────────────────────────────────────
    int merges(ModDict* target,
               const std::vector<const char*>& on_source,
               const std::vector<const char*>& on_target,
               MergeConflict conflict = MergeConflict::KEEP_LEFT);

    ModDict* filter(const std::string& field,
                    FilterOp op, const ModValue& value) const;
    ModDict* filter(const std::vector<std::string>& pattern,
                    FilterOp op, const ModValue& value) const;

    using SortResult  = std::vector<PyObject*>;
    SortResult sort_by(const std::string& field, bool reverse) const;

    ModDict* select(const std::vector<std::string>& fields) const;

    using GroupResult = std::vector<std::pair<ModValue, ModDict*>>;
    GroupResult group_by(const std::string& field) const;

    void create_index(const std::string& field_name);
    void create_index(const std::vector<std::string>& pattern);
    void drop_index(const std::string& field_name);
    void drop_index(const std::vector<std::string>& pattern);
    bool has_index(const std::string& field_name) const;
    bool has_index(const std::vector<std::string>& pattern) const;

    PyObject* to_python_dict() const;
    std::string to_string() const;
    PyObject* to_json() const;
    PyObject* to_python() const;
    void dump() const;

    std::vector<uint8_t> serialize() const;
    void deserialize(const uint8_t* data, size_t len);
};

#endif
