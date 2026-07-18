#include "field_index.h"
#include "mod_dict.h"
#include <algorithm>

/* ============================================================================
   Helpers
   ============================================================================ */

static bool is_numeric(const ModValue& v) {
    return v.type == ValueType::INT || v.type == ValueType::FLOAT;
}

static SortedEntry make_sorted_entry(uint64_t outer_key_hash, const ModValue& field_val) {
    SortedEntry e;
    e.outer_key_hash = outer_key_hash;
    e.slot = SIZE_MAX;
    e.type = field_val.type;
    if (field_val.obj) {
        if (field_val.type == ValueType::INT)
            e.int_val = PyLong_AsLongLong(field_val.obj);
        else if (field_val.type == ValueType::FLOAT)
            e.float_val = PyFloat_AsDouble(field_val.obj);
        else
            e.int_val = 0;
    } else {
        e.int_val = 0;
    }
    return e;
}

/* ============================================================================
   build — simple field (one level deep)
   ============================================================================ */

void FieldIndex::build(ModDict* owner, const std::string& fname) {
    field_name   = fname;
    is_wildcard  = false;
    pattern.clear();
    clear();

    for (auto& e : owner->outer.occupied()) {
        if (!e.value.is_row || !e.value.val_py) continue;
        uint64_t oh = e.key;

        PyObject* fv_obj = PyDict_GetItemString(e.value.val_py, fname.c_str());
        if (!fv_obj) continue;

        // Use content_hash_pyobj directly to avoid Py_INCREF/DECREF overhead
        // of ModValue::from_pyobject for non-numeric fields.
        uint64_t fh = content_hash_pyobj(fv_obj);

        std::vector<uint64_t>* bucket = hash_index.find(fh);
        if (bucket) bucket->push_back(oh);
        else        hash_index.insert(fh, {oh});

        // Only build sorted_index for numeric fields (needed for range queries)
        bool is_num = !PyBool_Check(fv_obj) &&
                      (PyLong_Check(fv_obj) || PyFloat_Check(fv_obj));
        if (is_num) {
            SortedEntry se;
            se.outer_key_hash = oh;
            se.slot           = SIZE_MAX;
            if (PyLong_Check(fv_obj)) {
                se.type    = ValueType::INT;
                se.int_val = PyLong_AsLongLong(fv_obj);
            } else {
                se.type      = ValueType::FLOAT;
                se.float_val = PyFloat_AsDouble(fv_obj);
            }
            sorted_index.push_back(se);
        }
    }

    std::sort(sorted_index.begin(), sorted_index.end(),
              [](const SortedEntry& a, const SortedEntry& b) { return a.less_than(b); });
}

/* ============================================================================
   Anchor helper — if first path segment is a literal outer key, return its
   entry so callers can scope the scan to just that row (start at depth=1).
   Returns nullptr if the segment is a wildcard or not an outer key.
   ============================================================================ */

static const OuterEntry* find_anchor(ModDict* owner,
                                     const std::string& seg,
                                     uint64_t& out_hash)
{
    if (seg == "__pass_key__") return nullptr;
    PyObject* tmp = PyUnicode_FromStringAndSize(seg.c_str(), seg.size());
    if (!tmp) { PyErr_Clear(); return nullptr; }
    out_hash = content_hash_pyobj(tmp);
    Py_DECREF(tmp);
    const OuterEntry* e = owner->outer.find(out_hash);
    return (e && e->val_py && e->is_row) ? e : nullptr;
}

/* ============================================================================
   build_wildcard — pattern with __pass_key__ wildcards
   ============================================================================ */

// wc_path: the ordered chain of keys selected by each non-terminal
// "__pass_key__" segment seen so far during this descent (one entry per "?",
// one "?" = one level — deeper nesting is written explicitly as "?.?.field").
// Grown/shrunk (push_back/pop_back) as we enter/leave a wildcard level so the
// same vector instance is reused across the whole recursive descent.
static void collect_wildcard(PyObject* dict,
                              const std::vector<std::string>& pattern,
                              size_t depth,
                              uint64_t oh,
                              FieldIndex* idx,
                              std::vector<PyObject*>& wc_path)
{
    if (!dict || !PyDict_Check(dict)) return;
    if (depth >= pattern.size()) return;

    bool last = (depth == pattern.size() - 1);

    if (pattern[depth] == "__pass_key__") {
        PyObject *k, *v; Py_ssize_t pos = 0;
        while (PyDict_Next(dict, &pos, &k, &v)) {
            if (last) {
                // Terminal ? checks the KEY itself, not the value
                ModValue fv = ModValue::from_pyobject(k);
                uint64_t fh = fv.hash();
                std::vector<uint64_t>* bucket = idx->hash_index.find(fh);
                if (bucket) bucket->push_back(oh);
                else        idx->hash_index.insert(fh, {oh});
                if (fv.type == ValueType::INT || fv.type == ValueType::FLOAT)
                    idx->sorted_index.push_back(make_sorted_entry(oh, fv));
            } else {
                wc_path.push_back(k);
                collect_wildcard(v, pattern, depth + 1, oh, idx, wc_path);
                wc_path.pop_back();
            }
        }
    } else {
        PyObject* child = PyDict_GetItemString(dict, pattern[depth].c_str());
        if (!child) return;
        if (last) {
            ModValue fv = ModValue::from_pyobject(child);
            uint64_t fh = fv.hash();
            std::vector<uint64_t>* bucket = idx->hash_index.find(fh);
            if (bucket) bucket->push_back(oh);
            else        idx->hash_index.insert(fh, {oh});
            if (fv.type == ValueType::INT || fv.type == ValueType::FLOAT)
                idx->sorted_index.push_back(make_sorted_entry(oh, fv));
            if (!wc_path.empty()) {
                auto* wbucket = idx->wildcard_leaf_index.find(fh);
                if (wbucket) wbucket->push_back({oh, wc_path});
                else         idx->wildcard_leaf_index.insert(fh, {{oh, wc_path}});
            }
        } else {
            collect_wildcard(child, pattern, depth + 1, oh, idx, wc_path);
        }
    }
}

void FieldIndex::build_wildcard(ModDict* owner, const std::vector<std::string>& pat) {
    is_wildcard = true;
    pattern     = pat;
    field_name  = "";
    clear();

    uint64_t ah = 0;
    const OuterEntry* anchor = (!pat.empty()) ? find_anchor(owner, pat[0], ah) : nullptr;
    has_anchor  = (anchor != nullptr);
    anchor_hash = has_anchor ? ah : 0;

    if (anchor) {
        // Anchored path: only scan the one row matched by pat[0], start at depth=1
        std::vector<PyObject*> wc_path;
        collect_wildcard(anchor->val_py, pat, 1, ah, this, wc_path);
    } else {
        for (auto& e : owner->outer.occupied()) {
            if (!e.value.is_row || !e.value.val_py) continue;
            std::vector<PyObject*> wc_path;
            collect_wildcard(e.value.val_py, pat, 0, e.key, this, wc_path);
        }
    }

    std::sort(sorted_index.begin(), sorted_index.end(),
              [](const SortedEntry& a, const SortedEntry& b) { return a.less_than(b); });
}

void FieldIndex::clear() {
    hash_index = FlatHashMap<uint64_t, std::vector<uint64_t>>();
    sorted_index.clear();
    wildcard_leaf_index = FlatHashMap<uint64_t, std::vector<std::pair<uint64_t,std::vector<PyObject*>>>>();
}

/* ============================================================================
   Incremental updates
   ============================================================================ */

void FieldIndex::on_insert(uint64_t outer_key_hash, const ModValue& field_val) {
    uint64_t fh = field_val.hash();
    std::vector<uint64_t>* bucket = hash_index.find(fh);
    if (bucket) bucket->push_back(outer_key_hash);
    else        hash_index.insert(fh, {outer_key_hash});

    if (is_numeric(field_val)) {
        SortedEntry e = make_sorted_entry(outer_key_hash, field_val);
        auto pos = std::lower_bound(sorted_index.begin(), sorted_index.end(), e,
                                    [](const SortedEntry& a, const SortedEntry& b) { return a.less_than(b); });
        sorted_index.insert(pos, e);
    }
}

void FieldIndex::on_remove(uint64_t outer_key_hash, const ModValue& field_val) {
    uint64_t fh = field_val.hash();
    std::vector<uint64_t>* bucket = hash_index.find(fh);
    if (bucket) {
        auto it = std::find(bucket->begin(), bucket->end(), outer_key_hash);
        if (it != bucket->end()) bucket->erase(it);
        if (bucket->empty()) hash_index.erase(fh);
    }

    if (is_numeric(field_val)) {
        SortedEntry target = make_sorted_entry(outer_key_hash, field_val);
        auto lo = std::lower_bound(sorted_index.begin(), sorted_index.end(), target,
                                   [](const SortedEntry& a, const SortedEntry& b) { return a.less_than(b); });
        for (auto it = lo; it != sorted_index.end() && it->equal_to(target); ++it) {
            if (it->outer_key_hash == outer_key_hash) { sorted_index.erase(it); break; }
        }
    }
}

void FieldIndex::on_insert_row(uint64_t oh, ModDict* owner) {
    if (!is_wildcard) {
        PyObject* row = owner->get_row_ref(oh);
        if (!row) return;
        PyObject* fv_obj = PyDict_GetItemString(row, field_name.c_str());
        if (!fv_obj) return;
        ModValue fv = ModValue::from_pyobject(fv_obj);
        on_insert(oh, fv);
    } else if (has_anchor) {
        // Anchored pattern: only update index when the anchored row itself changes
        if (oh != anchor_hash) return;
        PyObject* row = owner->get_row_ref(oh);
        if (!row) return;
        std::vector<PyObject*> wc_path;
        collect_wildcard(row, pattern, 1, oh, this, wc_path);
    } else {
        // Unanchored pattern (the common case: pattern[0] is an ordinary
        // per-row field, e.g. "meta.level" or "orders.?.status") — every row
        // maintains its own slice of the index, scan from depth 0.
        PyObject* row = owner->get_row_ref(oh);
        if (!row) return;
        std::vector<PyObject*> wc_path;
        collect_wildcard(row, pattern, 0, oh, this, wc_path);
    }
}

void FieldIndex::on_remove_row(uint64_t oh, ModDict* owner) {
    if (!is_wildcard) {
        PyObject* row = owner->get_row_ref(oh);
        if (!row) return;
        PyObject* fv_obj = PyDict_GetItemString(row, field_name.c_str());
        if (!fv_obj) return;
        ModValue fv = ModValue::from_pyobject(fv_obj);
        on_remove(oh, fv);
    } else {
        // Anchored pattern: only update index when the anchored row itself is removed
        if (has_anchor && oh != anchor_hash) return;
        // Full rebuild is simplest for wildcard remove
        PyObject* row = owner->get_row_ref(oh);
        if (!row) return;

        // For each value under wildcard that this row contributed, remove from
        // hash_index/sorted_index (precise, single pass over the still-valid row).
        std::function<void(PyObject*, size_t)> collect_remove = [&](PyObject* dict, size_t depth) {
            if (!dict || !PyDict_Check(dict) || depth >= pattern.size()) return;
            bool last = (depth == pattern.size() - 1);
            if (pattern[depth] == "__pass_key__") {
                PyObject *k, *v; Py_ssize_t pos = 0;
                while (PyDict_Next(dict, &pos, &k, &v)) {
                    if (last) { ModValue fv = ModValue::from_pyobject(v); on_remove(oh, fv); }
                    else       collect_remove(v, depth + 1);
                }
            } else {
                PyObject* child = PyDict_GetItemString(dict, pattern[depth].c_str());
                if (!child) return;
                if (last) { ModValue fv = ModValue::from_pyobject(child); on_remove(oh, fv); }
                else       collect_remove(child, depth + 1);
            }
        };
        collect_remove(row, 0);

        // wildcard_leaf_index entries for this row can be scattered across
        // several buckets (one per matched leaf) — purge by oh wholesale
        // rather than trying to re-derive and match each exact path.
        std::vector<uint64_t> dead_wc_buckets;
        for (auto& b : wildcard_leaf_index.occupied()) {
            auto new_end = std::remove_if(b.value.begin(), b.value.end(),
                [oh](const std::pair<uint64_t,std::vector<PyObject*>>& p){ return p.first == oh; });
            b.value.erase(new_end, b.value.end());
            if (b.value.empty()) dead_wc_buckets.push_back(b.key);
        }
        for (uint64_t bk : dead_wc_buckets) wildcard_leaf_index.erase(bk);
    }
}

void FieldIndex::remove_outer_key(uint64_t oh) {
    if (!is_wildcard) {
        // Simple field: oh appears in at most one bucket — first match wins.
        uint64_t dead_bucket = 0; bool found = false;
        for (auto& b : hash_index.occupied()) {
            auto it = std::find(b.value.begin(), b.value.end(), oh);
            if (it != b.value.end()) {
                b.value.erase(it);
                if (b.value.empty()) { dead_bucket = b.key; found = true; }
                break;
            }
        }
        if (found) hash_index.erase(dead_bucket);
    } else {
        // Wildcard: oh can appear multiple times in one bucket (one per matching
        // inner key) and across multiple buckets (different inner values) — must
        // scan and remove every occurrence, not just the first.
        std::vector<uint64_t> dead_buckets;
        for (auto& b : hash_index.occupied()) {
            auto new_end = std::remove(b.value.begin(), b.value.end(), oh);
            b.value.erase(new_end, b.value.end());
            if (b.value.empty()) dead_buckets.push_back(b.key);
        }
        for (uint64_t bk : dead_buckets) hash_index.erase(bk);

        std::vector<uint64_t> dead_wc_buckets;
        for (auto& b : wildcard_leaf_index.occupied()) {
            auto new_end = std::remove_if(b.value.begin(), b.value.end(),
                [oh](const std::pair<uint64_t,std::vector<PyObject*>>& p){ return p.first == oh; });
            b.value.erase(new_end, b.value.end());
            if (b.value.empty()) dead_wc_buckets.push_back(b.key);
        }
        for (uint64_t bk : dead_wc_buckets) wildcard_leaf_index.erase(bk);
    }

    // Remove from sorted_index (at most one entry per outer key for simple
    // fields; wildcard fields may have several — remove all matches).
    auto new_end = std::remove_if(sorted_index.begin(), sorted_index.end(),
                                  [oh](const SortedEntry& e){ return e.outer_key_hash == oh; });
    sorted_index.erase(new_end, sorted_index.end());
}

/* ============================================================================
   Queries
   ============================================================================ */

std::vector<uint64_t>* FieldIndex::find_eq(uint64_t field_val_hash) const {
    return const_cast<FlatHashMap<uint64_t, std::vector<uint64_t>>&>(hash_index).find(field_val_hash);
}

const std::vector<std::pair<uint64_t,std::vector<PyObject*>>>* FieldIndex::find_wildcard_leaf_eq(uint64_t field_val_hash) const {
    return const_cast<FlatHashMap<uint64_t, std::vector<std::pair<uint64_t,std::vector<PyObject*>>>>&>
        (wildcard_leaf_index).find(field_val_hash);
}

bool FieldIndex::is_numeric_range_supported(const ModValue& val) const {
    return is_numeric(val) && !sorted_index.empty();
}

std::vector<uint64_t> FieldIndex::find_range(FilterOp op, const ModValue& val) const {
    std::vector<uint64_t> result;
    if (sorted_index.empty()) return result;

    SortedEntry probe = make_sorted_entry(0, val);
    auto cmp = [](const SortedEntry& a, const SortedEntry& b) { return a.less_than(b); };

    switch (op) {
        case FilterOp::LT: {
            auto end = std::lower_bound(sorted_index.begin(), sorted_index.end(), probe, cmp);
            for (auto it = sorted_index.begin(); it != end; ++it) result.push_back(it->outer_key_hash);
            break;
        }
        case FilterOp::LE: {
            auto end = std::upper_bound(sorted_index.begin(), sorted_index.end(), probe, cmp);
            for (auto it = sorted_index.begin(); it != end; ++it) result.push_back(it->outer_key_hash);
            break;
        }
        case FilterOp::GT: {
            auto begin = std::upper_bound(sorted_index.begin(), sorted_index.end(), probe, cmp);
            for (auto it = begin; it != sorted_index.end(); ++it) result.push_back(it->outer_key_hash);
            break;
        }
        case FilterOp::GE: {
            auto begin = std::lower_bound(sorted_index.begin(), sorted_index.end(), probe, cmp);
            for (auto it = begin; it != sorted_index.end(); ++it) result.push_back(it->outer_key_hash);
            break;
        }
        default: break;
    }
    return result;
}
