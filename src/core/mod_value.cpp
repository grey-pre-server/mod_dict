#include "mod_value.h"
#include <cstring>
#include <cstdint>

// fnv1a64 is defined inline in mod_value.h

// ── Stable content hash ──────────────────────────────────────────────────────

uint64_t content_hash_pyobj(PyObject* obj) {
    if (!obj || obj == Py_None) return 0;
    if (PyBool_Check(obj))  return (obj == Py_True) ? 1ULL : 0ULL;
    if (PyLong_Check(obj)) {
        long long v = PyLong_AsLongLong(obj);
        if (v == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            PyObject* s = PyObject_Str(obj);
            if (!s) return 0;
            Py_ssize_t len; const char* sc = PyUnicode_AsUTF8AndSize(s, &len);
            uint64_t h = sc ? fnv1a64(sc, (size_t)len) : 0;
            Py_DECREF(s); return h;
        }
        return (uint64_t)v;
    }
    if (PyFloat_Check(obj)) {
        double v = PyFloat_AsDouble(obj);
        uint64_t bits; memcpy(&bits, &v, sizeof(bits)); return bits;
    }
    if (PyUnicode_Check(obj)) {
        // Fast path for compact ASCII strings (interned keys, short identifiers).
        // Accesses internal CPython buffer directly — avoids PyUnicode_AsUTF8AndSize call.
        if (PyUnicode_IS_COMPACT_ASCII(obj)) {
            Py_ssize_t len = ((PyASCIIObject*)obj)->length;
            const char* s  = (const char*)(((PyASCIIObject*)obj) + 1);
            return fnv1a64(s, (size_t)len);
        }
        Py_ssize_t len; const char* s = PyUnicode_AsUTF8AndSize(obj, &len);
        if (!s) { PyErr_Clear(); return 0; }
        return fnv1a64(s, (size_t)len);
    }
    if (PyBytes_Check(obj)) {
        // Salted so b"abc" and "abc" hash DIFFERENTLY: both branches feed the
        // same octets to fnv1a64, and FlatHashMap trusts the hash alone — so
        // without the salt, a str key and a bytes key with identical content
        // silently overwrote each other (same silent-data-loss class as the
        // (1,2)-vs-"(1, 2)" repr collision fixed alongside). Hashes are never
        // serialized (recomputed on load), so changing this is internal-only.
        uint64_t h = fnv1a64(PyBytes_AS_STRING(obj), (size_t)PyBytes_GET_SIZE(obj));
        return h ^ 0x62797465735F5F5FULL;  // "bytes___" — type tag
    }
    // Composite keys (e.g. a (region_id, item_id) tuple mirroring a DB's
    // composite primary key) — fold each element's own stable content hash
    // together (recursive, so nested tuples of primitives stay just as
    // stable as a lone string/int would be) instead of falling through to
    // the repr()-based fallback below. repr() on a tuple builds a formatted
    // Python string (recursing through repr() on every element, allocating
    // a new PyUnicode) just to hash THAT — measured ~3x slower on insert for
    // a typical (int, str) composite key than this direct combine.
    if (PyTuple_Check(obj)) {
        Py_ssize_t n = PyTuple_GET_SIZE(obj);
        uint64_t h = 14695981039346656037ULL;  // fnv1a64's own offset basis
        for (Py_ssize_t i = 0; i < n; i++) {
            uint64_t eh = content_hash_pyobj(PyTuple_GET_ITEM(obj, i));  // borrowed
            h = (h ^ eh) * 1099511628211ULL;  // fold, same prime fnv1a64 uses
        }
        return h;
    }
    // fallback: stable repr hash
    PyObject* r = PyObject_Repr(obj);
    if (!r) { PyErr_Clear(); return 0; }
    Py_ssize_t len; const char* s = PyUnicode_AsUTF8AndSize(r, &len);
    uint64_t h = s ? fnv1a64(s, (size_t)len) : 0;
    if (!s) PyErr_Clear();
    Py_DECREF(r); return h;
}

// ── Type detection ───────────────────────────────────────────────────────────

static ValueType detect_type(PyObject* obj) {
    if (!obj || obj == Py_None) return ValueType::NONE;
    if (PyBool_Check(obj))      return ValueType::BOOL;
    if (PyLong_Check(obj))      return ValueType::INT;
    if (PyFloat_Check(obj))     return ValueType::FLOAT;
    if (PyUnicode_Check(obj))   return ValueType::STRING;
    if (PyBytes_Check(obj))     return ValueType::BYTES;
    if (PyList_Check(obj))      return ValueType::LIST;
    if (PySet_Check(obj))       return ValueType::SET;
    if (PyFrozenSet_Check(obj)) return ValueType::FROZENSET;
    if (PyDict_Check(obj))      return ValueType::DICT;

    PyObject* tp_mod = PyObject_GetAttrString((PyObject*)Py_TYPE(obj), "__module__");
    if (!tp_mod) { PyErr_Clear(); return ValueType::PYOBJECT; }
    const char* mname = PyUnicode_AsUTF8(tp_mod);
    if (!mname) { Py_DECREF(tp_mod); PyErr_Clear(); return ValueType::PYOBJECT; }

    ValueType vt = ValueType::PYOBJECT;

    if (strcmp(mname, "datetime") == 0) {
        PyObject* tname = PyObject_GetAttrString((PyObject*)Py_TYPE(obj), "__name__");
        const char* n = tname ? PyUnicode_AsUTF8(tname) : nullptr;
        if (n) {
            if      (strcmp(n, "datetime") == 0) vt = ValueType::DATETIME;
            else if (strcmp(n, "date")     == 0) vt = ValueType::DATE;
            else if (strcmp(n, "time")     == 0) vt = ValueType::TIME;
        }
        Py_XDECREF(tname);
    } else if (strcmp(mname, "pathlib") == 0) {
        PyObject* tname = PyObject_GetAttrString((PyObject*)Py_TYPE(obj), "__name__");
        const char* n = tname ? PyUnicode_AsUTF8(tname) : nullptr;
        if (n) {
            if      (strcmp(n, "PurePosixPath") == 0 || strcmp(n, "PosixPath") == 0)
                vt = ValueType::PATH_POSIX;
            else if (strcmp(n, "PureWindowsPath") == 0 || strcmp(n, "WindowsPath") == 0)
                vt = ValueType::PATH_WINDOWS;
            else
                vt = ValueType::PATH;
        }
        Py_XDECREF(tname);
    } else if (strcmp(mname, "decimal") == 0) {
        vt = ValueType::DECIMAL;
    } else if (strncmp(mname, "shapely", 7) == 0) {
        vt = ValueType::GEOMETRY_SHAPELY;
    } else if (strncmp(mname, "geoalchemy2", 11) == 0) {
        vt = ValueType::GEOMETRY_GEOALCHEMY;
    }

    Py_DECREF(tp_mod);
    return vt;
}

// ── ModValue methods ─────────────────────────────────────────────────────────

ModValue ModValue::from_pyobject(PyObject* obj, ElasticPool*) {
    ModValue mv;
    if (!obj) obj = Py_None;
    Py_INCREF(obj);
    mv.obj      = obj;
    mv.hash_val = content_hash_pyobj(obj);
    mv.type     = detect_type(obj);
    return mv;
}

// Like from_pyobject(), but skips content_hash_pyobj() — compare() never
// reads hash_val, only equals()/hash() do. For callers that only ever call
// compare() (e.g. cursor sort/group), this avoids hashing a value nobody
// looks at (an FNV1a64 scan for strings, a repr() call for exotic types).
ModValue ModValue::from_pyobject_for_compare(PyObject* obj) {
    ModValue mv;
    if (!obj) obj = Py_None;
    Py_INCREF(obj);
    mv.obj  = obj;
    mv.type = detect_type(obj);
    return mv;
}

PyObject* ModValue::to_pyobject() const {
    PyObject* o = obj ? obj : Py_None;
    Py_INCREF(o);
    return o;
}

// int -> int64 without raising: false (and no exception left behind) when
// the value doesn't fit — the caller then falls back to Python's own rich
// comparison, which handles arbitrary precision. PyLong_AsLongLong() alone
// would set OverflowError and return -1, which the old fast paths silently
// compared as a value and leaked as an exception into an unrelated later
// call ("OverflowError: int too big to convert" out of filter().eq(2**70)).
static inline bool as_int64(PyObject* o, long long& out) {
    int overflow = 0;
    out = PyLong_AsLongLongAndOverflow(o, &overflow);
    if (overflow != 0) return false;
    if (out == -1 && PyErr_Occurred()) { PyErr_Clear(); return false; }
    return true;
}
// int/float -> double for the mixed numeric fast paths; a big int converts
// exactly enough for ordering (PyLong_AsDouble), false only beyond ~1e308.
static inline bool as_double(PyObject* o, ValueType t, double& out) {
    if (t == ValueType::FLOAT) { out = PyFloat_AsDouble(o); return true; }
    out = PyLong_AsDouble(o);
    if (out == -1.0 && PyErr_Occurred()) { PyErr_Clear(); return false; }
    return true;
}

bool ModValue::equals(const ModValue& other) const {
    if (hash_val != other.hash_val) return false;
    if (obj == other.obj) return true;
    if (type == ValueType::INT && other.type == ValueType::INT) {
        long long a, b;
        if (as_int64(obj, a) && as_int64(other.obj, b)) return a == b;
        // out of int64 range: generic path below
    } else if ((type == ValueType::INT || type == ValueType::FLOAT) &&
               (other.type == ValueType::INT || other.type == ValueType::FLOAT)) {
        double a, b;
        if (as_double(obj, type, a) && as_double(other.obj, other.type, b)) return a == b;
    }
    PyObject* a = obj      ? obj      : Py_None;
    PyObject* b = other.obj ? other.obj : Py_None;
    int r = PyObject_RichCompareBool(a, b, Py_EQ);
    if (r == -1) { PyErr_Clear(); return false; }
    return r == 1;
}

int ModValue::compare(const ModValue& other, bool* ok) const {
    if (ok) *ok = true;
    if (type == ValueType::INT && other.type == ValueType::INT) {
        long long a, b;
        if (as_int64(obj, a) && as_int64(other.obj, b))
            return (a < b) ? -1 : (a > b) ? 1 : 0;
        // out of int64 range: generic rich-compare path below
    } else if ((type == ValueType::INT || type == ValueType::FLOAT) &&
               (other.type == ValueType::INT || other.type == ValueType::FLOAT)) {
        double a, b;
        if (as_double(obj, type, a) && as_double(other.obj, other.type, b))
            return (a < b) ? -1 : (a > b) ? 1 : 0;
    }
    // One direct call instead of the two generic PyObject_RichCompareBool
    // dispatches (LT then GT) below — PyUnicode_Compare returns -1/0/1 and
    // only sets an exception on a genuine decode error (rare malformed data).
    if (type == ValueType::STRING && other.type == ValueType::STRING) {
        int c = PyUnicode_Compare(obj, other.obj);
        if (c == -1 && PyErr_Occurred()) { PyErr_Clear(); if (ok) *ok = false; return 0; }
        return (c < 0) ? -1 : (c > 0) ? 1 : 0;
    }
    PyObject* ao = obj       ? obj       : Py_None;
    PyObject* bo = other.obj ? other.obj : Py_None;
    int lt = PyObject_RichCompareBool(ao, bo, Py_LT);
    if (lt == -1) { PyErr_Clear(); if (ok) *ok = false; return 0; }
    if (lt == 1) return -1;
    int gt = PyObject_RichCompareBool(ao, bo, Py_GT);
    if (gt == -1) { PyErr_Clear(); if (ok) *ok = false; return 0; }
    if (gt == 1) return 1;
    return 0;
}
