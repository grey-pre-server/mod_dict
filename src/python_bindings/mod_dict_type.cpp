#include "../core/mod_dict.h"
#include "../core/field_index.h"
#include "converter_registry.h"
#include "error_handling.h"
#include "mod_dict_binding.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstddef>
#ifdef _WIN32
#  define portable_strdup _strdup
#else
#  define portable_strdup strdup
#endif

extern PyTypeObject ModDict_Type;

struct ModDictObject {
    PyObject_HEAD
    ModDict* internal;
    bool owns_internal;
    PyObject* parent_ref;
    PyObject* weakreflist;  // required for PyWeakref_NewRef() — live-cursor registry holds weak refs
};

// Raises NotImplementedError and returns if `s` is a cursor — for methods
// that read/write outer/order/links directly with no cursor-aware
// reimplementation, instead of silently operating on an always-empty outer.
#define MOD_DICT_NO_CURSOR(s, name) do { \
    if ((s)->internal->root) MOD_DICT_RAISE(PyExc_NotImplementedError, name " is not supported on a cursor"); \
} while(0)

// Inverse guard for methods that only make sense on a cursor (set_sort/
// set_filter/set_group/connect/insert/update_row/delete/insert_batch).
#define MOD_DICT_REQUIRE_CURSOR(s, name) do { \
    if (!(s)->internal->root) MOD_DICT_RAISE(PyExc_NotImplementedError, name " is only supported on a cursor — call .cursor(path) first"); \
} while(0)

// index_diff_to_pylist() lives in mod_dict.cpp (core, declared in
// mod_dict_binding.h) since it's also needed by core's own
// notify_live_cursors() for the "reorder" event.

/* helpers */
static ModDict* pyobj_to_moddict_temp(PyObject* obj) {
    PyObject* src = nullptr;
    bool src_owned = false;
    if (PyDict_Check(obj)) {
        src = obj;
    } else if (PyUnicode_Check(obj)) {
        PyObject* json_mod = PyImport_ImportModule("json");
        if (!json_mod) return nullptr;
        src = PyObject_CallMethod(json_mod, "loads", "O", obj);
        Py_DECREF(json_mod);
        if (!src) return nullptr;
        if (!PyDict_Check(src)) { PyErr_SetString(PyExc_TypeError,"JSON must be a dict"); Py_DECREF(src); return nullptr; }
        src_owned = true;
    } else { PyErr_SetString(PyExc_TypeError,"target must be ModDict, dict, or JSON string"); return nullptr; }
    ModDict* tmp = new ModDict();
    PyObject *k,*v; Py_ssize_t pos=0;
    while (PyDict_Next(src,&pos,&k,&v)) {
        ModValue mk = ModValue::from_pyobject(k);
        if (PyDict_Check(v)) tmp->insert_row(mk,v);
        else { ModValue mv=ModValue::from_pyobject(v); tmp->insert(mk,mv); }
    }
    if (src_owned) Py_DECREF(src);
    return tmp;
}

ModDict* ModDict_unwrap(PyObject* obj) {
    if (!PyObject_TypeCheck(obj, &ModDict_Type)) return nullptr;
    return ((ModDictObject*)obj)->internal;
}

// Wraps a freshly heap-allocated ModDict* with no other owner (e.g. loads()
// building a new instance from bytes, or filter/select/copy results) — the
// wrapper takes ownership so ModDict_dealloc frees it.
PyObject* ModDict_wrap_owned(ModDict* internal) {
    if (!internal) Py_RETURN_NONE;
    ModDictObject* w = PyObject_New(ModDictObject,&ModDict_Type);
    if (!w) return nullptr;
    w->internal=internal; w->owns_internal=true; w->parent_ref=nullptr; w->weakreflist=nullptr;
    internal->py_wrapper=w;
    return (PyObject*)w;
}

/* new/dealloc/init */
static PyObject* ModDict_new(PyTypeObject* type,PyObject*,PyObject*) {
    ModDictObject* s=(ModDictObject*)type->tp_alloc(type,0);
    MOD_DICT_CHECK_ALLOC(s);
    s->internal=new ModDict(); MOD_DICT_CHECK_ALLOC(s->internal);
    s->internal->py_wrapper=s; s->owns_internal=true; s->parent_ref=nullptr; s->weakreflist=nullptr;
    return (PyObject*)s;
}
static void ModDict_dealloc(ModDictObject* s) {
    if (s->weakreflist) PyObject_ClearWeakRefs((PyObject*)s);
    if (s->internal) { s->internal->py_wrapper=nullptr; if(s->owns_internal) delete s->internal; }
    Py_XDECREF(s->parent_ref);
    Py_TYPE(s)->tp_free((PyObject*)s);
}
static int ModDict_init(ModDictObject* s,PyObject* args,PyObject* kwargs) {
    PyObject* ini=nullptr;
    static char* kw[]={(char*)"data",nullptr};
    if(!PyArg_ParseTupleAndKeywords(args,kwargs,"|O",kw,&ini)) return -1;
    if (ini) {
        if (PyObject_TypeCheck(ini, &ModDict_Type)) {
            // copy from another ModDict
            ModDict* src = ((ModDictObject*)ini)->internal;
            for (auto& e : src->outer.occupied()) {
                if (!e.value.key_py) continue;
                ModValue mk = ModValue::from_pyobject(e.value.key_py);
                if (e.value.is_row && e.value.val_py) s->internal->insert_row(mk, e.value.val_py);
                else if (e.value.val_py) { ModValue mv = ModValue::from_pyobject(e.value.val_py); s->internal->insert(mk, mv); }
            }
        } else if (PyDict_Check(ini)) {
            PyObject *k,*v; Py_ssize_t pos=0;
            while (PyDict_Next(ini,&pos,&k,&v)) {
                ModValue mk=ModValue::from_pyobject(k);
                if(PyDict_Check(v)) s->internal->insert_row(mk,v);
                else { ModValue mv=ModValue::from_pyobject(v); s->internal->insert(mk,mv); }
            }
        } else if (PyMapping_Check(ini)) {
            // any Mapping: use .items()
            PyObject* items = PyMapping_Items(ini);
            if (!items) return -1;
            Py_ssize_t n = PyList_GET_SIZE(items);
            for (Py_ssize_t i = 0; i < n; i++) {
                PyObject* pair = PyList_GET_ITEM(items, i);
                PyObject* k = PyTuple_GET_ITEM(pair, 0);
                PyObject* v = PyTuple_GET_ITEM(pair, 1);
                ModValue mk = ModValue::from_pyobject(k);
                if (PyDict_Check(v)) s->internal->insert_row(mk, v);
                else { ModValue mv = ModValue::from_pyobject(v); s->internal->insert(mk, mv); }
            }
            Py_DECREF(items);
        } else {
            PyErr_SetString(PyExc_TypeError, "ModDict() argument must be a dict, ModDict, or Mapping");
            return -1;
        }
    }
    return 0;
}

// ── RowProxy ─────────────────────────────────────────────────────────────────
// Returned by ModDict.__getitem__ for rows when field indices exist.
// Forwards all dict operations; intercepts __setitem__ / __delitem__ / update
// so that in-place field writes (mn[k]["age"] = 99) keep FieldIndex consistent.

struct RowProxyObject {
    PyObject_HEAD
    PyObject*      row;
    ModDictObject* owner;
    uint64_t       outer_hash;
    // true only for the proxy returned directly by ModDict[table] — i.e. `row`
    // IS the table dict {pk: row}, and a key passed to __delitem__/.pop() is a
    // PRIMARY KEY, so link on_delete semantics (restrict/cascade/set_null)
    // apply. Nested proxies (returned by __getitem__ when a value is itself a
    // dict, e.g. mn[table][pk] or deeper) are field-level views of a single
    // row — a key there is a FIELD name, not a pk, so link delete semantics
    // must not apply; only the write-time reindex/validation does.
    bool           is_root;
};

// ── forward-declare callbacks so RowProxy_Type can reference them ─────────────
static void       RowProxy_dealloc (RowProxyObject*);
static PyObject*  RowProxy_getitem (RowProxyObject*, PyObject*);
static int        RowProxy_setitem (RowProxyObject*, PyObject*, PyObject*);
static int        RowProxy_contains(RowProxyObject*, PyObject*);
static PyObject*  RowProxy_iter    (RowProxyObject*);
static Py_ssize_t RowProxy_len     (RowProxyObject*);
static PyObject*  RowProxy_repr    (RowProxyObject*);
static PyObject*  RowProxy_richcmp (RowProxyObject*, PyObject*, int);
static PyObject*  RowProxy_getattro(RowProxyObject*, PyObject*);
static PyObject*  RowProxy_update  (RowProxyObject*, PyObject*, PyObject*);
static PyObject*  RowProxy_pop     (RowProxyObject*, PyObject*);

static PyMethodDef RowProxy_methods[] = {
    {"update",(PyCFunction)(PyCFunctionWithKeywords)RowProxy_update,METH_VARARGS|METH_KEYWORDS,""},
    {"pop",(PyCFunction)RowProxy_pop,METH_VARARGS,""},
    {NULL,NULL,0,NULL}};
static PyMappingMethods  RowProxy_mapping  = {(lenfunc)RowProxy_len,(binaryfunc)RowProxy_getitem,(objobjargproc)RowProxy_setitem};
static PySequenceMethods RowProxy_sequence = {.sq_contains=(objobjproc)RowProxy_contains};

PyTypeObject RowProxy_Type = {
    .tp_name        = "mod_dict.RowProxy",
    .tp_basicsize   = sizeof(RowProxyObject),
    .tp_dealloc     = (destructor)RowProxy_dealloc,
    .tp_repr        = (reprfunc)RowProxy_repr,
    .tp_as_sequence = &RowProxy_sequence,
    .tp_as_mapping  = &RowProxy_mapping,
    .tp_getattro    = (getattrofunc)RowProxy_getattro,
    .tp_flags       = Py_TPFLAGS_DEFAULT,
    .tp_richcompare = (richcmpfunc)RowProxy_richcmp,
    .tp_iter        = (getiterfunc)RowProxy_iter,
    .tp_methods     = RowProxy_methods,
};

// ── RowProxy implementations ──────────────────────────────────────────────────
static PyObject* RowProxy_create(ModDictObject* owner, PyObject* row, uint64_t oh, bool is_root = true) {
    RowProxyObject* p = PyObject_New(RowProxyObject, &RowProxy_Type);
    if (!p) return nullptr;
    Py_INCREF(row); Py_INCREF(owner);
    p->row = row; p->owner = owner; p->outer_hash = oh; p->is_root = is_root;
    return (PyObject*)p;
}
static void RowProxy_dealloc(RowProxyObject* s) {
    Py_XDECREF(s->row); Py_XDECREF(s->owner);
    Py_TYPE(s)->tp_free((PyObject*)s);
}
static PyObject* RowProxy_getitem(RowProxyObject* s, PyObject* key) {
    PyObject* child = PyObject_GetItem(s->row, key);
    // Wrap nested dict values too (same table/outer_hash, non-root), so a
    // write at any depth — mn[table][pk][field] = x, or deeper — still routes
    // through RowProxy_setitem and triggers reindex_row()/link validation.
    if (child && PyDict_Check(child) && s->owner->internal) {
        PyObject* nested = RowProxy_create(s->owner, child, s->outer_hash, false);
        Py_DECREF(child);
        return nested;
    }
    return child;
}
// If this row is a declared link's target table (references_pattern[0] ==
// this row's own outer key), routes the delete through link on_delete
// semantics (restrict/cascade/set_null) instead of a plain dict delete.
// Returns true if it handled the delete (caller must not also delete),
// false if there's no matching link (caller should do a plain delete).
static bool try_link_delete(RowProxyObject* s, PyObject* key, int& out_r) {
    if (!s->is_root || s->owner->internal->links.empty()) return false;
    const OuterEntry* te = s->owner->internal->outer.find(s->outer_hash);
    if (!te || !te->key_py || !PyUnicode_Check(te->key_py)) return false;
    const char* table = PyUnicode_AsUTF8(te->key_py);
    if (!table) { PyErr_Clear(); return false; }
    out_r = s->owner->internal->delete_with_link_semantics(std::string(table), key) ? 0 : -1;
    return true;
}
static int RowProxy_setitem(RowProxyObject* s, PyObject* key, PyObject* value) {
    if (!value) {
        int r;
        if (try_link_delete(s, key, r)) return r;  // deletion (+ reindex) already applied inside
        int dr = PyObject_DelItem(s->row, key);
        if (dr == 0 && s->owner->internal) {
            s->owner->internal->reindex_row(s->outer_hash);
            if (PyErr_Occurred()) return -1;  // reindex_row may raise (link validation)
        }
        return dr;
    }
    int r = PyObject_SetItem(s->row, key, value);
    if (r == 0 && s->owner->internal) {
        s->owner->internal->reindex_row(s->outer_hash);
        if (PyErr_Occurred()) return -1;  // reindex_row may raise (link validation)
    }
    return r;
}
static PyObject* RowProxy_update(RowProxyObject* s, PyObject* args, PyObject* kwargs) {
    PyObject* fn = PyObject_GetAttrString(s->row, "update");
    if (!fn) return nullptr;
    PyObject* res = PyObject_Call(fn, args, kwargs);
    Py_DECREF(fn);
    if (res && s->owner->internal) {
        s->owner->internal->reindex_row(s->outer_hash);
        if (PyErr_Occurred()) { Py_DECREF(res); return nullptr; }
    }
    return res;
}
static PyObject* RowProxy_pop(RowProxyObject* s, PyObject* args) {
    PyObject *key, *def=nullptr;
    if (!PyArg_ParseTuple(args, "O|O", &key, &def)) return nullptr;

    if (s->is_root && !s->owner->internal->links.empty()) {
        const OuterEntry* te = s->owner->internal->outer.find(s->outer_hash);
        if (te && te->key_py && PyUnicode_Check(te->key_py)) {
            const char* table = PyUnicode_AsUTF8(te->key_py);
            if (!table) PyErr_Clear();
            else {
                PyObject* existing = PyDict_GetItem(s->row, key);  // borrowed
                if (!existing) {
                    if (def) { Py_INCREF(def); return def; }
                    PyErr_SetObject(PyExc_KeyError, key);
                    return nullptr;
                }
                Py_INCREF(existing);  // hold — delete_with_link_semantics is about to remove it from s->row
                bool ok = s->owner->internal->delete_with_link_semantics(std::string(table), key);
                if (!ok) { Py_DECREF(existing); return nullptr; }
                return existing;  // transfers the held ref to the caller
            }
        }
    }

    PyObject* fn = PyObject_GetAttrString(s->row, "pop");
    if (!fn) return nullptr;
    PyObject* res = PyObject_Call(fn, args, nullptr);
    Py_DECREF(fn);
    if (res && s->owner->internal) {
        s->owner->internal->reindex_row(s->outer_hash);
        if (PyErr_Occurred()) { Py_DECREF(res); return nullptr; }
    }
    return res;
}
static int        RowProxy_contains(RowProxyObject* s, PyObject* k) { return PySequence_Contains(s->row, k); }
static PyObject*  RowProxy_iter    (RowProxyObject* s) { return PyObject_GetIter(s->row); }
static Py_ssize_t RowProxy_len     (RowProxyObject* s) { return PyObject_Length(s->row); }
static PyObject*  RowProxy_repr    (RowProxyObject* s) { return PyObject_Repr(s->row); }
static PyObject*  RowProxy_richcmp (RowProxyObject* s, PyObject* other, int op) {
    PyObject* r = (Py_TYPE(other)==&RowProxy_Type) ? ((RowProxyObject*)other)->row : other;
    return PyObject_RichCompare(s->row, r, op);
}
static PyObject*  RowProxy_getattro(RowProxyObject* s, PyObject* name) {
    PyObject* attr = PyObject_GenericGetAttr((PyObject*)s, name);
    if (attr) return attr;
    PyErr_Clear();
    return PyObject_GetAttr(s->row, name);
}

/* getitem/setitem/contains/len */
static PyObject* ModDict_getitem(ModDictObject* s,PyObject* key) {
    if (s->internal->root) {
        // Cursor mode: no RowProxy wrapping — cursor-scoped field indices
        // aren't wired until create_index() is made anchor-aware.
        PyObject* d = s->internal->resolve_cursor_dict();
        if (!d) return nullptr;
        PyObject* v = PyDict_GetItem(d, key);  // borrowed
        if (!v) { PyErr_SetObject(PyExc_KeyError, key); return nullptr; }
        Py_INCREF(v);
        return v;
    }
    uint64_t oh=content_hash_pyobj(key);
    auto* e=s->internal->outer.find(oh);
    if(!e){PyErr_SetObject(PyExc_KeyError,key);return nullptr;}
    if(e->is_row && !s->internal->indices.by_field.empty())
        return RowProxy_create(s, e->val_py, oh);
    Py_INCREF(e->val_py); return e->val_py;
}
static int ModDict_setitem(ModDictObject* s,PyObject* key,PyObject* value) {
    if (s->internal->root) {
        PyObject* d = s->internal->resolve_cursor_dict();
        if (!d) return -1;
        if (!value) {  // del cursor[key]
            if (PyDict_DelItem(d, key) != 0) return -1;
        } else if (PyDict_SetItem(d, key, value) != 0) {
            return -1;
        }
        // Keep the root's own field-indices consistent, same as RowProxy
        // does for root-level nested writes.
        s->internal->true_root()->reindex_row_no_validate(s->internal->cached_top_hash);
        return 0;
    }
    ModValue mk=ModValue::from_pyobject(key);
    if (!value) {  // del mn[key]
        if (!s->internal->remove(mk)) { PyErr_SetObject(PyExc_KeyError,key); return -1; }
        return 0;
    }
    if(PyDict_Check(value)) s->internal->insert_row(mk,value);
    else { ModValue mv=ModValue::from_pyobject(value); s->internal->insert(mk,mv); }
    return 0;
}
static int ModDict_contains(ModDictObject* s,PyObject* key) {
    if (s->internal->root) {
        PyObject* d = s->internal->resolve_cursor_dict();
        if (!d) return -1;
        return PyDict_Contains(d, key);
    }
    return s->internal->outer.find(content_hash_pyobj(key)) ? 1 : 0;
}
static Py_ssize_t ModDict_len(ModDictObject* s){
    if (s->internal->root) {
        PyObject* d = s->internal->resolve_cursor_dict();
        if (!d) return -1;
        if (s->internal->filter_predicate) return (Py_ssize_t)s->internal->visible_index.size();
        return PyDict_Size(d);
    }
    return (Py_ssize_t)s->internal->len();
}

static PyObject* ModDict_repr(ModDictObject* s){
    if (s->internal->root) {
        PyObject* d = s->internal->resolve_cursor_dict();
        if (!d) return nullptr;
        return PyObject_Repr(d);
    }
    PyObject* d=s->internal->to_python_dict(); if(!d) return nullptr;
    PyObject* r=PyObject_Repr(d); Py_DECREF(d); return r;
}
static PyObject* ModDict_get(ModDictObject* s,PyObject* args){
    PyObject* key; PyObject* def=Py_None;
    if(!PyArg_ParseTuple(args,"O|O",&key,&def)) return nullptr;
    uint64_t oh=content_hash_pyobj(key);
    auto* e=s->internal->outer.find(oh);
    if(!e){Py_INCREF(def);return def;}
    if(e->is_row && !s->internal->indices.by_field.empty())
        return RowProxy_create(s, e->val_py, oh);
    Py_INCREF(e->val_py); return e->val_py;
}

/* merge path parser */
static std::vector<const char*> parse_merge_path(PyObject* obj,std::vector<std::string>& st){
    auto tr=[](const std::string& s)->std::string{
        if(s=="*") return "__scan_key__"; if(s=="?") return "__pass_key__"; return s;};
    size_t base=st.size();
    if(PyUnicode_Check(obj)){
        std::string raw=PyUnicode_AsUTF8(obj);
        // space/tab is a literal alias for '.' — normalize, then split with
        // the original strict splitter (no collapsing; fields with a literal
        // '.'/' ' need the tuple form).
        for(char& c: raw) if(c==' '||c=='\t') c='.';
        if(raw.find('.')!=std::string::npos){size_t pos=0;while(true){size_t d=raw.find('.',pos);st.push_back(tr(raw.substr(pos,d==std::string::npos?d:d-pos)));if(d==std::string::npos)break;pos=d+1;}}
        else st.push_back(tr(raw));
    } else if(PyTuple_Check(obj)||PyList_Check(obj)){
        Py_ssize_t n=PySequence_Size(obj);
        for(Py_ssize_t i=0;i<n;i++){PyObject* it=PySequence_GetItem(obj,i);st.push_back(tr(PyUnicode_AsUTF8(it)));Py_DECREF(it);}
    }
    std::vector<const char*> r; for(size_t i=base;i<st.size();i++) r.push_back(st[i].c_str()); return r;
}
static PyObject* ModDict_update(ModDictObject* s,PyObject* args,PyObject* kwargs){
    MOD_DICT_NO_CURSOR(s, "update()");
    PyObject *to; PyObject *os=nullptr,*ot=nullptr; const char* cs="keep_right";
    static const char* kw[]={"target","from_path","to_path","conflict",NULL};
    if(!PyArg_ParseTupleAndKeywords(args,kwargs,"O|OOs",(char**)kw,&to,&os,&ot,&cs)) return nullptr;

    // Simple mode: mn.update(d) — plain key/value bulk insert, like dict.update()
    if(!os && !ot){
        ModDict* tmp=nullptr; ModDict* src=nullptr;
        if(PyObject_TypeCheck(to,&ModDict_Type)) src=((ModDictObject*)to)->internal;
        else { tmp=pyobj_to_moddict_temp(to); if(!tmp) return nullptr; src=tmp; }
        // skip_field_index=true defers indexing to one rebuild() per existing
        // index after the loop — O(n log n) total instead of O(n*k) from a
        // per-row shift into an already-indexed field.
        for(auto& e : src->outer.occupied()){
            if(!e.value.key_py) continue;
            ModValue mk=ModValue::from_pyobject(e.value.key_py);
            if(e.value.is_row && e.value.val_py) s->internal->insert_row(mk,e.value.val_py,true);
            else if(e.value.val_py){ ModValue mv=ModValue::from_pyobject(e.value.val_py); s->internal->insert(mk,mv); }
        }
        for (auto& fi : s->internal->indices.by_field.occupied()) fi.value->rebuild(s->internal);
        delete tmp;
        Py_RETURN_NONE;
    }

    // Path mode: mn.update(other, to_path=...) — from_path defaults to to_path
    // mn.update(other, '?', '?.geo')  →  match self[?] with other[?].geo
    if(!os) os=ot;  // from_path defaults to to_path
    std::vector<std::string> ss,ts;
    auto ons=parse_merge_path(os,ss), ont=parse_merge_path(ot,ts);
    if(ons.empty()) MOD_DICT_RAISE(PyExc_TypeError,"from_path must be str or tuple");
    if(ont.empty()) MOD_DICT_RAISE(PyExc_TypeError,"to_path must be str or tuple");
    MergeConflict c;
    if(!strcmp(cs,"keep_left")) c=MergeConflict::KEEP_LEFT;
    else if(!strcmp(cs,"keep_right")) c=MergeConflict::KEEP_RIGHT;
    else if(!strcmp(cs,"merge")) c=MergeConflict::MERGE;
    else if(!strcmp(cs,"concat")) c=MergeConflict::CONCAT;
    else MOD_DICT_RAISE(PyExc_ValueError,"conflict must be keep_left/keep_right/merge/concat");
    ModDict* tgt=nullptr; ModDict* tmp=nullptr;
    if(PyObject_TypeCheck(to,&ModDict_Type)) tgt=((ModDictObject*)to)->internal;
    else { tmp=pyobj_to_moddict_temp(to); if(!tmp) return nullptr; tgt=tmp; }
    int n=s->internal->merges(tgt,ons,ont,c); delete tmp;
    if(n<0) return nullptr; return PyLong_FromLong(n);
}

/* field/pattern parser */
// '.' (strict) or whitespace (collapsed) — space is a readability alias for
// '.'; fields containing a literal '.'/' ' need the tuple/list path form.
// Splits one "->"-free chunk on '.' (space/tab normalized to '.' first),
// translating a bare "?" segment to "__pass_key__". Shared by
// parse_field_or_pattern (whole strings) and parse_link_pattern (one hop
// of a "->" path).
static std::vector<std::string> split_dot_chunk(std::string raw){
    for(char& c: raw) if(c==' '||c=='\t') c='.';
    std::vector<std::string> segs;
    size_t pos=0;
    while(true){
        size_t d=raw.find('.',pos);
        std::string seg=raw.substr(pos,d==std::string::npos?std::string::npos:d-pos);
        segs.push_back(seg=="?"?"__pass_key__":seg);
        if(d==std::string::npos) break;
        pos=d+1;
    }
    return segs;
}
static bool parse_field_or_pattern(PyObject* arg,std::string& simple,std::vector<std::string>& pattern,bool& wc){
    if(PyUnicode_Check(arg)){
        std::string raw=PyUnicode_AsUTF8(arg);
        // space/tab is a literal alias for '.' — normalize, then split with
        // the original strict splitter (no collapsing).
        for(char& c: raw) if(c==' '||c=='\t') c='.';
        if(raw.find('.')!=std::string::npos || raw=="?"){
            pattern=split_dot_chunk(raw);
            wc=true;
        } else { simple=raw; wc=false; }
        return true;
    }
    if(PyTuple_Check(arg)||PyList_Check(arg)){
        Py_ssize_t n=PySequence_Size(arg);
        for(Py_ssize_t i=0;i<n;i++){PyObject* it=PySequence_GetItem(arg,i);if(!PyUnicode_Check(it)){Py_DECREF(it);return false;}std::string seg=PyUnicode_AsUTF8(it);Py_DECREF(it);pattern.push_back(seg=="?"?"__pass_key__":seg);}
        wc=true; return true;
    }
    return false;
}
// Splits raw on "->", dot-splits each chunk via split_dot_chunk, and inserts
// "__follow_link__" sentinels between chunks. Validates shape: the first
// chunk must be exactly ["table","?","field"] (a link source_path shape);
// every middle hop (there's another "->" after it) must be a single field
// name — a "->" hop always lands on an already-resolved row, so there's no
// anchor/wildcard segment to have there. The last chunk (the continuation
// read off the final target row) may be any dotted path. Returns false
// (PyErr set) on a malformed "->" pattern.
static bool parse_link_pattern(const std::string& raw, std::vector<std::string>& pattern){
    std::vector<std::string> chunks;
    size_t pos=0;
    while(true){
        size_t d=raw.find("->",pos);
        chunks.push_back(raw.substr(pos,d==std::string::npos?std::string::npos:d-pos));
        if(d==std::string::npos) break;
        pos=d+2;
    }
    for(size_t i=0;i<chunks.size();i++){
        if(chunks[i].empty()){
            PyErr_SetString(PyExc_ValueError,"filter: '->' must have a path segment on both sides");
            return false;
        }
        std::vector<std::string> segs=split_dot_chunk(chunks[i]);
        if(i==0){
            if(segs.size()!=3 || segs[1]!="__pass_key__"){
                PyErr_SetString(PyExc_ValueError,"filter: '->' left side must be a wildcard path like \"table.?.field\"");
                return false;
            }
        } else if(i+1<chunks.size()){
            if(segs.size()!=1){
                PyErr_SetString(PyExc_ValueError,"filter: '->' hop must be a single field name, not a nested path");
                return false;
            }
        }
        if(i>0) pattern.push_back("__follow_link__");
        for(auto& s:segs) pattern.push_back(s);
    }
    return true;
}

// Runs a filter() call, relaying through the parent_ref chain first if
// `owner` is a derived child (e.g. a prior filter()'s result) and `pattern`
// hops across a declared link — defined after pattern_has_link_hop/
// intersect_anchored below, forward-declared here so apply_filter (and
// FB_between/FB_in_, which call owner->internal->filter() directly for
// their own reasons) can all route through the same logic.
static ModDict* filter_maybe_relay(ModDictObject* owner, const std::string& simple,
                                    const std::vector<std::string>& pattern, bool wc,
                                    FilterOp op, const ModValue& fv);

static PyObject* apply_filter(ModDictObject* owner,const std::string& simple,const std::vector<std::string>& pattern,bool wc,FilterOp op,PyObject* val_obj){
    ModValue fv=ModValue::from_pyobject(val_obj);
    ModDict* result=filter_maybe_relay(owner,simple,pattern,wc,op,fv);
    // A null result can mean a specific error (e.g. "->" hop with no
    // declared link) rather than allocation failure — don't clobber it with
    // a generic MemoryError via MOD_DICT_CHECK_ALLOC.
    if(!result) return nullptr;
    ModDictObject* w=PyObject_New(ModDictObject,&ModDict_Type);
    if(!w){delete result;return nullptr;}
    w->internal=result; w->owns_internal=true; w->parent_ref=(PyObject*)owner; Py_INCREF(owner); w->weakreflist=nullptr;
    result->py_wrapper=w; return (PyObject*)w;
}
static FilterOp parse_op(const char* s){
    if(!strcmp(s,"==")) return FilterOp::EQ; if(!strcmp(s,"!=")) return FilterOp::NE;
    if(!strcmp(s,"<"))  return FilterOp::LT; if(!strcmp(s,"<=")) return FilterOp::LE;
    if(!strcmp(s,">"))  return FilterOp::GT; if(!strcmp(s,">=")) return FilterOp::GE;
    return (FilterOp)-1;
}

/* FilterBuilder */
struct FilterBuilderObject{PyObject_HEAD ModDictObject* owner;char* field;std::vector<std::string>* pattern;};
static void FilterBuilder_dealloc(FilterBuilderObject* s){Py_XDECREF(s->owner);free(s->field);delete s->pattern;Py_TYPE(s)->tp_free(s);}

/* ── scan_here helpers for returns="rows_here"/"values" ── */
static bool fh_compare(PyObject* pyobj, FilterOp op, const ModValue& fval) {
    ModValue v = ModValue::from_pyobject(pyobj);
    if (op == FilterOp::EQ) return v.equals(fval);
    if (op == FilterOp::NE) return !v.equals(fval);
    bool ok = true;
    int c = v.compare(fval, &ok);
    // Not comparable (e.g. None vs int) — excluded from every range predicate.
    if (!ok) return false;
    switch(op){
        case FilterOp::LT: return c<0;  case FilterOp::LE: return c<=0;
        case FilterOp::GT: return c>0;  case FilterOp::GE: return c>=0;
        default: return false;
    }
}
// `report_row` is null until the first "__follow_link__" jump, then pinned
// to `cur`'s value from right before that jump (the SOURCE/anchor row, e.g.
// the order — not wherever the "->" chain eventually lands) for the rest of
// this branch's recursion, however many further hops follow. rows_here/
// value_field report from `report_row` once it's pinned, `cur` otherwise —
// so "here" stays anchored to the row you're actually filtering, matching
// what returns="rows" already returns (its {table: {...}} nesting is keyed
// by the same anchor table), not the target you already named in .eq(...).
// `self`/`current_table` are only needed to resolve a jump. `*err` is set
// (with PyErr already raised) if a hop's link wasn't declared; callers must
// stop scanning and propagate once it's true. `cmp_out`, if non-null, gets
// the raw value that was actually compared (`k` or `child` — wherever it
// lived, target row included) appended in lockstep with `result` — used by
// FB_between, which needs to re-check that same value against `hi` but can
// no longer just re-read it off the reported item now that the item is the
// anchor row, not (necessarily) the row the field lives on.
static void scan_here(ModDict* self, std::string current_table, PyObject* report_row,
                      PyObject* cur, const std::vector<std::string>& pat, size_t depth,
                      FilterOp op, const ModValue& fval,
                      bool want_values, PyObject* vf_key, PyObject* result, PyObject* cmp_out, bool* err) {
    if (*err || !cur || !PyDict_Check(cur) || depth >= pat.size()) return;
    bool last = (depth == pat.size()-1);
    if (pat[depth] == "__pass_key__") {
        PyObject *k,*v; Py_ssize_t pos=0;
        while (PyDict_Next(cur,&pos,&k,&v)) {
            if (*err) return;
            if (last) {
                // Terminal ? checks the KEY itself
                if (!fh_compare(k,op,fval)) continue;
                PyObject* rr = report_row ? report_row : cur;
                PyObject* item = want_values ? (vf_key ? PyDict_GetItem(rr,vf_key) : nullptr) : rr;
                if (item) {
                    Py_INCREF(item); PyList_Append(result,item); Py_DECREF(item);
                    if (cmp_out) PyList_Append(cmp_out,k);
                }
            } else if (PyDict_Check(v)) {
                scan_here(self,current_table,report_row,v,pat,depth+1,op,fval,want_values,vf_key,result,cmp_out,err);
            }
        }
    } else {
        PyObject* child = PyDict_GetItemString(cur,pat[depth].c_str());
        if (!child) return;
        bool jump_next = (depth+1 < pat.size() && pat[depth+1]=="__follow_link__");
        if (last) {
            if (!fh_compare(child,op,fval)) return;
            PyObject* rr = report_row ? report_row : cur;
            PyObject* item = want_values ? (vf_key ? PyDict_GetItem(rr,vf_key) : nullptr) : rr;
            if (item) {
                Py_INCREF(item); PyList_Append(result,item); Py_DECREF(item);
                if (cmp_out) PyList_Append(cmp_out,child);
            }
        } else if (jump_next) {
            std::string next_table = current_table;
            bool no_link = false;
            PyObject* target_row = self->resolve_hop(next_table, pat[depth], child, &no_link);
            if (no_link) {
                PyErr_SetString(PyExc_ValueError, "filter: no link declared for this source_path - call mn.link() first");
                *err = true;
                return;
            }
            if (!target_row) return;  // nullable/dangling FK -> no match, not an error
            PyObject* pinned = report_row ? report_row : cur;  // pin on first jump only
            scan_here(self,next_table,pinned,target_row,pat,depth+2,op,fval,want_values,vf_key,result,cmp_out,err);
        } else {
            scan_here(self,current_table,report_row,child,pat,depth+1,op,fval,want_values,vf_key,result,cmp_out,err);
        }
    }
}
static PyObject* apply_filter_here(ModDictObject* owner,
                                    const std::string& simple,
                                    const std::vector<std::string>& pattern,
                                    bool wc, FilterOp op, const ModValue& fval,
                                    bool want_values, PyObject* vf_key, PyObject* cmp_out = nullptr) {
    PyObject* result = PyList_New(0); if (!result) return nullptr;
    std::vector<std::string> pat = wc ? pattern : std::vector<std::string>{simple};
    bool anchored = (!pat.empty() && pat[0] != "__pass_key__");
    const OuterEntry* anchor_e = nullptr;
    if (anchored) {
        PyObject* tmp = PyUnicode_FromStringAndSize(pat[0].c_str(), pat[0].size());
        if (tmp) {
            uint64_t h = content_hash_pyobj(tmp); Py_DECREF(tmp);
            const OuterEntry* e = owner->internal->outer.find(h);
            if (e && e->val_py && e->is_row) anchor_e = e; else anchored = false;
        } else { anchored = false; }
    }
    bool err = false;
    if (anchored && anchor_e) {
        scan_here(owner->internal,pat[0],nullptr,anchor_e->val_py,pat,1,op,fval,want_values,vf_key,result,cmp_out,&err);
    } else {
        for (auto& e : owner->internal->outer.occupied()) {
            if (!e.value.is_row || !e.value.val_py) continue;
            // Unanchored patterns can never contain "->" (its left side must
            // be an exact "table.?.field" link source_path, always anchored)
            // — current_table="" here is dead code for the jump branch.
            scan_here(owner->internal,std::string(),nullptr,e.value.val_py,pat,0,op,fval,want_values,vf_key,result,cmp_out,&err);
            if (err) break;
        }
    }
    if (err) { Py_DECREF(result); return nullptr; }
    return result;
}

static PyObject* fb_op(FilterBuilderObject* s,PyObject* args,PyObject* kw,FilterOp op){
    PyObject* v; const char* ret="rows"; PyObject* vf_obj=nullptr;
    static const char* kwl[]={"value","returns","value_field",nullptr};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"O|sO",(char**)kwl,&v,&ret,&vf_obj)) return nullptr;
    std::string simple(s->field); bool wc=(s->pattern!=nullptr);
    std::vector<std::string> empty; const std::vector<std::string>& pat=wc?*s->pattern:empty;
    if (strcmp(ret,"rows")!=0) {
        bool want_values=(strcmp(ret,"values")==0);
        if (want_values && !vf_obj) MOD_DICT_RAISE(PyExc_ValueError,"returns='values' requires value_field");
        ModValue fval=ModValue::from_pyobject(v);
        return apply_filter_here(s->owner,simple,pat,wc,op,fval,want_values,vf_obj);
    }
    return apply_filter(s->owner,simple,pat,wc,op,v);
}
static PyObject* FB_eq(FilterBuilderObject* s,PyObject* a,PyObject* kw){return fb_op(s,a,kw,FilterOp::EQ);}
static PyObject* FB_ne(FilterBuilderObject* s,PyObject* a,PyObject* kw){return fb_op(s,a,kw,FilterOp::NE);}
static PyObject* FB_lt(FilterBuilderObject* s,PyObject* a,PyObject* kw){return fb_op(s,a,kw,FilterOp::LT);}
static PyObject* FB_lte(FilterBuilderObject* s,PyObject* a,PyObject* kw){return fb_op(s,a,kw,FilterOp::LE);}
static PyObject* FB_gt(FilterBuilderObject* s,PyObject* a,PyObject* kw){return fb_op(s,a,kw,FilterOp::GT);}
static PyObject* FB_gte(FilterBuilderObject* s,PyObject* a,PyObject* kw){return fb_op(s,a,kw,FilterOp::GE);}
// FB_between/FB_in_ "rows" paths normally compose multiple filter() calls by
// chaining (between: re-filter the previous result) or by outer-key de-dup
// (in_: skip if the anchor hash is already present). Both assumptions break
// for a "->" pattern's anchored {table: {pk: row}} result: chaining re-filters
// an intermediate ModDict that never carries the original's `links` (so
// find_link() fails), and outer-key de-dup collapses every part down to
// whichever was merged first (all parts share the SAME single anchor hash).
// These two helpers do the same job correctly for that one shape, operating
// on two independent filter() calls off the *original* owner instead.
static bool pattern_has_link_hop(const std::vector<std::string>& pat) {
    return std::find(pat.begin(), pat.end(), "__follow_link__") != pat.end();
}
// Intersects two anchored (single outer-key, nested-dict-value) results at
// the inner-row level. Consumes and frees both a and b.
static ModDict* intersect_anchored(ModDict* a, ModDict* b) {
    ModDict* result = new ModDict();
    for (auto& e : a->outer.occupied()) {
        const OuterEntry* be = b->outer.find(e.key);
        if (!be || !be->val_py || !e.value.val_py || !e.value.is_row || !be->is_row
            || !PyDict_Check(e.value.val_py) || !PyDict_Check(be->val_py)) continue;
        PyObject* pruned = PyDict_New();
        PyObject *k2,*v2; Py_ssize_t pos2=0;
        while (PyDict_Next(e.value.val_py,&pos2,&k2,&v2))
            if (PyDict_Contains(be->val_py,k2)) PyDict_SetItem(pruned,k2,v2);
        if (PyDict_Size(pruned) > 0) {
            Py_XINCREF(e.value.key_py);
            result->outer.insert(e.key, {e.value.key_py, pruned, true});
            result->order.push_back(e.key);
        } else Py_DECREF(pruned);
    }
    delete a; delete b;
    return result;
}
// Unions src's anchored inner rows into dst (dst takes ownership of nothing
// new from src — copies refs). Unlike the plain outer-key de-dup this
// replaces, a second part sharing dst's single anchor hash gets its inner
// rows merged in rather than dropped.
static void union_anchored_into(ModDict* dst, ModDict* src) {
    for (auto& e : src->outer.occupied()) {
        OuterEntry* existing = dst->outer.find(e.key);
        if (!existing) {
            Py_XINCREF(e.value.key_py); Py_XINCREF(e.value.val_py);
            dst->outer.insert(e.key, {e.value.key_py, e.value.val_py, e.value.is_row});
            dst->order.push_back(e.key);
        } else if (e.value.is_row && existing->is_row && e.value.val_py && existing->val_py
                   && PyDict_Check(e.value.val_py) && PyDict_Check(existing->val_py)) {
            PyObject *k2,*v2; Py_ssize_t pos2=0;
            while (PyDict_Next(e.value.val_py,&pos2,&k2,&v2))
                if (!PyDict_Contains(existing->val_py,k2)) PyDict_SetItem(existing->val_py,k2,v2);
        }
    }
}
// A filter()/select() result is a fresh ModDict holding only ONE table's
// worth of pruned rows, with an empty `links` -- calling a "->" pattern on
// it has nothing to resolve against (the OTHER table and the declared link
// only exist on the original owner). Instead of threading parent-awareness
// through every core resolution function, relay the WHOLE call to the
// topmost ancestor (which has everything) and intersect its unrestricted
// answer with `owner`'s own current rows for the pattern's anchor table --
// reuses ModDict::filter()/filter_linked_eq() completely unchanged.
static ModDict* filter_link_hop_via_root(ModDictObject* owner,
                                          const std::vector<std::string>& pattern,
                                          FilterOp op, const ModValue& fv) {
    ModDictObject* root = owner;
    while (root->parent_ref) root = (ModDictObject*)root->parent_ref;

    PyObject* tmp = PyUnicode_FromStringAndSize(pattern[0].c_str(), pattern[0].size());
    if (!tmp) return nullptr;
    uint64_t anchor_hash = content_hash_pyobj(tmp);
    Py_DECREF(tmp);

    const OuterEntry* child_entry = owner->internal->outer.find(anchor_hash);
    if (!child_entry || !child_entry->val_py || !PyDict_Check(child_entry->val_py))
        return new ModDict();  // child doesn't restrict this table at all -> no match, not an error

    ModDict* root_result = root->internal->filter(pattern, op, fv);
    if (!root_result) return nullptr;  // PyErr already set (e.g. no link declared, even at the root)

    // Wrap the child's own single-anchor entry as a matching-shaped ModDict
    // and reuse intersect_anchored() (written for FB_between above) instead
    // of duplicating the inner-dict intersection here.
    ModDict* child_wrapper = new ModDict();
    Py_XINCREF(child_entry->key_py); Py_XINCREF(child_entry->val_py);
    child_wrapper->outer.insert(anchor_hash, {child_entry->key_py, child_entry->val_py, true});
    child_wrapper->order.push_back(anchor_hash);
    return intersect_anchored(child_wrapper, root_result);  // consumes both, returns the intersection
}
static ModDict* filter_maybe_relay(ModDictObject* owner, const std::string& simple,
                                    const std::vector<std::string>& pattern, bool wc,
                                    FilterOp op, const ModValue& fv) {
    if (wc && owner->parent_ref && pattern_has_link_hop(pattern))
        return filter_link_hop_via_root(owner, pattern, op, fv);
    return wc ? owner->internal->filter(pattern, op, fv) : owner->internal->filter(simple, op, fv);
}
static PyObject* FB_between(FilterBuilderObject* s,PyObject* args,PyObject* kw){
    PyObject *lo,*hi; const char* ret="rows"; PyObject* vf_obj=nullptr;
    static const char* kwl[]={"lo","hi","returns","value_field",nullptr};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"OO|sO",(char**)kwl,&lo,&hi,&ret,&vf_obj)) return nullptr;
    std::string simple(s->field); bool wc=(s->pattern!=nullptr); std::vector<std::string> empty;
    const std::vector<std::string>& pat=wc?*s->pattern:empty;
    if (strcmp(ret,"rows")!=0) {
        bool want_values=(strcmp(ret,"values")==0);
        if (want_values && !vf_obj) MOD_DICT_RAISE(PyExc_ValueError,"returns='values' requires value_field");
        ModValue lo_val=ModValue::from_pyobject(lo), hi_val=ModValue::from_pyobject(hi);
        // collect GE candidates as rows_here, then filter LE. cmp_list holds
        // the raw compared value in lockstep with ge — needed instead of
        // re-reading the pattern's last segment off each reported row,
        // since for a "->" path the reported row is the ANCHOR (e.g. the
        // order) while the compared field lives on the TARGET (e.g. the
        // customer) — p.back() wouldn't be found on the reported row at all.
        PyObject* cmp_list=PyList_New(0); if(!cmp_list) return nullptr;
        PyObject* ge=apply_filter_here(s->owner,simple,pat,wc,FilterOp::GE,lo_val,false,nullptr,cmp_list);
        if (!ge) { Py_DECREF(cmp_list); return nullptr; }
        PyObject* result=PyList_New(0); if(!result){Py_DECREF(ge);Py_DECREF(cmp_list);return nullptr;}
        Py_ssize_t n=PyList_GET_SIZE(ge);
        for (Py_ssize_t i=0;i<n;i++) {
            PyObject* row=PyList_GET_ITEM(ge,i);
            PyObject* fv_obj2=PyList_GET_ITEM(cmp_list,i);
            bool pass=true;
            if (fv_obj2){
                ModValue fv=ModValue::from_pyobject(fv_obj2);
                bool ok=true; int c=fv.compare(hi_val,&ok);
                pass = ok && (c<=0);   // not comparable (e.g. None vs int) -> excluded
            }
            if (pass) {
                PyObject* item=want_values?(vf_obj?PyDict_GetItem(row,vf_obj):nullptr):row;
                if(item){Py_INCREF(item);PyList_Append(result,item);Py_DECREF(item);}
            }
        }
        Py_DECREF(ge); Py_DECREF(cmp_list); return result;
    }
    if (wc && pattern_has_link_hop(pat)) {
        // Chaining (re-filtering r1) doesn't work here — r1 is a fresh ModDict
        // with no `links` of its own, so find_link() inside the second call
        // would fail. Compute GE/LE independently off the original owner and
        // intersect the two anchored results at the row level instead.
        ModDict* ge = filter_maybe_relay(s->owner, simple, pat, wc, FilterOp::GE, ModValue::from_pyobject(lo));
        if (!ge) return nullptr;
        ModDict* le = filter_maybe_relay(s->owner, simple, pat, wc, FilterOp::LE, ModValue::from_pyobject(hi));
        if (!le) { delete ge; return nullptr; }
        ModDict* result = intersect_anchored(ge, le);
        MOD_DICT_CHECK_ALLOC(result);
        // Rows are borrowed from s->owner's own data (zero-copy) -- keep it
        // alive via parent_ref, same as apply_filter() does. ModDict_wrap_owned
        // would leave parent_ref null, risking a use-after-free.
        ModDictObject* w=PyObject_New(ModDictObject,&ModDict_Type);
        if(!w){delete result;return nullptr;}
        w->internal=result; w->owns_internal=true; w->parent_ref=(PyObject*)s->owner; Py_INCREF(s->owner); w->weakreflist=nullptr;
        result->py_wrapper=w; return (PyObject*)w;
    }
    PyObject* r1=apply_filter(s->owner,simple,pat,wc,FilterOp::GE,lo); if(!r1) return nullptr;
    PyObject* r2=apply_filter((ModDictObject*)r1,simple,pat,wc,FilterOp::LE,hi); Py_DECREF(r1); return r2;
}
static PyObject* FB_in_(FilterBuilderObject* s,PyObject* args,PyObject* kw){
    PyObject* seq; const char* ret="rows"; PyObject* vf_obj=nullptr;
    static const char* kwl[]={"values","returns","value_field",nullptr};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"O|sO",(char**)kwl,&seq,&ret,&vf_obj)) return nullptr;
    if(!PySequence_Check(seq)) MOD_DICT_RAISE(PyExc_TypeError,"in_: argument must be a sequence");
    std::string simple(s->field); bool wc=(s->pattern!=nullptr); std::vector<std::string> empty;
    const std::vector<std::string>& pat=wc?*s->pattern:empty;
    if (strcmp(ret,"rows")!=0) {
        bool want_values=(strcmp(ret,"values")==0);
        if (want_values && !vf_obj) MOD_DICT_RAISE(PyExc_ValueError,"returns='values' requires value_field");
        PyObject* result=PyList_New(0); if(!result) return nullptr;
        Py_ssize_t n=PySequence_Size(seq);
        for(Py_ssize_t i=0;i<n;i++){
            PyObject* item=PySequence_GetItem(seq,i); if(!item){Py_DECREF(result);return nullptr;}
            ModValue mv=ModValue::from_pyobject(item); Py_DECREF(item);
            PyObject* part=apply_filter_here(s->owner,simple,pat,wc,FilterOp::EQ,mv,want_values,vf_obj);
            if(!part){Py_DECREF(result);return nullptr;}
            Py_ssize_t m=PyList_GET_SIZE(part);
            for(Py_ssize_t j=0;j<m;j++){PyObject* r=PyList_GET_ITEM(part,j);Py_INCREF(r);PyList_Append(result,r);Py_DECREF(r);}
            Py_DECREF(part);
        }
        return result;
    }
    ModDict* merged=new ModDict();
    Py_ssize_t n=PySequence_Size(seq);
    for(Py_ssize_t i=0;i<n;i++){
        PyObject* item=PySequence_GetItem(seq,i); if(!item){delete merged;return nullptr;}
        ModValue mv=ModValue::from_pyobject(item); Py_DECREF(item);
        ModDict* part=filter_maybe_relay(s->owner,simple,pat,wc,FilterOp::EQ,mv);
        if(!part){delete merged;return nullptr;}
        if(wc && pattern_has_link_hop(pat)){
            // Every part shares the SAME single anchor hash (an anchored
            // "->" result is always {table: {...}}) -- union inner rows
            // instead of the plain de-dup below, which would silently keep
            // only the first value's matches.
            union_anchored_into(merged, part);
        } else {
            for(auto& e:part->outer.occupied()) if(!merged->outer.find(e.key)){Py_XINCREF(e.value.key_py);Py_XINCREF(e.value.val_py);merged->outer.insert(e.key,{e.value.key_py,e.value.val_py,e.value.is_row});merged->order.push_back(e.key);}
        }
        delete part;
    }
    ModDictObject* w=PyObject_New(ModDictObject,&ModDict_Type); if(!w){delete merged;return nullptr;}
    w->internal=merged;w->owns_internal=true;w->parent_ref=(PyObject*)s->owner;Py_INCREF(s->owner);w->weakreflist=nullptr;merged->py_wrapper=w;
    return (PyObject*)w;
}
static PyMethodDef FB_methods[]={
    {"eq",(PyCFunction)(PyCFunctionWithKeywords)FB_eq,METH_VARARGS|METH_KEYWORDS,"eq(value,*,returns='rows',value_field=None)"},
    {"ne",(PyCFunction)(PyCFunctionWithKeywords)FB_ne,METH_VARARGS|METH_KEYWORDS,"ne(value,*,returns='rows',value_field=None)"},
    {"lt",(PyCFunction)(PyCFunctionWithKeywords)FB_lt,METH_VARARGS|METH_KEYWORDS,"lt(value,*,returns='rows',value_field=None)"},
    {"lte",(PyCFunction)(PyCFunctionWithKeywords)FB_lte,METH_VARARGS|METH_KEYWORDS,"lte(value,*,returns='rows',value_field=None)"},
    {"gt",(PyCFunction)(PyCFunctionWithKeywords)FB_gt,METH_VARARGS|METH_KEYWORDS,"gt(value,*,returns='rows',value_field=None)"},
    {"gte",(PyCFunction)(PyCFunctionWithKeywords)FB_gte,METH_VARARGS|METH_KEYWORDS,"gte(value,*,returns='rows',value_field=None)"},
    {"between",(PyCFunction)(PyCFunctionWithKeywords)FB_between,METH_VARARGS|METH_KEYWORDS,"between(lo,hi,*,returns='rows',value_field=None)"},
    {"in_",(PyCFunction)(PyCFunctionWithKeywords)FB_in_,METH_VARARGS|METH_KEYWORDS,"in_(values,*,returns='rows',value_field=None)"},
    {NULL,NULL,0,NULL}};
PyTypeObject FilterBuilder_Type={
    .tp_name="mod_dict.FilterBuilder",.tp_basicsize=sizeof(FilterBuilderObject),
    .tp_dealloc=(destructor)FilterBuilder_dealloc,.tp_flags=Py_TPFLAGS_DEFAULT,
    .tp_doc="FilterBuilder",.tp_methods=FB_methods};

static PyObject* FilterBuilder_new_obj(ModDictObject* owner,PyObject* fo){
    std::string simple; std::vector<std::string>* pat=nullptr; bool wc;
    if(PyUnicode_Check(fo)){
        std::string raw=PyUnicode_AsUTF8(fo);
        if(raw.find("->")!=std::string::npos){
            std::vector<std::string> tmp;
            if(!parse_link_pattern(raw,tmp)) return nullptr;  // PyErr already set
            pat=new std::vector<std::string>(std::move(tmp));
            wc=true;
        }
    }
    if(!pat){
        std::vector<std::string> tmp; if(!parse_field_or_pattern(fo,simple,tmp,wc)){PyErr_SetString(PyExc_TypeError,"filter: field must be str or tuple");return nullptr;}if(wc)pat=new std::vector<std::string>(std::move(tmp));
    }
    FilterBuilderObject* fb=(FilterBuilderObject*)FilterBuilder_Type.tp_alloc(&FilterBuilder_Type,0);
    if(!fb){delete pat;return nullptr;}
    fb->owner=owner;Py_INCREF(owner);fb->field=portable_strdup(simple.c_str());fb->pattern=pat;
    return (PyObject*)fb;
}

static PyObject* ModDict_filter(ModDictObject* s,PyObject* args){
    MOD_DICT_NO_CURSOR(s, "filter()");
    PyObject* fo; if(!PyArg_ParseTuple(args,"O",&fo)) return nullptr;
    return FilterBuilder_new_obj(s,fo);
}
static PyObject* ModDict_filter_legacy(ModDictObject* s,PyObject* args,PyObject* kw){
    MOD_DICT_NO_CURSOR(s, "filter()");
    PyObject *on,*val; const char* op_s; static const char* kwl[]={"on","op","value",NULL};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"OsO",(char**)kwl,&on,&op_s,&val)) return nullptr;
    std::string simple; std::vector<std::string> pattern; bool wc;
    if(!parse_field_or_pattern(on,simple,pattern,wc)) MOD_DICT_RAISE(PyExc_TypeError,"on must be str or tuple");
    FilterOp op=parse_op(op_s); if((int)op==-1) MOD_DICT_RAISE(PyExc_ValueError,"op must be ==,!=,<,<=,>,>=");
    return apply_filter(s,simple,pattern,wc,op,val);
}

/* Iterator */
// snapshot: nullptr in root mode (walks owner->internal->outer's slots
// directly, as before). In cursor mode, a PyList snapshot of the anchored
// dict's keys taken at iterator-creation time — simpler and safer than a
// live PyDict_Next walk (mutating a dict mid-iteration is undefined
// behavior in Python regardless, so a snapshot costs nothing extra there).
struct ModDictIterObject{PyObject_HEAD ModDictObject* owner; size_t position; PyObject* snapshot;};
static void ModDictIter_dealloc(ModDictIterObject* s){Py_XDECREF(s->owner);Py_XDECREF(s->snapshot);Py_TYPE(s)->tp_free((PyObject*)s);}
static PyObject* ModDictIter_next(ModDictIterObject* s){
    if (s->snapshot) {
        if (s->position >= (size_t)PyList_GET_SIZE(s->snapshot)) { PyErr_SetNone(PyExc_StopIteration); return nullptr; }
        PyObject* k = PyList_GET_ITEM(s->snapshot, s->position++);
        Py_INCREF(k); return k;
    }
    const auto& outer = s->owner->internal->outer;
    while(s->position < outer.capacity()){
        const auto* slot = outer.begin() + s->position++;
        if(slot->occupied){
            PyObject* k = slot->value.key_py ? slot->value.key_py : Py_None;
            Py_INCREF(k); return k;
        }
    }
    PyErr_SetNone(PyExc_StopIteration); return nullptr;
}
PyTypeObject ModDictIter_Type={
    .tp_name="mod_dict.ModDictIter",.tp_basicsize=sizeof(ModDictIterObject),
    .tp_dealloc=(destructor)ModDictIter_dealloc,.tp_flags=Py_TPFLAGS_DEFAULT,
    .tp_iter=PyObject_SelfIter,.tp_iternext=(iternextfunc)ModDictIter_next};
// Forward-declared: shared by ModDict_iter and view_keys()/view_values()/
// view_items() below, defined after ModDict_iter for readability.
static PyObject* cursor_view_key_list(const ModDictObject* s, PyObject* d);
static PyObject* ModDict_iter(ModDictObject* s){
    ModDictIterObject* it=PyObject_New(ModDictIterObject,&ModDictIter_Type);
    if(!it){PyErr_NoMemory();return nullptr;}
    it->owner=s; Py_INCREF(s); it->position=0; it->snapshot=nullptr;
    if (s->internal->root) {
        PyObject* d = s->internal->resolve_cursor_dict();
        if (!d) { Py_DECREF(it); return nullptr; }
        it->snapshot = cursor_view_key_list(s, d);
        if (!it->snapshot) { Py_DECREF(it); return nullptr; }
    }
    return (PyObject*)it;
}

// view_keys()/view_values()/view_items() — the cursor's current sort/filter
// VIEW, deliberately named apart from keys()/values()/items() (which stay
// raw/unaffected by sort/filter on a cursor, same as [key]/in/del) so the
// name itself says whether sort/filter is being honored — no method that
// looks dict-like silently means something else depending on context.
static PyObject* cursor_view_key_list(const ModDictObject* s, PyObject* d) {
    if (s->internal->filter_predicate) {
        auto& order = s->internal->visible_index;
        PyObject* list = PyList_New((Py_ssize_t)order.size());
        if (!list) return nullptr;
        for (size_t i = 0; i < order.size(); i++) { Py_INCREF(order[i]); PyList_SET_ITEM(list, (Py_ssize_t)i, order[i]); }
        return list;
    }
    if (s->internal->has_derived_order) {
        auto& order = s->internal->sort_index;
        PyObject* list = PyList_New((Py_ssize_t)order.size());
        if (!list) return nullptr;
        for (size_t i = 0; i < order.size(); i++) { Py_INCREF(order[i]); PyList_SET_ITEM(list, (Py_ssize_t)i, order[i]); }
        return list;
    }
    return PyDict_Keys(d);
}
static PyObject* ModDict_view_keys(ModDictObject* s, PyObject*) {
    MOD_DICT_REQUIRE_CURSOR(s, "view_keys()");
    PyObject* d = s->internal->resolve_cursor_dict();
    if (!d) return nullptr;
    return cursor_view_key_list(s, d);
}
static PyObject* ModDict_view_values(ModDictObject* s, PyObject*) {
    MOD_DICT_REQUIRE_CURSOR(s, "view_values()");
    PyObject* d = s->internal->resolve_cursor_dict();
    if (!d) return nullptr;
    PyObject* keys = cursor_view_key_list(s, d);
    if (!keys) return nullptr;
    Py_ssize_t n = PyList_GET_SIZE(keys);
    PyObject* list = PyList_New(n);
    if (!list) { Py_DECREF(keys); return nullptr; }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* k = PyList_GET_ITEM(keys, i);
        PyObject* row = PyDict_GetItem(d, k);  // borrowed
        Py_INCREF(row ? row : Py_None);
        PyList_SET_ITEM(list, i, row ? row : Py_None);
    }
    Py_DECREF(keys);
    return list;
}
static PyObject* ModDict_view_items(ModDictObject* s, PyObject*) {
    MOD_DICT_REQUIRE_CURSOR(s, "view_items()");
    PyObject* d = s->internal->resolve_cursor_dict();
    if (!d) return nullptr;
    PyObject* keys = cursor_view_key_list(s, d);
    if (!keys) return nullptr;
    Py_ssize_t n = PyList_GET_SIZE(keys);
    PyObject* list = PyList_New(n);
    if (!list) { Py_DECREF(keys); return nullptr; }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* k = PyList_GET_ITEM(keys, i); Py_INCREF(k);
        PyObject* row = PyDict_GetItem(d, k);  // borrowed
        Py_INCREF(row ? row : Py_None);
        PyObject* pair = PyTuple_Pack(2, k, row ? row : Py_None);
        Py_DECREF(k); Py_DECREF(row ? row : Py_None);
        if (!pair) { Py_DECREF(keys); Py_DECREF(list); return nullptr; }
        PyList_SET_ITEM(list, i, pair);
    }
    Py_DECREF(keys);
    return list;
}

static PyObject* ModDict_keys(ModDictObject* s,PyObject*){
    MOD_DICT_NO_CURSOR(s, "keys()");
    const auto& ord=s->internal->order;
    PyObject* list=PyList_New((Py_ssize_t)ord.size()); if(!list) return nullptr;
    Py_ssize_t idx=0;
    for(uint64_t oh:ord){
        const OuterEntry* e=s->internal->outer.find(oh); if(!e) continue;
        PyObject* k=e->key_py?e->key_py:Py_None; Py_INCREF(k); PyList_SET_ITEM(list,idx++,k);
    }
    return list;
}
static PyObject* ModDict_values(ModDictObject* s,PyObject*){
    MOD_DICT_NO_CURSOR(s, "values()");
    const auto& ord=s->internal->order;
    PyObject* list=PyList_New((Py_ssize_t)ord.size()); if(!list) return nullptr;
    Py_ssize_t idx=0;
    for(uint64_t oh:ord){
        const OuterEntry* e=s->internal->outer.find(oh); if(!e) continue;
        PyObject* v=e->is_row?s->internal->get_row(oh):(e->val_py?(Py_INCREF(e->val_py),e->val_py):(Py_INCREF(Py_None),Py_None));
        PyList_SET_ITEM(list,idx++,v);
    }
    return list;
}
static PyObject* ModDict_items(ModDictObject* s,PyObject*){
    MOD_DICT_NO_CURSOR(s, "items()");
    const auto& ord=s->internal->order;
    PyObject* list=PyList_New((Py_ssize_t)ord.size()); if(!list) return nullptr;
    Py_ssize_t idx=0;
    for(uint64_t oh:ord){
        const OuterEntry* e=s->internal->outer.find(oh); if(!e) continue;
        PyObject* k=e->key_py?e->key_py:Py_None; Py_INCREF(k);
        PyObject* v=e->is_row?s->internal->get_row(oh):(e->val_py?(Py_INCREF(e->val_py),e->val_py):(Py_INCREF(Py_None),Py_None));
        PyObject* pair=PyTuple_Pack(2,k,v); Py_DECREF(k); Py_DECREF(v);
        PyList_SET_ITEM(list,idx++,pair);
    }
    return list;
}
static PyObject* ModDict_to_dict(ModDictObject* s,PyObject*){
    MOD_DICT_NO_CURSOR(s, "to_dict()");
    return s->internal->to_python_dict();
}

/* Serialization */
static PyObject* ModDict_serialize(ModDictObject* s,PyObject*){
    MOD_DICT_NO_CURSOR(s, "serialize()");
    std::vector<uint8_t> data=s->internal->serialize(); if(PyErr_Occurred()) return nullptr;
    return PyBytes_FromStringAndSize((const char*)data.data(),(Py_ssize_t)data.size());
}
static PyObject* ModDict_deserialize(ModDictObject* s,PyObject* args){
    MOD_DICT_NO_CURSOR(s, "deserialize()");
    const char* data; Py_ssize_t len;
    if(!PyArg_ParseTuple(args,"y#",&data,&len)) return nullptr;
    s->internal->deserialize((const uint8_t*)data,(size_t)len);
    if(PyErr_Occurred()) return nullptr;
    Py_INCREF(s); return (PyObject*)s;
}

/* Index */
static PyObject* ModDict_create_index(ModDictObject* s,PyObject* args){
    // Sharing indices.by_field with the root is automatic (mod_dict.h), but
    // build()/build_wildcard() still scan owner->outer directly, which is
    // always empty for a cursor — needs an anchor-aware scan to support this.
    MOD_DICT_NO_CURSOR(s, "create_index()");
    PyObject* a; if(!PyArg_ParseTuple(args,"O",&a)) return nullptr;
    std::string simple; std::vector<std::string> pat; bool wc;
    if(!parse_field_or_pattern(a,simple,pat,wc)){PyErr_SetString(PyExc_TypeError,"create_index: str or tuple required");return nullptr;}
    if(wc) s->internal->create_index(pat); else s->internal->create_index(simple);
    Py_RETURN_NONE;
}
static PyObject* ModDict_drop_index(ModDictObject* s,PyObject* args){
    MOD_DICT_NO_CURSOR(s, "drop_index()");
    PyObject* a; if(!PyArg_ParseTuple(args,"O",&a)) return nullptr;
    std::string simple; std::vector<std::string> pat; bool wc;
    if(!parse_field_or_pattern(a,simple,pat,wc)){PyErr_SetString(PyExc_TypeError,"drop_index: str or tuple required");return nullptr;}
    if(wc) s->internal->drop_index(pat); else s->internal->drop_index(simple);
    Py_RETURN_NONE;
}
static PyObject* ModDict_has_index(ModDictObject* s,PyObject* args){
    MOD_DICT_NO_CURSOR(s, "has_index()");
    PyObject* a; if(!PyArg_ParseTuple(args,"O",&a)) return nullptr;
    std::string simple; std::vector<std::string> pat; bool wc;
    if(!parse_field_or_pattern(a,simple,pat,wc)){PyErr_SetString(PyExc_TypeError,"has_index: str or tuple required");return nullptr;}
    return PyBool_FromLong(wc?s->internal->has_index(pat):s->internal->has_index(simple));
}

/* Cursor flags: set_sort/set_filter/set_group */
static PyObject* ModDict_set_sort(ModDictObject* s, PyObject* args, PyObject* kw) {
    MOD_DICT_REQUIRE_CURSOR(s, "set_sort()");
    PyObject* fo; int rev = 0;
    static const char* kwl[] = {"field", "reverse", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|p", (char**)kwl, &fo, &rev)) return nullptr;
    std::string simple; std::vector<std::string> pattern; bool wc = false;
    if (!parse_field_or_pattern(fo, simple, pattern, wc)) MOD_DICT_RAISE(PyExc_TypeError, "set_sort: field must be str or tuple");
    std::vector<std::string> field = wc ? pattern : std::vector<std::string>{simple};
    auto diff = s->internal->set_sort(field, (bool)rev);
    if (PyErr_Occurred()) return nullptr;
    return index_diff_to_pylist(diff);
}
static PyObject* ModDict_set_group(ModDictObject* s, PyObject* args) {
    MOD_DICT_REQUIRE_CURSOR(s, "set_group()");
    PyObject* fo;
    if (!PyArg_ParseTuple(args, "O", &fo)) return nullptr;
    std::vector<std::string> field;
    if (fo != Py_None) {
        std::string simple; std::vector<std::string> pattern; bool wc = false;
        if (!parse_field_or_pattern(fo, simple, pattern, wc)) MOD_DICT_RAISE(PyExc_TypeError, "set_group: field must be str, tuple, or None");
        field = wc ? pattern : std::vector<std::string>{simple};
    }
    auto diff = s->internal->set_group(field);
    if (PyErr_Occurred()) return nullptr;
    return index_diff_to_pylist(diff);
}
static PyObject* ModDict_set_filter(ModDictObject* s, PyObject* args) {
    MOD_DICT_REQUIRE_CURSOR(s, "set_filter()");
    PyObject* pred;
    if (!PyArg_ParseTuple(args, "O", &pred)) return nullptr;
    if (pred == Py_None) pred = nullptr;
    else if (!PyCallable_Check(pred)) MOD_DICT_RAISE(PyExc_TypeError, "set_filter: predicate must be callable or None");
    auto diff = s->internal->set_filter(pred);
    if (PyErr_Occurred()) return nullptr;
    return index_diff_to_pylist(diff);
}

/* Cursor observability: connect() + point-mutation API */
static PyObject* ModDict_connect(ModDictObject* s, PyObject* args) {
    MOD_DICT_REQUIRE_CURSOR(s, "connect()");
    const char* event; PyObject* cb;
    if (!PyArg_ParseTuple(args, "sO", &event, &cb)) return nullptr;
    if (!PyCallable_Check(cb)) MOD_DICT_RAISE(PyExc_TypeError, "connect: callback must be callable");
    if (!s->internal->live_connect_listeners) {
        s->internal->live_connect_listeners = PyDict_New();
        if (!s->internal->live_connect_listeners) return nullptr;
    }
    PyObject* key = PyUnicode_FromString(event);
    if (!key) return nullptr;
    PyObject* listeners = PyDict_GetItem(s->internal->live_connect_listeners, key);  // borrowed
    if (!listeners) {
        listeners = PyList_New(0);
        if (!listeners) { Py_DECREF(key); return nullptr; }
        if (PyDict_SetItem(s->internal->live_connect_listeners, key, listeners) != 0) {
            Py_DECREF(key); Py_DECREF(listeners); return nullptr;
        }
        Py_DECREF(listeners);  // the dict now holds the owning reference
    }
    Py_DECREF(key);
    if (PyList_Append(listeners, cb) != 0) return nullptr;
    Py_RETURN_NONE;
}

static PyObject* ModDict_cursor_insert(ModDictObject* s, PyObject* args) {
    MOD_DICT_REQUIRE_CURSOR(s, "insert()");
    PyObject *key, *row;
    if (!PyArg_ParseTuple(args, "OO", &key, &row)) return nullptr;
    if (!PyDict_Check(row)) MOD_DICT_RAISE(PyExc_TypeError, "insert: row must be a dict");
    Py_ssize_t new_pos = s->internal->cursor_insert(key, row);
    if (PyErr_Occurred()) return nullptr;
    PyObject* idx = py_index_or_none(new_pos);
    if (!idx) return nullptr;
    // Pairs the index with the row itself — a connect() listener may live
    // far from the call site with no other way to reach this row's data.
    PyObject* payload = PyTuple_Pack(2, idx, row);
    Py_DECREF(idx);
    if (!payload) return nullptr;
    s->internal->dispatch_event("insert", payload);
    if (PyErr_Occurred()) { Py_DECREF(payload); return nullptr; }
    return payload;
}

static PyObject* ModDict_cursor_update_row(ModDictObject* s, PyObject* args) {
    MOD_DICT_REQUIRE_CURSOR(s, "update_row()");
    PyObject *key, *changes;
    if (!PyArg_ParseTuple(args, "OO", &key, &changes)) return nullptr;
    if (!PyDict_Check(changes)) MOD_DICT_RAISE(PyExc_TypeError, "update_row: changes must be a dict");

    PyObject* d = s->internal->resolve_cursor_dict();
    if (!d) return nullptr;
    PyObject* old_row = PyDict_GetItem(d, key);  // borrowed
    if (!old_row) { PyErr_SetObject(PyExc_KeyError, key); return nullptr; }
    PyObject* old_snapshot = PyDict_Copy(old_row);  // shallow — just for changed_fields comparison below
    if (!old_snapshot) return nullptr;

    auto pos_pair = s->internal->cursor_update_row(key, changes);
    if (PyErr_Occurred()) { Py_DECREF(old_snapshot); return nullptr; }

    // changed: {field: new_value} for keys in `changes` whose value actually
    // differs from what was there before the write (not just "was present
    // in `changes`") — a connect() listener gets the actual new values, not
    // just field names, so it doesn't need a separate lookup into the row.
    PyObject* changed = PyDict_New();
    if (!changed) { Py_DECREF(old_snapshot); return nullptr; }
    PyObject *ck, *cv; Py_ssize_t pos = 0;
    while (PyDict_Next(changes, &pos, &ck, &cv)) {
        PyObject* prev = PyDict_GetItem(old_snapshot, ck);  // borrowed, NULL = field didn't exist before
        int eq = prev ? PyObject_RichCompareBool(prev, cv, Py_EQ) : 0;
        if (eq < 0) { Py_DECREF(old_snapshot); Py_DECREF(changed); return nullptr; }
        if (!eq && PyDict_SetItem(changed, ck, cv) != 0) { Py_DECREF(old_snapshot); Py_DECREF(changed); return nullptr; }
    }
    Py_DECREF(old_snapshot);

    PyObject* old_i = py_index_or_none(pos_pair.first);
    if (!old_i) { Py_DECREF(changed); return nullptr; }
    PyObject* new_i = py_index_or_none(pos_pair.second);
    if (!new_i) { Py_DECREF(old_i); Py_DECREF(changed); return nullptr; }
    PyObject* index_part = PyTuple_Pack(2, old_i, new_i);
    Py_DECREF(old_i); Py_DECREF(new_i);
    if (!index_part) { Py_DECREF(changed); return nullptr; }
    PyObject* result = PyTuple_Pack(2, index_part, changed);
    Py_DECREF(index_part); Py_DECREF(changed);
    if (!result) return nullptr;

    s->internal->dispatch_event("update", result);
    if (PyErr_Occurred()) { Py_DECREF(result); return nullptr; }
    return result;
}

static PyObject* ModDict_cursor_delete(ModDictObject* s, PyObject* args) {
    MOD_DICT_REQUIRE_CURSOR(s, "delete()");
    PyObject* key;
    if (!PyArg_ParseTuple(args, "O", &key)) return nullptr;
    Py_ssize_t old_pos = s->internal->cursor_delete(key);
    if (PyErr_Occurred()) return nullptr;
    PyObject* payload = py_index_or_none(old_pos);
    if (!payload) return nullptr;
    s->internal->dispatch_event("delete", payload);
    if (PyErr_Occurred()) { Py_DECREF(payload); return nullptr; }
    return payload;
}

static PyObject* ModDict_cursor_insert_batch(ModDictObject* s, PyObject* args, PyObject* kw) {
    MOD_DICT_REQUIRE_CURSOR(s, "insert_batch()");
    PyObject* rows; PyObject* key_obj = nullptr;
    static const char* kwl[]={"rows","key",nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kw, "O|O", (char**)kwl, &rows, &key_obj)) return nullptr;

    PyObject* rows_dict;
    bool built_rows_dict = false;
    if (PyDict_Check(rows)) {
        if (key_obj) MOD_DICT_RAISE(PyExc_TypeError, "insert_batch: key= is only used when rows is a list, not a dict");
        rows_dict = rows;
        PyObject *rk, *rv; Py_ssize_t rpos = 0;
        while (PyDict_Next(rows_dict, &rpos, &rk, &rv)) {
            if (!PyDict_Check(rv)) MOD_DICT_RAISE(PyExc_TypeError, "insert_batch: every value must be a dict (a row)");
        }
    } else {
        // rows is a list (or other iterable) of plain row dicts — key=
        // names the field each row's own outer key is extracted from,
        // building {row[key]: row} in one C-level pass instead of requiring
        // the caller to build that mapping themselves in a Python loop.
        if (!key_obj) MOD_DICT_RAISE(PyExc_TypeError, "insert_batch: rows is not a dict — pass key= to extract each row's identifier from its own field");
        rows_dict = PyDict_New();
        if (!rows_dict) return nullptr;
        built_rows_dict = true;
        PyObject* iter = PyObject_GetIter(rows);
        if (!iter) { Py_DECREF(rows_dict); return nullptr; }
        PyObject* row;
        while ((row = PyIter_Next(iter))) {
            if (!PyDict_Check(row)) {
                PyErr_SetString(PyExc_TypeError, "insert_batch: every row must be a dict");
                Py_DECREF(row); Py_DECREF(iter); Py_DECREF(rows_dict); return nullptr;
            }
            PyObject* pk = PyDict_GetItem(row, key_obj);  // borrowed
            if (!pk) {
                PyErr_SetObject(PyExc_KeyError, key_obj);
                Py_DECREF(row); Py_DECREF(iter); Py_DECREF(rows_dict); return nullptr;
            }
            PyDict_SetItem(rows_dict, pk, row);  // SetItem INCREFs both itself
            Py_DECREF(row);
        }
        Py_DECREF(iter);
        if (PyErr_Occurred()) { Py_DECREF(rows_dict); return nullptr; }
    }

    auto positions = s->internal->cursor_insert_batch(rows_dict);
    if (PyErr_Occurred()) { if (built_rows_dict) Py_DECREF(rows_dict); return nullptr; }

    // Pairs each position with its own row (same reasoning as insert()) —
    // zip against rows_dict in the same PyDict_Next order cursor_insert_batch()
    // itself used to build `positions`.
    PyObject* payload = PyList_New((Py_ssize_t)positions.size());
    if (!payload) { if (built_rows_dict) Py_DECREF(rows_dict); return nullptr; }
    PyObject *k, *v; Py_ssize_t dpos = 0; size_t i = 0;
    while (PyDict_Next(rows_dict, &dpos, &k, &v)) {
        PyObject* idx = py_index_or_none(positions[i]);
        if (!idx) { Py_DECREF(payload); if (built_rows_dict) Py_DECREF(rows_dict); return nullptr; }
        PyObject* pair = PyTuple_Pack(2, idx, v);
        Py_DECREF(idx);
        if (!pair) { Py_DECREF(payload); if (built_rows_dict) Py_DECREF(rows_dict); return nullptr; }
        PyList_SET_ITEM(payload, (Py_ssize_t)i, pair);  // steals the reference
        i++;
    }
    if (built_rows_dict) Py_DECREF(rows_dict);

    s->internal->dispatch_event("insert", payload);  // one event for the whole batch, same as a single insert()
    if (PyErr_Occurred()) { Py_DECREF(payload); return nullptr; }
    return payload;
}

/* Links */
static bool parse_on_delete(const char* s, LinkOnDelete& out) {
    if(!strcmp(s,"restrict")){out=LinkOnDelete::RESTRICT;return true;}
    if(!strcmp(s,"cascade")) {out=LinkOnDelete::CASCADE; return true;}
    if(!strcmp(s,"set_null")){out=LinkOnDelete::SET_NULL;return true;}
    return false;
}
static PyObject* ModDict_link(ModDictObject* s,PyObject* args,PyObject* kw){
    MOD_DICT_NO_CURSOR(s, "link()");
    PyObject *src,*ref; const char* on_del="restrict";
    static const char* kwl[]={"source_path","references_path","on_delete",nullptr};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"OO|s",(char**)kwl,&src,&ref,&on_del)) return nullptr;
    std::string s1,s2; std::vector<std::string> src_pat,ref_pat; bool wc;
    if(!parse_field_or_pattern(src,s1,src_pat,wc) || !wc){
        PyErr_SetString(PyExc_ValueError,"link: source_path must be a wildcard path like 'table.?.field'");
        return nullptr;
    }
    if(!parse_field_or_pattern(ref,s2,ref_pat,wc) || !wc){
        PyErr_SetString(PyExc_ValueError,"link: references_path must be 'table.?' (pk) or 'table.?.field' (non-pk)");
        return nullptr;
    }
    LinkOnDelete mode;
    if(!parse_on_delete(on_del,mode)){
        PyErr_SetString(PyExc_ValueError,"link: on_delete must be 'restrict', 'cascade', or 'set_null'");
        return nullptr;
    }
    s->internal->link(src_pat,ref_pat,mode);
    if(PyErr_Occurred()) return nullptr;
    Py_RETURN_NONE;
}
static PyObject* ModDict_follow(ModDictObject* s,PyObject* args,PyObject* kw){
    MOD_DICT_NO_CURSOR(s, "follow()");
    PyObject *src,*keys_seq=nullptr,*values_seq=nullptr;
    static const char* kwl[]={"source_path","keys","values",nullptr};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"O|OO",(char**)kwl,&src,&keys_seq,&values_seq)) return nullptr;
    if(keys_seq==Py_None) keys_seq=nullptr;
    if(values_seq==Py_None) values_seq=nullptr;
    if(keys_seq && values_seq){
        PyErr_SetString(PyExc_ValueError,"follow: keys and values are mutually exclusive");
        return nullptr;
    }

    std::string simple; std::vector<std::string> src_pat; bool wc;
    if(!parse_field_or_pattern(src,simple,src_pat,wc) || !wc){
        PyErr_SetString(PyExc_ValueError,"follow: source_path must be a wildcard path like 'table.?.field'");
        return nullptr;
    }

    std::vector<uint64_t> key_hashes;
    if(keys_seq){
        if(!PySequence_Check(keys_seq)){PyErr_SetString(PyExc_TypeError,"follow: keys must be a sequence");return nullptr;}
        Py_ssize_t n=PySequence_Size(keys_seq);
        for(Py_ssize_t i=0;i<n;i++){
            PyObject* item=PySequence_GetItem(keys_seq,i); if(!item) return nullptr;
            key_hashes.push_back(content_hash_pyobj(item));
            Py_DECREF(item);
        }
    }

    std::vector<PyObject*> values_vec;  // owned refs, released below
    if(values_seq){
        if(!PySequence_Check(values_seq)){PyErr_SetString(PyExc_TypeError,"follow: values must be a sequence");return nullptr;}
        Py_ssize_t n=PySequence_Size(values_seq);
        values_vec.reserve((size_t)n);
        for(Py_ssize_t i=0;i<n;i++){
            PyObject* item=PySequence_GetItem(values_seq,i);
            if(!item){ for(PyObject* p:values_vec) Py_DECREF(p); return nullptr; }
            values_vec.push_back(item);
        }
    }

    ModDict* result=s->internal->follow(src_pat,
        keys_seq?&key_hashes:nullptr,
        values_seq?&values_vec:nullptr);

    for(PyObject* p:values_vec) Py_DECREF(p);

    if(!result) return nullptr;  // PyErr already set (no such link declared, or table missing)
    return ModDict_wrap_owned(result);
}

/* from_dict/from_json */
static PyObject* ModDict_from_dict(PyObject* cls,PyObject* arg){
    if(!PyDict_Check(arg)){PyErr_SetString(PyExc_TypeError,"argument must be a dict");return nullptr;}
    ModDictObject* s=(ModDictObject*)((PyTypeObject*)cls)->tp_alloc((PyTypeObject*)cls,0);
    if(!s) return nullptr;
    s->internal=new ModDict(); s->owns_internal=true; s->internal->py_wrapper=s; s->parent_ref=nullptr; s->weakreflist=nullptr;
    PyObject *k,*v; Py_ssize_t pos=0;
    while(PyDict_Next(arg,&pos,&k,&v)){ModValue mk=ModValue::from_pyobject(k);if(PyDict_Check(v))s->internal->insert_row(mk,v);else{ModValue mv=ModValue::from_pyobject(v);s->internal->insert(mk,mv);}}
    return (PyObject*)s;
}
static PyObject* ModDict_from_json(PyObject* cls,PyObject* arg){
    if(!PyUnicode_Check(arg)){PyErr_SetString(PyExc_TypeError,"argument must be a JSON string");return nullptr;}
    PyObject* jm=PyImport_ImportModule("json"); if(!jm) return nullptr;
    PyObject* parsed=PyObject_CallMethod(jm,"loads","O",arg); Py_DECREF(jm);
    if(!parsed) return nullptr;
    if(!PyDict_Check(parsed)){PyErr_SetString(PyExc_TypeError,"JSON must be an object");Py_DECREF(parsed);return nullptr;}
    ModDictObject* s=(ModDictObject*)((PyTypeObject*)cls)->tp_alloc((PyTypeObject*)cls,0);
    if(!s){Py_DECREF(parsed);return nullptr;}
    s->internal=new ModDict(); s->owns_internal=true; s->internal->py_wrapper=s; s->parent_ref=nullptr; s->weakreflist=nullptr;
    PyObject *k,*v; Py_ssize_t pos=0;
    while(PyDict_Next(parsed,&pos,&k,&v)){ModValue mk=ModValue::from_pyobject(k);if(PyDict_Check(v))s->internal->insert_row(mk,v);else{ModValue mv=ModValue::from_pyobject(v);s->internal->insert(mk,mv);}}
    Py_DECREF(parsed); return (PyObject*)s;
}

/* from_rows / from_row */
static PyObject* ModDict_from_rows(PyObject* cls, PyObject* args, PyObject* kw){
    PyObject* rows; PyObject* key_obj;
    static const char* kwl[]={"rows","key",nullptr};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"OO",(char**)kwl,&rows,&key_obj)) return nullptr;

    ModDictObject* s=(ModDictObject*)((PyTypeObject*)cls)->tp_alloc((PyTypeObject*)cls,0);
    if(!s) return nullptr;
    s->internal=new ModDict(); s->owns_internal=true; s->internal->py_wrapper=s; s->parent_ref=nullptr; s->weakreflist=nullptr;

    PyObject* iter=PyObject_GetIter(rows);
    if(!iter){ Py_DECREF(s); return nullptr; }
    PyObject* row;
    while((row=PyIter_Next(iter))){
        // support dict and Mapping-like objects (row[key])
        PyObject* pk=nullptr;
        if(PyDict_Check(row)) pk=PyDict_GetItem(row,key_obj);   // borrowed
        else                  pk=PyObject_GetItem(row,key_obj);  // new ref — need DECREF
        if(!pk){ Py_DECREF(row); Py_DECREF(iter); Py_DECREF(s); return nullptr; }

        PyObject* row_dict = PyDict_Check(row) ? row : nullptr;
        if(!row_dict){
            // convert Mapping-like to dict
            row_dict = PyDict_New();
            PyObject* items = PyMapping_Items(row);
            if(items){
                for(Py_ssize_t i=0;i<PyList_Size(items);i++){
                    PyObject* pair=PyList_GET_ITEM(items,i);
                    PyDict_SetItem(row_dict,PyTuple_GET_ITEM(pair,0),PyTuple_GET_ITEM(pair,1));
                }
                Py_DECREF(items);
            }
        } else {
            Py_INCREF(row_dict);
        }

        bool borrowed_pk = PyDict_Check(row);
        ModValue mk=ModValue::from_pyobject(pk);
        s->internal->insert_row(mk, row_dict);
        Py_DECREF(row_dict);
        if(!borrowed_pk) Py_DECREF(pk);
        Py_DECREF(row);
    }
    Py_DECREF(iter);
    if(PyErr_Occurred()){ Py_DECREF(s); return nullptr; }
    return (PyObject*)s;
}

// Instance method — writes {row[key]: row for row in rows} into self at
// `path` (a single top-level key), same effect as
// `self[path] = {row[key]: row for row in rows}`. from_rows() above can't
// do this itself: it's a classmethod, so even `md.from_rows(...)` binds
// `cls` to the type, never to the instance it was called through.
static PyObject* ModDict_load_rows(ModDictObject* s, PyObject* args, PyObject* kw){
    PyObject* rows; PyObject* key_obj; PyObject* path_obj;
    static const char* kwl[]={"rows","key","path",nullptr};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"OOO",(char**)kwl,&rows,&key_obj,&path_obj)) return nullptr;
    if(!PyUnicode_Check(path_obj)){ PyErr_SetString(PyExc_TypeError,"path must be a str"); return nullptr; }

    PyObject* built = PyDict_New();
    if(!built) return nullptr;
    PyObject* iter=PyObject_GetIter(rows);
    if(!iter){ Py_DECREF(built); return nullptr; }
    PyObject* row;
    while((row=PyIter_Next(iter))){
        // support dict and Mapping-like objects (row[key]) — same as from_rows()
        PyObject* pk=nullptr;
        if(PyDict_Check(row)) pk=PyDict_GetItem(row,key_obj);   // borrowed
        else                  pk=PyObject_GetItem(row,key_obj);  // new ref — need DECREF
        if(!pk){
            if(!PyErr_Occurred()) PyErr_SetObject(PyExc_KeyError, key_obj);  // PyDict_GetItem doesn't set one itself
            Py_DECREF(row); Py_DECREF(iter); Py_DECREF(built); return nullptr;
        }

        PyObject* row_dict = PyDict_Check(row) ? row : nullptr;
        bool own_row_dict = false;
        if(!row_dict){
            row_dict = PyDict_New();
            own_row_dict = true;
            PyObject* items = PyMapping_Items(row);
            if(items){
                for(Py_ssize_t i=0;i<PyList_Size(items);i++){
                    PyObject* pair=PyList_GET_ITEM(items,i);
                    PyDict_SetItem(row_dict,PyTuple_GET_ITEM(pair,0),PyTuple_GET_ITEM(pair,1));
                }
                Py_DECREF(items);
            }
        }

        bool borrowed_pk = PyDict_Check(row);
        PyDict_SetItem(built, pk, row_dict);  // SetItem INCREFs pk/row_dict itself
        if(own_row_dict) Py_DECREF(row_dict);
        if(!borrowed_pk) Py_DECREF(pk);
        Py_DECREF(row);
    }
    Py_DECREF(iter);
    if(PyErr_Occurred()){ Py_DECREF(built); return nullptr; }

    ModValue mk = ModValue::from_pyobject(path_obj);
    s->internal->insert_row(mk, built);
    Py_DECREF(built);
    Py_RETURN_NONE;
}

static PyObject* ModDict_from_row(PyObject* cls, PyObject* arg){
    // Convert a single Mapping-like row to a plain dict and return it as PyObject*
    // (not a ModDict — just a convenience converter)
    if(PyDict_Check(arg)){ Py_INCREF(arg); return arg; }
    PyObject* d=PyDict_New(); if(!d) return nullptr;
    PyObject* items=PyMapping_Items(arg);
    if(!items){ Py_DECREF(d); return nullptr; }
    for(Py_ssize_t i=0;i<PyList_Size(items);i++){
        PyObject* pair=PyList_GET_ITEM(items,i);
        PyDict_SetItem(d,PyTuple_GET_ITEM(pair,0),PyTuple_GET_ITEM(pair,1));
    }
    Py_DECREF(items);
    return d;
}

/* cursor */
static PyObject* ModDict_cursor(ModDictObject* s, PyObject* args) {
    PyObject* arg;
    if (!PyArg_ParseTuple(args, "O", &arg)) return nullptr;
    std::string simple; std::vector<std::string> pattern; bool wc=false;
    if (!parse_field_or_pattern(arg, simple, pattern, wc)) {
        PyErr_SetString(PyExc_TypeError, "cursor: path must be a string or tuple/list of segments");
        return nullptr;
    }
    std::vector<std::string> path = wc ? pattern : std::vector<std::string>{simple};

    ModDict* result = s->internal->cursor(path);
    if (!result) return nullptr;  // PyErr already set (empty/wildcard path, or not found)

    ModDictObject* w = PyObject_New(ModDictObject, &ModDict_Type);
    if (!w) { delete result; return nullptr; }
    w->internal = result; w->owns_internal = true; w->parent_ref = (PyObject*)s; Py_INCREF(s); w->weakreflist = nullptr;
    result->py_wrapper = w;

    PyObject* wr = PyWeakref_NewRef((PyObject*)w, nullptr);
    if (!wr) { Py_DECREF(w); return nullptr; }
    result->register_live_cursor(wr);

    return (PyObject*)w;
}

/* group_by/select/sort_by */
static PyObject* ModDict_group_by(ModDictObject* s,PyObject* args){
    MOD_DICT_NO_CURSOR(s, "group_by()");
    const char* field; if(!PyArg_ParseTuple(args,"s",&field)) return nullptr;
    auto groups=s->internal->group_by(std::string(field)); if(PyErr_Occurred()) return nullptr;
    PyObject* result=PyDict_New(); if(!result) return nullptr;
    for(auto& [fv,gd]:groups){
        PyObject* key=fv.to_pyobject(); if(!key){Py_DECREF(result);return nullptr;}
        ModDictObject* w=PyObject_New(ModDictObject,&ModDict_Type);
        if(!w){Py_DECREF(key);Py_DECREF(result);return nullptr;}
        w->internal=gd;w->owns_internal=true;w->parent_ref=(PyObject*)s;Py_INCREF(s);w->weakreflist=nullptr;gd->py_wrapper=w;
        PyDict_SetItem(result,key,(PyObject*)w);Py_DECREF(key);Py_DECREF((PyObject*)w);
    }
    return result;
}
// Resolve dot-notation path inside a Python dict row. Borrowed ref.
static PyObject* resolve_field_path(PyObject* row, const std::string& field) {
    PyObject* cur = row;
    size_t pos = 0;
    while (true) {
        size_t d = field.find('.', pos);
        std::string seg = field.substr(pos, d == std::string::npos ? d : d - pos);
        if (!cur || !PyDict_Check(cur)) return nullptr;
        cur = PyDict_GetItemString(cur, seg.c_str());
        if (d == std::string::npos) break;
        pos = d + 1;
    }
    return cur;
}

// Default result-row key for a select() path with no explicit label: the
// text after the last "->" (if any), then the text after the last "." in
// that (space/tab count as "." too, matching the rest of the path syntax).
// "age.a" -> "a", "score" -> "score", "orders.?.customer_id->name" -> "name".
static std::string default_select_label(const std::string& raw){
    size_t start=0;
    size_t arrow=raw.rfind("->");
    if(arrow!=std::string::npos) start=arrow+2;
    size_t dot=std::string::npos;
    for(size_t i=raw.size(); i-->start; ){
        char c=raw[i];
        if(c=='.'||c==' '||c=='\t'){ dot=i; break; }
    }
    return dot==std::string::npos ? raw.substr(start) : raw.substr(dot+1);
}
// Mirrors filter_link_hop_via_root for select(): relay the whole
// select_anchored() call to the topmost ancestor (unrestricted), then keep
// only the entries whose key is present in `owner`'s own rows for the
// shared anchor table. select()'s result is flat, keyed by the anchor row's
// own key (not filter()'s nested {table:{...}}) -- a key-membership filter,
// not an inner-dict intersection.
static ModDict* select_link_hop_via_root(ModDictObject* owner,
                                          const std::vector<std::vector<std::string>>& patterns,
                                          const std::vector<std::string>& labels) {
    ModDictObject* root = owner;
    while (root->parent_ref) root = (ModDictObject*)root->parent_ref;

    const std::string& anchor_table = patterns[0][0];
    PyObject* tmp = PyUnicode_FromStringAndSize(anchor_table.c_str(), anchor_table.size());
    if (!tmp) return nullptr;
    uint64_t anchor_hash = content_hash_pyobj(tmp);
    Py_DECREF(tmp);

    const OuterEntry* child_entry = owner->internal->outer.find(anchor_hash);
    if (!child_entry || !child_entry->val_py || !PyDict_Check(child_entry->val_py))
        return new ModDict();  // child doesn't have this table at all -> no match, not an error

    ModDict* root_result = root->internal->select_anchored(patterns, labels);
    if (!root_result) return nullptr;

    ModDict* result = new ModDict();
    PyObject *k, *v; Py_ssize_t pos = 0;
    while (PyDict_Next(child_entry->val_py, &pos, &k, &v)) {
        uint64_t kh = content_hash_pyobj(k);
        const OuterEntry* re = root_result->outer.find(kh);
        if (!re || !re->val_py) continue;
        Py_INCREF(re->key_py); Py_INCREF(re->val_py);
        result->outer.insert(kh, {re->key_py, re->val_py, re->is_row});
        result->order.push_back(kh);
    }
    delete root_result;
    return result;
}
// Implements select(fields, returns="rows"): walks a "->" hop chain (the
// same pattern shape parse_link_pattern already produces for ordinary
// select()/filter() paths). Each path is resolved independently to a
// (table, {pk:row}) pair — a path with one or more "->" hops lands on the
// final hop's target table (chaining follow() calls, exactly the multi-hop
// follow(keys=...) composition the .pyi docs already describe, automated);
// a path with no hop at all just contributes its own anchor table's current
// rows, unrestricted by any field it names (the field, if any, is only ever
// used to identify which hop chain to walk, never to extract a value in
// this mode). All per-path results are merged into ONE ModDict, unioning
// rows together if two paths land on the same table. Built entirely from
// already-public ModDict::find_link()/follow() — no core changes.
static ModDict* select_rows_mode(ModDictObject* owner, const std::vector<std::vector<std::string>>& patterns) {
    ModDictObject* root = owner;
    while (root->parent_ref) root = (ModDictObject*)root->parent_ref;

    ModDict* combined = new ModDict();
    for (auto& pattern : patterns) {
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
        std::string current_table = groups[0][0];

        // Materialize this path's starting {pk:row} set: the caller's own
        // current rows for the anchor table if it's a derived child (same
        // restriction convention as filter_link_hop_via_root/
        // select_link_hop_via_root), else every row of the anchor table.
        PyObject* tmp = PyUnicode_FromStringAndSize(current_table.c_str(), current_table.size());
        if (!tmp) { delete combined; return nullptr; }
        uint64_t anchor_hash = content_hash_pyobj(tmp);
        Py_DECREF(tmp);
        const OuterEntry* anchor_entry = owner->parent_ref
            ? owner->internal->outer.find(anchor_hash)
            : root->internal->outer.find(anchor_hash);
        if (!anchor_entry || !anchor_entry->val_py || !PyDict_Check(anchor_entry->val_py)) {
            if (owner->parent_ref) continue;  // child doesn't restrict this table -> this path contributes nothing
            delete combined;
            PyErr_SetString(PyExc_ValueError, "select: anchor table not found");
            return nullptr;
        }
        ModDict* last_result = new ModDict();
        {
            PyObject *k, *v; Py_ssize_t pos = 0;
            while (PyDict_Next(anchor_entry->val_py, &pos, &k, &v)) {
                uint64_t kh = content_hash_pyobj(k);
                Py_INCREF(k); Py_INCREF(v);
                last_result->outer.insert(kh, {k, v, true});
                last_result->order.push_back(kh);
            }
        }

        if (n_hops > 0) {
            std::vector<uint64_t> current_keys;
            for (uint64_t oh : last_result->order) current_keys.push_back(oh);
            std::vector<std::string> src_pat = groups[0];
            for (size_t i = 0; i < n_hops; i++) {
                if (i > 0) src_pat = {current_table, "__pass_key__", groups[i][0]};
                const LinkDecl* ld = root->internal->find_link(src_pat);
                if (!ld) {
                    delete last_result; delete combined;
                    PyErr_SetString(PyExc_ValueError, "select: no link declared for this source_path - call mn.link() first");
                    return nullptr;
                }
                ModDict* hop_result = root->internal->follow(src_pat, &current_keys, nullptr);
                if (!hop_result) { delete last_result; delete combined; return nullptr; }
                delete last_result;
                last_result = hop_result;
                current_table = ld->references_pattern[0];
                current_keys.clear();
                for (uint64_t oh : last_result->order) current_keys.push_back(oh);
            }
        }

        // Merge (current_table, last_result) into combined -- new table
        // entry, or union rows into an existing one from an earlier path.
        PyObject* table_key = PyUnicode_FromStringAndSize(current_table.c_str(), current_table.size());
        if (!table_key) { delete last_result; delete combined; return nullptr; }
        uint64_t table_hash = content_hash_pyobj(table_key);
        OuterEntry* existing = combined->outer.find(table_hash);
        if (!existing) {
            PyObject* inner = last_result->to_python_dict();
            delete last_result;
            if (!inner) { Py_DECREF(table_key); delete combined; return nullptr; }
            if (PyDict_Size(inner) > 0) {
                combined->outer.insert(table_hash, {table_key, inner, true});
                combined->order.push_back(table_hash);
            } else {
                Py_DECREF(inner); Py_DECREF(table_key);
            }
        } else {
            Py_DECREF(table_key);  // combined already owns a key for this table
            for (auto& e : last_result->outer.occupied()) {
                if (!e.value.val_py) continue;
                if (!PyDict_Contains(existing->val_py, e.value.key_py))
                    PyDict_SetItem(existing->val_py, e.value.key_py, e.value.val_py);
            }
            delete last_result;
        }
    }
    return combined;
}
// Shared by select() (single path) and select_mass() (list/dict of paths) —
// `fo` is always a list-of-paths or {label:path} dict by the time this runs;
// select() wraps its single string into a 1-element list before calling in.
static PyObject* select_core(ModDictObject* s, PyObject* fo, const char* ret){
    // fields is either a list of paths (result keyed by each path's default
    // last-segment label — collision raises) or a {label: path} dict (result
    // keyed by the given labels, collision-free by construction).
    std::vector<std::string> paths, labels;
    bool has_labels=false;
    if(PyDict_Check(fo)){
        Py_ssize_t n=PyDict_Size(fo);
        paths.reserve((size_t)n); labels.reserve((size_t)n);
        PyObject *k,*v; Py_ssize_t pos=0;
        while(PyDict_Next(fo,&pos,&k,&v)){
            if(!PyUnicode_Check(k)||!PyUnicode_Check(v)) MOD_DICT_RAISE(PyExc_TypeError,
                "select: dict form must be {label: path}, both strings");
            labels.emplace_back(PyUnicode_AsUTF8(k));
            paths.emplace_back(PyUnicode_AsUTF8(v));
        }
    } else if(PyList_Check(fo)){
        Py_ssize_t n=PyList_Size(fo); paths.reserve((size_t)n); labels.reserve((size_t)n);
        for(Py_ssize_t i=0;i<n;i++){
            PyObject* it=PyList_GET_ITEM(fo,i);
            if(!PyUnicode_Check(it)) MOD_DICT_RAISE(PyExc_TypeError,"select: fields must be a list of strings");
            paths.emplace_back(PyUnicode_AsUTF8(it));
        }
        for(auto& p:paths) labels.push_back(default_select_label(p));
        has_labels=true;  // labels are auto-computed, defer collision check until we know if "rows" table-landing mode ignores them
    } else {
        MOD_DICT_RAISE(PyExc_TypeError,"select: fields must be a list of paths or a {label: path} dict");
    }

    // Detect wildcard/anchored paths ("orders.?.customer_id", optionally with
    // a "->" hop). If any path is wildcard-shaped, all must be — they're
    // scanned together off one shared anchor table, row by row.
    bool any_wc=false, all_wc=true;
    for(auto& f:paths){
        bool wc=(f.find("->")!=std::string::npos);
        if(!wc && (f.find('.')!=std::string::npos || f=="?")){
            for(auto& seg:split_dot_chunk(f)) if(seg=="__pass_key__"){ wc=true; break; }
        }
        any_wc=any_wc||wc; all_wc=all_wc&&wc;
    }
    if(any_wc && !all_wc) MOD_DICT_RAISE(PyExc_ValueError,
        "select: cannot mix wildcard paths (e.g. \"orders.?.customer_id\") with plain paths in one call");

    // "rows" table-landing mode doesn't use labels at all (each path lands
    // on a table, not a labeled value) — skip the auto-label collision
    // check in that case; every other mode does use labels, so it applies.
    bool table_landing = (any_wc && strcmp(ret,"rows")==0);
    if(has_labels && !table_landing){
        for(size_t i=0;i<labels.size();i++) for(size_t j=i+1;j<labels.size();j++)
            if(labels[i]==labels[j]) MOD_DICT_RAISE_FMT(PyExc_ValueError,
                "select: \"%s\" and \"%s\" both default to the result key \"%s\" - "
                "use the {label: path} dict form to disambiguate",
                paths[i].c_str(), paths[j].c_str(), labels[i].c_str());
    }

    ModDict* result;
    if(any_wc){
        std::vector<std::vector<std::string>> patterns; patterns.reserve(paths.size());
        for(auto& f:paths){
            std::vector<std::string> pat;
            if(f.find("->")!=std::string::npos){
                if(!parse_link_pattern(f,pat)) return nullptr;  // PyErr already set
            } else {
                pat=split_dot_chunk(f);
            }
            if(pat.size()<2 || pat[1]!="__pass_key__") MOD_DICT_RAISE(PyExc_ValueError,
                "select: wildcard paths must be a table-anchored path like \"table.?.field\"");
            patterns.push_back(std::move(pat));
        }
        if(table_landing){
            result=select_rows_mode(s,patterns);
        } else {
            bool has_hop=false;
            for(auto& p:patterns) if(pattern_has_link_hop(p)){ has_hop=true; break; }
            if(has_hop && s->parent_ref){
                result=select_link_hop_via_root(s,patterns,labels);
            } else {
                result=s->internal->select_anchored(patterns,labels);
            }
        }
    } else {
        result=s->internal->select(paths,labels);
    }
    // A null result can carry a specific error (e.g. "->" hop with no
    // declared link, mixed anchors) rather than allocation failure — don't
    // clobber it with a generic MemoryError via MOD_DICT_CHECK_ALLOC.
    if(!result) return nullptr;
    if(strcmp(ret,"values")==0){
        // Columnar: one flat list per requested field, values in row order.
        // N fields -> a list of N lists (not a list of projected dicts).
        Py_ssize_t n_fields=(Py_ssize_t)labels.size();
        PyObject* cols=PyList_New(n_fields);
        if(!cols){ delete result; return nullptr; }
        for(Py_ssize_t i=0;i<n_fields;i++){
            PyObject* col=PyList_New(0);
            if(!col){ Py_DECREF(cols); delete result; return nullptr; }
            PyList_SET_ITEM(cols,i,col);
        }
        for(uint64_t oh : result->order){
            const OuterEntry* e=result->outer.find(oh);
            if(!e || !e->val_py) continue;
            for(Py_ssize_t i=0;i<n_fields;i++){
                PyObject* col=PyList_GET_ITEM(cols,i);
                PyObject* v=PyDict_GetItemString(e->val_py,labels[i].c_str());
                if(!v) v=Py_None;
                Py_INCREF(v);
                PyList_Append(col,v);
                Py_DECREF(v);
            }
        }
        delete result; return cols;
    }
    // Keep parent_ref so a further "->" filter()/select() chained onto this
    // result can relay up to the root — ModDict_wrap_owned leaves it null,
    // which was fine before chaining existed (select()'s rows hold their own
    // incremented refs, no use-after-free risk) but breaks the relay chain.
    {
        ModDictObject* w=PyObject_New(ModDictObject,&ModDict_Type);
        if(!w){delete result;return nullptr;}
        w->internal=result; w->owns_internal=true; w->parent_ref=(PyObject*)s; Py_INCREF(s); w->weakreflist=nullptr;
        result->py_wrapper=w; return (PyObject*)w;
    }
}
static PyObject* ModDict_select_mass(ModDictObject* s,PyObject* args,PyObject* kw){
    MOD_DICT_NO_CURSOR(s, "select_mass()");
    PyObject* fo; const char* ret="rows";
    static const char* kwl[]={"fields","returns",nullptr};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"O|s",const_cast<char**>(kwl),&fo,&ret)) return nullptr;
    if(strcmp(ret,"rows")!=0 && strcmp(ret,"rows_here")!=0 && strcmp(ret,"values")!=0)
        MOD_DICT_RAISE(PyExc_ValueError,"select_mass: returns must be 'rows', 'rows_here', or 'values'");
    return select_core(s,fo,ret);
}
static PyObject* ModDict_select(ModDictObject* s,PyObject* args,PyObject* kw){
    MOD_DICT_NO_CURSOR(s, "select()");
    PyObject* path_obj; const char* ret="rows";
    static const char* kwl[]={"path","returns",nullptr};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"O|s",const_cast<char**>(kwl),&path_obj,&ret)) return nullptr;
    if(!PyUnicode_Check(path_obj))
        MOD_DICT_RAISE(PyExc_TypeError,"select: path must be a single string — use select_mass() for multiple fields");
    if(strcmp(ret,"rows")!=0 && strcmp(ret,"rows_here")!=0 && strcmp(ret,"values")!=0)
        MOD_DICT_RAISE(PyExc_ValueError,"select: returns must be 'rows', 'rows_here', or 'values'");

    std::string path(PyUnicode_AsUTF8(path_obj));
    bool wc=(path.find("->")!=std::string::npos);
    if(!wc && (path.find('.')!=std::string::npos || path=="?")){
        for(auto& seg:split_dot_chunk(path)) if(seg=="__pass_key__"){ wc=true; break; }
    }
    bool table_landing = wc && strcmp(ret,"rows")==0;

    PyObject* fo=PyList_New(1);
    if(!fo) return nullptr;
    Py_INCREF(path_obj);
    PyList_SET_ITEM(fo,0,path_obj);
    PyObject* result=select_core(s,fo,ret);
    Py_DECREF(fo);
    if(!result) return nullptr;

    if(strcmp(ret,"values")==0){
        // result is [col] — a single inner list for the one requested field
        PyObject* inner=PyList_GET_ITEM(result,0);
        Py_INCREF(inner);
        Py_DECREF(result);
        return inner;
    }
    if(table_landing) return result;  // nested table-landing shape — nothing to flatten

    // rows/rows_here: result is a ModDict wrapper of {pk: {label: value}}
    // single-field rows — flatten to {pk: value}; the field name is already
    // redundant with the call (select("age")), no need to repeat it per row.
    ModDictObject* rw=(ModDictObject*)result;
    PyObject* flat=PyDict_New();
    if(!flat){ Py_DECREF(result); return nullptr; }
    std::string label=default_select_label(path);
    for(uint64_t oh : rw->internal->order){
        const OuterEntry* e=rw->internal->outer.find(oh);
        if(!e || !e->val_py) continue;
        PyObject* v=PyDict_GetItemString(e->val_py,label.c_str());
        PyObject* k=e->key_py?e->key_py:Py_None;
        if(PyDict_SetItem(flat,k,v)!=0){ Py_DECREF(flat); Py_DECREF(result); return nullptr; }
    }
    Py_DECREF(result);
    return flat;
}
static PyObject* ModDict_sort_by(ModDictObject* s,PyObject* args,PyObject* kw){
    MOD_DICT_NO_CURSOR(s, "sort_by()");  // cursor's own set_sort()/sort_index supersede this
    const char* field=nullptr; int rev=0; const char* ret="rows"; int inplace=0;
    static const char* kwl[]={"field","reverse","returns","inplace",nullptr};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"s|psp",const_cast<char**>(kwl),&field,&rev,&ret,&inplace)) return nullptr;
    auto sorted=s->internal->sort_by(std::string(field),(bool)rev); if(PyErr_Occurred()) return nullptr;
    if(inplace){
        if(strcmp(ret,"rows")!=0){
            PyErr_SetString(PyExc_ValueError,"inplace=True cannot be combined with returns — sort modifies the collection in-place and returns None");
            for(auto* k:sorted) Py_DECREF(k);
            return nullptr;
        }
        s->internal->order.clear();
        s->internal->order.reserve(sorted.size());
        for(PyObject* k : sorted){
            s->internal->order.push_back(content_hash_pyobj(k));
            Py_DECREF(k);
        }
        Py_RETURN_NONE;
    }
    bool want_keys  = strcmp(ret,"parent_keys")==0;
    bool want_vals  = strcmp(ret,"values")==0;
    PyObject* list=PyList_New((Py_ssize_t)sorted.size()); if(!list) return nullptr;
    for(size_t i=0;i<sorted.size();i++){
        PyObject* key=sorted[i];
        if(want_keys){
            PyList_SET_ITEM(list,(Py_ssize_t)i,key);
        } else {
            uint64_t oh=content_hash_pyobj(key);
            auto* e=s->internal->outer.find(oh);
            if(want_vals){
                // return the sorted field value, not the row
                PyObject* fv=nullptr;
                if(e && e->val_py && e->is_row){
                    fv=resolve_field_path(e->val_py, std::string(field));
                }
                if(fv){Py_INCREF(fv);}else{fv=Py_None;Py_INCREF(fv);}
                Py_DECREF(key);
                PyList_SET_ITEM(list,(Py_ssize_t)i,fv);
            } else {
                // "rows" — return row (with RowProxy if index active)
                PyObject* val=(e && e->val_py)?(Py_INCREF(e->val_py),e->val_py):(Py_INCREF(Py_None),Py_None);
                Py_DECREF(key);
                PyList_SET_ITEM(list,(Py_ssize_t)i,val);
            }
        }
    }
    return list;
}


static PyObject* ModDict_reindex(ModDictObject* s, PyObject* args) {
    MOD_DICT_NO_CURSOR(s, "reindex()");  // ambiguous on a cursor (key relative to root or to the anchor?) — undefined for now
    PyObject* key;
    if (!PyArg_ParseTuple(args,"O",&key)) return nullptr;
    s->internal->reindex_row(content_hash_pyobj(key));
    if (PyErr_Occurred()) return nullptr;  // reindex_row may raise (link validation)
    Py_RETURN_NONE;
}

static PyObject* ModDict_at(ModDictObject* s, PyObject* args){
    Py_ssize_t i; if(!PyArg_ParseTuple(args,"n",&i)) return nullptr;
    if (s->internal->root) {
        PyObject* d = s->internal->resolve_cursor_dict();
        if (!d) return nullptr;
        if (s->internal->filter_predicate) {
            auto& order = s->internal->visible_index;
            Py_ssize_t n = (Py_ssize_t)order.size();
            Py_ssize_t idx = i; if (idx < 0) idx += n;
            if (idx < 0 || idx >= n) { PyErr_SetString(PyExc_IndexError,"index out of range"); return nullptr; }
            PyObject* v = PyDict_GetItem(d, order[idx]);  // borrowed
            if (!v) { PyErr_SetString(PyExc_IndexError,"index out of range"); return nullptr; }
            Py_INCREF(v);
            return v;
        }
        if (s->internal->has_derived_order) {
            auto& order = s->internal->sort_index;
            Py_ssize_t n = (Py_ssize_t)order.size();
            Py_ssize_t idx = i; if (idx < 0) idx += n;
            if (idx < 0 || idx >= n) { PyErr_SetString(PyExc_IndexError,"index out of range"); return nullptr; }
            PyObject* v = PyDict_GetItem(d, order[idx]);  // borrowed
            if (!v) { PyErr_SetString(PyExc_IndexError,"index out of range"); return nullptr; }
            Py_INCREF(v);
            return v;
        }
        PyObject* keys = PyDict_Keys(d);
        if (!keys) return nullptr;
        Py_ssize_t n = PyList_GET_SIZE(keys);
        Py_ssize_t idx = i; if (idx < 0) idx += n;
        if (idx < 0 || idx >= n) { Py_DECREF(keys); PyErr_SetString(PyExc_IndexError,"index out of range"); return nullptr; }
        PyObject* v = PyDict_GetItem(d, PyList_GET_ITEM(keys, idx));  // borrowed
        Py_DECREF(keys);
        if (!v) { PyErr_SetString(PyExc_IndexError,"index out of range"); return nullptr; }
        Py_INCREF(v);
        return v;
    }
    uint64_t oh;
    if(!s->internal->at(i,oh)){ PyErr_SetString(PyExc_IndexError,"index out of range"); return nullptr; }
    const OuterEntry* e=s->internal->outer.find(oh);
    if(!e){ PyErr_SetString(PyExc_IndexError,"index out of range"); return nullptr; }
    if(e->is_row) return s->internal->get_row(oh);
    PyObject* v=e->val_py?e->val_py:Py_None; Py_INCREF(v); return v;
}

static PyObject* ModDict_copy(ModDictObject* s, PyObject*){
    MOD_DICT_NO_CURSOR(s, "copy()");
    ModDict* c = s->internal->deep_copy();
    if(!c){ PyErr_NoMemory(); return nullptr; }
    return ModDict_wrap_owned(c);
}

static PyObject* ModDict_pop(ModDictObject* s, PyObject* args){
    MOD_DICT_NO_CURSOR(s, "pop()");
    PyObject* key; PyObject* def = nullptr;
    if(!PyArg_ParseTuple(args,"O|O",&key,&def)) return nullptr;
    PyObject* val = ModDict_getitem(s, key);
    if(!val){
        if(!PyErr_ExceptionMatches(PyExc_KeyError)) return nullptr;
        PyErr_Clear();
        if(def){ Py_INCREF(def); return def; }
        PyErr_SetObject(PyExc_KeyError, key);
        return nullptr;
    }
    ModValue mk = ModValue::from_pyobject(key);
    s->internal->remove(mk);
    return val;  // already Py_INCREF'd by ModDict_getitem
}

/* method table */
static PyMethodDef ModDict_methods[]={
    {"get",(PyCFunction)ModDict_get,METH_VARARGS,"Get value by key with default"},
{"keys",(PyCFunction)ModDict_keys,METH_NOARGS,"keys()->list"},
    {"values",(PyCFunction)ModDict_values,METH_NOARGS,"values()->list"},
    {"items",(PyCFunction)ModDict_items,METH_NOARGS,"items()->list"},
    {"update",(PyCFunction)ModDict_update,METH_VARARGS|METH_KEYWORDS,"update(target,from_path=None,to_path=None,conflict='keep_right')->int|None"},
    {"filter",(PyCFunction)ModDict_filter,METH_VARARGS,"filter(field)->FilterBuilder"},
    {"_filter",(PyCFunction)ModDict_filter_legacy,METH_VARARGS|METH_KEYWORDS,"_filter(on,op,value)->ModDict"},
    {"create_index",(PyCFunction)ModDict_create_index,METH_VARARGS,"create_index(field)->None"},
    {"drop_index",(PyCFunction)ModDict_drop_index,METH_VARARGS,"drop_index(field)->None"},
    {"has_index",(PyCFunction)ModDict_has_index,METH_VARARGS,"has_index(field)->bool"},
    {"link",(PyCFunction)(PyCFunctionWithKeywords)ModDict_link,METH_VARARGS|METH_KEYWORDS,
     "link(source_path,references_path,on_delete='restrict')->None — declare a relationship"},
    {"follow",(PyCFunction)(PyCFunctionWithKeywords)ModDict_follow,METH_VARARGS|METH_KEYWORDS,
     "follow(source_path,keys=None,values=None)->ModDict — traverse a declared link"},
    {"sort_by",(PyCFunction)ModDict_sort_by,METH_VARARGS|METH_KEYWORDS,"sort_by(field,reverse=False,returns='rows',inplace=False)->list|None"},
    {"select",(PyCFunction)ModDict_select,METH_VARARGS|METH_KEYWORDS,"select(path,returns='rows')->dict|list|ModDict — single field; flattened {pk:value}/[value,...], no per-row field-name wrapper"},
    {"select_mass",(PyCFunction)ModDict_select_mass,METH_VARARGS|METH_KEYWORDS,"select_mass(fields,returns='rows')->ModDict|list[list] — multiple fields, one column/sub-dict per field"},
    {"group_by",(PyCFunction)ModDict_group_by,METH_VARARGS,"group_by(field)->dict"},
    {"cursor",(PyCFunction)ModDict_cursor,METH_VARARGS,"cursor(path)->ModDict — live handle anchored at an existing nested table; path must already exist"},
    {"set_sort",(PyCFunction)(PyCFunctionWithKeywords)ModDict_set_sort,METH_VARARGS|METH_KEYWORDS,"set_sort(field,reverse=False)->list[(old_index|None,new_index)] — cursor only"},
    {"set_group",(PyCFunction)ModDict_set_group,METH_VARARGS,"set_group(field_or_None)->list[(old_index|None,new_index)] — cursor only"},
    {"set_filter",(PyCFunction)ModDict_set_filter,METH_VARARGS,"set_filter(predicate_or_None)->list[(old_index|None,new_index)] — cursor only"},
    {"connect",(PyCFunction)ModDict_connect,METH_VARARGS,"connect(event_type,callback)->None — cursor only; events: insert/update/delete/reorder"},
    {"insert",(PyCFunction)ModDict_cursor_insert,METH_VARARGS,"insert(key,row)->(int|None,dict) — (new_index, row); cursor only"},
    {"update_row",(PyCFunction)ModDict_cursor_update_row,METH_VARARGS,"update_row(key,changes)->((old_index|None,new_index|None),changes) — changes: {field:new_value} for fields that actually changed; cursor only"},
    {"delete",(PyCFunction)ModDict_cursor_delete,METH_VARARGS,"delete(key)->int|None — old_index; cursor only"},
    {"insert_batch",(PyCFunction)ModDict_cursor_insert_batch,METH_VARARGS|METH_KEYWORDS,"insert_batch(rows,key=None)->list[(int|None,dict)] — rows: dict[key,row], or list[dict] with key= naming the field to extract each row's key from; (new_index, row) per row, in rows' iteration order; cursor only; fires one 'insert' event for the whole batch"},
    {"view_keys",(PyCFunction)ModDict_view_keys,METH_NOARGS,"view_keys()->list — keys in the cursor's current sort/filter view, same order as __iter__/.at(); cursor only"},
    {"view_values",(PyCFunction)ModDict_view_values,METH_NOARGS,"view_values()->list[dict] — rows in the cursor's current sort/filter view; cursor only"},
    {"view_items",(PyCFunction)ModDict_view_items,METH_NOARGS,"view_items()->list[(key,dict)] — (key,row) pairs in the cursor's current sort/filter view; cursor only"},
    {"from_dict",(PyCFunction)ModDict_from_dict,METH_O|METH_CLASS,"from_dict(d)->ModDict"},
    {"from_json",(PyCFunction)ModDict_from_json,METH_O|METH_CLASS,"from_json(s)->ModDict"},
    {"from_rows",(PyCFunction)ModDict_from_rows,METH_VARARGS|METH_KEYWORDS|METH_CLASS,"from_rows(rows,key)->ModDict"},
    {"from_row",(PyCFunction)ModDict_from_row,METH_O|METH_CLASS,"from_row(row)->dict"},
    {"load_rows",(PyCFunction)ModDict_load_rows,METH_VARARGS|METH_KEYWORDS,"load_rows(rows,key,path)->None — writes self[path]={row[key]:row for row in rows}"},
    {"to_dict",(PyCFunction)ModDict_to_dict,METH_NOARGS,"to_dict()->dict — shallow copy as plain dict, bypasses RowProxy"},
    {"serialize",(PyCFunction)ModDict_serialize,METH_NOARGS,"serialize()->bytes"},
    {"deserialize",(PyCFunction)ModDict_deserialize,METH_VARARGS,"deserialize(data)->ModDict — mutates self in place, returns self for chaining"},
    {"reindex",(PyCFunction)ModDict_reindex,METH_VARARGS,"reindex(key)->None — rebuild field indices for one row after deep nested write"},
    {"copy",(PyCFunction)ModDict_copy,METH_NOARGS,"copy()->ModDict — deep copy"},
    {"pop",(PyCFunction)ModDict_pop,METH_VARARGS,"pop(key[,default])->value — remove and return value"},
    {"at",(PyCFunction)ModDict_at,METH_VARARGS,"at(i)->value — get row by insertion-order index (negative = from end)"},
    {NULL,NULL,0,NULL}};

static PyMappingMethods ModDict_mapping={
    .mp_length=(lenfunc)ModDict_len,
    .mp_subscript=(binaryfunc)ModDict_getitem,
    .mp_ass_subscript=(objobjargproc)ModDict_setitem};
static PySequenceMethods ModDict_sequence={.sq_contains=(objobjproc)ModDict_contains};
PyTypeObject ModDict_Type={
    .tp_name="mod_dict.ModDict",.tp_basicsize=sizeof(ModDictObject),
    .tp_dealloc=(destructor)ModDict_dealloc,.tp_repr=(reprfunc)ModDict_repr,
    .tp_as_sequence=&ModDict_sequence,.tp_as_mapping=&ModDict_mapping,
    .tp_str=(reprfunc)ModDict_repr,
    .tp_flags=Py_TPFLAGS_DEFAULT|Py_TPFLAGS_BASETYPE,
    .tp_doc="ModDict — nested dictionary with indexed field queries (C++ backed).",
    .tp_weaklistoffset=offsetof(ModDictObject,weakreflist),
    .tp_iter=(getiterfunc)ModDict_iter,.tp_methods=ModDict_methods,
    .tp_init=(initproc)ModDict_init,.tp_new=ModDict_new};
