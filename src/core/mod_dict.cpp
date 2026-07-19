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

// Forward decls — defined later in this file, needed by reindex_row() above
// their definition (links validation runs on every reindex, not just link()).
static std::string pattern_key(const std::vector<std::string>& pattern);
static const OuterEntry* resolve_table(const ModDict* self, const std::string& seg, uint64_t& out_hash);
static bool validate_link(ModDict* self, const LinkDecl& ld);

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
// Space/tab is a literal alias for '.' — normalize then split with the
// original strict single-char splitter. No collapsing: "meta  level" (two
// spaces) is "meta..level" after normalization and produces an empty
// segment, exactly like "meta..level" already does with dots — same
// strictness either way, no special-cased leniency for one separator.
// Field names containing a literal '.' or ' ' need the tuple/list path
// form instead (each element taken as one exact segment, no splitting).
static std::vector<std::string> split_path(const std::string& field) {
    std::string norm = field;
    for (char& c : norm) if (c == ' ' || c == '\t') c = '.';
    std::vector<std::string> segs;
    size_t pos = 0;
    while (true) {
        size_t d = norm.find('.', pos);
        segs.push_back(norm.substr(pos, d == std::string::npos ? d : d - pos));
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

ModDict::ModDict() : indices(own_indices) {}

ModDict::ModDict(ModDict* anchor_root, std::vector<std::string> path)
    : indices(anchor_root->indices), root(anchor_root), anchor_path(std::move(path)) {}

ModDict::~ModDict() {
    for (auto& e : outer.occupied()) {
        Py_XDECREF(e.value.key_py);
        Py_XDECREF(e.value.val_py);
    }
    // A cursor's `indices` aliases root->indices — only the true owner frees.
    if (!root) for (auto& fi : indices.by_field.occupied()) delete fi.value;
    Py_XDECREF(on_change_cb);
    Py_XDECREF(on_merge_cb);
    Py_XDECREF(filter_predicate);
    Py_XDECREF(live_connect_listeners);
    for (PyObject* k : sort_index) Py_XDECREF(k);
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

void ModDict::insert_row(const ModValue& outer_key, PyObject* dict_obj, bool skip_field_index) {
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
        if (!skip_field_index)
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

    if (!skip_field_index)
        for (auto& fi : indices.by_field.occupied())
            fi.value->on_insert_row(oh, this);

    true_root()->notify_live_cursors(oh);
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

void ModDict::reindex_row_no_validate(uint64_t oh, ModDict* originator) {
    // if accessed via alias, reindex the original key so the index stays correct
    if (const uint64_t* orig = alias_to_orig.find(oh)) oh = *orig;
    for (auto& fi : indices.by_field.occupied()) {
        fi.value->remove_outer_key(oh);
        fi.value->on_insert_row(oh, this);
    }
    // Covers reindex_row() too (it delegates here first) — single hook, no
    // double-notification. Fires even if reindex_row()'s later link
    // validation raises: the data write already happened and indices
    // already reflect it, independent of whether that raises afterward.
    true_root()->notify_live_cursors(oh, originator);
}

void ModDict::reindex_row(uint64_t oh) {
    // if accessed via alias, reindex the original key so the index stays correct
    if (const uint64_t* orig = alias_to_orig.find(oh)) oh = *orig;
    reindex_row_no_validate(oh);

    // If this row is a declared link's SOURCE table, re-validate it now —
    // catches a dangling reference introduced by whatever write triggered
    // this reindex (new row, changed field), not just at link() declaration
    // time. Raises on the first bad reference found (PyErr set); the write
    // itself already happened (no rollback — same as any Python exception
    // raised after a successful dict mutation elsewhere in this codebase).
    if (!links.empty()) {
        const OuterEntry* te = outer.find(oh);
        if (te && te->key_py && PyUnicode_Check(te->key_py)) {
            const char* table = PyUnicode_AsUTF8(te->key_py);
            if (table) {
                std::string table_s(table);
                for (auto& ld : links) {
                    if (ld.source_pattern[0] == table_s && !validate_link(this, ld)) return;
                }
            } else {
                PyErr_Clear();
            }
        }
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
        true_root()->notify_live_cursors(orig_h);
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
    true_root()->notify_live_cursors(h);
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

// ── "->" link-hop resolution (shared by prune_match and select_anchored) ─────

enum class LinkHopResult { OK, NO_MATCH, NO_LINK };

// Resolves one "->" hop: fk_val is the value just read from `field` on a row
// of `current_table`. Looks up the link declared for exactly
// {current_table, "?", field} and, if found, resolves fk_val against its
// target (pk or non-pk, mirroring follow()'s own resolution). NO_LINK if no
// such link was declared — caller should raise. NO_MATCH if fk_val is
// None/missing/dangling — never an error, same nullable-FK semantics as
// everywhere else in the links feature. On OK, *out_ld/*out_target_row are
// borrowed (target_row lives in the table dict, ld lives in self->links).
static LinkHopResult resolve_link_hop(const ModDict* self, const std::string& current_table,
                                       const std::string& field, PyObject* fk_val,
                                       const LinkDecl** out_ld, PyObject** out_target_row)
{
    std::vector<std::string> source_pattern{current_table, "__pass_key__", field};
    const LinkDecl* ld = self->find_link(source_pattern);
    if (!ld) return LinkHopResult::NO_LINK;
    *out_ld = ld;
    if (!fk_val || fk_val == Py_None) return LinkHopResult::NO_MATCH;

    uint64_t target_anchor_hash = 0;
    const OuterEntry* target_anchor = resolve_table(self, ld->references_pattern[0], target_anchor_hash);
    if (!target_anchor || !PyDict_Check(target_anchor->val_py)) return LinkHopResult::NO_MATCH;

    bool target_is_pk = (ld->references_pattern.size() == 2);
    PyObject* target_row = nullptr;
    if (target_is_pk) {
        target_row = PyDict_GetItem(target_anchor->val_py, fk_val);
    } else {
        auto* p = self->indices.by_field.find(pattern_key(ld->references_pattern));
        FieldIndex* target_field_idx = p ? *p : nullptr;
        auto* leaf = target_field_idx ? target_field_idx->find_wildcard_leaf_eq(content_hash_pyobj(fk_val)) : nullptr;
        if (leaf) {
            for (auto& m : *leaf) {
                if (m.second.empty()) continue;
                target_row = PyDict_GetItem(target_anchor->val_py, m.second[0]);
                if (target_row) break;
            }
        }
    }
    if (!target_row) return LinkHopResult::NO_MATCH;
    *out_target_row = target_row;
    return LinkHopResult::OK;
}

// Returns:
//   nullptr  — no match
//   Py_True  — matched, caller should use full original row (terminal ? or simple field)
//   new dict — matched, pruned to only matching inner entries (caller owns ref)
// `self`/`current_table` are only used when a "__follow_link__" hop is hit —
// current_table is the table `cur` belongs to right now, updated to the hop's
// target table after a successful jump (see the jump_next branch below).
static PyObject* prune_match(PyObject* cur,
                              const std::vector<std::string>& pat, size_t depth,
                              FilterOp op, const ModValue& val,
                              const ModDict* self, const std::string& current_table)
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
                PyObject* sub = prune_match(v, pat, depth + 1, op, val, self, current_table);
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
        bool jump_next = (depth + 1 < pat.size() && pat[depth + 1] == "__follow_link__");
        if (last) {
            ModValue fv = ModValue::from_pyobject(child);
            return compare_values(fv, op, val) ? Py_True : nullptr;
        } else if (jump_next) {
            // `child` is an FK value (not a nested dict) — resolve the "->" hop
            // and continue matching on the target row. Propagate the target's
            // result AS-IS (Py_True/nullptr), never wrapped under this field's
            // key: a "...field->..." hop collapses to a single yes/no about the
            // ORIGINAL row, same as an ordinary terminal-field match.
            const LinkDecl* ld = nullptr; PyObject* target_row = nullptr;
            LinkHopResult res = resolve_link_hop(self, current_table, pat[depth], child, &ld, &target_row);
            if (res == LinkHopResult::NO_LINK) {
                PyErr_SetString(PyExc_ValueError,
                    "filter: no link declared for this source_path - call mn.link() first");
                return nullptr;
            }
            if (res == LinkHopResult::NO_MATCH) return nullptr;
            return prune_match(target_row, pat, depth + 2, op, val, self, ld->references_pattern[0]);
        } else {
            PyObject* sub = prune_match(child, pat, depth + 1, op, val, self, current_table);
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
    // "->" hop(s) present — dispatch away from the ordinary single-table
    // engine entirely (never builds/touches a FieldIndex over a pattern
    // containing the "__follow_link__" sentinel). EQ gets an index-chain-join
    // fast path; every other op falls back to a per-anchor-row linear scan
    // via the link-aware prune_match.
    if (std::find(pattern.begin(), pattern.end(), "__follow_link__") != pattern.end()) {
        if (op == FilterOp::EQ) return filter_linked_eq(pattern, value);
        ModDict* result = new ModDict();
        uint64_t anchor_hash = 0;
        const OuterEntry* anchor_entry = resolve_table(this, pattern[0], anchor_hash);
        if (!anchor_entry || !PyDict_Check(anchor_entry->val_py)) return result;
        PyObject* pruned = prune_match(anchor_entry->val_py, pattern, 1, op, value, this, pattern[0]);
        if (pruned) {
            if (pruned == Py_True) filter_add_row(result, this, anchor_hash);
            else { filter_add_pruned_row(result, this, anchor_hash, pruned); Py_DECREF(pruned); }
        } else if (PyErr_Occurred()) {
            delete result; return nullptr;
        }
        return result;
    }

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
        // self/current_table only matter if `pattern` contains "__follow_link__",
        // which can't happen here (that shape is handled entirely by the
        // early-return branch above, before this lambda is ever built).
        PyObject* pruned = prune_match(val_py, pattern, start_depth, op, value, this, pattern.empty()?std::string():pattern[0]);
        if (!pruned) return;
        if (pruned == Py_True) {
            filter_add_row(result, this, oh);
        } else {
            filter_add_pruned_row(result, this, oh, pruned);
            Py_DECREF(pruned);
        }
    };

    bool terminal_pass_key = !pattern.empty() && pattern.back() == "__pass_key__";
    // idx->is_wildcard just means "built via build_wildcard" — true for ANY
    // multi-segment pattern, including purely literal ones like "meta.score"
    // or a single-element tuple ("first name",) with no "?" anywhere.
    // wildcard_leaf_index is only populated when a "__pass_key__" actually
    // occurs in the pattern — has_pass_key is the real signal for which EQ
    // path applies; a purely literal pattern has no wildcard to prune, so it
    // must fall through to the plain find_eq()+filter_add_row() path below.
    bool has_pass_key = std::find(pattern.begin(), pattern.end(), "__pass_key__") != pattern.end();

    if (op == FilterOp::EQ && has_pass_key && terminal_pass_key) {
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
    } else if (op == FilterOp::EQ && has_pass_key) {
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

ModDict* ModDict::select(const std::vector<std::string>& paths, const std::vector<std::string>& labels) const {
    // Pre-split all field paths once
    std::vector<std::vector<std::string>> split_paths;
    split_paths.reserve(paths.size());
    for (const auto& f : paths) split_paths.push_back(split_path(f));

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
        for (size_t i = 0; i < paths.size(); i++) {
            std::vector<const char*> segs;
            for (const auto& s : split_paths[i]) segs.push_back(s.c_str());
            PyObject* fv = get_nested_segs(e.val_py, segs);
            if (fv) { PyDict_SetItemString(new_row, labels[i].c_str(), fv); has_any = true; }
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

// Like get_nested_segs, but understands "->" hops: on reaching a literal
// segment immediately followed by "__follow_link__", resolves it as an FK via
// resolve_link_hop() and continues on the target row instead of navigating
// into `cur` as a nested dict. Returns a borrowed value, or nullptr (no PyErr)
// on a missing/nullable/dangling path — same as get_nested_segs — or nullptr
// with PyErr set if a hop's link was never declared.
static PyObject* get_nested_via_links(const ModDict* self, PyObject* cur,
                                       const std::vector<std::string>& pat, size_t depth,
                                       std::string current_table)
{
    for (; depth < pat.size(); depth++) {
        if (!cur || !PyDict_Check(cur)) return nullptr;
        PyObject* child = PyDict_GetItemString(cur, pat[depth].c_str());
        if (!child) return nullptr;
        if (depth + 1 < pat.size() && pat[depth + 1] == "__follow_link__") {
            const LinkDecl* ld = nullptr; PyObject* target_row = nullptr;
            LinkHopResult res = resolve_link_hop(self, current_table, pat[depth], child, &ld, &target_row);
            if (res == LinkHopResult::NO_LINK) {
                PyErr_SetString(PyExc_ValueError, "select: no link declared for this source_path - call mn.link() first");
                return nullptr;
            }
            if (res == LinkHopResult::NO_MATCH) return nullptr;
            cur = target_row;
            current_table = ld->references_pattern[0];
            depth++;  // loop's own ++ then skips past the "__follow_link__" marker too
            continue;
        }
        cur = child;
    }
    return cur;
}

// ── select_anchored ─────────────────────────────────────────────────────────
// One or more patterns of shape [table, "?", ...], all sharing the same
// anchor table, optionally containing "->" hops. Unlike select()'s flat mode
// (which projects every outer entry of `this`), this scans ONE specific
// anchor table's rows and projects each into a flat result keyed by that
// row's own key — select()'s first-ever wildcard/anchor support.
ModDict* ModDict::select_anchored(const std::vector<std::vector<std::string>>& patterns,
                                   const std::vector<std::string>& field_labels) const
{
    ModDict* result = new ModDict();
    if (patterns.empty()) return result;

    const std::string& anchor_table = patterns[0][0];
    for (auto& p : patterns) {
        if (p[0] != anchor_table) {
            PyErr_SetString(PyExc_ValueError,
                "select: all wildcard fields must share the same anchor table");
            delete result; return nullptr;
        }
    }

    uint64_t anchor_hash = 0;
    const OuterEntry* anchor_entry = resolve_table(this, anchor_table, anchor_hash);
    if (!anchor_entry || !PyDict_Check(anchor_entry->val_py)) return result;

    PyObject *pk, *row; Py_ssize_t pos = 0;
    while (PyDict_Next(anchor_entry->val_py, &pos, &pk, &row)) {
        if (!PyDict_Check(row)) continue;
        PyObject* new_row = PyDict_New();
        if (!new_row) { delete result; return nullptr; }
        bool has_any = false;
        for (size_t i = 0; i < patterns.size(); i++) {
            PyObject* fv = get_nested_via_links(this, row, patterns[i], 2, anchor_table);
            if (PyErr_Occurred()) { Py_DECREF(new_row); delete result; return nullptr; }
            if (fv) { PyDict_SetItemString(new_row, field_labels[i].c_str(), fv); has_any = true; }
        }
        if (has_any) {
            Py_INCREF(pk);
            uint64_t rh = content_hash_pyobj(pk);
            result->outer.insert(rh, {pk, new_row, true});
            result->order.push_back(rh);
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

// ── Links ────────────────────────────────────────────────────────────────────
// v1 scope: source_pattern = [table, "__pass_key__", field] exactly.
// references_pattern = [table, "__pass_key__"] (pk) or [table, "__pass_key__", field] (non-pk).

const LinkDecl* ModDict::find_link(const std::vector<std::string>& source_pattern) const {
    for (auto& l : links) if (l.source_pattern == source_pattern) return &l;
    return nullptr;
}

// Resolves outer.find(hash of literal string seg) — used for the table/anchor
// segment of a link path (always segment 0, always a literal table name).
static const OuterEntry* resolve_table(const ModDict* self, const std::string& seg, uint64_t& out_hash) {
    PyObject* tmp = PyUnicode_FromStringAndSize(seg.c_str(), seg.size());
    if (!tmp) { PyErr_Clear(); return nullptr; }
    out_hash = content_hash_pyobj(tmp);
    Py_DECREF(tmp);
    const OuterEntry* e = self->outer.find(out_hash);
    return (e && e->val_py && e->is_row) ? e : nullptr;
}

// Literal (no-wildcard) multi-segment path resolution, for cursor anchors.
// path[0] must be a real top-level outer key (resolved the same way
// resolve_table() does) — its OWN row value is the target when path has
// only one segment (e.g. cursor("users") -> self["users"]'s value). Every
// further segment is a literal PyDict_GetItemString descent into nested raw
// dicts (e.g. cursor("u1.orders") -> self["u1"]["orders"]). Returns the
// final (borrowed) PyObject*, or nullptr if any segment is missing / an
// intermediate segment isn't dict-shaped. out_top_hash receives path[0]'s
// outer hash (needed by callers to key the live-cursor notification
// registry against the top-level row that, if it changes, could affect
// this anchor). Does NOT require the final result to be dict-shaped itself
// — callers that need a {pk:row}-shaped target (cursor()) check that themselves.
static PyObject* resolve_anchor_path(const ModDict* self,
                                      const std::vector<std::string>& path,
                                      uint64_t& out_top_hash) {
    if (path.empty()) return nullptr;
    const OuterEntry* top = resolve_table(self, path[0], out_top_hash);
    if (!top || !top->val_py) return nullptr;
    PyObject* cur = top->val_py;
    for (size_t i = 1; i < path.size(); i++) {
        if (!PyDict_Check(cur)) return nullptr;
        PyObject* next = PyDict_GetItemString(cur, path[i].c_str());  // borrowed
        if (!next) return nullptr;
        cur = next;
    }
    return cur;
}

ModDict* ModDict::cursor(const std::vector<std::string>& path) const {
    if (path.empty()) { PyErr_SetString(PyExc_ValueError, "cursor: path must not be empty"); return nullptr; }
    for (auto& seg : path) {
        if (seg == "__pass_key__" || seg == "__scan_key__") {
            PyErr_SetString(PyExc_ValueError, "cursor: path must be fully literal (no '?'/'*' wildcard segments)");
            return nullptr;
        }
    }
    ModDict* actual_root = true_root();
    std::vector<std::string> full_path = anchor_path;  // prefix from `this` (empty if this is itself a root)
    full_path.insert(full_path.end(), path.begin(), path.end());

    uint64_t top_hash = 0;
    PyObject* target = resolve_anchor_path(actual_root, full_path, top_hash);
    if (!target || !PyDict_Check(target)) {
        PyErr_SetString(PyExc_ValueError, "cursor: path does not resolve to an existing table");
        return nullptr;
    }
    return new ModDict(actual_root, std::move(full_path));
}

PyObject* ModDict::resolve_cursor_dict() {
    uint64_t top_hash = 0;
    PyObject* cur = resolve_anchor_path(root, anchor_path, top_hash);
    if (!cur || !PyDict_Check(cur)) {
        PyErr_SetString(PyExc_RuntimeError, "cursor: anchored table no longer exists");
        return nullptr;
    }
    cached_top_hash = top_hash;
    if (cur != cached_anchor_dict) {
        // Wholesale rebind (data["u1"]["orders"]=new_dict) bypasses every
        // mutation hook that would otherwise keep sort_index/filter_membership
        // in sync — detected here via identity mismatch instead.
        cached_anchor_dict = cur;
        if (has_derived_order) rebuild_sort_index();
        if (filter_predicate) { rebuild_filter_membership(); if (PyErr_Occurred()) return nullptr; }
    }
    return cur;
}

void ModDict::register_live_cursor(PyObject* weakref) {
    ModDict* actual_root = true_root();
    std::string key = pattern_key(anchor_path);
    auto* bucket = actual_root->live_cursors.find(key);
    if (bucket) bucket->push_back(weakref);
    else actual_root->live_cursors.insert(key, {weakref});
}

// Walks a literal dotted path (already split into segments) from `row`,
// returning the final (borrowed) PyObject*, or nullptr if any segment is
// missing / an intermediate value isn't dict-shaped. Used by set_sort/
// set_group to read a field's current value for comparison — cursor sort/
// group fields are always literal paths within one row, no "?" wildcard.
static PyObject* read_field_path(PyObject* row, const std::vector<std::string>& segs) {
    PyObject* cur = row;
    for (auto& seg : segs) {
        if (!PyDict_Check(cur)) return nullptr;
        PyObject* next = PyDict_GetItemString(cur, seg.c_str());  // borrowed
        if (!next) return nullptr;
        cur = next;
    }
    return cur;
}

std::vector<PyObject*> ModDict::current_presentation_order(PyObject* d) const {
    if (has_derived_order) return sort_index;  // copy of borrowed pointers
    std::vector<PyObject*> keys;
    PyObject *k, *v; Py_ssize_t pos = 0;
    while (PyDict_Next(d, &pos, &k, &v)) keys.push_back(k);
    return keys;
}

bool ModDict::less_by_values(const ModValue& ga, const ModValue& gb,
                              const ModValue& sa, const ModValue& sb) const {
    if (!group_field.empty()) {
        bool ok = true;
        int c = ga.compare(gb, &ok);
        if (!ok) {
            if (ga.obj && !gb.obj) return true;
            if (!ga.obj && gb.obj) return false;
            c = 0;
        }
        if (c != 0) return c < 0;
    }
    if (!sort_field.empty()) {
        bool ok = true;
        int c = sa.compare(sb, &ok);
        if (!ok) {
            if (sa.obj && !sb.obj) return !sort_reverse;
            if (!sa.obj && sb.obj) return sort_reverse;
            return false;
        }
        return sort_reverse ? c > 0 : c < 0;
    }
    return false;
}

bool ModDict::sort_index_less(PyObject* a, PyObject* b) const {
    PyObject* d = cached_anchor_dict;
    auto value_of = [&](PyObject* key, const std::vector<std::string>& path) -> ModValue {
        PyObject* row = PyDict_GetItem(d, key);  // borrowed
        PyObject* fv = row ? read_field_path(row, path) : nullptr;
        return fv ? ModValue::from_pyobject(fv) : ModValue();
    };
    ModValue ga = !group_field.empty() ? value_of(a, group_field) : ModValue();
    ModValue gb = !group_field.empty() ? value_of(b, group_field) : ModValue();
    ModValue sa = !sort_field.empty()  ? value_of(a, sort_field)  : ModValue();
    ModValue sb = !sort_field.empty()  ? value_of(b, sort_field)  : ModValue();
    return less_by_values(ga, gb, sa, sb);
}

void ModDict::rebuild_sort_index() {
    for (PyObject* k : sort_index) Py_XDECREF(k);
    sort_index.clear();
    has_derived_order = true;
    if (!cached_anchor_dict) return;

    bool need_group = !group_field.empty();
    bool need_sort  = !sort_field.empty();

    if (!need_group && !need_sort) {
        // No comparator needed at all — natural PyDict order, nothing to precompute.
        PyObject *k, *v; Py_ssize_t pos = 0;
        while (PyDict_Next(cached_anchor_dict, &pos, &k, &v)) { Py_INCREF(k); sort_index.push_back(k); }
        return;
    }

    // Precompute each row's sort-relevant field value(s) ONCE — O(n) field
    // reads/ModValue conversions — then sort by the precomputed values:
    // O(n log n) CHEAP comparisons with no repeated dict/path lookups. The
    // same decorate-sort-undecorate strategy Python's own
    // list.sort(key=...) uses internally. The naive alternative (call
    // sort_index_less(), which looks values up fresh, as std::sort's
    // comparator) redundantly re-extracts+re-converts the same field value
    // O(n log n) times instead of O(n) — this was the dominant, benchmark-
    // measured cost behind set_sort() being 20-30x slower than list.sort()
    // at 10k-50k rows; this fixes that specifically (set_sort()'s absolute
    // cost is still higher than a bare list.sort() — ModValue/PyObject
    // comparisons aren't free — just no longer asymptotically worse in the
    // number of times a field gets extracted).
    struct Entry { PyObject* key; ModValue group_val; ModValue sort_val; };
    std::vector<Entry> entries;
    PyObject *k, *v; Py_ssize_t pos = 0;
    while (PyDict_Next(cached_anchor_dict, &pos, &k, &v)) {
        Entry e; e.key = k;
        if (need_group) {
            PyObject* fv = read_field_path(v, group_field);
            e.group_val = fv ? ModValue::from_pyobject(fv) : ModValue();
        }
        if (need_sort) {
            PyObject* fv = read_field_path(v, sort_field);
            e.sort_val = fv ? ModValue::from_pyobject(fv) : ModValue();
        }
        entries.push_back(std::move(e));
    }

    std::sort(entries.begin(), entries.end(), [&](const Entry& x, const Entry& y) {
        return less_by_values(x.group_val, y.group_val, x.sort_val, y.sort_val);
    });

    for (auto& e : entries) { Py_INCREF(e.key); sort_index.push_back(e.key); }
}

bool ModDict::try_bisect_insert_sort_index(PyObject* key, Py_ssize_t& out_new_pos) {
    if (!has_derived_order || filter_predicate || !cached_anchor_dict) return false;
    if (!PyDict_GetItem(cached_anchor_dict, key)) return false;  // caller wrote it already; defensive

    auto it = std::lower_bound(sort_index.begin(), sort_index.end(), key,
        [&](PyObject* a, PyObject* b) { return sort_index_less(a, b); });
    out_new_pos = (Py_ssize_t)(it - sort_index.begin());
    Py_INCREF(key);
    sort_index.insert(it, key);
    return true;
}

Py_ssize_t ModDict::find_sort_index_position(PyObject* key) const {
    if (!has_derived_order) return -1;
    uint64_t h = content_hash_pyobj(key);
    for (size_t i = 0; i < sort_index.size(); i++)
        if (content_hash_pyobj(sort_index[i]) == h) return (Py_ssize_t)i;
    return -1;
}

void ModDict::rebuild_filter_membership() {
    filter_membership = FlatHashMap<uint64_t, char>();
    if (!filter_predicate || !cached_anchor_dict) return;
    PyObject *k, *v; Py_ssize_t pos = 0;
    while (PyDict_Next(cached_anchor_dict, &pos, &k, &v)) {
        PyObject* res = PyObject_CallOneArg(filter_predicate, v);
        if (!res) return;  // PyErr set — caller must check
        int truthy = PyObject_IsTrue(res);
        Py_DECREF(res);
        if (truthy < 0) return;  // PyErr set
        if (truthy) filter_membership.insert(content_hash_pyobj(k), 1);
    }
}

ModDict::IndexDiff ModDict::set_sort(const std::vector<std::string>& field, bool reverse) {
    IndexDiff diff;
    PyObject* d = resolve_cursor_dict();
    if (!d) return diff;  // PyErr already set

    std::vector<PyObject*> old_order = current_presentation_order(d);
    FlatHashMap<uint64_t, Py_ssize_t> old_pos;
    for (size_t i = 0; i < old_order.size(); i++) old_pos.insert(content_hash_pyobj(old_order[i]), (Py_ssize_t)i);

    sort_field = field;
    sort_reverse = reverse;
    rebuild_sort_index();

    for (size_t i = 0; i < sort_index.size(); i++) {
        uint64_t h = content_hash_pyobj(sort_index[i]);
        const Py_ssize_t* op = old_pos.find(h);
        diff.emplace_back(op ? *op : (Py_ssize_t)-1, (Py_ssize_t)i);
    }
    return diff;
}

ModDict::IndexDiff ModDict::set_group(const std::vector<std::string>& group_by_field) {
    IndexDiff diff;
    PyObject* d = resolve_cursor_dict();
    if (!d) return diff;

    std::vector<PyObject*> old_order = current_presentation_order(d);
    FlatHashMap<uint64_t, Py_ssize_t> old_pos;
    for (size_t i = 0; i < old_order.size(); i++) old_pos.insert(content_hash_pyobj(old_order[i]), (Py_ssize_t)i);

    group_field = group_by_field;
    rebuild_sort_index();

    for (size_t i = 0; i < sort_index.size(); i++) {
        uint64_t h = content_hash_pyobj(sort_index[i]);
        const Py_ssize_t* op = old_pos.find(h);
        diff.emplace_back(op ? *op : (Py_ssize_t)-1, (Py_ssize_t)i);
    }
    return diff;
}

ModDict::IndexDiff ModDict::set_filter(PyObject* predicate) {
    IndexDiff diff;
    PyObject* d = resolve_cursor_dict();
    if (!d) return diff;

    std::vector<PyObject*> old_order = current_presentation_order(d);
    FlatHashMap<uint64_t, Py_ssize_t> old_pos;
    for (size_t i = 0; i < old_order.size(); i++) old_pos.insert(content_hash_pyobj(old_order[i]), (Py_ssize_t)i);
    // old *visible* positions only — rows the previous filter (if any) excluded
    // don't get an old_pos entry, matching "old=-1 means newly appeared".
    // FlatHashMap has no copy ctor (see flat_hash_map.h) — move it out; it's
    // about to be discarded/rebuilt below regardless.
    FlatHashMap<uint64_t, char> old_membership = std::move(filter_membership);
    bool had_filter = (filter_predicate != nullptr);

    Py_XDECREF(filter_predicate);
    Py_XINCREF(predicate);
    filter_predicate = predicate;
    if (filter_predicate) {
        rebuild_filter_membership();
        if (PyErr_Occurred()) return diff;
    } else {
        filter_membership = FlatHashMap<uint64_t, char>();
    }

    // Compose: iterate the current full row set in its current presentation
    // order (old_order — unaffected by the filter change itself, since
    // set_filter never touches sort_index/has_derived_order), keep only rows
    // passing the new filter (or all, if now inactive); "new" positions are
    // dense over survivors. old_index is only defined for rows that were
    // ALSO visible under the OLD filter (or unconditionally, if there wasn't one).
    Py_ssize_t new_i = 0;
    for (PyObject* key : old_order) {
        uint64_t h = content_hash_pyobj(key);
        bool passes_new = !filter_predicate || filter_membership.find(h);
        if (!passes_new) continue;
        bool visible_old = !had_filter || old_membership.find(h);
        const Py_ssize_t* op = old_pos.find(h);
        diff.emplace_back(visible_old && op ? *op : (Py_ssize_t)-1, new_i++);
    }
    return diff;
}

ModDict::IndexDiff ModDict::resync_and_diff() {
    IndexDiff diff;
    if (!cached_anchor_dict) return diff;

    // Hash old_order's keys UP FRONT and never dereference the raw pointers
    // again after this point — rebuild_sort_index() below DECREFs
    // sort_index's old entries, and if a row was just deleted (its ONLY
    // other reference, the dict's own, already dropped via PyDict_DelItem),
    // that DECREF can be the one that actually frees it. A hash computed
    // now stays valid; the pointer itself might not.
    std::vector<PyObject*> old_order = current_presentation_order(cached_anchor_dict);
    FlatHashMap<uint64_t, Py_ssize_t> old_pos;
    std::vector<uint64_t> old_hashes;
    old_hashes.reserve(old_order.size());
    for (size_t i = 0; i < old_order.size(); i++) {
        uint64_t h = content_hash_pyobj(old_order[i]);
        old_hashes.push_back(h);
        old_pos.insert(h, (Py_ssize_t)i);
    }
    bool had_filter = (filter_predicate != nullptr);
    FlatHashMap<uint64_t, char> old_membership = had_filter ? std::move(filter_membership) : FlatHashMap<uint64_t, char>();

    if (has_derived_order) rebuild_sort_index();
    if (had_filter) {
        rebuild_filter_membership();
        if (PyErr_Occurred()) return diff;
    }

    std::vector<PyObject*> new_order = current_presentation_order(cached_anchor_dict);
    FlatHashMap<uint64_t, Py_ssize_t> new_pos;
    std::vector<uint64_t> new_hashes;
    new_hashes.reserve(new_order.size());
    for (size_t i = 0; i < new_order.size(); i++) {
        uint64_t h = content_hash_pyobj(new_order[i]);
        new_hashes.push_back(h);
        new_pos.insert(h, (Py_ssize_t)i);
    }

    // Union of "was visible old" and "is visible new" hashes, deduped — a
    // plain removal or a filter-caused disappearance both need a
    // (old_index, -1) entry even though the row isn't in new_order at all.
    FlatHashMap<uint64_t, char> seen;
    auto consider = [&](uint64_t h) {
        if (seen.find(h)) return;
        seen.insert(h, 1);
        bool visible_old = !had_filter || old_membership.find(h);
        bool visible_new = !filter_predicate || filter_membership.find(h);
        const Py_ssize_t* op = old_pos.find(h);
        const Py_ssize_t* np = new_pos.find(h);
        Py_ssize_t oi = (visible_old && op) ? *op : (Py_ssize_t)-1;
        Py_ssize_t ni = (visible_new && np) ? *np : (Py_ssize_t)-1;
        if (oi == -1 && ni == -1) return;  // never visible on either side
        if (oi == ni) return;              // unchanged position, nothing to report
        diff.emplace_back(oi, ni);
    };
    for (uint64_t h : old_hashes) consider(h);
    for (uint64_t h : new_hashes) consider(h);
    return diff;
}

PyObject* index_diff_to_pylist(const ModDict::IndexDiff& diff) {
    PyObject* result = PyList_New((Py_ssize_t)diff.size());
    if (!result) return nullptr;
    for (size_t i = 0; i < diff.size(); i++) {
        Py_ssize_t old_i = diff[i].first, new_i = diff[i].second;
        PyObject* ko;
        if (old_i < 0) { Py_INCREF(Py_None); ko = Py_None; }
        else { ko = PyLong_FromSsize_t(old_i); if (!ko) { Py_DECREF(result); return nullptr; } }
        PyObject* vo = PyLong_FromSsize_t(new_i);
        if (!vo) { Py_DECREF(ko); Py_DECREF(result); return nullptr; }
        PyObject* tup = PyTuple_Pack(2, ko, vo);
        Py_DECREF(ko); Py_DECREF(vo);
        if (!tup) { Py_DECREF(result); return nullptr; }
        PyList_SET_ITEM(result, i, tup);  // steals the reference
    }
    return result;
}

PyObject* py_index_or_none(Py_ssize_t idx) {
    if (idx < 0) Py_RETURN_NONE;
    return PyLong_FromSsize_t(idx);
}

PyObject* py_index_list(const std::vector<Py_ssize_t>& positions) {
    PyObject* result = PyList_New((Py_ssize_t)positions.size());
    if (!result) return nullptr;
    for (size_t i = 0; i < positions.size(); i++) {
        PyObject* v = py_index_or_none(positions[i]);
        if (!v) { Py_DECREF(result); return nullptr; }
        PyList_SET_ITEM(result, i, v);  // steals the reference
    }
    return result;
}

void ModDict::dispatch_event(const char* event_type, PyObject* payload) {
    if (!live_connect_listeners) return;
    PyObject* key = PyUnicode_FromString(event_type);
    if (!key) return;
    PyObject* listeners = PyDict_GetItem(live_connect_listeners, key);  // borrowed
    Py_DECREF(key);
    if (!listeners) return;
    Py_ssize_t n = PyList_Size(listeners);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* cb = PyList_GetItem(listeners, i);  // borrowed
        PyObject* res = PyObject_CallOneArg(cb, payload);
        if (!res) return;  // listener raised — propagate (fail loud), stop this dispatch
        Py_DECREF(res);
    }
}

void ModDict::notify_live_cursors(uint64_t changed_top_hash, ModDict* originator) {
    ModDict* actual_root = true_root();
    for (auto& bucket : actual_root->live_cursors.occupied()) {
        auto& weakrefs = bucket.value;
        for (size_t i = 0; i < weakrefs.size(); ) {
            PyObject* wr = weakrefs[i];
            PyObject* target = PyWeakref_GetObject(wr);  // borrowed; Py_None if dead
            if (!target || target == Py_None) {
                Py_DECREF(wr);
                weakrefs.erase(weakrefs.begin() + (long)i);
                continue;
            }
            ModDict* cur = ModDict_unwrap(target);
            if (cur && cur != originator && !cur->anchor_path.empty()) {
                PyObject* tmp = PyUnicode_FromStringAndSize(cur->anchor_path[0].c_str(), cur->anchor_path[0].size());
                uint64_t seg0_hash = tmp ? content_hash_pyobj(tmp) : 0;
                Py_XDECREF(tmp);
                if (tmp && seg0_hash == changed_top_hash) {
                    // Ensures cached_anchor_dict is populated (a never-yet-
                    // read sibling cursor starts with it null) before diffing.
                    if (!cur->resolve_cursor_dict()) { PyErr_Clear(); i++; continue; }
                    IndexDiff diff = cur->resync_and_diff();
                    if (PyErr_Occurred()) { PyErr_Clear(); i++; continue; }  // don't let one bad cursor abort the broadcast
                    PyObject* payload = index_diff_to_pylist(diff);
                    if (payload) { cur->dispatch_event("reorder", payload); Py_DECREF(payload); }
                    if (PyErr_Occurred()) PyErr_Clear();  // a listener's exception shouldn't abort the broadcast either
                }
            }
            i++;
        }
    }
}

Py_ssize_t ModDict::cursor_insert(PyObject* key, PyObject* row) {
    PyObject* d = resolve_cursor_dict();
    if (!d) return -1;
    // Bootstrap sort_index as a maintained snapshot (natural order if no
    // sort/group field set) BEFORE the write — without this, a fallback
    // rebuild below would have nothing meaningful to distinguish "before"
    // from "after". Only matters on first use; a no-op once already active.
    if (!has_derived_order) rebuild_sort_index();
    // Checked BEFORE the write: try_bisect_insert_sort_index() is only valid
    // for a genuinely new key (see its own comment) — an overwrite of an
    // existing key can move that row's sort position, which the O(log n)
    // bisect path doesn't handle, so it must fall back to a full rebuild.
    bool is_new_key = !PyDict_Contains(d, key);
    if (PyDict_SetItem(d, key, row) != 0) return -1;
    // Notifies siblings (excluding `this`, via originator) — reindex_row_no_
    // validate() already does this; a second explicit call here would
    // re-notify the same siblings a second time for nothing (their state
    // is already resynced by the first call, so the second finds no
    // further change and fires an empty, redundant event).
    true_root()->reindex_row_no_validate(cached_top_hash, this);

    Py_ssize_t new_pos = -1;
    if (is_new_key && try_bisect_insert_sort_index(key, new_pos)) return new_pos;

    // Fallback (filter active, or an existing key overwritten): a full
    // rebuild is still needed for correctness — this row's sort position
    // could move by an arbitrary amount, or filter visibility could flip —
    // but only THIS row's own resulting position needs reporting (see the
    // class-level comment on cursor_insert's declaration).
    if (has_derived_order) rebuild_sort_index();
    if (filter_predicate) {
        rebuild_filter_membership();
        if (PyErr_Occurred()) return -1;
        if (!filter_membership.find(content_hash_pyobj(key))) return -1;
    }
    return find_sort_index_position(key);
}

std::pair<Py_ssize_t,Py_ssize_t> ModDict::cursor_update_row(PyObject* key, PyObject* changes) {
    PyObject* d = resolve_cursor_dict();
    if (!d) return {-1, -1};
    PyObject* row = PyDict_GetItem(d, key);  // borrowed
    if (!row) { PyErr_SetObject(PyExc_KeyError, key); return {-1, -1}; }
    if (!has_derived_order) rebuild_sort_index();  // see cursor_insert() comment

    Py_ssize_t old_pos = find_sort_index_position(key);
    bool was_visible = !filter_predicate || filter_membership.find(content_hash_pyobj(key));
    if (!was_visible) old_pos = -1;

    if (PyDict_Update(row, changes) != 0) return {-1, -1};
    true_root()->reindex_row_no_validate(cached_top_hash, this);  // notifies siblings — see cursor_insert() comment

    if (has_derived_order) rebuild_sort_index();
    Py_ssize_t new_pos = find_sort_index_position(key);
    if (filter_predicate) {
        rebuild_filter_membership();
        if (PyErr_Occurred()) return {old_pos, -1};
        if (!filter_membership.find(content_hash_pyobj(key))) new_pos = -1;
    }
    return {old_pos, new_pos};
}

Py_ssize_t ModDict::cursor_delete(PyObject* key) {
    PyObject* d = resolve_cursor_dict();
    if (!d) return -1;
    if (!PyDict_Contains(d, key)) { PyErr_SetObject(PyExc_KeyError, key); return -1; }
    if (!has_derived_order) rebuild_sort_index();  // see cursor_insert() comment

    Py_ssize_t old_pos = find_sort_index_position(key);
    bool was_visible = !filter_predicate || filter_membership.find(content_hash_pyobj(key));
    if (!was_visible) old_pos = -1;

    if (PyDict_DelItem(d, key) != 0) return -1;
    true_root()->reindex_row_no_validate(cached_top_hash, this);  // notifies siblings — see cursor_insert() comment

    if (has_derived_order) rebuild_sort_index();
    if (filter_predicate) {
        rebuild_filter_membership();
        if (PyErr_Occurred()) return old_pos;
    }
    return old_pos;
}

std::vector<Py_ssize_t> ModDict::cursor_insert_batch(PyObject* rows) {
    std::vector<Py_ssize_t> positions;
    PyObject* d = resolve_cursor_dict();
    if (!d) return positions;
    if (!has_derived_order) rebuild_sort_index();
    if (PyDict_Update(d, rows) != 0) return positions;  // merges every {key: row} pair in one call
    true_root()->reindex_row_no_validate(cached_top_hash, this);

    if (has_derived_order) rebuild_sort_index();
    if (filter_predicate) {
        rebuild_filter_membership();
        if (PyErr_Occurred()) return positions;
    }

    // A hash->position map built ONCE (O(n)) — an O(n) find_sort_index_
    // position() call per batch row would be O(n*k) for a k-row batch.
    FlatHashMap<uint64_t, Py_ssize_t> pos_by_hash;
    if (has_derived_order)
        for (size_t i = 0; i < sort_index.size(); i++)
            pos_by_hash.insert(content_hash_pyobj(sort_index[i]), (Py_ssize_t)i);

    PyObject *k, *v; Py_ssize_t pos = 0;
    while (PyDict_Next(rows, &pos, &k, &v)) {
        uint64_t h = content_hash_pyobj(k);
        if (filter_predicate && !filter_membership.find(h)) { positions.push_back(-1); continue; }
        const Py_ssize_t* p = pos_by_hash.find(h);
        positions.push_back(p ? *p : -1);
    }
    return positions;
}

// Validates that every current row matching ld.source_pattern resolves to a
// real row in ld.references_pattern's target (or is None/absent — a
// nullable FK, not a dangling reference). Shared by link() (at declaration,
// against existing data) and reindex_row() (after a write, against the
// row that just changed). Returns false with PyErr set on the first
// dangling reference found; true if everything currently resolves.
static bool validate_link(ModDict* self, const LinkDecl& ld) {
    bool target_is_pk = (ld.references_pattern.size() == 2);

    uint64_t target_anchor_hash = 0;
    const OuterEntry* target_anchor = resolve_table(self, ld.references_pattern[0], target_anchor_hash);
    if (!target_anchor || !PyDict_Check(target_anchor->val_py)) {
        PyErr_SetString(PyExc_ValueError, "link: references table not found (or not a table-shaped row)");
        return false;
    }

    FieldIndex* target_field_idx = nullptr;
    if (!target_is_pk) {
        self->create_index(ld.references_pattern);
        auto* p = self->indices.by_field.find(pattern_key(ld.references_pattern));
        target_field_idx = p ? *p : nullptr;
    }

    uint64_t source_anchor_hash = 0;
    const OuterEntry* source_anchor = resolve_table(self, ld.source_pattern[0], source_anchor_hash);
    if (!source_anchor || !PyDict_Check(source_anchor->val_py)) {
        PyErr_SetString(PyExc_ValueError, "link: source table not found (or not a table-shaped row)");
        return false;
    }

    PyObject *pk, *row; Py_ssize_t pos = 0;
    while (PyDict_Next(source_anchor->val_py, &pos, &pk, &row)) {
        if (!PyDict_Check(row)) continue;
        PyObject* val = PyDict_GetItemString(row, ld.source_pattern[2].c_str());
        if (!val || val == Py_None) continue;  // absent/None -> not a reference, skip

        bool resolves;
        if (target_is_pk) {
            resolves = PyDict_GetItem(target_anchor->val_py, val) != nullptr;
        } else {
            uint64_t vh = content_hash_pyobj(val);
            resolves = target_field_idx && target_field_idx->find_eq(vh) != nullptr;
        }
        if (!resolves) {
            PyErr_Format(PyExc_ValueError,
                "link: dangling reference - %R.%s = %R does not resolve to any row in '%s'",
                pk, ld.source_pattern[2].c_str(), val, ld.references_pattern[0].c_str());
            return false;
        }
    }
    return true;
}

void ModDict::link(const std::vector<std::string>& source_pattern,
                    const std::vector<std::string>& references_pattern,
                    LinkOnDelete on_delete)
{
    if (source_pattern.size() != 3 || source_pattern[1] != "__pass_key__") {
        PyErr_SetString(PyExc_ValueError,
            "link: source_path must be exactly 'table.?.field' (anchor, one wildcard, one field)");
        return;
    }
    if (!((references_pattern.size() == 2 && references_pattern[1] == "__pass_key__") ||
          (references_pattern.size() == 3 && references_pattern[1] == "__pass_key__"
           && references_pattern[2] != "__pass_key__"))) {
        PyErr_SetString(PyExc_ValueError,
            "link: references_path must be 'table.?' (pk) or 'table.?.field' (non-pk)");
        return;
    }

    if (find_link(source_pattern)) return;  // identical link already declared — no-op

    LinkDecl ld{source_pattern, references_pattern, on_delete};
    if (!validate_link(this, ld)) return;  // PyErr already set

    create_index(source_pattern);  // build reverse-lookup structures for follow()/cascade
    links.push_back(std::move(ld));
}

PyObject* ModDict::resolve_hop(std::string& current_table, const std::string& field,
                                PyObject* fk_val, bool* no_link) const
{
    const LinkDecl* ld = nullptr; PyObject* target_row = nullptr;
    LinkHopResult res = resolve_link_hop(this, current_table, field, fk_val, &ld, &target_row);
    *no_link = (res == LinkHopResult::NO_LINK);
    if (res != LinkHopResult::OK) return nullptr;
    current_table = ld->references_pattern[0];
    return target_row;
}

ModDict* ModDict::follow(const std::vector<std::string>& source_pattern,
                          const std::vector<uint64_t>* key_filter,
                          const std::vector<PyObject*>* value_filter) const
{
    const LinkDecl* ld = find_link(source_pattern);
    if (!ld) {
        PyErr_SetString(PyExc_ValueError, "follow: no link declared for this source_path — call mn.link() first");
        return nullptr;
    }

    uint64_t target_anchor_hash = 0;
    const OuterEntry* target_anchor = resolve_table(this, ld->references_pattern[0], target_anchor_hash);
    if (!target_anchor || !PyDict_Check(target_anchor->val_py)) {
        PyErr_SetString(PyExc_ValueError, "follow: references table not found");
        return nullptr;
    }
    bool target_is_pk = (ld->references_pattern.size() == 2);

    FieldIndex* target_field_idx = nullptr;
    if (!target_is_pk) {
        auto* p = indices.by_field.find(pattern_key(ld->references_pattern));
        target_field_idx = p ? *p : nullptr;
    }

    ModDict* result = new ModDict();

    auto add_target_row = [&](PyObject* key, PyObject* row) {
        uint64_t rh = content_hash_pyobj(key);
        if (result->outer.find(rh)) return;  // dedup: multiple sources may resolve to the same target row
        Py_INCREF(key); Py_INCREF(row);
        result->outer.insert(rh, {key, row, true});
        result->order.push_back(rh);
    };

    auto resolve_value = [&](PyObject* val) {
        if (!val || val == Py_None) return;  // nullable FK — no match, not an error
        if (target_is_pk) {
            PyObject* target_row = PyDict_GetItem(target_anchor->val_py, val);
            if (target_row) add_target_row(val, target_row);
        } else {
            auto* leaf = target_field_idx ? target_field_idx->find_wildcard_leaf_eq(content_hash_pyobj(val)) : nullptr;
            if (!leaf) return;
            for (auto& m : *leaf) {
                if (m.second.empty()) continue;
                PyObject* target_pk = m.second[0];
                PyObject* target_row = PyDict_GetItem(target_anchor->val_py, target_pk);
                if (target_row) add_target_row(target_pk, target_row);
            }
        }
    };

    if (value_filter) {
        // Bypass the source table entirely — resolve given values directly.
        for (PyObject* v : *value_filter) resolve_value(v);
        return result;
    }

    uint64_t source_anchor_hash = 0;
    const OuterEntry* source_anchor = resolve_table(this, source_pattern[0], source_anchor_hash);
    if (!source_anchor || !PyDict_Check(source_anchor->val_py)) {
        PyErr_SetString(PyExc_ValueError, "follow: source table not found");
        return nullptr;
    }

    PyObject *pk, *row; Py_ssize_t pos = 0;
    while (PyDict_Next(source_anchor->val_py, &pos, &pk, &row)) {
        if (!PyDict_Check(row)) continue;
        if (key_filter) {
            uint64_t kh = content_hash_pyobj(pk);
            if (std::find(key_filter->begin(), key_filter->end(), kh) == key_filter->end()) continue;
        }
        resolve_value(PyDict_GetItemString(row, source_pattern[2].c_str()));
    }
    return result;
}

// ── "->" EQ fast path ───────────────────────────────────────────────────────
// Chains FieldIndex::find_wildcard_leaf_eq lookups across each hop's already-
// built index — the source-side index every link() call builds, and the
// target-side index built lazily like any other wildcard filter — instead of
// scanning rows. Never touches field_index.cpp: every index it reads is an
// ordinary [table,"?",...] wildcard index, nothing "->"-aware about it.
ModDict* ModDict::filter_linked_eq(const std::vector<std::string>& pattern, const ModValue& value) const {
    // Split on "__follow_link__" into hop groups: groups[0] is hop 1's full
    // source pattern [T0,"?",F0]; groups[1..n-2] are single-field FK names for
    // subsequent hops; groups.back() is the final comparison path.
    std::vector<std::vector<std::string>> groups;
    {
        std::vector<std::string> cur_group;
        for (auto& seg : pattern) {
            if (seg == "__follow_link__") { groups.push_back(std::move(cur_group)); cur_group.clear(); }
            else cur_group.push_back(seg);
        }
        groups.push_back(std::move(cur_group));
    }
    size_t n_hops = groups.size() - 1;
    if (groups[0].size() != 3 || groups[0][1] != "__pass_key__") {
        PyErr_SetString(PyExc_ValueError, "filter: '->' left side must be a wildcard path like \"table.?.field\"");
        return nullptr;
    }

    // Left-to-right: resolve each hop's LinkDecl, tracking the table we land on.
    std::vector<const LinkDecl*> hop_lds;
    std::string cur_table = groups[0][0];
    std::vector<std::string> src_pat = groups[0];
    for (size_t i = 0; i < n_hops; i++) {
        if (i > 0) {
            if (groups[i].size() != 1) {
                PyErr_SetString(PyExc_ValueError, "filter: '->' hop must be a single field name, not a nested path");
                return nullptr;
            }
            src_pat = {cur_table, "__pass_key__", groups[i][0]};
        }
        const LinkDecl* ld = find_link(src_pat);
        if (!ld) {
            PyErr_SetString(PyExc_ValueError, "filter: no link declared for this source_path - call mn.link() first");
            return nullptr;
        }
        hop_lds.push_back(ld);
        cur_table = ld->references_pattern[0];
    }

    // Rightmost condition: index on [cur_table, "?", ...final_group] (built
    // lazily, same as any ordinary wildcard filter) -> matching PKs in cur_table.
    std::vector<std::string> final_pattern{cur_table, "__pass_key__"};
    for (auto& s : groups.back()) final_pattern.push_back(s);
    std::string final_key = pattern_key(final_pattern);
    auto* final_idx_ptr = indices.by_field.find(final_key);
    if (!final_idx_ptr) {
        const_cast<ModDict*>(this)->create_index(final_pattern);
        final_idx_ptr = indices.by_field.find(final_key);
    }
    FieldIndex* final_idx = final_idx_ptr ? *final_idx_ptr : nullptr;
    auto* leaf = final_idx ? final_idx->find_wildcard_leaf_eq(value.hash()) : nullptr;

    std::vector<PyObject*> pks;  // current hop's matching PKs (borrowed refs)
    if (leaf) for (auto& m : *leaf) if (!m.second.empty()) pks.push_back(m.second[0]);
    if (pks.empty()) return new ModDict();

    // Right-to-left: reverse-resolve through each hop's own source_pattern
    // index (guaranteed built — link() always calls create_index(source_pattern)).
    for (size_t i = n_hops; i-- > 0; ) {
        auto* src_idx_p = indices.by_field.find(pattern_key(hop_lds[i]->source_pattern));
        FieldIndex* src_idx = src_idx_p ? *src_idx_p : nullptr;
        std::vector<PyObject*> next_pks;
        std::unordered_map<uint64_t, char> seen;
        for (PyObject* pk : pks) {
            auto* rev_leaf = src_idx ? src_idx->find_wildcard_leaf_eq(content_hash_pyobj(pk)) : nullptr;
            if (!rev_leaf) continue;
            for (auto& m : *rev_leaf) {
                if (m.second.empty()) continue;
                PyObject* rpk = m.second[0];
                if (seen.emplace(content_hash_pyobj(rpk), 0).second) next_pks.push_back(rpk);
            }
        }
        pks = std::move(next_pks);
        if (pks.empty()) return new ModDict();
    }

    // pks now = matching keys of pattern[0] (the original anchor table).
    ModDict* result = new ModDict();
    uint64_t t0_hash = 0;
    const OuterEntry* t0_anchor = resolve_table(this, pattern[0], t0_hash);
    if (!t0_anchor || !PyDict_Check(t0_anchor->val_py)) return result;
    PyObject* pruned = PyDict_New();
    if (!pruned) { delete result; return nullptr; }
    for (PyObject* pk : pks) {
        PyObject* row = PyDict_GetItem(t0_anchor->val_py, pk);
        if (row) PyDict_SetItem(pruned, pk, row);
    }
    if (PyDict_Size(pruned) == 0) { Py_DECREF(pruned); return result; }
    Py_INCREF(t0_anchor->key_py);
    result->outer.insert(t0_hash, {t0_anchor->key_py, pruned, true});
    result->order.push_back(t0_hash);
    return result;
}

bool ModDict::delete_with_link_semantics(const std::string& table, PyObject* key) {
    uint64_t table_hash = 0;
    const OuterEntry* table_anchor = resolve_table(this, table, table_hash);
    if (!table_anchor || !PyDict_Check(table_anchor->val_py)) {
        PyErr_SetString(PyExc_KeyError, "table not found");
        return false;
    }
    PyObject* row_being_deleted = PyDict_GetItem(table_anchor->val_py, key);  // borrowed, valid pre-delete

    // Precompute, for every link targeting this table, the value referencing
    // rows would match against (the key itself for pk-based, or a field read
    // from the row NOW, before it's gone, for non-pk), and look up who
    // currently references that value.
    struct Pending {
        const LinkDecl* ld;
        const std::vector<std::pair<uint64_t,std::vector<PyObject*>>>* leaf;
    };
    std::vector<Pending> pending;
    for (auto& ld : links) {
        if (ld.references_pattern[0] != table) continue;
        bool target_is_pk = (ld.references_pattern.size() == 2);
        PyObject* match_value = key;
        if (!target_is_pk) {
            if (!row_being_deleted || !PyDict_Check(row_being_deleted)) continue;
            match_value = PyDict_GetItemString(row_being_deleted, ld.references_pattern[2].c_str());
            if (!match_value) continue;
        }
        auto* src_idx_p = indices.by_field.find(pattern_key(ld.source_pattern));
        FieldIndex* src_idx = src_idx_p ? *src_idx_p : nullptr;
        auto* leaf = src_idx ? src_idx->find_wildcard_leaf_eq(content_hash_pyobj(match_value)) : nullptr;
        pending.push_back({&ld, leaf});
    }

    // restrict — refuse before mutating anything.
    for (auto& p : pending) {
        if (p.ld->on_delete == LinkOnDelete::RESTRICT && p.leaf && !p.leaf->empty()) {
            PyErr_Format(PyExc_ValueError,
                "link: cannot delete key from '%s' - still referenced by %zu row(s) in '%s' (on_delete='restrict')",
                table.c_str(), p.leaf->size(), p.ld->source_pattern[0].c_str());
            return false;
        }
    }

    // Snapshot referencing pks for cascade/set_null now, while leaf pointers
    // are still valid (they live in the FieldIndex we're about to reindex).
    struct Effect { LinkOnDelete mode; std::string src_table; std::string src_field; std::vector<PyObject*> pks; };
    std::vector<Effect> effects;
    for (auto& p : pending) {
        if (p.ld->on_delete == LinkOnDelete::RESTRICT || !p.leaf || p.leaf->empty()) continue;
        Effect eff{p.ld->on_delete, p.ld->source_pattern[0], p.ld->source_pattern[2], {}};
        for (auto& m : *p.leaf) if (!m.second.empty()) { Py_INCREF(m.second[0]); eff.pks.push_back(m.second[0]); }
        effects.push_back(std::move(eff));
    }

    auto release_effects = [&]() { for (auto& e : effects) for (PyObject* p2 : e.pks) Py_DECREF(p2); };

    // Delete now, THEN reindex — this scrubs `key`'s own contribution to any
    // index (e.g. if `key` itself references something) before we look up
    // who referenced `key`, which is what makes cascade cycle-safe below.
    if (PyDict_DelItem(table_anchor->val_py, key) != 0) { release_effects(); return false; }
    reindex_row_no_validate(table_hash);

    for (auto& e : effects) {
        if (e.mode == LinkOnDelete::SET_NULL) {
            uint64_t sh = 0;
            const OuterEntry* src_anchor = resolve_table(this, e.src_table, sh);
            if (src_anchor && PyDict_Check(src_anchor->val_py)) {
                for (PyObject* rpk : e.pks) {
                    PyObject* rrow = PyDict_GetItem(src_anchor->val_py, rpk);
                    if (rrow) PyDict_SetItemString(rrow, e.src_field.c_str(), Py_None);
                }
                reindex_row_no_validate(sh);
            }
        } else if (e.mode == LinkOnDelete::CASCADE) {
            for (PyObject* rpk : e.pks) {
                if (!delete_with_link_semantics(e.src_table, rpk)) { release_effects(); return false; }
            }
        }
    }
    release_effects();
    return true;
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
        if (PyErr_Occurred()) return buf;
        Serializer::serialize_pyobject(buf, e.value.val_py ? e.value.val_py : Py_None);
        if (PyErr_Occurred()) return buf;
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
            if (!key_mv.obj) { if (PyErr_Occurred()) return; goto truncated; }

            ModValue val_mv = Serializer::deserialize_value(ptr, end, nullptr);
            if (!val_mv.obj) { if (PyErr_Occurred()) return; goto truncated; }

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
