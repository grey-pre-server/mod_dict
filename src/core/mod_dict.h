#ifndef MOD_DICT_MOD_DICT_H
#define MOD_DICT_MOD_DICT_H

#include "mod_value.h"
#include "flat_hash_map.h"
#include <Python.h>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>

enum class MergeConflict { KEEP_LEFT, KEEP_RIGHT, MERGE, CONCAT };
enum class FilterOp      { EQ, NE, LT, LE, GT, GE };
enum class LinkOnDelete  { RESTRICT, CASCADE, SET_NULL };

// ──────────────────────────────────────────────────────────────────────────────
// LinkDecl — a declared relationship between rows within one ModDict.
//
// v1 scope (deliberately narrow, see project memory project-link-feature-design):
//   source_pattern     = [table, "__pass_key__", field]   e.g. "employees.?.manager_id"
//   references_pattern = [table, "__pass_key__"]          pk-based, e.g. "employees.?"
//                      or [table, "__pass_key__", field]  non-pk,   e.g. "customers.?.email"
// Self-reference (source table == target table) is allowed — cycles are safe
// under CASCADE because deleting a row scrubs its own reverse-index entry
// before cascade looks up who referenced it (no separate visited-set needed).
// ──────────────────────────────────────────────────────────────────────────────
struct LinkDecl {
    std::vector<std::string> source_pattern;
    std::vector<std::string> references_pattern;
    LinkOnDelete on_delete = LinkOnDelete::RESTRICT;
};

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
    PyObject* key_py = nullptr;
    PyObject* val_py = nullptr;  // nullptr = alias entry (val always >= Py_None for real entries)
    bool      is_row = false;
};

class ModDict {
public:
    FlatHashMap<uint64_t, OuterEntry> outer;
    std::vector<uint64_t> order;  // outer hashes in insertion order, no aliases

    // Alias side-tables (keep main outer entries small)
    FlatHashMap<uint64_t, uint64_t> alias_to_orig;  // alias_hash → orig_hash
    FlatHashMap<uint64_t, uint64_t> orig_to_alias;  // orig_hash  → alias_hash

    struct {
        FlatHashMap<std::string, class FieldIndex*> by_field;
    } indices;

    std::vector<LinkDecl> links;

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
    bool     contains(const ModValue& key) const;
    ModDict* deep_copy() const;

    size_t len()   const { return order.size(); }
    bool   empty() const { return order.empty(); }

    // Access by insertion-order index (negative = from end). Returns nullptr if out of range.
    // Returns borrowed key_py and val_py via out params; returns false if OOB.
    bool at(Py_ssize_t i, uint64_t& out_hash) const {
        Py_ssize_t n = (Py_ssize_t)order.size();
        if (i < 0) i += n;
        if (i < 0 || i >= n) return false;
        out_hash = order[(size_t)i];
        return true;
    }

    // Full row → Py_INCREF'd dict, O(1).
    PyObject* get_row(uint64_t outer_hash) const;

    // Sub-dict or scalar at path. path = "\x01"-joined segments.
    PyObject* get_subrow(uint64_t outer_hash, const std::string& path) const;

    // Borrowed ref to the stored row dict (for FieldIndex, merges). No Py_INCREF.
    PyObject* get_row_ref(uint64_t outer_hash) const;

    bool remove(const ModValue& key);
    void reindex_row(uint64_t outer_hash);

    // Same field-index rebuild as reindex_row(), but skips the link-validation
    // pass. Used internally by delete_with_link_semantics() for the reindex
    // steps that happen BETWEEN cascade steps, where the table is expected to
    // be transiently inconsistent (a referrer not yet cascade-deleted still
    // pointing at a just-deleted row) — validating there would reject states
    // the cascade is already in the middle of fixing. The cascade's own logic
    // guarantees the table is fully consistent once it returns.
    void reindex_row_no_validate(uint64_t outer_hash);

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

    // EQ-only fast path for a pattern containing one or more "__follow_link__"
    // hops (e.g. "orders.?.customer_id->name") — chains FieldIndex::
    // find_wildcard_leaf_eq lookups backwards from the rightmost (compared)
    // segment through each hop's already-built link index, never falling back
    // to a per-row scan. Called by filter(pattern,...) when op==EQ and the
    // pattern contains a hop; ValueError (ptr set) if any hop's link wasn't
    // declared via link() first.
    ModDict* filter_linked_eq(const std::vector<std::string>& pattern, const ModValue& value) const;

    using SortResult  = std::vector<PyObject*>;
    SortResult sort_by(const std::string& field, bool reverse) const;

    // paths[i] is looked up on each row; labels[i] is the key it's stored
    // under in the result row (result key naming — default "last segment of
    // the path" vs. explicit user labels — is a binding-layer concern; this
    // just requires paths.size()==labels.size(), no uniqueness check of its
    // own — a later paths[i] silently overwrites an earlier one under the
    // same label, same as any PyDict_SetItemString would).
    ModDict* select(const std::vector<std::string>& paths, const std::vector<std::string>& labels) const;

    // Anchored/wildcard select — one or more patterns of shape "table.?...",
    // optionally containing "__follow_link__" hops, all sharing the same
    // anchor table (segment 0). Result is a FLAT ModDict keyed by each
    // matched anchor row's own key (same convention as the plain select()
    // above), one projected {field_labels[i]: value} dict per row — not
    // filter()'s {table: {...}} nesting. Rows missing every field are
    // skipped, same as plain select().
    ModDict* select_anchored(const std::vector<std::vector<std::string>>& patterns,
                              const std::vector<std::string>& field_labels) const;

    using GroupResult = std::vector<std::pair<ModValue, ModDict*>>;
    GroupResult group_by(const std::string& field) const;

    void create_index(const std::string& field_name);
    void create_index(const std::vector<std::string>& pattern);
    void drop_index(const std::string& field_name);
    void drop_index(const std::vector<std::string>& pattern);
    bool has_index(const std::string& field_name) const;
    bool has_index(const std::vector<std::string>& pattern) const;

    // ──────────────────────────────────────────────────
    // Links (see LinkDecl above)
    // ──────────────────────────────────────────────────

    // Declares source_pattern -> references_pattern. Validates every existing
    // match resolves to a real target row (raises ValueError on any dangling
    // reference — ptr set on error). No-op (silently ignored) if an identical
    // link is already declared.
    void link(const std::vector<std::string>& source_pattern,
              const std::vector<std::string>& references_pattern,
              LinkOnDelete on_delete = LinkOnDelete::RESTRICT);

    // Finds the declared link matching source_pattern exactly (raises
    // ValueError if none). Returns a new ModDict of the resolved target rows
    // for every current match of source_pattern (outer keys = target keys).
    //
    // key_filter (optional): only scan source rows whose own key hashes into
    // one of these — lets a caller chain hops by passing the previous hop's
    // result keys, e.g. org.follow(path, &prev_result_key_hashes).
    //
    // value_filter (optional, mutually exclusive with key_filter): skip
    // scanning the source table entirely and resolve these values directly
    // against the target — for values obtained from somewhere other than a
    // source-table scan (e.g. an external list of ids).
    ModDict* follow(const std::vector<std::string>& source_pattern,
                     const std::vector<uint64_t>* key_filter = nullptr,
                     const std::vector<PyObject*>* value_filter = nullptr) const;

    const LinkDecl* find_link(const std::vector<std::string>& source_pattern) const;

    // Deletes `key` from `table`'s nested row, applying on_delete semantics
    // for any declared link that targets this table:
    //   restrict  — refuses (raises ValueError, table left untouched) if any
    //               row still references `key`.
    //   cascade   — deletes referencing rows too, recursively. Safe under
    //               cycles: deletes `key` (and reindexes) BEFORE looking up
    //               who referenced it, so a cycle's own back-reference is
    //               already scrubbed by the time recursion would return to
    //               it — no separate visited-set needed.
    //   set_null  — clears the reference field on referencing rows.
    // Returns false (PyErr set) on restrict-refusal or a real Python error;
    // true if the deletion (and any cascade/set_null side effects) completed.
    bool delete_with_link_semantics(const std::string& table, PyObject* key);

    PyObject* to_python_dict() const;
    std::string to_string() const;
    PyObject* to_json() const;
    PyObject* to_python() const;
    void dump() const;

    std::vector<uint8_t> serialize() const;
    void deserialize(const uint8_t* data, size_t len);

    // True if data starts with the ModDict container magic (produced by serialize()).
    // Lets module-level dumps()/loads() distinguish a whole-ModDict blob from a
    // single arbitrary-value blob without duplicating the magic constant.
    static bool has_container_magic(const uint8_t* data, size_t len);
};

#endif
