#include <Python.h>
#include <structmember.h>
#include "converter_registry.h"
#include "mod_dict_binding.h"
#include "../core/mod_dict.h"
#include "../core/codecs/serializer.h"

extern PyTypeObject ModDict_Type;
extern PyTypeObject ModDictIter_Type;
extern PyTypeObject FilterBuilder_Type;
extern PyTypeObject RowProxy_Type;

/* ============================================================================
   ShapelyWKB / GeoAlchemyWKB — лёгкие обёртки для raw WKB bytes.
   Позволяют явно пометить байты как гео-тип без наличия библиотеки на стороне
   записи. serializer.cpp матчит их напрямую по типу (PyObject_TypeCheck
   против ShapelyWKB_Type/GeoAlchemyWKB_Type, объявленных extern там) —
   не через __module__/duck-typing, обёртки живут в модуле "mod_dict", не
   "shapely"/"geoalchemy2".

   md.ShapelyWKB(raw_bytes) / md.GeoAlchemyWKB(raw_bytes) → оба хранятся как
   единый TypeId::WKB (см. reconstruct_wkb() в serializer.cpp для чтения).
   ============================================================================ */

// ── ShapelyWKB ──────────────────────────────────────────────────────────────
typedef struct { PyObject_HEAD PyObject* wkb; } ShapelyWKBObject;

static int ShapelyWKB_init(ShapelyWKBObject* self, PyObject* args, PyObject*) {
    PyObject* b;
    if (!PyArg_ParseTuple(args, "O!", &PyBytes_Type, &b)) return -1;
    Py_INCREF(b);
    Py_XDECREF(self->wkb);
    self->wkb = b;
    return 0;
}
static void ShapelyWKB_dealloc(ShapelyWKBObject* self) {
    Py_XDECREF(self->wkb);
    Py_TYPE(self)->tp_free(self);
}
static PyObject* ShapelyWKB_repr(ShapelyWKBObject* self) {
    return PyUnicode_FromFormat("ShapelyWKB(<%zd bytes>)", PyBytes_GET_SIZE(self->wkb));
}
static PyMemberDef ShapelyWKB_members[] = {
    {"wkb", T_OBJECT_EX, offsetof(ShapelyWKBObject, wkb), READONLY, "WKB bytes"},
    {nullptr}
};
PyTypeObject ShapelyWKB_Type = {
    .ob_base      = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name      = "mod_dict.ShapelyWKB",
    .tp_basicsize = sizeof(ShapelyWKBObject),
    .tp_dealloc   = (destructor)ShapelyWKB_dealloc,
    .tp_repr      = (reprfunc)ShapelyWKB_repr,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "Wrap raw WKB bytes to be stored, serialized as WKB geometry data.\n"
                    "Reconstruction on the reading side follows set_geo_backend() - see its docstring.",
    .tp_members   = ShapelyWKB_members,
    .tp_init      = (initproc)ShapelyWKB_init,
    .tp_new       = PyType_GenericNew,
};

// ── GeoAlchemyWKB ────────────────────────────────────────────────────────────
typedef struct { PyObject_HEAD PyObject* data; } GeoAlchemyWKBObject;

static int GeoAlchemyWKB_init(GeoAlchemyWKBObject* self, PyObject* args, PyObject*) {
    PyObject* b;
    if (!PyArg_ParseTuple(args, "O!", &PyBytes_Type, &b)) return -1;
    Py_INCREF(b);
    Py_XDECREF(self->data);
    self->data = b;
    return 0;
}
static void GeoAlchemyWKB_dealloc(GeoAlchemyWKBObject* self) {
    Py_XDECREF(self->data);
    Py_TYPE(self)->tp_free(self);
}
static PyObject* GeoAlchemyWKB_repr(GeoAlchemyWKBObject* self) {
    return PyUnicode_FromFormat("GeoAlchemyWKB(<%zd bytes>)", PyBytes_GET_SIZE(self->data));
}
static PyMemberDef GeoAlchemyWKB_members[] = {
    {"data", T_OBJECT_EX, offsetof(GeoAlchemyWKBObject, data), READONLY, "WKB bytes"},
    {nullptr}
};
PyTypeObject GeoAlchemyWKB_Type = {
    .ob_base      = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name      = "mod_dict.GeoAlchemyWKB",
    .tp_basicsize = sizeof(GeoAlchemyWKBObject),
    .tp_dealloc   = (destructor)GeoAlchemyWKB_dealloc,
    .tp_repr      = (reprfunc)GeoAlchemyWKB_repr,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "Wrap raw WKB bytes to be stored, serialized as WKB geometry data.\n"
                    "Reconstruction on the reading side follows set_geo_backend() - see its docstring.",
    .tp_members   = GeoAlchemyWKB_members,
    .tp_init      = (initproc)GeoAlchemyWKB_init,
    .tp_new       = PyType_GenericNew,
};

/* ============================================================================
   register_converter
   ============================================================================ */

static PyObject* py_register_converter(PyObject*, PyObject* args) {
    PyObject* type_obj;
    PyObject* callable;
    if (!PyArg_ParseTuple(args, "OO", &type_obj, &callable)) return nullptr;
    if (!PyType_Check(type_obj)) {
        PyErr_SetString(PyExc_TypeError, "first argument must be a type/class");
        return nullptr;
    }
    if (!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "second argument must be callable");
        return nullptr;
    }
    converter_registry_register(type_obj, callable);
    Py_RETURN_NONE;
}

/* ============================================================================
   set_geo_backend — which library a deserialized shapely/geoalchemy2 geometry
   reconstructs into. Required if both are installed (otherwise ambiguous),
   optional (auto-detected) if only one is. Pass None to clear the preference.
   ============================================================================ */

static PyObject* py_set_geo_backend(PyObject*, PyObject* arg) {
    if (arg == Py_None) {
        Serializer::set_geo_backend(nullptr);
        Py_RETURN_NONE;
    }
    if (!PyUnicode_Check(arg)) {
        PyErr_SetString(PyExc_TypeError, "set_geo_backend: name must be a str or None");
        return nullptr;
    }
    const char* name = PyUnicode_AsUTF8(arg);
    if (!name) return nullptr;
    if (!Serializer::set_geo_backend(name)) return nullptr;
    Py_RETURN_NONE;
}

/* ============================================================================
   dumps / loads — module-level serialization for arbitrary objects.

   A ModDict is serialized with its own container format (magic+version+outer
   entries, same as ModDict.serialize()) so loads() reconstructs a ModDict
   back. Anything else uses the generic single-value format. No implicit
   ModDict → dict conversion — call mn.to_dict() first if a plain dict is
   what you want serialized.
   ============================================================================ */

static PyObject* py_dumps(PyObject*, PyObject* obj) {
    std::vector<uint8_t> buf;
    ModDict* mdo = ModDict_unwrap(obj);
    if (mdo) {
        buf = mdo->serialize();
    } else {
        Serializer::serialize_pyobject(buf, obj);
    }
    if (PyErr_Occurred()) return nullptr;
    return PyBytes_FromStringAndSize((const char*)buf.data(), (Py_ssize_t)buf.size());
}

static PyObject* py_loads(PyObject*, PyObject* args) {
    const char* data; Py_ssize_t len;
    if (!PyArg_ParseTuple(args, "y#", &data, &len)) return nullptr;

    if (ModDict::has_container_magic((const uint8_t*)data, (size_t)len)) {
        ModDict* internal = new ModDict();
        internal->deserialize((const uint8_t*)data, (size_t)len);
        if (PyErr_Occurred()) { delete internal; return nullptr; }
        return ModDict_wrap_owned(internal);
    }

    const uint8_t* ptr = (const uint8_t*)data;
    const uint8_t* end = ptr + len;
    ModValue mv = Serializer::deserialize_value(ptr, end, nullptr);
    if (PyErr_Occurred()) return nullptr;
    // ModValue's destructor Py_XDECREFs obj — null it out here so returning
    // the pointer transfers ownership instead of leaving a dangling ref
    // (deserialize_value hands back a fresh refcount=1 object; without this
    // the ~ModValue() at function exit would drop it to 0 and free it out
    // from under the caller).
    PyObject* result = mv.obj;
    mv.obj = nullptr;
    if (result) return result;
    Py_RETURN_NONE;
}

static PyMethodDef module_methods[] = {
    {"register_converter", py_register_converter, METH_VARARGS,
     "register_converter(type, callable)\n"
     "Register a converter for a custom type.\n"
     "callable(obj) must return a value ModDict can store natively.\n"
     "Subclasses are matched via MRO walk."},
    {"dumps", py_dumps, METH_O,
     "dumps(obj)->bytes\n"
     "Serialize any supported object. A ModDict is serialized with its native\n"
     "container format (round-trips back to ModDict via loads()); everything\n"
     "else uses the generic single-value format. Call mn.to_dict() first if\n"
     "you want the plain-dict form serialized instead."},
    {"loads", py_loads, METH_VARARGS,
     "loads(data)->object\n"
     "Deserialize bytes produced by dumps() (or ModDict.serialize())."},
    {"set_geo_backend", py_set_geo_backend, METH_O,
     "set_geo_backend(name)\n"
     "Which library a deserialized shapely/geoalchemy2 geometry reconstructs\n"
     "into: \"shapely\" or \"geoalchemy2\". Required if both are installed\n"
     "(deserializing a geometry otherwise raises ValueError, ambiguous);\n"
     "optional (auto-detected) if only one is installed. Pass None to clear\n"
     "the preference. Raises ValueError/ImportError immediately if the name\n"
     "is invalid or that library isn't importable."},
    {nullptr, nullptr, 0, nullptr}
};

static PyModuleDef mod_dict_module = {
    .m_base    = PyModuleDef_HEAD_INIT,
    .m_name    = "mod_dict",
    .m_doc     = "mod_dict — nested dictionary with indexed field queries, merge and serialization",
    .m_size    = -1,
    .m_methods = module_methods,
};

PyMODINIT_FUNC PyInit_mod_dict(void) {
    if (PyType_Ready(&ModDictIter_Type) < 0) return nullptr;
    if (PyType_Ready(&RowProxy_Type) < 0) return nullptr;
    if (PyType_Ready(&ModDict_Type) < 0) return nullptr;
    if (PyType_Ready(&FilterBuilder_Type) < 0) return nullptr;
    if (PyType_Ready(&ShapelyWKB_Type) < 0) return nullptr;
    if (PyType_Ready(&GeoAlchemyWKB_Type) < 0) return nullptr;

    converter_registry_init();

    PyObject* m = PyModule_Create(&mod_dict_module);
    if (!m) return nullptr;

    Py_INCREF(&ModDict_Type);      PyModule_AddObject(m, "ModDict",       (PyObject*)&ModDict_Type);
    Py_INCREF(&ShapelyWKB_Type);   PyModule_AddObject(m, "ShapelyWKB",    (PyObject*)&ShapelyWKB_Type);
    Py_INCREF(&GeoAlchemyWKB_Type);PyModule_AddObject(m, "GeoAlchemyWKB", (PyObject*)&GeoAlchemyWKB_Type);

    return m;
}
