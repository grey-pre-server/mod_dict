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
    if (PyBytes_Check(obj))
        return fnv1a64(PyBytes_AS_STRING(obj), (size_t)PyBytes_GET_SIZE(obj));
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

PyObject* ModValue::to_pyobject() const {
    PyObject* o = obj ? obj : Py_None;
    Py_INCREF(o);
    return o;
}

bool ModValue::equals(const ModValue& other) const {
    if (hash_val != other.hash_val) return false;
    if (obj == other.obj) return true;
    if (type == ValueType::INT && other.type == ValueType::INT)
        return PyLong_AsLongLong(obj) == PyLong_AsLongLong(other.obj);
    if ((type == ValueType::INT || type == ValueType::FLOAT) &&
        (other.type == ValueType::INT || other.type == ValueType::FLOAT)) {
        double a = (type == ValueType::FLOAT) ? PyFloat_AsDouble(obj)       : (double)PyLong_AsLongLong(obj);
        double b = (other.type == ValueType::FLOAT) ? PyFloat_AsDouble(other.obj) : (double)PyLong_AsLongLong(other.obj);
        return a == b;
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
        long long a = PyLong_AsLongLong(obj);
        long long b = PyLong_AsLongLong(other.obj);
        return (a < b) ? -1 : (a > b) ? 1 : 0;
    }
    if ((type == ValueType::INT || type == ValueType::FLOAT) &&
        (other.type == ValueType::INT || other.type == ValueType::FLOAT)) {
        double a = (type == ValueType::FLOAT) ? PyFloat_AsDouble(obj)       : (double)PyLong_AsLongLong(obj);
        double b = (other.type == ValueType::FLOAT) ? PyFloat_AsDouble(other.obj) : (double)PyLong_AsLongLong(other.obj);
        return (a < b) ? -1 : (a > b) ? 1 : 0;
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
