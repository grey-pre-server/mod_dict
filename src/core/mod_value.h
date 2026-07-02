#ifndef MOD_DICT_MOD_VALUE_H
#define MOD_DICT_MOD_VALUE_H

#include <Python.h>
#include <cstdint>
#include <cstring>

inline uint64_t fnv1a64(const char* s, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 1099511628211ULL;
    return h;
}

class ModDict;
class ElasticPool;  // kept for API compat (ignored)

enum class ValueType : uint8_t {
    NONE     = 0,
    BOOL     = 1,
    INT      = 2,
    FLOAT    = 3,
    STRING   = 4,
    LIST     = 5,
    MODDICT  = 6,
    BYTES    = 7,
    PATH     = 8,
    DATETIME = 10,
    DATE     = 11,
    TIME     = 12,
    SET      = 13,
    GEOMETRY_SHAPELY    = 14,
    GEOMETRY_GEOALCHEMY = 15,
    PATH_POSIX   = 16,
    PATH_WINDOWS = 17,
    PYOBJECT     = 18,
    FROZENSET    = 19,
    DECIMAL      = 20,
    DICT         = 21,
};

// Stable content hash (not Python hash-randomized). Used in FlatHashMap keys.
uint64_t content_hash_pyobj(PyObject* obj);

// ModValue owns a PyObject* (RAII). Safe on stack and in std::vector.
// NOT safe in FlatHashMap — use OuterEntry (raw PyObject*) there instead.
struct ModValue {
    PyObject* obj      = nullptr;
    uint64_t  hash_val = 0;
    ValueType type     = ValueType::NONE;

    ModValue() = default;
    ~ModValue() { Py_XDECREF(obj); }

    ModValue(const ModValue& o) : obj(o.obj), hash_val(o.hash_val), type(o.type) {
        Py_XINCREF(obj);
    }
    ModValue(ModValue&& o) noexcept : obj(o.obj), hash_val(o.hash_val), type(o.type) {
        o.obj = nullptr;
    }
    ModValue& operator=(const ModValue& o) {
        if (this != &o) { Py_XDECREF(obj); obj=o.obj; hash_val=o.hash_val; type=o.type; Py_XINCREF(obj); }
        return *this;
    }
    ModValue& operator=(ModValue&& o) noexcept {
        if (this != &o) { Py_XDECREF(obj); obj=o.obj; hash_val=o.hash_val; type=o.type; o.obj=nullptr; }
        return *this;
    }

    // pool param kept for backward compat with callsites — ignored.
    static ModValue from_pyobject(PyObject* obj, ElasticPool* pool = nullptr);
    PyObject* to_pyobject() const;
    void destroy(ElasticPool* pool = nullptr) {
        Py_XDECREF(obj); obj = nullptr; hash_val = 0; type = ValueType::NONE;
    }

    uint64_t hash() const { return hash_val; }
    bool equals(const ModValue& other) const;

    // Returns -1/0/1 for less/equal/greater. For non-numeric types whose
    // Python objects don't support rich comparison with each other (e.g.
    // None vs int in an unnormalized dataset), the underlying TypeError is
    // cleared and *ok is set to false — callers must treat a not-comparable
    // pair as excluded from any <,<=,>,>= predicate rather than trust the
    // returned 0. Pass ok=nullptr only when comparability is already
    // guaranteed by the caller (e.g. both operands confirmed numeric).
    int compare(const ModValue& other, bool* ok = nullptr) const;
};

#endif
