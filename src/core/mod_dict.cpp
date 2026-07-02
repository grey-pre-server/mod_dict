#include "mod_dict.h"
#include "field_index.h"
#include "../python_bindings/mod_dict_binding.h"
#include "../python_bindings/converter_registry.h"
#include "codecs/serializer.h"
#include "codecs/codec_base.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <unordered_map>

// ── Utilities ────────────────────────────────────────────────────────────────

static std::vector<std::string> split_path_sep(const std::string& s) {
    std::vector<std::string> parts;
    size_t start = 0, pos;
    while ((pos = s.find('\x01', start)) != std::string::npos) {
        parts.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    parts.push_back(s.substr(start));
    return parts;
}

// Split "age.a.b" → ["age","a","b"]. Single segment stays as-is.
static std::vector<std::string> split_path(const std::string& field) {
    std::vector<std::string> segs;
    size_t pos = 0;
    while (true) {
        size_t d = field.find('.', pos);
        segs.push_back(field.substr(pos, d == std::string::npos ? d : d - pos));
        if (d == std::string::npos) break;
        pos = d + 1;
    }
    return segs;
}

// Traverse nested dict by path segments. Returns borrowed ref or nullptr.
static PyObject* get_nested_segs(PyObject* dict, const std::vector<const char*>& segs) {
    PyObject* cur = dict;
    for (const char* seg : segs) {
        if (!cur || !PyDict_Check(cur)) return nullptr;
        cur = PyDict_GetItemString(cur, seg);
    }
    return cur;
}

// Borrowed ref — traverse Python dict with path segments. Returns nullptr if missing.
static PyObject* get_nested(PyObject* obj, const std::vector<std::string>& path, size_t start = 0) {
    PyObject* cur = obj;
    for (size_t i = start; i < path.size(); i++) {
        if (!cur || !PyDict_Check(cur)) return nullptr;
        cur = PyDict_GetItemString(cur, path[i].c_str());
    }
    return cur;
}

// Deep-copy: dicts copied recursively, scalars/others Py_INCREF'd.
static PyObject* deep_copy_pyobj(PyObject* obj) {
    if (PyDict_Check(obj)) {
        PyObject* copy = PyDict_New();
        if (!copy) return nullptr;
        PyObject *k, *v; Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &k, &v)) {
            PyObject* vc = deep_copy_pyobj(v);
            if (!vc) { Py_DECREF(copy); return nullptr; }
            PyDict_SetItem(copy, k, vc);
            Py_DECREF(vc);
        }
        return copy;
    }
    if (PyList_Check(obj)) {
        Py_ssize_t n = PyList_GET_SIZE(obj);
        PyObject* copy = PyList_New(n);
        if (!copy) return nullptr;
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* vc = deep_copy_pyobj(PyList_GET_ITEM(obj, i));
            if (!vc) { Py_DECREF(copy); return nullptr; }
            PyList_SET_ITEM(copy, i, vc);
        }
        return copy;
    }
    Py_INCREF(obj);
    return obj;
}

// ── Constructor / Destructor ─────────────────────────────────────────────────

ModDict::ModDict() {}

ModDict::~ModDict() {
    for (auto& e : outer.occupied()) {
        Py_XDECREF(e.value.key_py);
        Py_XDECREF(e.value.val_py);
    }
    for (auto& fi : indices.by_field.occupied()) delete fi.value;
    Py_XDECREF(on_change_cb);
    Py_XDECREF(on_merge_cb);
}


// ── Insert ───────────────────────────────────────────────────────────────────

bool ModDict::insert(const ModValue& key, const ModValue& value) {
    uint64_t h = key.hash_val;
    OuterEntry* existing = outer.find(h);

    // transparent alias redirect
    if (const uint64_t* orig = alias_to_orig.find(h)) {
        h = *orig;
        existing = outer.find(h);
    }

    bool is_new = !existing;
    PyObject* k = key.obj   ? key.obj   : Py_None;
    PyObject* v = value.obj ? value.obj : Py_None;

    if (existing) {
        Py_XDECREF(existing->key_py);
        Py_XDECREF(existing->val_py);
        Py_INCREF(k); Py_INCREF(v);
        existing->key_py = k;
        existing->val_py = v;
        existing->is_row = false;
    } else {
        Py_INCREF(k); Py_INCREF(v);
        outer.insert(h, {k, v, false});
        order.push_back(h);
    }
    return is_new;
}

void ModDict::insert_row(const ModValue& outer_key, PyObject* dict_obj) {
    uint64_t oh = outer_key.hash_val;
    OuterEntry* existing = outer.find(oh);

    // transparent alias redirect — preserve the original's key_py
    bool via_alias = false;
    if (const uint64_t* orig = alias_to_orig.find(oh)) {
        oh = *orig; via_alias = true;
        existing = outer.find(oh);
    }

    // Apply registered converters recursively (no-op if registry empty — O(1) fast path)
    PyObject* row_v = converter_registry_convert_deep(dict_obj);

    if (existing) {
        for (auto& fi : indices.by_field.occupied())
            fi.value->on_remove_row(oh, this);
        Py_XDECREF(existing->val_py);
        existing->val_py = row_v;
        existing->is_row = true;
        // key_py stays unchanged — keep the original key, not the alias key
        if (!via_alias) {
            Py_XDECREF(existing->key_py);
            PyObject* k = outer_key.obj ? outer_key.obj : Py_None;
            Py_INCREF(k);
            existing->key_py = k;
        }
    } else {
        PyObject* k = outer_key.obj ? outer_key.obj : Py_None;
        Py_INCREF(k);
        outer.insert(oh, {k, row_v, true});
        order.push_back(oh);
    }

    for (auto& fi : indices.by_field.occupied())
        fi.value->on_insert_row(oh, this);
}

// ── Read ─────────────────────────────────────────────────────────────────────

bool ModDict::contains(const ModValue& key) const {
    return outer.find(key.hash_val) != nullptr;
}

ModDict* ModDict::deep_copy() const {
    ModDict* c = new ModDict();
    if (!c) return nullptr;
    for (uint64_t oh : order) {
        const OuterEntry* e = outer.find(oh);
        if (!e) continue;
        ModValue mk = ModValue::from_pyobject(e->key_py ? e->key_py : Py_None);
        if (e->is_row && e->val_py) {
            PyObject* row_copy = deep_copy_pyobj(e->val_py);
            if (!row_copy) { delete c; return nullptr; }
            c->insert_row(mk, row_copy);
            Py_DECREF(row_copy);
        } else {
            ModValue mv = ModValue::from_pyobject(e->val_py ? e->val_py : Py_None);
            c->insert(mk, mv);
        }
    }
    return c;
}

PyObject* ModDict::get_row(uint64_t oh) const {
    const OuterEntry* e = outer.find(oh);
    if (!e || !e->is_row || !e->val_py) return nullptr;
    Py_INCREF(e->val_py);
    return e->val_py;
}

PyObject* ModDict::get_row_ref(uint64_t oh) const {
    const OuterEntry* e = outer.find(oh);
    return (e && e->is_row) ? e->val_py : nullptr;
}

PyObject* ModDict::get_subrow(uint64_t oh, const std::string& prefix) const {
    const OuterEntry* e = outer.find(oh);
    if (!e || !e->is_row || !e->val_py) return nullptr;

    auto segs = split_path_sep(prefix);
    PyObject* cur = e->val_py;

    for (size_t i = 0; i < segs.size(); i++) {
        if (!PyDict_Check(cur)) {
            if (i == segs.size() - 1) break;  // scalar at final segment
            PyErr_Format(PyExc_KeyError, "%s", segs[i].c_str());
            return nullptr;
        }
        PyObject* next = PyDict_GetItemString(cur, segs[i].c_str());
        if (!next) {
            PyErr_Format(PyExc_KeyError, "%s", segs[i].c_str());
            return nullptr;
        }
        cur = next;
    }
    Py_INCREF(cur);
    return cur;
}


// ── Reindex one row (after in-place field write via RowProxy) ────────────────

void ModDict::reindex_row(uint64_t oh) {
    // if accessed via alias, reindex the original key so the index stays correct
    OuterEntry* e = outer.find(oh);
    if (const uint64_t* orig = alias_to_orig.find(oh)) oh = *orig;
    for (auto& fi : indices.by_field.occupied()) {
        fi.value->remove_outer_key(oh);
        fi.value->on_insert_row(oh, this);
    }
}

// ── Remove ───────────────────────────────────────────────────────────────────

bool ModDict::remove(const ModValue& key) {
    uint64_t h = key.hash_val;
    OuterEntry* e = outer.find(h);
    if (!e) return false;

    auto erase_order = [&](uint64_t oh) {
        auto it = std::find(order.begin(), order.end(), oh);
        if (it != order.end()) order.erase(it);
    };

    // Is h an alias?
    if (const uint64_t* orig_p = alias_to_orig.find(h)) {
        uint64_t orig_h = *orig_p;
        // remove alias entry
        Py_XDECREF(e->key_py); e->key_py = nullptr;
        outer.erase(h);
        alias_to_orig.erase(h);
        orig_to_alias.erase(orig_h);
        // remove original (symmetric)
        OuterEntry* orig = outer.find(orig_h);
        if (orig) {
            if (orig->is_row)
                for (auto& fi : indices.by_field.occupied())
                    fi.value->on_remove_row(orig_h, this);
            Py_XDECREF(orig->key_py); Py_XDECREF(orig->val_py);
            orig->key_py = nullptr; orig->val_py = nullptr;
            outer.erase(orig_h);
        }
        erase_order(orig_h);
        return true;
    }

    if (e->is_row)
        for (auto& fi : indices.by_field.occupied())
            fi.value->on_remove_row(h, this);

    // if original has an alias, cascade-delete it
    if (const uint64_t* alias_p = orig_to_alias.find(h)) {
        uint64_t alias_h = *alias_p;
        OuterEntry* ae = outer.find(alias_h);
        if (ae) { Py_XDECREF(ae->key_py); ae->key_py = nullptr; }
        outer.erase(alias_h);
        alias_to_orig.erase(alias_h);
        orig_to_alias.erase(h);
    }

    Py_XDECREF(e->key_py); Py_XDECREF(e->val_py);
    e->key_py = nullptr; e->val_py = nullptr;
    outer.erase(h);
    erase_order(h);
    return true;
}

// ── compare helper ───────────────────────────────────────────────────────────

static bool compare_values(const ModValue& a, FilterOp op, const ModValue& b) {
    switch (op) {
        case FilterOp::EQ: return  a.equals(b);
        case FilterOp::NE: return !a.equals(b);
        case FilterOp::LT:
        case FilterOp::LE:
        case FilterOp::GT:
        case FilterOp::GE: {
            bool ok = true;
            int c = a.compare(b, &ok);
            // Not comparable (e.g. None vs int in an unnormalized field) —
            // excluded from every range predicate, not just silently "equal".
            if (!ok) return false;
            switch (op) {
                case FilterOp::LT: return c < 0;
                case FilterOp::LE: return c <= 0;
                case FilterOp::GT: return c > 0;
                case FilterOp::GE: return c >= 0;
                default: return false;
            }
        }
    }
    return false;
}

// ── filter ───────────────────────────────────────────────────────────────────

static void filter_add_row(ModDict* result, const ModDict* src, uint64_t oh) {
    const OuterEntry* oe = src->outer.find(oh);
    if (!oe || !oe->is_row) return;
    Py_XINCREF(oe->key_py);
    Py_XINCREF(oe->val_py);
    result->outer.insert(oh, {oe->key_py, oe->val_py, true});
    result->order.push_back(oh);
}

static void filter_add_pruned_row(ModDict* result, const ModDict* src, uint64_t oh, PyObject* val_py) {
    const OuterEntry* oe = src->outer.find(oh);
    if (!oe || !oe->is_row || !oe->key_py) return;
    Py_INCREF(oe->key_py);
    Py_INCREF(val_py);
    result->outer.insert(oh, {oe->key_py, val_py, true});
    result->order.push_back(oh);
}

// Returns:
//   nullptr  — no match
//   Py_True  — matched, caller should use full original row (terminal ? or simple field)
//   new dict — matched, pruned to only matching inner entries (caller owns ref)
static PyObject* prune_match(PyObject* cur,
                              const std::vector<std::string>& pat, size_t depth,
                              FilterOp op, const ModValue& val)
{
    if (!cur || depth >= pat.size()) return nullptr;
    bool last = (depth == pat.size() - 1);

    if (pat[depth] == "__pass_key__") {
        if (last) {
            // Terminal ?: check if any KEY equals val — return full cur
            if (!PyDict_Check(cur)) return nullptr;
            PyObject *k, *v; Py_ssize_t pos = 0;
            while (PyDict_Next(cur, &pos, &k, &v)) {
                ModValue fv = ModValue::from_pyobject(k);
                if (compare_values(fv, op, val)) return Py_True;
            }
            return nullptr;
        } else {
            // Non-terminal ?: prune this level — keep only matching inner keys
            if (!PyDict_Check(cur)) return nullptr;
            PyObject* pruned = PyDict_New();
            if (!pruned) return nullptr;
            PyObject *k, *v; Py_ssize_t pos = 0;
            while (PyDict_Next(cur, &pos, &k, &v)) {
                PyObject* sub = prune_match(v, pat, depth + 1, op, val);
                if (!sub) continue;
                if (sub == Py_True) {
                    PyDict_SetItem(pruned, k, v);
                } else {
                    PyDict_SetItem(pruned, k, sub);
                    Py_DECREF(sub);
                }
            }
            if (PyDict_Size(pruned) == 0) { Py_DECREF(pruned); return nullptr; }
            return pruned;
        }
    } else {
        // Literal segment — navigate to child
        if (!PyDict_Check(cur)) return nullptr;
        PyObject* child = PyDict_GetItemString(cur, pat[depth].c_str());
        if (!child) return nullptr;
        if (last) {
            ModValue fv = ModValue::from_pyobject(child);
            return compare_values(fv, op, val) ? Py_True : nullptr;
        } else {
            PyObject* sub = prune_match(child, pat, depth + 1, op, val);
            if (!sub) return nullptr;
            if (sub == Py_True) return Py_True;
            // sub is pruned version of child — wrap in {literal_key: sub}
            PyObject* wrapper = PyDict_New();
            if (!wrapper) { Py_DECREF(sub); return nullptr; }
            PyObject* key_obj = PyUnicode_FromStringAndSize(pat[depth].c_str(), pat[depth].size());
            if (!key_obj) { Py_DECREF(sub); Py_DECREF(wrapper); return nullptr; }
            PyDict_SetItem(wrapper, key_obj, sub);
            Py_DECREF(key_obj);
            Py_DECREF(sub);
            return wrapper;
        }
    }
}

ModDict* ModDict::filter(const std::string& field, FilterOp op, const ModValue& value) const {
    ModDict* result = new ModDict();

    auto* idx_ptr = indices.by_field.find(field);
    if (!idx_ptr) {
        const_cast<ModDict*>(this)->create_index(field);
        idx_ptr = indices.by_field.find(field);
    }
    FieldIndex* idx = *idx_ptr;

    if (op == FilterOp::EQ) {
        auto* bucket = idx->find_eq(value.hash());
        if (bucket) for (uint64_t oh : *bucket) filter_add_row(result, this, oh);
    } else if (op != FilterOp::NE && idx->is_numeric_range_supported(value)) {
        for (uint64_t oh : idx->find_range(op, value)) filter_add_row(result, this, oh);
    } else {
        for (auto& e : outer.occupied()) {
            if (!e.value.is_row || !e.value.val_py) continue;
            PyObject* fv_obj = get_nested(e.value.val_py, {field});
            if (!fv_obj) continue;
            ModValue fv = ModValue::from_pyobject(fv_obj);
            if (compare_values(fv, op, value)) filter_add_row(result, this, e.key);
        }
    }
    return result;
}

// Walks `pattern` from start_depth using wc_keys[i] for each "__pass_key__"
// segment (one "?" = one level, so wc_keys[i] is exactly the key to descend
// through at that "?"). Literal segments are resolved directly from pattern
// (their identity is static, no need to have stored them). Builds/reuses
// nested dicts in `pruned_root` for every segment UP TO the LAST "?" — at
// that point the WHOLE original sub-object is kept as-is (any literal
// segments after the last "?" were only needed to validate the match, not
// to further prune the result — mirrors what the old prune_match did).
static void insert_pruned_path(PyObject* pruned_root, PyObject* row,
                                const std::vector<std::string>& pattern, size_t start_depth,
                                const std::vector<PyObject*>& wc_keys)
{
    size_t last_wc_depth = SIZE_MAX;
    for (size_t d = start_depth; d < pattern.size(); d++)
        if (pattern[d] == "__pass_key__") last_wc_depth = d;
    if (last_wc_depth == SIZE_MAX) return;  // caller guarantees a "?" exists

    PyObject* cur_pruned = pruned_root;
    PyObject* cur_row = row;
    size_t wc_i = 0;

    for (size_t depth = start_depth; depth < last_wc_depth; depth++) {
        PyObject* key_obj; bool owns_key = false;
        if (pattern[depth] == "__pass_key__") {
            if (wc_i >= wc_keys.size()) return;
            key_obj = wc_keys[wc_i++];
        } else {
            key_obj = PyUnicode_FromStringAndSize(pattern[depth].c_str(), pattern[depth].size());
            if (!key_obj) return;
            owns_key = true;
        }
        if (!cur_row || !PyDict_Check(cur_row)) { if (owns_key) Py_DECREF(key_obj); return; }

        PyObject* next_pruned = PyDict_GetItem(cur_pruned, key_obj);
        if (!next_pruned) {
            PyObject* nd = PyDict_New();
            if (!nd) { if (owns_key) Py_DECREF(key_obj); return; }
            PyDict_SetItem(cur_pruned, key_obj, nd);
            Py_DECREF(nd);
            next_pruned = nd;
        }
        PyObject* next_row = PyDict_GetItem(cur_row, key_obj);
        if (owns_key) Py_DECREF(key_obj);

        cur_pruned = next_pruned;
        cur_row = next_row;
    }

    // Last "?": keep the whole matched sub-object under its key, unpruned.
    if (!cur_row || !PyDict_Check(cur_row) || wc_i >= wc_keys.size()) return;
    PyObject* final_key = wc_keys[wc_i];
    PyObject* val = PyDict_GetItem(cur_row, final_key);
    if (val) PyDict_SetItem(cur_pruned, final_key, val);
}

// Fast reconstruction for wildcard EQ matches (any number of "?" in pattern):
// FieldIndex::wildcard_leaf_index already has the exact chain of keys each "?"
// resolved to per match — direct PyDict_GetItem walk, no rescanning the row.
static void add_pruned_from_leaf(ModDict* result, const ModDict* self,
                                  const std::vector<std::string>& pattern, size_t start_depth,
                                  const std::vector<std::pair<uint64_t,std::vector<PyObject*>>>& matches)
{
    std::unordered_map<uint64_t, PyObject*> pending;  // oh -> pruned dict (owned, not yet in result)
    for (auto& m : matches) {
        uint64_t oh = m.first; const std::vector<PyObject*>& wc_keys = m.second;
        const OuterEntry* oe = self->outer.find(oh);
        if (!oe || !oe->val_py) continue;

        PyObject*& pruned = pending[oh];
        if (!pruned) pruned = PyDict_New();
        insert_pruned_path(pruned, oe->val_py, pattern, start_depth, wc_keys);
    }
    for (auto& kv : pending) {
        filter_add_pruned_row(result, self, kv.first, kv.second);
        Py_DECREF(kv.second);
    }
}

ModDict* ModDict::filter(const std::vector<std::string>& pattern, FilterOp op, const ModValue& value) const {
    std::string key;
    for (size_t i = 0; i < pattern.size(); i++) { if (i) key += '\x01'; key += pattern[i]; }

    ModDict* result = new ModDict();

    auto* idx_ptr = indices.by_field.find(key);
    if (!idx_ptr) {
        const_cast<ModDict*>(this)->create_index(pattern);
        idx_ptr = indices.by_field.find(key);
    }
    FieldIndex* idx = *idx_ptr;

    // For wildcard patterns, compute anchor once (used in all paths below)
    bool anchored = (!pattern.empty() && pattern[0] != "__pass_key__");
    uint64_t anchor_hash = 0;
    const OuterEntry* anchor_entry = nullptr;
    if (anchored) {
        PyObject* tmp = PyUnicode_FromStringAndSize(pattern[0].c_str(), pattern[0].size());
        if (tmp) { anchor_hash = content_hash_pyobj(tmp); Py_DECREF(tmp); }
        anchor_entry = outer.find(anchor_hash);
        if (!anchor_entry || !anchor_entry->val_py || !anchor_entry->is_row)
            anchored = false;
    }

    // Helper: run prune_match and add result to result ModDict
    auto add_pruned = [&](uint64_t oh, PyObject* val_py, size_t start_depth) {
        PyObject* pruned = prune_match(val_py, pattern, start_depth, op, value);
        if (!pruned) return;
        if (pruned == Py_True) {
            filter_add_row(result, this, oh);
        } else {
            filter_add_pruned_row(result, this, oh, pruned);
            Py_DECREF(pruned);
        }
    };

    bool terminal_pass_key = !pattern.empty() && pattern.back() == "__pass_key__";

    if (op == FilterOp::EQ && idx->is_wildcard && terminal_pass_key) {
        // Terminal ?: the matched key IS `value` itself. Dict keys are unique
        // within a row, so a given oh can appear at most once in the bucket
        // regardless of anchoring — no dedup needed. Direct O(1) lookup, no rescan.
        auto* bucket = idx->find_eq(value.hash());
        if (bucket && value.obj) {
            for (uint64_t oh : *bucket) {
                const OuterEntry* oe = outer.find(oh);
                if (!oe || !oe->val_py) continue;
                PyObject* leaf_val = PyDict_GetItem(oe->val_py, value.obj);
                if (!leaf_val) continue;
                PyObject* pruned = PyDict_New();
                PyDict_SetItem(pruned, value.obj, leaf_val);
                filter_add_pruned_row(result, this, oh, pruned);
                Py_DECREF(pruned);
            }
        }
    } else if (op == FilterOp::EQ && idx->is_wildcard) {
        // Non-terminal "?" — any number of levels ("?.field", "?.?.field", ...).
        // wildcard_leaf_index already has the exact chain of keys each "?"
        // resolved to per match — reconstruct directly, no rescan.
        auto* leaf = idx->find_wildcard_leaf_eq(value.hash());
        if (leaf) add_pruned_from_leaf(result, this, pattern, anchored ? 1 : 0, *leaf);
    } else if (op == FilterOp::EQ) {
        auto* bucket = idx->find_eq(value.hash());
        if (bucket) for (uint64_t oh : *bucket) filter_add_row(result, this, oh);
    } else if (op != FilterOp::NE && idx->is_numeric_range_supported(value)) {
        if (!idx->is_wildcard) {
            for (uint64_t oh : idx->find_range(op, value)) filter_add_row(result, this, oh);
        } else {
            // Range queries on wildcard fields don't have a leaf index (only EQ
            // does) — dedup at least avoids rescanning a row once per duplicate.
            std::unordered_map<uint64_t, char> seen;
            for (uint64_t oh : idx->find_range(op, value)) {
                if (!seen.emplace(oh, 0).second) continue;
                const OuterEntry* oe = outer.find(oh);
                if (!oe || !oe->val_py) continue;
                add_pruned(oh, oe->val_py, anchored ? 1 : 0);
            }
        }
    } else {
        // Linear scan
        if (anchored) {
            add_pruned(anchor_hash, anchor_entry->val_py, 1);
        } else {
            for (auto& e : outer.occupied()) {
                if (!e.value.is_row || !e.value.val_py) continue;
                add_pruned(e.key, e.value.val_py, 0);
            }
        }
    }
    return result;
}

// ── select ───────────────────────────────────────────────────────────────────

ModDict* ModDict::select(const std::vector<std::string>& fields) const {
    // Pre-split all field paths once
    std::vector<std::vector<std::string>> paths;
    paths.reserve(fields.size());
    for (const auto& f : fields) paths.push_back(split_path(f));

    ModDict* result = new ModDict();

    for (uint64_t oh : order) {
        const OuterEntry* ep = outer.find(oh);
        if (!ep) continue;
        const OuterEntry& e = *ep;
        if (!e.is_row || !e.val_py) {
            if (!e.is_row && e.val_py) {
                Py_XINCREF(e.key_py);
                Py_XINCREF(e.val_py);
                result->outer.insert(oh, {e.key_py, e.val_py, false});
                result->order.push_back(oh);
            }
            continue;
        }
        PyObject* new_row = PyDict_New();
        if (!new_row) { delete result; return nullptr; }
        bool has_any = false;
        for (size_t i = 0; i < fields.size(); i++) {
            std::vector<const char*> segs;
            for (const auto& s : paths[i]) segs.push_back(s.c_str());
            PyObject* fv = get_nested_segs(e.val_py, segs);
            if (fv) { PyDict_SetItemString(new_row, fields[i].c_str(), fv); has_any = true; }
        }
        if (has_any) {
            Py_XINCREF(e.key_py);
            result->outer.insert(oh, {e.key_py, new_row, true});
            result->order.push_back(oh);
        } else {
            Py_DECREF(new_row);
        }
    }
    return result;
}

// ── sort_by ──────────────────────────────────────────────────────────────────

ModDict::SortResult ModDict::sort_by(const std::string& field, bool reverse) const {
    SortResult result;
    // Anchored paths (first segment = outer key) not supported — ambiguous level
    {
        auto segs = split_path(field);
        if (segs.size() > 1 && segs[0] != "__pass_key__") {
            PyObject* tmp = PyUnicode_FromStringAndSize(segs[0].c_str(), segs[0].size());
            if (tmp) {
                uint64_t h = content_hash_pyobj(tmp); Py_DECREF(tmp);
                if (outer.find(h)) {
                    PyErr_SetString(PyExc_ValueError,
                        "sort_by: anchored paths (first segment = outer key) are not supported. "
                        "Extract the sub-collection first: md.ModDict(mn[key]).sort_by(...)");
                    return result;
                }
            }
        }
    }
    auto* idx_ptr = indices.by_field.find(field);
    if (!idx_ptr) {
        auto segs = split_path(field);
        if (segs.size() > 1) const_cast<ModDict*>(this)->create_index(segs);
        else                  const_cast<ModDict*>(this)->create_index(field);
        idx_ptr = indices.by_field.find(field);
        if (!idx_ptr) {  // wildcard index keyed by pattern_key
            std::string key; for (size_t i=0;i<segs.size();i++){if(i)key+='\x01';key+=segs[i];}
            idx_ptr = indices.by_field.find(key);
        }
    }
    FieldIndex* idx = *idx_ptr;
    if (idx->sorted_index.empty()) return result;  // field not numeric — return empty list
    result.reserve(idx->sorted_index.size());
    auto add = [&](const SortedEntry& se) {
        const OuterEntry* oe = outer.find(se.outer_key_hash);
        if (!oe || !oe->key_py) return;
        Py_INCREF(oe->key_py);
        result.push_back(oe->key_py);
    };
    if (!reverse) { for (auto& se : idx->sorted_index) add(se); }
    else          { for (auto it = idx->sorted_index.rbegin(); it != idx->sorted_index.rend(); ++it) add(*it); }
    return result;
}

// ── group_by ─────────────────────────────────────────────────────────────────

ModDict::GroupResult ModDict::group_by(const std::string& field) const {
    GroupResult result;
    auto segs = split_path(field);
    // Anchored paths (first segment = outer key) not supported
    if (segs.size() > 1 && segs[0] != "__pass_key__") {
        PyObject* tmp = PyUnicode_FromStringAndSize(segs[0].c_str(), segs[0].size());
        if (tmp) {
            uint64_t h = content_hash_pyobj(tmp); Py_DECREF(tmp);
            if (outer.find(h)) {
                PyErr_SetString(PyExc_ValueError,
                    "group_by: anchored paths (first segment = outer key) are not supported. "
                    "Extract the sub-collection first: md.ModDict(mn[key]).group_by(...)");
                return result;
            }
        }
    }
    std::vector<const char*> seg_ptrs; for (const auto& seg : segs) seg_ptrs.push_back(seg.c_str());
    bool is_path = segs.size() > 1;

    std::string idx_key = field;
    auto* idx_ptr = indices.by_field.find(field);
    if (!idx_ptr) {
        if (is_path) {
            const_cast<ModDict*>(this)->create_index(segs);
            idx_key.clear(); for (size_t i=0;i<segs.size();i++){if(i)idx_key+='\x01';idx_key+=segs[i];}
            idx_ptr = indices.by_field.find(idx_key);
        } else {
            const_cast<ModDict*>(this)->create_index(field);
            idx_ptr = indices.by_field.find(field);
        }
    }
    FieldIndex* idx = *idx_ptr;

    for (auto& bi : idx->hash_index.occupied()) {
        if (bi.value.empty()) continue;

        PyObject* fv_obj = nullptr;
        for (uint64_t kh : bi.value) {
            PyObject* row = get_row_ref(kh);
            if (!row) continue;
            fv_obj = is_path ? get_nested_segs(row, seg_ptrs)
                             : PyDict_GetItemString(row, field.c_str());
            if (fv_obj) break;
        }
        if (!fv_obj) continue;

        ModValue fv = ModValue::from_pyobject(fv_obj);

        ModDict* group = new ModDict();
        for (uint64_t kh : bi.value) {
            const OuterEntry* oe = outer.find(kh);
            if (!oe || !oe->is_row) continue;
            Py_XINCREF(oe->key_py);
            Py_XINCREF(oe->val_py);
            group->outer.insert(kh, {oe->key_py, oe->val_py, true});
            group->order.push_back(kh);
        }
        result.push_back({std::move(fv), group});
    }
    return result;
}

// ── merges ───────────────────────────────────────────────────────────────────

// Set a value at a nested path inside a Python dict, creating sub-dicts as needed.
static void set_nested_in_dict(PyObject* dict, const std::vector<const char*>& segs,
                                PyObject* value, MergeConflict conflict) {
    if (segs.empty()) return;
    PyObject* cur = dict;
    for (size_t i = 0; i + 1 < segs.size(); i++) {
        PyObject* next = PyDict_GetItemString(cur, segs[i]);
        if (!next || !PyDict_Check(next)) {
            next = PyDict_New(); if (!next) return;
            PyDict_SetItemString(cur, segs[i], next);
            Py_DECREF(next);
            next = PyDict_GetItemString(cur, segs[i]);
        }
        cur = next;
    }
    const char* last = segs.back();
    if (conflict == MergeConflict::KEEP_LEFT && PyDict_GetItemString(cur, last)) return;
    PyDict_SetItemString(cur, last, value);
}

// Get nested value using path segs from a dict (no wildcards).
// Recursively merge from tgt_dict into self_dict following parallel paths.
// Handles __pass_key__ wildcard: iterates over tgt keys and mirrors into self.
static void merge_wildcard_path(PyObject* self_dict, PyObject* tgt_dict,
                                 const std::vector<const char*>& self_segs,
                                 const std::vector<const char*>& tgt_segs,
                                 size_t depth, MergeConflict conflict) {
    if (depth >= self_segs.size() || depth >= tgt_segs.size()) return;
    if (!PyDict_Check(tgt_dict)) return;

    bool last_self = (depth == self_segs.size() - 1);
    bool last_tgt  = (depth == tgt_segs.size()  - 1);
    bool self_wild = (strcmp(self_segs[depth], "__pass_key__") == 0);
    bool tgt_wild  = (strcmp(tgt_segs[depth],  "__pass_key__") == 0);

    if (tgt_wild) {
        // Iterate over all keys in tgt_dict at this level
        PyObject *k, *v; Py_ssize_t pos = 0;
        while (PyDict_Next(tgt_dict, &pos, &k, &v)) {
            PyObject* self_sub;
            if (self_wild) {
                // Match by same key in self_dict
                self_sub = PyDict_GetItem(self_dict, k);
                if (!self_sub || !PyDict_Check(self_sub)) {
                    if (last_self) {
                        if (conflict == MergeConflict::KEEP_LEFT && self_sub) continue;
                        PyDict_SetItem(self_dict, k, v);
                    } else {
                        self_sub = PyDict_New(); if (!self_sub) return;
                        PyDict_SetItem(self_dict, k, self_sub);
                        Py_DECREF(self_sub);
                        self_sub = PyDict_GetItem(self_dict, k);
                        if (PyDict_Check(v))
                            merge_wildcard_path(self_sub, v, self_segs, tgt_segs, depth + 1, conflict);
                    }
                    continue;
                }
                if (last_self) {
                    if (conflict == MergeConflict::KEEP_LEFT) continue;
                    PyDict_SetItem(self_dict, k, v);
                } else if (PyDict_Check(v)) {
                    merge_wildcard_path(self_sub, v, self_segs, tgt_segs, depth + 1, conflict);
                }
            } else {
                // Self has a literal key — not a common case, skip
            }
        }
    } else {
        // tgt has a literal key
        PyObject* tgt_val = PyDict_GetItemString(tgt_dict, tgt_segs[depth]);
        if (!tgt_val) return;

        if (self_wild) {
            // Self is wildcard — iterate over all keys in self_dict
            PyObject *k, *v2; Py_ssize_t pos = 0;
            while (PyDict_Next(self_dict, &pos, &k, &v2)) {
                if (last_self) {
                    if (conflict == MergeConflict::KEEP_LEFT) continue;
                    PyDict_SetItem(self_dict, k, tgt_val);
                } else if (PyDict_Check(v2)) {
                    merge_wildcard_path(v2, tgt_val, self_segs, tgt_segs, depth + 1, conflict);
                }
            }
        } else {
            // Both literal
            if (last_self) {
                if (conflict == MergeConflict::KEEP_LEFT && PyDict_GetItemString(self_dict, self_segs[depth])) return;
                PyDict_SetItemString(self_dict, self_segs[depth], tgt_val);
            } else {
                if (!PyDict_Check(tgt_val)) return;
                PyObject* self_sub = PyDict_GetItemString(self_dict, self_segs[depth]);
                if (!self_sub || !PyDict_Check(self_sub)) {
                    self_sub = PyDict_New(); if (!self_sub) return;
                    PyDict_SetItemString(self_dict, self_segs[depth], self_sub);
                    Py_DECREF(self_sub);
                    self_sub = PyDict_GetItemString(self_dict, self_segs[depth]);
                }
                merge_wildcard_path(self_sub, tgt_val, self_segs, tgt_segs, depth + 1, conflict);
            }
        }
    }
}

static bool has_pass_key(const std::vector<const char*>& segs) {
    for (auto* s : segs) if (strcmp(s, "__pass_key__") == 0) return true;
    return false;
}

// Deep-merge other_dict into self_dict: recurse into nested dicts rather than replacing them.
// This preserves keys in self's sub-dicts that are absent in other's sub-dicts.
static void merge_dicts(PyObject* self_dict, PyObject* other_dict, MergeConflict conflict) {
    PyObject *k, *v; Py_ssize_t pos = 0;
    while (PyDict_Next(other_dict, &pos, &k, &v)) {
        if (conflict == MergeConflict::KEEP_LEFT && PyDict_Contains(self_dict, k)) continue;
        if (PyDict_Check(v)) {
            PyObject* self_sub = PyDict_GetItem(self_dict, k);
            if (self_sub && PyDict_Check(self_sub)) {
                merge_dicts(self_sub, v, conflict);
                continue;
            }
        }
        PyDict_SetItem(self_dict, k, v);
    }
}

int ModDict::merges(ModDict* target,
                    const std::vector<const char*>& on_source,
                    const std::vector<const char*>& on_target,
                    MergeConflict conflict)
{
    if (on_source.empty() || on_target.empty()) {
        PyErr_SetString(PyExc_ValueError, "on_source and on_target must not be empty");
        return -1;
    }

    int merged = 0;
    bool has_indices = (indices.by_field.size() > 0);

    std::vector<const char*> self_field_segs(on_source.begin() + 1, on_source.end());
    std::vector<const char*> other_field_segs(on_target.begin() + 1, on_target.end());
    bool has_field_filter = !other_field_segs.empty();

    // Lambda: apply one match pair (self_oh, other_oh)
    auto apply_row = [&](uint64_t self_oh, uint64_t other_oh) {
        OuterEntry* self_e = outer.find(self_oh);
        OuterEntry* tgt_e  = target->outer.find(other_oh);
        if (!self_e || !tgt_e) return;
        if (!self_e->is_row || !tgt_e->is_row) return;
        if (!self_e->val_py || !tgt_e->val_py) return;

        if (has_field_filter) {
            if (has_pass_key(self_field_segs) || has_pass_key(other_field_segs)) {
                merge_wildcard_path(self_e->val_py, tgt_e->val_py,
                                    self_field_segs, other_field_segs, 0, conflict);
            } else {
                PyObject* tgt_val = get_nested_segs(tgt_e->val_py, other_field_segs);
                if (!tgt_val) return;
                set_nested_in_dict(self_e->val_py, self_field_segs, tgt_val, conflict);
            }
        } else {
            // Merge all top-level fields
            merge_dicts(self_e->val_py, tgt_e->val_py, conflict);
        }
    };

    bool used_src_idx = false;  // set when src_idx path is taken (for smart rebuild skip)
    std::string merge_src_fname;  // field name used in src_idx merge (for rebuild skip)
    std::string merge_tgt_fname;
    bool src_is_key = strcmp(on_source[0], "__scan_key__") == 0 ||
                      strcmp(on_source[0], "__pass_key__") == 0;
    bool tgt_is_key = strcmp(on_target[0], "__scan_key__") == 0 ||
                      strcmp(on_target[0], "__pass_key__") == 0;

    bool tgt_pass = strcmp(on_target[0], "__pass_key__") == 0;

    if (src_is_key && tgt_is_key) {
        if (tgt_pass) {
            // ? on target side: also insert keys from target not present in self
            for (auto& te : target->outer.occupied()) {
                if (!te.value.is_row || !te.value.val_py) continue;
                uint64_t oh = te.key;
                if (!outer.find(oh)) {
                    // insert new key from target
                    Py_XINCREF(te.value.key_py); Py_XINCREF(te.value.val_py);
                    insert_row(ModValue::from_pyobject(te.value.key_py), te.value.val_py);
                    merged++;
                } else {
                    apply_row(oh, oh);
                    merged++;
                }
            }
        } else {
            for (auto& e : outer.occupied()) {
                if (!e.value.is_row) continue;
                uint64_t oh = e.key;
                if (!target->outer.find(oh)) continue;
                apply_row(oh, oh);
                merged++;
            }
        }
    } else if (src_is_key && !tgt_is_key) {
        std::string tgt_fname(on_target[0]);
        FieldIndex** tgt_idx = target->indices.by_field.find(tgt_fname);
        if (tgt_idx) {
            for (auto& e : outer.occupied()) {
                if (!e.value.is_row) continue;
                uint64_t oh = e.key;
                auto* bucket = (*tgt_idx)->find_eq(oh);
                if (!bucket || bucket->empty()) continue;
                apply_row(oh, (*bucket)[0]);
                merged++;
            }
        } else {
            FlatHashMap<uint64_t, uint64_t> tgt_field_idx;
            for (auto& te : target->outer.occupied()) {
                if (!te.value.is_row || !te.value.val_py) continue;
                PyObject* fv = PyDict_GetItemString(te.value.val_py, tgt_fname.c_str());
                if (fv) tgt_field_idx.insert(content_hash_pyobj(fv), te.key);
            }
            for (auto& e : outer.occupied()) {
                if (!e.value.is_row) continue;
                uint64_t oh = e.key;
                uint64_t* other_oh = tgt_field_idx.find(oh);
                if (!other_oh) continue;
                apply_row(oh, *other_oh);
                merged++;
            }
        }
    } else if (!src_is_key && tgt_is_key) {
        std::string src_fname(on_source[0]);
        for (auto& e : outer.occupied()) {
            if (!e.value.is_row || !e.value.val_py) continue;
            uint64_t oh = e.key;
            PyObject* fv = PyDict_GetItemString(e.value.val_py, src_fname.c_str());
            if (!fv) continue;
            uint64_t other_oh = content_hash_pyobj(fv);
            if (!target->outer.find(other_oh)) continue;
            apply_row(oh, other_oh);
            merged++;
        }
    } else {
        std::string src_fname(on_source[0]);
        std::string tgt_fname(on_target[0]);
        FieldIndex** src_idx = indices.by_field.find(src_fname);
        FieldIndex** tgt_idx = target->indices.by_field.find(tgt_fname);
        if (src_idx) {
            used_src_idx = true;
            merge_src_fname = src_fname;
            merge_tgt_fname = tgt_fname;
            // Iterate target (potentially smaller), look up in self's index.
            // Inline the !has_field_filter fast path to skip redundant target->outer.find.
            for (auto& te : target->outer.occupied()) {
                if (!te.value.is_row || !te.value.val_py) continue;
                PyObject* fv = PyDict_GetItemString(te.value.val_py, tgt_fname.c_str());
                if (!fv) continue;
                auto* bucket = (*src_idx)->find_eq(content_hash_pyobj(fv));
                if (!bucket || bucket->empty()) continue;
                for (uint64_t self_oh : *bucket) {
                    if (!has_field_filter) {
                        OuterEntry* self_e = outer.find(self_oh);
                        if (!self_e || !self_e->is_row || !self_e->val_py) continue;
                        merge_dicts(self_e->val_py, te.value.val_py, conflict);
                    } else {
                        apply_row(self_oh, te.key);
                    }
                    merged++;
                }
            }
        } else if (tgt_idx) {
            // Target has index: iterate self, look up in target's index.
            for (auto& e : outer.occupied()) {
                if (!e.value.is_row || !e.value.val_py) continue;
                uint64_t oh = e.key;
                PyObject* fv = PyDict_GetItemString(e.value.val_py, src_fname.c_str());
                if (!fv) continue;
                auto* bucket = (*tgt_idx)->find_eq(content_hash_pyobj(fv));
                if (!bucket || bucket->empty()) continue;
                apply_row(oh, (*bucket)[0]);
                merged++;
            }
        } else {
            FlatHashMap<uint64_t, uint64_t> tgt_field_idx;
            for (auto& te : target->outer.occupied()) {
                if (!te.value.is_row || !te.value.val_py) continue;
                PyObject* fv = PyDict_GetItemString(te.value.val_py, tgt_fname.c_str());
                if (fv) tgt_field_idx.insert(content_hash_pyobj(fv), te.key);
            }
            for (auto& e : outer.occupied()) {
                if (!e.value.is_row || !e.value.val_py) continue;
                uint64_t oh = e.key;
                PyObject* fv = PyDict_GetItemString(e.value.val_py, src_fname.c_str());
                if (!fv) continue;
                uint64_t* other_oh = tgt_field_idx.find(content_hash_pyobj(fv));
                if (!other_oh) continue;
                apply_row(oh, *other_oh);
                merged++;
            }
        }
    }

    if (has_indices && merged > 0) {
        // Peek at one target row to find which top-level fields were actually written.
        // Simple (non-wildcard) indices for untouched fields can be skipped entirely.
        std::vector<std::string> target_top_fields;
        if (!has_field_filter) {
            for (auto& te : target->outer.occupied()) {
                if (!te.value.is_row || !te.value.val_py) continue;
                PyObject *k, *v; Py_ssize_t pos = 0;
                while (PyDict_Next(te.value.val_py, &pos, &k, &v))
                    if (PyUnicode_Check(k)) target_top_fields.push_back(PyUnicode_AsUTF8(k));
                break; // one row is enough — assume homogeneous schema
            }
        } else if (!self_field_segs.empty()) {
            // Field-path merge: only the top-level segment can affect simple indices
            target_top_fields.push_back(self_field_segs[0]);
        }

        for (auto& fi : indices.by_field.occupied()) {
            // Skip when join field values are unchanged (equality match on same field)
            if (used_src_idx && merge_src_fname == merge_tgt_fname &&
                fi.value->field_name == merge_src_fname &&
                (conflict == MergeConflict::KEEP_LEFT || conflict == MergeConflict::KEEP_RIGHT))
                continue;
            // Skip simple-field index if its field was not written during this merge
            if (!target_top_fields.empty() && !fi.value->is_wildcard &&
                !fi.value->field_name.empty()) {
                bool touched = false;
                for (const auto& f : target_top_fields)
                    if (f == fi.value->field_name) { touched = true; break; }
                if (!touched) continue;
            }
            if (fi.value->is_wildcard)
                fi.value->build_wildcard(this, fi.value->pattern);
            else
                fi.value->build(this, fi.value->field_name);
        }
    }

    if (on_merge_cb && merged > 0) {
        PyObject* cnt = PyLong_FromLong(merged);
        PyObject_CallOneArg(on_merge_cb, cnt);
        Py_DECREF(cnt);
    }
    return merged;
}

// ── create/drop/has index ────────────────────────────────────────────────────

static std::string pattern_key(const std::vector<std::string>& pattern) {
    std::string key;
    for (size_t i = 0; i < pattern.size(); i++) { if (i) key += '\x01'; key += pattern[i]; }
    return key;
}

void ModDict::create_index(const std::string& field_name) {
    if (indices.by_field.find(field_name)) return;
    FieldIndex* idx = new FieldIndex();
    idx->build(this, field_name);
    indices.by_field.insert(field_name, idx);
}

void ModDict::create_index(const std::vector<std::string>& pattern) {
    std::string key = pattern_key(pattern);
    if (indices.by_field.find(key)) return;
    FieldIndex* idx = new FieldIndex();
    idx->build_wildcard(this, pattern);
    indices.by_field.insert(key, idx);
}

void ModDict::drop_index(const std::string& field_name) {
    FieldIndex** f = indices.by_field.find(field_name);
    if (f) { delete *f; indices.by_field.erase(field_name); }
}

void ModDict::drop_index(const std::vector<std::string>& pattern) {
    std::string key = pattern_key(pattern);
    FieldIndex** f = indices.by_field.find(key);
    if (f) { delete *f; indices.by_field.erase(key); }
}

bool ModDict::has_index(const std::string& field_name) const {
    return indices.by_field.find(field_name) != nullptr;
}

bool ModDict::has_index(const std::vector<std::string>& pattern) const {
    return indices.by_field.find(pattern_key(pattern)) != nullptr;
}

// ── to_python_dict / dump / to_json ─────────────────────────────────────────

PyObject* ModDict::to_python_dict() const {
    PyObject* d = PyDict_New();
    if (!d) return nullptr;
    for (auto& e : outer.occupied()) {
        if (!e.value.key_py) continue;
        PyObject* py_val = e.value.is_row
            ? get_row(e.key)
            : (e.value.val_py ? (Py_INCREF(e.value.val_py), e.value.val_py) : (Py_INCREF(Py_None), Py_None));
        if (!py_val) { Py_DECREF(d); return nullptr; }
        PyDict_SetItem(d, e.value.key_py, py_val);
        Py_DECREF(py_val);
    }
    return d;
}

PyObject* ModDict::to_python() const { return to_python_dict(); }

void ModDict::dump() const {
    for (auto& e : outer.occupied()) {
        PyObject* py_val = e.value.is_row ? get_row(e.key)
                         : (e.value.val_py ? (Py_INCREF(e.value.val_py), e.value.val_py) : (Py_INCREF(Py_None), Py_None));
        PyObject* rk = e.value.key_py ? PyObject_Repr(e.value.key_py) : nullptr;
        PyObject* rv = py_val         ? PyObject_Repr(py_val)          : nullptr;
        printf("  %s : %s\n",
               rk ? PyUnicode_AsUTF8(rk) : "?",
               rv ? PyUnicode_AsUTF8(rv) : "?");
        Py_XDECREF(rk); Py_XDECREF(rv); Py_XDECREF(py_val);
    }
}

std::string ModDict::to_string() const {
    return "ModDict(" + std::to_string(outer.size()) + " entries)";
}

PyObject* ModDict::to_json() const {
    PyObject* d = to_python_dict();
    if (!d) return nullptr;
    PyObject* json_mod = PyImport_ImportModule("json");
    if (!json_mod) { Py_DECREF(d); return nullptr; }
    PyObject* result = PyObject_CallMethod(json_mod, "dumps", "O", d);
    Py_DECREF(json_mod);
    Py_DECREF(d);
    return result;
}

// ── Serialize / Deserialize ──────────────────────────────────────────────────

static constexpr uint32_t SERIAL_MAGIC   = 0x4D443034u;  // "MD04"
static constexpr uint8_t  SERIAL_VERSION = 1;

std::vector<uint8_t> ModDict::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(outer.size() * 320 + 256);

    write_u32(buf, SERIAL_MAGIC);
    buf.push_back(SERIAL_VERSION);
    write_u32(buf, (uint32_t)outer.size());

    for (auto& e : outer.occupied()) {
        if (!e.value.key_py) continue;
        write_u64(buf, e.key);
        buf.push_back(e.value.is_row ? 1 : 0);

        Serializer::serialize_pyobject(buf, e.value.key_py);
        Serializer::serialize_pyobject(buf, e.value.val_py ? e.value.val_py : Py_None);
    }
    return buf;
}

void ModDict::deserialize(const uint8_t* data, size_t len) {
    const uint8_t* ptr = data;
    const uint8_t* end = data + len;
    auto check = [&](size_t n) -> bool { return ptr + n <= end; };

    if (!check(5)) {
        PyErr_SetString(PyExc_ValueError, "ModDict.deserialize: buffer too short");
        return;
    }

    uint32_t magic = read_u32(ptr);
    if (magic != SERIAL_MAGIC) {
        PyErr_SetString(PyExc_ValueError,
            "ModDict.deserialize: bad magic (re-serialize with current version)");
        return;
    }
    uint8_t ver = *ptr++;
    if (ver != SERIAL_VERSION) {
        PyErr_SetString(PyExc_ValueError, "ModDict.deserialize: unsupported version");
        return;
    }

    constexpr uint32_t MAX_ENTRIES = 100'000'000u;
#define CHECK_LIMIT(n, lim, msg) if ((n) > (lim)) { \
    PyErr_SetString(PyExc_ValueError, "ModDict.deserialize: " msg); return; }

    order.clear();
    if (!check(4)) goto truncated;
    {
        uint32_t n_outer = read_u32(ptr);
        CHECK_LIMIT(n_outer, MAX_ENTRIES, "too many entries");
        order.reserve(n_outer);
        outer.reserve(n_outer);

        for (uint32_t i = 0; i < n_outer; i++) {
            if (!check(9)) goto truncated;
            uint64_t oh     = read_u64(ptr);
            bool     is_row = (*ptr++ != 0);

            ModValue key_mv = Serializer::deserialize_value(ptr, end, nullptr);
            if (!key_mv.obj) goto truncated;

            ModValue val_mv = Serializer::deserialize_value(ptr, end, nullptr);
            if (!val_mv.obj) goto truncated;

            PyObject* k = key_mv.obj;
            PyObject* v = val_mv.obj;
            Py_INCREF(k); Py_INCREF(v);
            outer.insert(oh, {k, v, is_row});
            order.push_back(oh);
        }
    }

#undef CHECK_LIMIT
    return;

truncated:
    PyErr_SetString(PyExc_ValueError, "ModDict.deserialize: buffer truncated");
}

bool ModDict::has_container_magic(const uint8_t* data, size_t len) {
    if (len < 4) return false;
    uint32_t magic = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                      ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    return magic == SERIAL_MAGIC;
}
