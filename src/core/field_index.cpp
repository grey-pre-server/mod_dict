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
   build_wildcard — pattern with __pass_key__ wildcards
   ============================================================================ */

static void collect_wildcard(PyObject* dict,
                              const std::vector<std::string>& pattern,
                              size_t depth,
                              uint64_t oh,
                              FieldIndex* idx)
{
    if (!dict || !PyDict_Check(dict)) return;
    if (depth >= pattern.size()) return;

    bool last = (depth == pattern.size() - 1);

    if (pattern[depth] == "__pass_key__") {
        PyObject *k, *v; Py_ssize_t pos = 0;
        while (PyDict_Next(dict, &pos, &k, &v)) {
            if (last) {
                ModValue fv = ModValue::from_pyobject(v);
                uint64_t fh = fv.hash();
                std::vector<uint64_t>* bucket = idx->hash_index.find(fh);
                if (bucket) bucket->push_back(oh);
                else        idx->hash_index.insert(fh, {oh});
                if (idx->sorted_index.capacity() > 0 || true)
                    if (fv.type == ValueType::INT || fv.type == ValueType::FLOAT)
                        idx->sorted_index.push_back(make_sorted_entry(oh, fv));
            } else {
                collect_wildcard(v, pattern, depth + 1, oh, idx);
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
        } else {
            collect_wildcard(child, pattern, depth + 1, oh, idx);
        }
    }
}

void FieldIndex::build_wildcard(ModDict* owner, const std::vector<std::string>& pat) {
    is_wildcard = true;
    pattern     = pat;
    field_name  = "";
    clear();

    for (auto& e : owner->outer.occupied()) {
        if (!e.value.is_row || !e.value.val_py) continue;
        collect_wildcard(e.value.val_py, pat, 0, e.key, this);
    }

    std::sort(sorted_index.begin(), sorted_index.end(),
              [](const SortedEntry& a, const SortedEntry& b) { return a.less_than(b); });
}

void FieldIndex::clear() {
    hash_index = FlatHashMap<uint64_t, std::vector<uint64_t>>();
    sorted_index.clear();
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
    } else {
        PyObject* row = owner->get_row_ref(oh);
        if (!row) return;
        collect_wildcard(row, pattern, 0, oh, this);
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
        // Full rebuild is simplest for wildcard remove
        PyObject* row = owner->get_row_ref(oh);
        if (!row) return;

        // For each value under wildcard that this row contributed, remove from index
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
    }
}

void FieldIndex::remove_outer_key(uint64_t oh) {
    // Scan all hash_index buckets — remove oh regardless of what field value it had.
    uint64_t dead_bucket = 0; bool found = false;
    for (auto& b : hash_index.occupied()) {
        auto it = std::find(b.value.begin(), b.value.end(), oh);
        if (it != b.value.end()) {
            b.value.erase(it);
            if (b.value.empty()) { dead_bucket = b.key; found = true; }
            break; // a row appears in at most one bucket per simple field
        }
    }
    if (found) hash_index.erase(dead_bucket);

    // Remove from sorted_index (at most one entry per outer key)
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
