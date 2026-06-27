#include "serializer.h"
#include "../mod_dict.h"
#include <cstring>
#include <vector>

namespace Serializer {

static void serialize_pyobj(std::vector<uint8_t>& buf, PyObject* obj);

static void backfill_length(std::vector<uint8_t>& buf, size_t len_pos) {
    uint32_t len = (uint32_t)(buf.size() - len_pos - 4);
    buf[len_pos]   =  len        & 0xFF;
    buf[len_pos+1] = (len >>  8) & 0xFF;
    buf[len_pos+2] = (len >> 16) & 0xFF;
    buf[len_pos+3] = (len >> 24) & 0xFF;
}

static void write_u32_at(std::vector<uint8_t>& buf, size_t pos, uint32_t val) {
    buf[pos]   =  val        & 0xFF;
    buf[pos+1] = (val >>  8) & 0xFF;
    buf[pos+2] = (val >> 16) & 0xFF;
    buf[pos+3] = (val >> 24) & 0xFF;
}

/* ============================================================================
   serialize_pyobj — direct PyObject* serialization without ModValue overhead.
   No Py_INCREF/Py_DECREF, no content_hash_pyobj, no PyObject_Repr for dicts.
   ============================================================================ */

static void serialize_pyobj(std::vector<uint8_t>& buf, PyObject* obj) {
    if (!obj || obj == Py_None) {
        buf.push_back(to_byte(TypeId::NONE));
        write_u32(buf, 0);
        return;
    }

    // Order matters: bool before int (PyBool_Check also passes PyLong_Check)
    if (PyBool_Check(obj)) {
        buf.push_back(to_byte(TypeId::BOOL));
        write_u32(buf, 1);
        buf.push_back(obj == Py_True ? 1 : 0);
        return;
    }

    if (PyLong_Check(obj)) {
        int64_t v = PyLong_AsLongLong(obj);
        if (v == -1 && PyErr_Occurred()) { PyErr_Clear(); v = 0; }
        buf.push_back(to_byte(TypeId::INT));
        write_u32(buf, 8);
        write_i64(buf, v);
        return;
    }

    if (PyFloat_Check(obj)) {
        double v = PyFloat_AsDouble(obj);
        uint64_t bits; memcpy(&bits, &v, 8);
        buf.push_back(to_byte(TypeId::FLOAT));
        write_u32(buf, 8);
        write_u64(buf, bits);
        return;
    }

    if (PyUnicode_Check(obj)) {
        Py_ssize_t len; const char* s = PyUnicode_AsUTF8AndSize(obj, &len);
        if (!s) { PyErr_Clear(); s = ""; len = 0; }
        buf.push_back(to_byte(TypeId::STRING));
        write_u32(buf, (uint32_t)len);
        write_bytes(buf, (const uint8_t*)s, (size_t)len);
        return;
    }

    if (PyBytes_Check(obj)) {
        Py_ssize_t len = PyBytes_GET_SIZE(obj);
        buf.push_back(to_byte(TypeId::BYTES));
        write_u32(buf, (uint32_t)len);
        write_bytes(buf, (const uint8_t*)PyBytes_AS_STRING(obj), (size_t)len);
        return;
    }

    if (PyList_Check(obj)) {
        buf.push_back(to_byte(TypeId::LIST));
        size_t lp = buf.size(); write_u32(buf, 0);
        size_t cp = buf.size(); write_u32(buf, 0);
        uint32_t count = (uint32_t)PyList_GET_SIZE(obj);
        for (uint32_t i = 0; i < count; i++)
            serialize_pyobj(buf, PyList_GET_ITEM(obj, i));
        write_u32_at(buf, cp, count);
        backfill_length(buf, lp);
        return;
    }

    if (PyDict_Check(obj)) {
        buf.push_back(to_byte(TypeId::MODDICT));
        size_t lp = buf.size(); write_u32(buf, 0);
        size_t cp = buf.size(); write_u32(buf, 0);
        uint32_t count = 0;
        PyObject *k, *v; Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &k, &v)) {
            serialize_pyobj(buf, k);
            serialize_pyobj(buf, v);
            count++;
        }
        write_u32_at(buf, cp, count);
        backfill_length(buf, lp);
        return;
    }

    if (PySet_Check(obj) || PyFrozenSet_Check(obj)) {
        TypeId tid = PyFrozenSet_Check(obj) ? TypeId::FROZENSET : TypeId::SET;
        buf.push_back(to_byte(tid));
        size_t lp = buf.size(); write_u32(buf, 0);
        size_t cp = buf.size(); write_u32(buf, 0);
        uint32_t count = 0;
        PyObject* it = PyObject_GetIter(obj);
        if (it) {
            PyObject* item;
            while ((item = PyIter_Next(it))) {
                serialize_pyobj(buf, item);
                Py_DECREF(item);
                count++;
            }
            Py_DECREF(it);
        } else PyErr_Clear();
        write_u32_at(buf, cp, count);
        backfill_length(buf, lp);
        return;
    }

    // Check module-based types (datetime, pathlib, decimal, shapely, etc.)
    PyObject* tp_mod = PyObject_GetAttrString((PyObject*)Py_TYPE(obj), "__module__");
    if (!tp_mod) { PyErr_Clear(); goto fallback_none; }
    {
        const char* mname = PyUnicode_AsUTF8(tp_mod);
        if (!mname) { Py_DECREF(tp_mod); PyErr_Clear(); goto fallback_none; }

        if (strcmp(mname, "datetime") == 0) {
            PyObject* tname = PyObject_GetAttrString((PyObject*)Py_TYPE(obj), "__name__");
            const char* n = tname ? PyUnicode_AsUTF8(tname) : nullptr;
            bool is_dt   = n && strcmp(n, "datetime") == 0;
            bool is_date = n && strcmp(n, "date")     == 0;
            bool is_time = n && strcmp(n, "time")     == 0;
            Py_XDECREF(tname);
            Py_DECREF(tp_mod);

            if (is_dt) {
                PyObject* ts = PyObject_CallMethod(obj, "timestamp", nullptr);
                int64_t us = ts ? (int64_t)(PyFloat_AsDouble(ts) * 1e6) : 0;
                Py_XDECREF(ts);
                if (PyErr_Occurred()) PyErr_Clear();
                buf.push_back(to_byte(TypeId::DATETIME)); write_u32(buf, 8); write_i64(buf, us);
            } else if (is_date) {
                PyObject* ord = PyObject_CallMethod(obj, "toordinal", nullptr);
                int32_t v = ord ? (int32_t)(PyLong_AsLong(ord) - 719163) : 0;
                Py_XDECREF(ord);
                if (PyErr_Occurred()) PyErr_Clear();
                buf.push_back(to_byte(TypeId::DATE)); write_u32(buf, 4); write_i32(buf, v);
            } else if (is_time) {
                PyObject* h  = PyObject_GetAttrString(obj, "hour");
                PyObject* m  = PyObject_GetAttrString(obj, "minute");
                PyObject* s  = PyObject_GetAttrString(obj, "second");
                PyObject* us = PyObject_GetAttrString(obj, "microsecond");
                uint64_t val = 0;
                if (h && m && s && us)
                    val = (uint64_t)( (long long)PyLong_AsLong(h)  * 3600000000LL
                                    + (long long)PyLong_AsLong(m)  *   60000000LL
                                    + (long long)PyLong_AsLong(s)  *    1000000LL
                                    + (long long)PyLong_AsLong(us) );
                Py_XDECREF(h); Py_XDECREF(m); Py_XDECREF(s); Py_XDECREF(us);
                if (PyErr_Occurred()) PyErr_Clear();
                buf.push_back(to_byte(TypeId::TIME)); write_u32(buf, 8); write_u64(buf, val);
            } else {
                buf.push_back(to_byte(TypeId::NONE)); write_u32(buf, 0);
            }
            return;
        }

        if (strcmp(mname, "pathlib") == 0) {
            PyObject* tname = PyObject_GetAttrString((PyObject*)Py_TYPE(obj), "__name__");
            const char* n = tname ? PyUnicode_AsUTF8(tname) : nullptr;
            TypeId tid = TypeId::PATH;
            if (n) {
                if      (strcmp(n, "PurePosixPath") == 0 || strcmp(n, "PosixPath") == 0)
                    tid = TypeId::PATH_POSIX;
                else if (strcmp(n, "PureWindowsPath") == 0 || strcmp(n, "WindowsPath") == 0)
                    tid = TypeId::PATH_WINDOWS;
            }
            Py_XDECREF(tname);
            Py_DECREF(tp_mod);
            PyObject* sv = PyObject_Str(obj);
            Py_ssize_t len = 0; const char* s = nullptr;
            if (sv) s = PyUnicode_AsUTF8AndSize(sv, &len);
            if (!s) { PyErr_Clear(); s = ""; len = 0; }
            buf.push_back(to_byte(tid));
            write_u32(buf, (uint32_t)len);
            write_bytes(buf, (const uint8_t*)s, (size_t)len);
            Py_XDECREF(sv);
            return;
        }

        if (strcmp(mname, "decimal") == 0) {
            Py_DECREF(tp_mod);
            PyObject* sv = PyObject_Str(obj);
            Py_ssize_t len = 0; const char* s = nullptr;
            if (sv) s = PyUnicode_AsUTF8AndSize(sv, &len);
            if (!s) { PyErr_Clear(); s = ""; len = 0; }
            buf.push_back(to_byte(TypeId::DECIMAL));
            write_u32(buf, (uint32_t)len);
            write_bytes(buf, (const uint8_t*)s, (size_t)len);
            Py_XDECREF(sv);
            return;
        }

        if (strncmp(mname, "shapely", 7) == 0) {
            Py_DECREF(tp_mod);
            PyObject* wkb = PyObject_GetAttrString(obj, "wkb");
            if (wkb && PyBytes_Check(wkb)) {
                Py_ssize_t len = PyBytes_GET_SIZE(wkb);
                buf.push_back(to_byte(TypeId::GEOMETRY_SHAPELY));
                write_u32(buf, (uint32_t)len);
                write_bytes(buf, (const uint8_t*)PyBytes_AS_STRING(wkb), (size_t)len);
            } else {
                PyErr_Clear();
                buf.push_back(to_byte(TypeId::NONE)); write_u32(buf, 0);
            }
            Py_XDECREF(wkb);
            return;
        }

        if (strncmp(mname, "geoalchemy2", 11) == 0) {
            Py_DECREF(tp_mod);
            PyObject* desc = PyObject_GetAttrString(obj, "desc");
            PyObject* wkb = desc ? PyObject_Bytes(desc) : nullptr;
            Py_XDECREF(desc);
            if (wkb && PyBytes_Check(wkb)) {
                Py_ssize_t len = PyBytes_GET_SIZE(wkb);
                buf.push_back(to_byte(TypeId::GEOMETRY_GEOALCHEMY));
                write_u32(buf, (uint32_t)len);
                write_bytes(buf, (const uint8_t*)PyBytes_AS_STRING(wkb), (size_t)len);
            } else {
                PyErr_Clear();
                buf.push_back(to_byte(TypeId::NONE)); write_u32(buf, 0);
            }
            Py_XDECREF(wkb);
            return;
        }

        Py_DECREF(tp_mod);
    }

fallback_none:
    buf.push_back(to_byte(TypeId::NONE));
    write_u32(buf, 0);
}

/* ============================================================================
   serialize_value — thin wrapper that delegates to serialize_pyobj.
   Kept for backward compat.
   ============================================================================ */

void serialize_value(std::vector<uint8_t>& buf, const ModValue& val) {
    serialize_pyobj(buf, val.obj ? val.obj : Py_None);
}

/* ============================================================================
   deserialize_value — build PyObject*, return as ModValue.
   Does NOT call content_hash_pyobj (no PyObject_Repr overhead).
   Caller receives a ModValue with obj (refcount=1), type=NONE, hash=0.
   ============================================================================ */

ModValue deserialize_value(const uint8_t*& ptr, const uint8_t* end, ElasticPool*) {
    if (ptr + 5 > end) return ModValue();

    TypeId   tid    = from_byte(*ptr++);
    uint32_t length = read_u32(ptr);

    if (ptr + length > end) return ModValue();
    const uint8_t* data_end = ptr + length;

    PyObject* result = nullptr;

    switch (tid) {
        case TypeId::NONE:
            result = Py_None; Py_INCREF(result);
            break;

        case TypeId::BOOL:
            result = (*ptr != 0) ? Py_True : Py_False;
            Py_INCREF(result);
            break;

        case TypeId::INT:
            result = PyLong_FromLongLong(read_i64(ptr));
            break;

        case TypeId::FLOAT: {
            uint64_t bits = read_u64(ptr);
            double v; memcpy(&v, &bits, 8);
            result = PyFloat_FromDouble(v);
            break;
        }

        case TypeId::STRING:
            result = PyUnicode_FromStringAndSize((const char*)ptr, (Py_ssize_t)length);
            if (!result) { PyErr_Clear(); result = PyUnicode_FromString(""); }
            break;

        case TypeId::BYTES:
            result = PyBytes_FromStringAndSize((const char*)ptr, (Py_ssize_t)length);
            break;

        case TypeId::DATETIME: {
            int64_t us = read_i64(ptr);
            PyObject* dt_mod = PyImport_ImportModule("datetime");
            PyObject* dt_cls = dt_mod ? PyObject_GetAttrString(dt_mod, "datetime") : nullptr;
            double secs = (double)us / 1e6;
            result = dt_cls ? PyObject_CallMethod(dt_cls, "fromtimestamp", "d", secs) : nullptr;
            Py_XDECREF(dt_cls); Py_XDECREF(dt_mod);
            if (!result) PyErr_Clear();
            break;
        }

        case TypeId::DATE: {
            int32_t days = read_i32(ptr);
            PyObject* dt_mod = PyImport_ImportModule("datetime");
            PyObject* dt_cls = dt_mod ? PyObject_GetAttrString(dt_mod, "date") : nullptr;
            PyObject* ord = PyLong_FromLong(days + 719163);
            result = (dt_cls && ord) ? PyObject_CallMethod(dt_cls, "fromordinal", "O", ord) : nullptr;
            Py_XDECREF(ord); Py_XDECREF(dt_cls); Py_XDECREF(dt_mod);
            if (!result) PyErr_Clear();
            break;
        }

        case TypeId::TIME: {
            uint64_t us_total = read_u64(ptr);
            int h  = (int)(us_total / 3600000000ULL); us_total %= 3600000000ULL;
            int m  = (int)(us_total /   60000000ULL); us_total %=   60000000ULL;
            int s  = (int)(us_total /    1000000ULL); us_total %=    1000000ULL;
            int us = (int)us_total;
            PyObject* dt_mod = PyImport_ImportModule("datetime");
            PyObject* dt_cls = dt_mod ? PyObject_GetAttrString(dt_mod, "time") : nullptr;
            result = dt_cls ? PyObject_CallFunction(dt_cls, "iiii", h, m, s, us) : nullptr;
            Py_XDECREF(dt_cls); Py_XDECREF(dt_mod);
            if (!result) PyErr_Clear();
            break;
        }

        case TypeId::DECIMAL: {
            PyObject* dec_mod = PyImport_ImportModule("decimal");
            PyObject* dec_cls = dec_mod ? PyObject_GetAttrString(dec_mod, "Decimal") : nullptr;
            PyObject* sv = PyUnicode_FromStringAndSize((const char*)ptr, (Py_ssize_t)length);
            result = (dec_cls && sv) ? PyObject_CallOneArg(dec_cls, sv) : nullptr;
            Py_XDECREF(sv); Py_XDECREF(dec_cls); Py_XDECREF(dec_mod);
            if (!result) PyErr_Clear();
            break;
        }

        case TypeId::PATH:
        case TypeId::PATH_POSIX:
        case TypeId::PATH_WINDOWS: {
            const char* cls_name = (tid == TypeId::PATH_POSIX)   ? "PurePosixPath"
                                 : (tid == TypeId::PATH_WINDOWS)  ? "PureWindowsPath"
                                 :                                   "Path";
            PyObject* path_mod = PyImport_ImportModule("pathlib");
            PyObject* cls = path_mod ? PyObject_GetAttrString(path_mod, cls_name) : nullptr;
            PyObject* sv = PyUnicode_FromStringAndSize((const char*)ptr, (Py_ssize_t)length);
            result = (cls && sv) ? PyObject_CallOneArg(cls, sv) : nullptr;
            Py_XDECREF(sv); Py_XDECREF(cls); Py_XDECREF(path_mod);
            if (!result) PyErr_Clear();
            break;
        }

        case TypeId::GEOMETRY:
            result = PyBytes_FromStringAndSize((const char*)ptr, (Py_ssize_t)length);
            break;

        case TypeId::GEOMETRY_SHAPELY: {
            PyObject* wkb = PyBytes_FromStringAndSize((const char*)ptr, (Py_ssize_t)length);
            PyObject* sh  = wkb ? PyImport_ImportModule("shapely.wkb") : nullptr;
            result = sh ? PyObject_CallMethod(sh, "loads", "O", wkb) : nullptr;
            Py_XDECREF(sh); Py_XDECREF(wkb);
            if (!result) PyErr_Clear();
            break;
        }

        case TypeId::GEOMETRY_GEOALCHEMY: {
            PyObject* wkb = PyBytes_FromStringAndSize((const char*)ptr, (Py_ssize_t)length);
            PyObject* ga  = wkb ? PyImport_ImportModule("geoalchemy2") : nullptr;
            PyObject* cls = ga  ? PyObject_GetAttrString(ga, "WKBElement") : nullptr;
            result = cls ? PyObject_CallOneArg(cls, wkb) : nullptr;
            Py_XDECREF(cls); Py_XDECREF(ga); Py_XDECREF(wkb);
            if (!result) PyErr_Clear();
            break;
        }

        case TypeId::LIST:
        case TypeId::SET:
        case TypeId::FROZENSET: {
            if (length < 4) break;
            uint32_t count = read_u32(ptr);
            const uint8_t* col_end = data_end;
            result = (tid == TypeId::LIST) ? PyList_New(0) : PySet_New(nullptr);
            if (!result) { PyErr_Clear(); break; }
            for (uint32_t i = 0; i < count && ptr < col_end; i++) {
                ModValue mv = deserialize_value(ptr, col_end, nullptr);
                PyObject* item = mv.obj ? mv.obj : Py_None;
                if (tid == TypeId::LIST)
                    PyList_Append(result, item);
                else
                    PySet_Add(result, item);
            }
            if (tid == TypeId::FROZENSET) {
                PyObject* fs = PyFrozenSet_New(result);
                Py_DECREF(result);
                result = fs ? fs : Py_None;
                if (result == Py_None) Py_INCREF(result);
            }
            break;
        }

        case TypeId::MODDICT: {
            if (length < 4) break;
            uint32_t count = read_u32(ptr);
            const uint8_t* col_end = data_end;
            result = PyDict_New();
            if (!result) { PyErr_Clear(); break; }
            for (uint32_t i = 0; i < count && ptr < col_end; i++) {
                ModValue mk = deserialize_value(ptr, col_end, nullptr);
                ModValue mv = deserialize_value(ptr, col_end, nullptr);
                PyObject* k = mk.obj ? mk.obj : Py_None;
                PyObject* v = mv.obj ? mv.obj : Py_None;
                PyDict_SetItem(result, k, v);
            }
            break;
        }

        default:
            result = Py_None; Py_INCREF(result);
            break;
    }

    ptr = data_end;

    if (!result) {
        result = Py_None; Py_INCREF(result);
    }

    // Return result directly without calling from_pyobject (avoids content_hash_pyobj).
    // Callers (mod_dict deserialize, recursive calls) only need obj pointer.
    ModValue mv;
    mv.obj      = result;  // transfer ownership (refcount=1)
    mv.type     = ValueType::NONE;
    mv.hash_val = 0;
    return mv;
}

/* ============================================================================
   Stubs for interned API (not used — serialization done in mod_dict.cpp)
   ============================================================================ */

void collect_strings(const ModDict*, StringIndex&, StringTable&) {}
void collect_strings_val(const ModValue&, StringIndex&, StringTable&) {}

void serialize_value_i(std::vector<uint8_t>& buf, const ModValue& val, const StringIndex&) {
    serialize_pyobj(buf, val.obj ? val.obj : Py_None);
}
void serialize_dict_i(std::vector<uint8_t>& buf, const ModDict*, const StringIndex&) { write_u32(buf, 0); }
void serialize_dict(std::vector<uint8_t>& buf, const ModDict*) { write_u32(buf, 0); }
void deserialize_dict(const uint8_t*, size_t, ModDict*) {}

ModValue deserialize_value_i(const uint8_t*& ptr, const uint8_t* end,
                              const StringTable&, ElasticPool* pool) {
    return deserialize_value(ptr, end, pool);
}
void deserialize_dict_i(const uint8_t*&, const uint8_t*, const StringTable&, ModDict*) {}

std::vector<uint8_t> serialize_interned(const ModDict*) { return {}; }
void deserialize_interned(const uint8_t*, size_t, ModDict*) {}

// Public entry point for direct PyObject* serialization (used by mod_dict.cpp)
void serialize_pyobject(std::vector<uint8_t>& buf, PyObject* obj) {
    serialize_pyobj(buf, obj);
}

} // namespace Serializer
