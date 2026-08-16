#include "serializer.h"
#include "../mod_dict.h"
#include <cstring>
#include <vector>

// Defined in python_bindings/module.cpp - explicit WKB-wrapper classes for
// tagging raw bytes without the real shapely/geoalchemy2 library installed
// on the writing side. Their __module__ is "mod_dict", not "shapely"/
// "geoalchemy2", so they need a direct type check, not the module-name
// sniffing used for the real library objects below.
extern PyTypeObject ShapelyWKB_Type;
extern PyTypeObject GeoAlchemyWKB_Type;

namespace Serializer {

static void serialize_pyobj(std::vector<uint8_t>& buf, PyObject* obj);

// ── WKB geometry deserialize backend preference ───────────────────────────────
static std::string s_geo_backend;  // empty = unset (auto-detect)

// Process-wide cache of "which geo libraries exist, and the callable that
// reconstructs a value" — resolved ONCE, on the first reconstruct after
// (re)configuration, then reused for every subsequent value.
//
// Without this, reconstruct_wkb() re-ran PyImport_ImportModule("shapely.wkb")
// AND PyImport_ImportModule("geoalchemy2") for EVERY geometry deserialized.
// A successful import is a cheap sys.modules hit, but a FAILED one is not
// cached anywhere — CPython walks the whole sys.path finder chain and builds
// then discards an ImportError each time. In a shapely-only environment that
// meant a guaranteed-failing geoalchemy2 probe per value: measured 498us per
// point of which shapely.wkb.loads itself was 5us — 99% of deserialize time
// was probing for a library that isn't there. 10k points: 5.7s.
//
// Invalidated by set_geo_backend() (any call, including None) so a library
// installed mid-process and then selected is picked up on the next
// reconstruct rather than being stuck as "absent" forever.
struct GeoLibCache {
    bool resolved = false;
    bool has_shapely = false;
    bool has_geoalchemy = false;
    PyObject* shapely_loads = nullptr;   // owned: shapely.wkb.loads
    PyObject* shapely_get_srid = nullptr;// owned: shapely.get_srid (write side; may stay null on old shapely)
    PyObject* wkbelement_cls = nullptr;  // owned: geoalchemy2.WKBElement
    void clear() {
        Py_CLEAR(shapely_loads);
        Py_CLEAR(shapely_get_srid);
        Py_CLEAR(wkbelement_cls);
        resolved = has_shapely = has_geoalchemy = false;
    }
    void resolve() {
        clear();
        PyObject* sh = PyImport_ImportModule("shapely.wkb");
        if (sh) {
            shapely_loads = PyObject_GetAttrString(sh, "loads");
            has_shapely = (shapely_loads != nullptr);
            if (!has_shapely) PyErr_Clear();
            Py_DECREF(sh);
            PyObject* top = PyImport_ImportModule("shapely");
            if (top) {
                shapely_get_srid = PyObject_GetAttrString(top, "get_srid");
                if (!shapely_get_srid) PyErr_Clear();  // pre-2.0 shapely — no SRID to carry
                Py_DECREF(top);
            } else PyErr_Clear();
        } else PyErr_Clear();
        PyObject* ga = PyImport_ImportModule("geoalchemy2");
        if (ga) {
            wkbelement_cls = PyObject_GetAttrString(ga, "WKBElement");
            has_geoalchemy = (wkbelement_cls != nullptr);
            if (!has_geoalchemy) PyErr_Clear();
            Py_DECREF(ga);
        } else PyErr_Clear();
        resolved = true;
    }
};
static GeoLibCache s_geo_libs;

bool set_geo_backend(const char* name) {
    s_geo_libs.clear();  // re-probe on next reconstruct — see GeoLibCache
    if (!name) { s_geo_backend.clear(); return true; }
    // "wkb_bytes" hands back the raw WKB unparsed — needs no library, so it
    // skips the installed-check below. Without it, raw bytes were reachable
    // only BY ACCIDENT (when neither library happened to be installed), which
    // made the result depend on the environment rather than on the code.
    if (strcmp(name, "wkb_bytes") == 0) { s_geo_backend = name; return true; }
    if (strcmp(name, "shapely") != 0 && strcmp(name, "geoalchemy2") != 0) {
        PyErr_Format(PyExc_ValueError,
            "set_geo_backend: name must be 'shapely', 'geoalchemy2', or 'wkb_bytes', got '%s'", name);
        return false;
    }
    PyObject* mod = PyImport_ImportModule(name);
    if (!mod) {
        PyErr_Format(PyExc_ImportError,
            "set_geo_backend: '%s' is not installed", name);
        return false;
    }
    Py_DECREF(mod);
    s_geo_backend = name;
    return true;
}

const char* get_geo_backend() {
    return s_geo_backend.empty() ? nullptr : s_geo_backend.c_str();
}

// Reconstruct a shapely geometry or geoalchemy2 WKBElement from raw WKB bytes
// — or hand the bytes straight back under the "wkb_bytes" backend.
// Honors an explicit set_geo_backend() preference; otherwise auto-detects
// among whichever of {shapely, geoalchemy2} is importable — falls back to
// the raw bytes if neither is installed, raises if both are (ambiguous,
// caller must disambiguate via set_geo_backend()). Library presence and
// the reconstructing callables come from GeoLibCache — resolved once, not
// per value.
static PyObject* reconstruct_wkb(PyObject* wkb) {
    const char* pref = get_geo_backend();
    // Answered before touching the library cache — "wkb_bytes" deliberately
    // needs neither library, so don't even resolve whether they exist.
    if (pref && strcmp(pref, "wkb_bytes") == 0) {
        Py_INCREF(wkb);
        return wkb;
    }

    if (!s_geo_libs.resolved) s_geo_libs.resolve();
    bool has_shapely    = s_geo_libs.has_shapely;
    bool has_geoalchemy = s_geo_libs.has_geoalchemy;

    bool want_shapely;
    if (pref) {
        want_shapely = (strcmp(pref, "shapely") == 0);
        if (want_shapely && !has_shapely) {
            PyErr_SetString(PyExc_ImportError,
                "reconstruct WKB: geo backend set to 'shapely' but shapely is not installed");
            return nullptr;
        }
        if (!want_shapely && !has_geoalchemy) {
            PyErr_SetString(PyExc_ImportError,
                "reconstruct WKB: geo backend set to 'geoalchemy2' but geoalchemy2 is not installed");
            return nullptr;
        }
    } else {
        if (has_shapely && has_geoalchemy) {
            // Deliberately still an error rather than picking one: the choice
            // decides what TYPE the caller gets back, so guessing here would
            // be a silent-wrong-result source.
            PyErr_SetString(PyExc_ValueError,
                "reconstruct WKB: both shapely and geoalchemy2 are installed - pick one "
                "explicitly: md.set_geo_backend(\"shapely\") -> shapely geometry, "
                "md.set_geo_backend(\"geoalchemy2\") -> WKBElement, or "
                "md.set_geo_backend(\"wkb_bytes\") -> the raw WKB bytes, unparsed");
            return nullptr;
        }
        if (!has_shapely && !has_geoalchemy) {
            Py_INCREF(wkb);
            return wkb;  // neither installed - hand back the raw WKB bytes, no data loss
        }
        want_shapely = has_shapely;
    }

    // Direct call on the cached callable — no import, no attribute lookup.
    return want_shapely ? PyObject_CallOneArg(s_geo_libs.shapely_loads, wkb)
                        : PyObject_CallOneArg(s_geo_libs.wkbelement_cls, wkb);
}

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

// Writes one TypeId::WKB record from a WKB-carrying object (bytes, or a
// memoryview as geoalchemy2's WKBElement.data often is). Raises (PyErr set,
// nothing written) if the object can't be viewed as bytes — never the old
// "swallow the error and write None" path, which turned a geometry into a
// silent None.
//
// SRID: geoalchemy2/shapely keep the SRID as an ATTRIBUTE unless the bytes
// are already EWKB (extended=True, what PostGIS returns) — plain WKB bytes
// don't carry it, so rebuilding from raw bytes alone silently reset it to
// -1. Rather than adding an SRID field to the container format (a format
// change), fold it into the bytes: upgrade plain WKB to EWKB in-stream by
// setting the SRID flag in the geometry-type word and inserting the 4-byte
// SRID after it. Both geoalchemy2 (WKBElement autodetects the flag) and
// shapely (wkb.loads reads it) reconstruct SRID from EWKB unaided. Bytes
// that are already EWKB pass through untouched.
static const uint32_t EWKB_SRID_FLAG = 0x20000000u;

static void write_wkb_value(std::vector<uint8_t>& buf, PyObject* wkb_obj, long srid) {
    PyObject* b = PyBytes_Check(wkb_obj) ? (Py_INCREF(wkb_obj), wkb_obj) : PyObject_Bytes(wkb_obj);
    if (!b) return;  // PyErr set
    const uint8_t* p = (const uint8_t*)PyBytes_AS_STRING(b);
    size_t len = (size_t)PyBytes_GET_SIZE(b);

    // Only touch the header when we have an SRID to add and the bytes are a
    // well-formed WKB header (1 byte order + 4 byte type) without the flag.
    bool need_srid = (srid >= 0) && len >= 5;
    uint32_t gtype = 0;
    bool little = false;
    if (need_srid) {
        little = (p[0] == 1);
        gtype = little ? (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24)
                       : (uint32_t)p[4] | ((uint32_t)p[3] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 24);
        if (gtype & EWKB_SRID_FLAG) need_srid = false;  // already EWKB — leave as is
    }

    buf.push_back(to_byte(TypeId::WKB));
    if (!need_srid) {
        write_u32(buf, (uint32_t)len);
        write_bytes(buf, p, len);
    } else {
        write_u32(buf, (uint32_t)(len + 4));
        buf.push_back(p[0]);
        uint32_t t = gtype | EWKB_SRID_FLAG;
        uint32_t s = (uint32_t)srid;
        auto put = [&](uint32_t v) {
            if (little) { buf.push_back(v & 0xFF); buf.push_back((v >> 8) & 0xFF); buf.push_back((v >> 16) & 0xFF); buf.push_back((v >> 24) & 0xFF); }
            else        { buf.push_back((v >> 24) & 0xFF); buf.push_back((v >> 16) & 0xFF); buf.push_back((v >> 8) & 0xFF); buf.push_back(v & 0xFF); }
        };
        put(t);
        put(s);
        write_bytes(buf, p + 5, len - 5);
    }
    Py_DECREF(b);
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

    if (PyByteArray_Check(obj)) {
        Py_ssize_t len = PyByteArray_GET_SIZE(obj);
        buf.push_back(to_byte(TypeId::BYTEARRAY));
        write_u32(buf, (uint32_t)len);
        write_bytes(buf, (const uint8_t*)PyByteArray_AS_STRING(obj), (size_t)len);
        return;
    }

    if (PyList_Check(obj)) {
        buf.push_back(to_byte(TypeId::LIST));
        size_t lp = buf.size(); write_u32(buf, 0);
        size_t cp = buf.size(); write_u32(buf, 0);
        uint32_t count = (uint32_t)PyList_GET_SIZE(obj);
        for (uint32_t i = 0; i < count; i++) {
            serialize_pyobj(buf, PyList_GET_ITEM(obj, i));
            if (PyErr_Occurred()) return;
        }
        write_u32_at(buf, cp, count);
        backfill_length(buf, lp);
        return;
    }

    if (PyTuple_Check(obj)) {
        buf.push_back(to_byte(TypeId::TUPLE));
        size_t lp = buf.size(); write_u32(buf, 0);
        size_t cp = buf.size(); write_u32(buf, 0);
        uint32_t count = (uint32_t)PyTuple_GET_SIZE(obj);
        for (uint32_t i = 0; i < count; i++) {
            serialize_pyobj(buf, PyTuple_GET_ITEM(obj, i));
            if (PyErr_Occurred()) return;
        }
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
            if (PyErr_Occurred()) return;
            serialize_pyobj(buf, v);
            if (PyErr_Occurred()) return;
            count++;
        }
        write_u32_at(buf, cp, count);
        backfill_length(buf, lp);
        return;
    }

    if (PyObject_TypeCheck(obj, &ShapelyWKB_Type) || PyObject_TypeCheck(obj, &GeoAlchemyWKB_Type)) {
        const char* attr = PyObject_TypeCheck(obj, &ShapelyWKB_Type) ? "wkb" : "data";
        PyObject* b = PyObject_GetAttrString(obj, attr);
        if (!b) return;  // PyErr set — fail loud, same as any unsupported type
        write_wkb_value(buf, b, -1);  // our own wrappers carry no SRID attribute
        Py_DECREF(b);
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
                if (PyErr_Occurred()) break;
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

        if (strcmp(mname, "uuid") == 0) {
            Py_DECREF(tp_mod);
            PyObject* sv = PyObject_Str(obj);
            Py_ssize_t len = 0; const char* s = nullptr;
            if (sv) s = PyUnicode_AsUTF8AndSize(sv, &len);
            if (!s) { PyErr_Clear(); s = ""; len = 0; }
            buf.push_back(to_byte(TypeId::UUID));
            write_u32(buf, (uint32_t)len);
            write_bytes(buf, (const uint8_t*)s, (size_t)len);
            Py_XDECREF(sv);
            return;
        }

        if (strncmp(mname, "shapely", 7) == 0 || strncmp(mname, "geoalchemy2", 11) == 0) {
            bool is_shapely = strncmp(mname, "shapely", 7) == 0;
            Py_DECREF(tp_mod);
            PyObject* wkb;
            long srid = -1;
            if (is_shapely) {
                // .wkb is PLAIN WKB — shapely 2.x keeps the SRID out of it
                // (no .srid attribute either; only shapely.get_srid()), so
                // it was silently dropped on every round-trip. Read it via
                // get_srid and fold it into EWKB like the geoalchemy branch.
                wkb = PyObject_GetAttrString(obj, "wkb");
                // get_srid comes from the same once-resolved cache the read
                // side uses — no per-value import/attribute lookup.
                if (!s_geo_libs.resolved) s_geo_libs.resolve();
                PyObject* s = s_geo_libs.shapely_get_srid
                            ? PyObject_CallOneArg(s_geo_libs.shapely_get_srid, obj) : nullptr;
                if (s) {
                    // get_srid returns a numpy scalar (numpy.intc), NOT a
                    // Python int — PyLong_Check rejects it. PyNumber_Long
                    // coerces anything with __index__/__int__.
                    PyObject* as_long = PyNumber_Long(s);
                    if (as_long) { srid = PyLong_AsLong(as_long); Py_DECREF(as_long); }
                    else PyErr_Clear();
                    Py_DECREF(s);
                }
                else PyErr_Clear();  // no get_srid (pre-2.0 shapely) or it raised — no SRID to carry
                if (srid == 0) srid = -1;  // shapely's "unset" is 0, not -1 — don't tag SRID=0
            } else {
                // WKBElement: `.data` is the WKB (bytes OR memoryview);
                // `.desc` — what this read before — is the HEX STRING
                // representation, so PyObject_Bytes(desc) raised TypeError,
                // which was then swallowed and the geometry written as None.
                // A geoalchemy2 geometry serialized to None, silently.
                wkb = PyObject_GetAttrString(obj, "data");
                PyObject* s = PyObject_GetAttrString(obj, "srid");
                if (s) {
                    PyObject* as_long = PyNumber_Long(s);  // tolerate numpy ints here too
                    if (as_long) { srid = PyLong_AsLong(as_long); Py_DECREF(as_long); }
                    else PyErr_Clear();
                    Py_DECREF(s);
                }
                else PyErr_Clear();
            }
            if (!wkb) return;  // PyErr set — fail loud
            write_wkb_value(buf, wkb, srid);
            Py_DECREF(wkb);
            return;
        }

        Py_DECREF(tp_mod);
    }

fallback_none:
    {
        PyObject* tp_name = PyObject_GetAttrString((PyObject*)Py_TYPE(obj), "__qualname__");
        const char* tn = tp_name ? PyUnicode_AsUTF8(tp_name) : nullptr;
        PyErr_Format(PyExc_TypeError,
            "cannot serialize value of type '%s' - register a converter via "
            "md.register_converter(type, callable) or convert it to a supported "
            "type first",
            tn ? tn : Py_TYPE(obj)->tp_name);
        Py_XDECREF(tp_name);
    }
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

        case TypeId::WKB: {
            PyObject* wkb = PyBytes_FromStringAndSize((const char*)ptr, (Py_ssize_t)length);
            result = wkb ? reconstruct_wkb(wkb) : nullptr;
            Py_XDECREF(wkb);
            // Note: unlike the other cases here, a null result may carry a real
            // pending exception (ambiguous backend, missing preferred library) -
            // don't clear it, so it propagates instead of silently becoming None.
            break;
        }

        case TypeId::UUID: {
            PyObject* uuid_mod = PyImport_ImportModule("uuid");
            PyObject* uuid_cls = uuid_mod ? PyObject_GetAttrString(uuid_mod, "UUID") : nullptr;
            PyObject* sv = PyUnicode_FromStringAndSize((const char*)ptr, (Py_ssize_t)length);
            result = (uuid_cls && sv) ? PyObject_CallOneArg(uuid_cls, sv) : nullptr;
            Py_XDECREF(sv); Py_XDECREF(uuid_cls); Py_XDECREF(uuid_mod);
            if (!result) PyErr_Clear();
            break;
        }

        case TypeId::BYTEARRAY:
            result = PyByteArray_FromStringAndSize((const char*)ptr, (Py_ssize_t)length);
            break;

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

        case TypeId::TUPLE: {
            if (length < 4) break;
            uint32_t count = read_u32(ptr);
            const uint8_t* col_end = data_end;
            result = PyTuple_New((Py_ssize_t)count);
            if (!result) { PyErr_Clear(); break; }
            for (uint32_t i = 0; i < count && ptr < col_end; i++) {
                ModValue mv = deserialize_value(ptr, col_end, nullptr);
                PyObject* item = mv.obj ? mv.obj : Py_None;
                Py_INCREF(item);
                PyTuple_SET_ITEM(result, i, item);
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

    // A null result usually just means "couldn't reconstruct, fall back to
    // None" (e.g. optional lib missing for a best-effort case above) - but if
    // it comes with a pending exception (e.g. ambiguous WKB backend), that's a
    // real error: leave result null and don't clear it, so callers checking
    // PyErr_Occurred() (or mv.obj == nullptr) see the failure instead of a
    // silently-substituted None.
    if (!result && !PyErr_Occurred()) {
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
