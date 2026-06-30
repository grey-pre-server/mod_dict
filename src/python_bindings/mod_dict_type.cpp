#include "../core/mod_dict.h"
#include "../core/field_index.h"
#include "converter_registry.h"
#include "error_handling.h"
#include <string>
#include <vector>
#include <cstring>
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
};

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

PyObject* ModDict_wrap(ModDict* internal) {
    if (!internal) Py_RETURN_NONE;
    if (internal->py_wrapper) { Py_INCREF(internal->py_wrapper); return (PyObject*)internal->py_wrapper; }
    ModDictObject* w = PyObject_New(ModDictObject,&ModDict_Type);
    if (!w) return nullptr;
    w->internal=internal; w->owns_internal=false; w->parent_ref=nullptr;
    internal->py_wrapper=w;
    return (PyObject*)w;
}

/* new/dealloc/init */
static PyObject* ModDict_new(PyTypeObject* type,PyObject*,PyObject*) {
    ModDictObject* s=(ModDictObject*)type->tp_alloc(type,0);
    MOD_DICT_CHECK_ALLOC(s);
    s->internal=new ModDict(); MOD_DICT_CHECK_ALLOC(s->internal);
    s->internal->py_wrapper=s; s->owns_internal=true; s->parent_ref=nullptr;
    return (PyObject*)s;
}
static void ModDict_dealloc(ModDictObject* s) {
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

static PyMethodDef RowProxy_methods[] = {
    {"update",(PyCFunction)(PyCFunctionWithKeywords)RowProxy_update,METH_VARARGS|METH_KEYWORDS,""},
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
static PyObject* RowProxy_create(ModDictObject* owner, PyObject* row, uint64_t oh) {
    RowProxyObject* p = PyObject_New(RowProxyObject, &RowProxy_Type);
    if (!p) return nullptr;
    Py_INCREF(row); Py_INCREF(owner);
    p->row = row; p->owner = owner; p->outer_hash = oh;
    return (PyObject*)p;
}
static void RowProxy_dealloc(RowProxyObject* s) {
    Py_XDECREF(s->row); Py_XDECREF(s->owner);
    Py_TYPE(s)->tp_free((PyObject*)s);
}
static PyObject* RowProxy_getitem(RowProxyObject* s, PyObject* key) {
    return PyObject_GetItem(s->row, key);
}
static int RowProxy_setitem(RowProxyObject* s, PyObject* key, PyObject* value) {
    int r = value ? PyObject_SetItem(s->row, key, value)
                  : PyObject_DelItem(s->row, key);
    if (r == 0 && s->owner->internal)
        s->owner->internal->reindex_row(s->outer_hash);
    return r;
}
static PyObject* RowProxy_update(RowProxyObject* s, PyObject* args, PyObject* kwargs) {
    PyObject* fn = PyObject_GetAttrString(s->row, "update");
    if (!fn) return nullptr;
    PyObject* res = PyObject_Call(fn, args, kwargs);
    Py_DECREF(fn);
    if (res && s->owner->internal)
        s->owner->internal->reindex_row(s->outer_hash);
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
    uint64_t oh=content_hash_pyobj(key);
    auto* e=s->internal->outer.find(oh);
    if(!e){PyErr_SetObject(PyExc_KeyError,key);return nullptr;}
    // alias: resolve to original entry
    if(!e->val_py){
        const uint64_t* orig_p = s->internal->alias_to_orig.find(oh);
        if(!orig_p){PyErr_SetObject(PyExc_KeyError,key);return nullptr;}
        OuterEntry* orig=s->internal->outer.find(*orig_p);
        if(!orig||!orig->val_py){PyErr_SetObject(PyExc_KeyError,key);return nullptr;}
        if(orig->is_row && !s->internal->indices.by_field.empty())
            return RowProxy_create(s, orig->val_py, *orig_p);
        Py_INCREF(orig->val_py); return orig->val_py;
    }
    if(!e->val_py) Py_RETURN_NONE;
    if(e->is_row && !s->internal->indices.by_field.empty())
        return RowProxy_create(s, e->val_py, oh);
    Py_INCREF(e->val_py); return e->val_py;
}
static int ModDict_setitem(ModDictObject* s,PyObject* key,PyObject* value) {
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
    return s->internal->outer.find(content_hash_pyobj(key)) ? 1 : 0;
}
static Py_ssize_t ModDict_len(ModDictObject* s){return (Py_ssize_t)s->internal->len();}

static PyObject* ModDict_repr(ModDictObject* s){
    PyObject* d=s->internal->to_python_dict(); if(!d) return nullptr;
    PyObject* r=PyObject_Repr(d); Py_DECREF(d); return r;
}
static PyObject* ModDict_get(ModDictObject* s,PyObject* args){
    PyObject* key; PyObject* def=Py_None;
    if(!PyArg_ParseTuple(args,"O|O",&key,&def)) return nullptr;
    uint64_t oh=content_hash_pyobj(key);
    auto* e=s->internal->outer.find(oh);
    if(!e){Py_INCREF(def);return def;}
    if(!e->val_py) Py_RETURN_NONE;
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
        if(raw.find('.')!=std::string::npos){size_t pos=0;while(true){size_t d=raw.find('.',pos);st.push_back(tr(raw.substr(pos,d==std::string::npos?d:d-pos)));if(d==std::string::npos)break;pos=d+1;}}
        else st.push_back(tr(raw));
    } else if(PyTuple_Check(obj)||PyList_Check(obj)){
        Py_ssize_t n=PySequence_Size(obj);
        for(Py_ssize_t i=0;i<n;i++){PyObject* it=PySequence_GetItem(obj,i);st.push_back(tr(PyUnicode_AsUTF8(it)));Py_DECREF(it);}
    }
    std::vector<const char*> r; for(size_t i=base;i<st.size();i++) r.push_back(st[i].c_str()); return r;
}
static PyObject* ModDict_update(ModDictObject* s,PyObject* args,PyObject* kwargs){
    PyObject *to; PyObject *os=nullptr,*ot=nullptr; const char* cs="keep_right";
    static const char* kw[]={"target","from_path","to_path","conflict",NULL};
    if(!PyArg_ParseTupleAndKeywords(args,kwargs,"O|OOs",(char**)kw,&to,&os,&ot,&cs)) return nullptr;

    // Simple mode: mn.update(d) — plain key/value bulk insert, like dict.update()
    if(!os && !ot){
        ModDict* tmp=nullptr; ModDict* src=nullptr;
        if(PyObject_TypeCheck(to,&ModDict_Type)) src=((ModDictObject*)to)->internal;
        else { tmp=pyobj_to_moddict_temp(to); if(!tmp) return nullptr; src=tmp; }
        for(auto& e : src->outer.occupied()){
            if(!e.value.key_py) continue;
            ModValue mk=ModValue::from_pyobject(e.value.key_py);
            if(e.value.is_row && e.value.val_py) s->internal->insert_row(mk,e.value.val_py);
            else if(e.value.val_py){ ModValue mv=ModValue::from_pyobject(e.value.val_py); s->internal->insert(mk,mv); }
        }
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
static bool parse_field_or_pattern(PyObject* arg,std::string& simple,std::vector<std::string>& pattern,bool& wc){
    if(PyUnicode_Check(arg)){
        std::string raw=PyUnicode_AsUTF8(arg);
        if(raw.find('.')!=std::string::npos || raw=="?"){
            size_t pos=0; while(true){size_t d=raw.find('.',pos);std::string seg=raw.substr(pos,d==std::string::npos?d:d-pos);pattern.push_back(seg=="?"?"__pass_key__":seg);if(d==std::string::npos)break;pos=d+1;}
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

static PyObject* apply_filter(ModDictObject* owner,const std::string& simple,const std::vector<std::string>& pattern,bool wc,FilterOp op,PyObject* val_obj){
    ModValue fv=ModValue::from_pyobject(val_obj);
    ModDict* result=wc?owner->internal->filter(pattern,op,fv):owner->internal->filter(simple,op,fv);
    MOD_DICT_CHECK_ALLOC(result);
    ModDictObject* w=PyObject_New(ModDictObject,&ModDict_Type);
    if(!w){delete result;return nullptr;}
    w->internal=result; w->owns_internal=true; w->parent_ref=(PyObject*)owner; Py_INCREF(owner);
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
    int c = v.compare(fval);
    switch(op){
        case FilterOp::EQ: return c==0; case FilterOp::NE: return c!=0;
        case FilterOp::LT: return c<0;  case FilterOp::LE: return c<=0;
        case FilterOp::GT: return c>0;  case FilterOp::GE: return c>=0;
        default: return false;
    }
}
static void scan_here(PyObject* cur, const std::vector<std::string>& pat, size_t depth,
                      FilterOp op, const ModValue& fval,
                      bool want_values, PyObject* vf_key, PyObject* result) {
    if (!cur || !PyDict_Check(cur) || depth >= pat.size()) return;
    bool last = (depth == pat.size()-1);
    if (pat[depth] == "__pass_key__") {
        PyObject *k,*v; Py_ssize_t pos=0;
        while (PyDict_Next(cur,&pos,&k,&v)) {
            if (last) {
                // Terminal ? checks the KEY itself
                if (!fh_compare(k,op,fval)) continue;
                PyObject* item = want_values ? (vf_key ? PyDict_GetItem(cur,vf_key) : nullptr) : cur;
                if (item) { Py_INCREF(item); PyList_Append(result,item); Py_DECREF(item); }
            } else if (PyDict_Check(v)) {
                scan_here(v,pat,depth+1,op,fval,want_values,vf_key,result);
            }
        }
    } else {
        PyObject* child = PyDict_GetItemString(cur,pat[depth].c_str());
        if (!child) return;
        if (last) {
            if (!fh_compare(child,op,fval)) return;
            PyObject* item = want_values ? (vf_key ? PyDict_GetItem(cur,vf_key) : nullptr) : cur;
            if (item) { Py_INCREF(item); PyList_Append(result,item); Py_DECREF(item); }
        } else {
            scan_here(child,pat,depth+1,op,fval,want_values,vf_key,result);
        }
    }
}
static PyObject* apply_filter_here(ModDictObject* owner,
                                    const std::string& simple,
                                    const std::vector<std::string>& pattern,
                                    bool wc, FilterOp op, const ModValue& fval,
                                    bool want_values, PyObject* vf_key) {
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
    if (anchored && anchor_e) {
        scan_here(anchor_e->val_py,pat,1,op,fval,want_values,vf_key,result);
    } else {
        for (auto& e : owner->internal->outer.occupied()) {
            if (!e.value.is_row || !e.value.val_py) continue;
            scan_here(e.value.val_py,pat,0,op,fval,want_values,vf_key,result);
        }
    }
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
        // collect GE candidates as rows_here, then filter LE
        PyObject* ge=apply_filter_here(s->owner,simple,pat,wc,FilterOp::GE,lo_val,false,nullptr);
        if (!ge) return nullptr;
        PyObject* result=PyList_New(0); if(!result){Py_DECREF(ge);return nullptr;}
        std::vector<std::string> p=wc?pat:std::vector<std::string>{simple};
        Py_ssize_t n=PyList_GET_SIZE(ge);
        for (Py_ssize_t i=0;i<n;i++) {
            PyObject* row=PyList_GET_ITEM(ge,i);
            // extract field value and check <= hi
            PyObject* fv_obj2=nullptr;
            if (!p.empty() && p.back()!="__pass_key__")
                fv_obj2=PyDict_GetItemString(row,p.back().c_str());
            bool pass=true;
            if (fv_obj2){ModValue fv=ModValue::from_pyobject(fv_obj2);pass=(fv.compare(hi_val)<=0);}
            if (pass) {
                PyObject* item=want_values?(vf_obj?PyDict_GetItem(row,vf_obj):nullptr):row;
                if(item){Py_INCREF(item);PyList_Append(result,item);Py_DECREF(item);}
            }
        }
        Py_DECREF(ge); return result;
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
        ModDict* part=wc?s->owner->internal->filter(pat,FilterOp::EQ,mv):s->owner->internal->filter(simple,FilterOp::EQ,mv);
        if(!part){delete merged;return nullptr;}
        for(auto& e:part->outer.occupied()) if(!merged->outer.find(e.key)){Py_XINCREF(e.value.key_py);Py_XINCREF(e.value.val_py);merged->outer.insert(e.key,{e.value.key_py,e.value.val_py,e.value.is_row});}
        delete part;
    }
    ModDictObject* w=PyObject_New(ModDictObject,&ModDict_Type); if(!w){delete merged;return nullptr;}
    w->internal=merged;w->owns_internal=true;w->parent_ref=(PyObject*)s->owner;Py_INCREF(s->owner);merged->py_wrapper=w;
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
    {std::vector<std::string> tmp; if(!parse_field_or_pattern(fo,simple,tmp,wc)){PyErr_SetString(PyExc_TypeError,"filter: field must be str or tuple");return nullptr;}if(wc)pat=new std::vector<std::string>(std::move(tmp));}
    FilterBuilderObject* fb=(FilterBuilderObject*)FilterBuilder_Type.tp_alloc(&FilterBuilder_Type,0);
    if(!fb){delete pat;return nullptr;}
    fb->owner=owner;Py_INCREF(owner);fb->field=portable_strdup(simple.c_str());fb->pattern=pat;
    return (PyObject*)fb;
}

static PyObject* ModDict_filter(ModDictObject* s,PyObject* args){
    PyObject* fo; if(!PyArg_ParseTuple(args,"O",&fo)) return nullptr;
    return FilterBuilder_new_obj(s,fo);
}
static PyObject* ModDict_filter_legacy(ModDictObject* s,PyObject* args,PyObject* kw){
    PyObject *on,*val; const char* op_s; static const char* kwl[]={"on","op","value",NULL};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"OsO",(char**)kwl,&on,&op_s,&val)) return nullptr;
    std::string simple; std::vector<std::string> pattern; bool wc;
    if(!parse_field_or_pattern(on,simple,pattern,wc)) MOD_DICT_RAISE(PyExc_TypeError,"on must be str or tuple");
    FilterOp op=parse_op(op_s); if((int)op==-1) MOD_DICT_RAISE(PyExc_ValueError,"op must be ==,!=,<,<=,>,>=");
    return apply_filter(s,simple,pattern,wc,op,val);
}

/* Iterator */
struct ModDictIterObject{PyObject_HEAD ModDictObject* owner; size_t position;};
static void ModDictIter_dealloc(ModDictIterObject* s){Py_XDECREF(s->owner);Py_TYPE(s)->tp_free((PyObject*)s);}
static PyObject* ModDictIter_next(ModDictIterObject* s){
    const auto& outer = s->owner->internal->outer;
    while(s->position < outer.capacity()){
        const auto* slot = outer.begin() + s->position++;
        if(slot->occupied && slot->value.val_py){  // val_py==nullptr means alias entry
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
static PyObject* ModDict_iter(ModDictObject* s){
    ModDictIterObject* it=PyObject_New(ModDictIterObject,&ModDictIter_Type);
    if(!it){PyErr_NoMemory();return nullptr;}
    it->owner=s; Py_INCREF(s); it->position=0;
    return (PyObject*)it;
}

static PyObject* ModDict_keys(ModDictObject* s,PyObject*){
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
static PyObject* ModDict_aliases(ModDictObject* s,PyObject*){
    PyObject* d=PyDict_New(); if(!d) return nullptr;
    for(auto& e:s->internal->alias_to_orig.occupied()){
        OuterEntry* ae  = s->internal->outer.find(e.key);
        OuterEntry* orig= s->internal->outer.find(e.value);
        PyObject* ak = ae   && ae->key_py   ? ae->key_py   : Py_None;
        PyObject* ok = orig && orig->key_py ? orig->key_py : Py_None;
        PyDict_SetItem(d, ak, ok);
    }
    return d;
}

/* Serialization */
static PyObject* ModDict_serialize(ModDictObject* s,PyObject*){
    std::vector<uint8_t> data=s->internal->serialize(); if(PyErr_Occurred()) return nullptr;
    return PyBytes_FromStringAndSize((const char*)data.data(),(Py_ssize_t)data.size());
}
static PyObject* ModDict_deserialize(ModDictObject* s,PyObject* args){
    const char* data; Py_ssize_t len;
    if(!PyArg_ParseTuple(args,"y#",&data,&len)) return nullptr;
    s->internal->deserialize((const uint8_t*)data,(size_t)len);
    if(PyErr_Occurred()) return nullptr;
    Py_INCREF(s); return (PyObject*)s;
}

/* Index */
static PyObject* ModDict_create_index(ModDictObject* s,PyObject* args){
    PyObject* a; if(!PyArg_ParseTuple(args,"O",&a)) return nullptr;
    std::string simple; std::vector<std::string> pat; bool wc;
    if(!parse_field_or_pattern(a,simple,pat,wc)){PyErr_SetString(PyExc_TypeError,"create_index: str or tuple required");return nullptr;}
    if(wc) s->internal->create_index(pat); else s->internal->create_index(simple);
    Py_RETURN_NONE;
}
static PyObject* ModDict_drop_index(ModDictObject* s,PyObject* args){
    PyObject* a; if(!PyArg_ParseTuple(args,"O",&a)) return nullptr;
    std::string simple; std::vector<std::string> pat; bool wc;
    if(!parse_field_or_pattern(a,simple,pat,wc)){PyErr_SetString(PyExc_TypeError,"drop_index: str or tuple required");return nullptr;}
    if(wc) s->internal->drop_index(pat); else s->internal->drop_index(simple);
    Py_RETURN_NONE;
}
static PyObject* ModDict_has_index(ModDictObject* s,PyObject* args){
    PyObject* a; if(!PyArg_ParseTuple(args,"O",&a)) return nullptr;
    std::string simple; std::vector<std::string> pat; bool wc;
    if(!parse_field_or_pattern(a,simple,pat,wc)){PyErr_SetString(PyExc_TypeError,"has_index: str or tuple required");return nullptr;}
    return PyBool_FromLong(wc?s->internal->has_index(pat):s->internal->has_index(simple));
}

/* from_dict/from_json */
static PyObject* ModDict_from_dict(PyObject* cls,PyObject* arg){
    if(!PyDict_Check(arg)){PyErr_SetString(PyExc_TypeError,"argument must be a dict");return nullptr;}
    ModDictObject* s=(ModDictObject*)((PyTypeObject*)cls)->tp_alloc((PyTypeObject*)cls,0);
    if(!s) return nullptr;
    s->internal=new ModDict(); s->owns_internal=true; s->internal->py_wrapper=s; s->parent_ref=nullptr;
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
    s->internal=new ModDict(); s->owns_internal=true; s->internal->py_wrapper=s; s->parent_ref=nullptr;
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
    s->internal=new ModDict(); s->owns_internal=true; s->internal->py_wrapper=s; s->parent_ref=nullptr;

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

/* group_by/select/sort_by */
static PyObject* ModDict_group_by(ModDictObject* s,PyObject* args){
    const char* field; if(!PyArg_ParseTuple(args,"s",&field)) return nullptr;
    auto groups=s->internal->group_by(std::string(field)); if(PyErr_Occurred()) return nullptr;
    PyObject* result=PyDict_New(); if(!result) return nullptr;
    for(auto& [fv,gd]:groups){
        PyObject* key=fv.to_pyobject(); if(!key){Py_DECREF(result);return nullptr;}
        ModDictObject* w=PyObject_New(ModDictObject,&ModDict_Type);
        if(!w){Py_DECREF(key);Py_DECREF(result);return nullptr;}
        w->internal=gd;w->owns_internal=true;w->parent_ref=(PyObject*)s;Py_INCREF(s);gd->py_wrapper=w;
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

static PyObject* ModDict_select(ModDictObject* s,PyObject* args,PyObject* kw){
    PyObject* fo; const char* ret="rows";
    static const char* kwl[]={"fields","returns",nullptr};
    if(!PyArg_ParseTupleAndKeywords(args,kw,"O|s",const_cast<char**>(kwl),&fo,&ret)) return nullptr;
    if(!PyList_Check(fo)) MOD_DICT_RAISE(PyExc_TypeError,"fields must be a list of strings");
    std::vector<std::string> fields; Py_ssize_t n=PyList_Size(fo); fields.reserve((size_t)n);
    for(Py_ssize_t i=0;i<n;i++){PyObject* it=PyList_GET_ITEM(fo,i);if(!PyUnicode_Check(it))MOD_DICT_RAISE(PyExc_TypeError,"fields must be a list of strings");fields.emplace_back(PyUnicode_AsUTF8(it));}
    ModDict* result=s->internal->select(fields); MOD_DICT_CHECK_ALLOC(result);
    if(strcmp(ret,"values")==0){
        // return list of projected row dicts in insertion order
        PyObject* lst=PyList_New(0);
        for(auto& e : result->outer.occupied()){
            if(e.value.val_py){ Py_INCREF(e.value.val_py); PyList_Append(lst,e.value.val_py); Py_DECREF(e.value.val_py); }
        }
        delete result; return lst;
    }
    return ModDict_wrap(result);
}
static PyObject* ModDict_sort_by(ModDictObject* s,PyObject* args,PyObject* kw){
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
    PyObject* key;
    if (!PyArg_ParseTuple(args,"O",&key)) return nullptr;
    s->internal->reindex_row(content_hash_pyobj(key));
    Py_RETURN_NONE;
}

static PyObject* ModDict_alias(ModDictObject* s, PyObject* args) {
    PyObject* key_obj; PyObject* alias_obj;
    if (!PyArg_ParseTuple(args, "OO", &key_obj, &alias_obj)) return nullptr;

    uint64_t orig_hash = content_hash_pyobj(key_obj);
    OuterEntry* orig = s->internal->outer.find(orig_hash);
    if (!orig) {
        PyErr_SetObject(PyExc_KeyError, key_obj);
        return nullptr;
    }

    uint64_t alias_hash = content_hash_pyobj(alias_obj);
    OuterEntry* existing = s->internal->outer.find(alias_hash);
    if (existing) {
        PyErr_SetString(PyExc_KeyError, "alias key already exists");
        return nullptr;
    }

    if (s->internal->orig_to_alias.find(orig_hash)) {
        PyErr_SetString(PyExc_KeyError, "key already has an alias");
        return nullptr;
    }

    Py_INCREF(alias_obj);
    // alias entry: val_py = nullptr (discriminant), resolved via side tables at access time
    OuterEntry ae;
    ae.key_py = alias_obj;
    ae.val_py = nullptr;
    ae.is_row = false;
    s->internal->outer.insert(alias_hash, ae);
    s->internal->alias_to_orig.insert(alias_hash, orig_hash);
    s->internal->orig_to_alias.insert(orig_hash, alias_hash);

    Py_RETURN_NONE;
}

static PyObject* ModDict_at(ModDictObject* s, PyObject* args){
    Py_ssize_t i; if(!PyArg_ParseTuple(args,"n",&i)) return nullptr;
    uint64_t oh;
    if(!s->internal->at(i,oh)){ PyErr_SetString(PyExc_IndexError,"index out of range"); return nullptr; }
    const OuterEntry* e=s->internal->outer.find(oh);
    if(!e){ PyErr_SetString(PyExc_IndexError,"index out of range"); return nullptr; }
    if(e->is_row) return s->internal->get_row(oh);
    PyObject* v=e->val_py?e->val_py:Py_None; Py_INCREF(v); return v;
}

static PyObject* ModDict_copy(ModDictObject* s, PyObject*){
    ModDict* c = s->internal->deep_copy();
    if(!c){ PyErr_NoMemory(); return nullptr; }
    return ModDict_wrap(c);
}

static PyObject* ModDict_pop(ModDictObject* s, PyObject* args){
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
    {"sort_by",(PyCFunction)ModDict_sort_by,METH_VARARGS|METH_KEYWORDS,"sort_by(field,reverse=False,returns='rows',inplace=False)->list|None"},
    {"select",(PyCFunction)ModDict_select,METH_VARARGS|METH_KEYWORDS,"select(fields,returns='rows')->ModDict|list"},
    {"group_by",(PyCFunction)ModDict_group_by,METH_VARARGS,"group_by(field)->dict"},
    {"from_dict",(PyCFunction)ModDict_from_dict,METH_O|METH_CLASS,"from_dict(d)->ModDict"},
    {"from_json",(PyCFunction)ModDict_from_json,METH_O|METH_CLASS,"from_json(s)->ModDict"},
    {"from_rows",(PyCFunction)ModDict_from_rows,METH_VARARGS|METH_KEYWORDS|METH_CLASS,"from_rows(rows,key)->ModDict"},
    {"from_row",(PyCFunction)ModDict_from_row,METH_O|METH_CLASS,"from_row(row)->dict"},
    {"serialize",(PyCFunction)ModDict_serialize,METH_NOARGS,"serialize()->bytes"},
    {"deserialize",(PyCFunction)ModDict_deserialize,METH_VARARGS,"deserialize(data)->None"},
    {"reindex",(PyCFunction)ModDict_reindex,METH_VARARGS,"reindex(key)->None — rebuild field indices for one row after deep nested write"},
    {"alias",(PyCFunction)ModDict_alias,METH_VARARGS,"alias(key,alias)->None — create transparent alias for an existing key"},
    {"aliases",(PyCFunction)ModDict_aliases,METH_NOARGS,"aliases()->dict — {alias_key: original_key} mapping"},
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
    .tp_iter=(getiterfunc)ModDict_iter,.tp_methods=ModDict_methods,
    .tp_init=(initproc)ModDict_init,.tp_new=ModDict_new};
