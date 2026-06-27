#include <Python.h>
#include <structmember.h>
#include "converter_registry.h"

extern PyTypeObject ModDict_Type;
extern PyTypeObject ModDictIter_Type;
extern PyTypeObject FilterBuilder_Type;
extern PyTypeObject RowProxy_Type;

/* ============================================================================
   ShapelyWKB / GeoAlchemyWKB — лёгкие обёртки для raw WKB bytes.
   Позволяют явно пометить байты как гео-тип без наличия библиотеки на стороне
   записи. duck-typing в from_pyobject подхватит их по атрибутам.

   md.ShapelyWKB(raw_bytes)    → хранится как GEOMETRY_SHAPELY
   md.GeoAlchemyWKB(raw_bytes) → хранится как GEOMETRY_GEOALCHEMY
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
static PyTypeObject ShapelyWKB_Type = {
    .ob_base      = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name      = "mod_dict.ShapelyWKB",
    .tp_basicsize = sizeof(ShapelyWKBObject),
    .tp_dealloc   = (destructor)ShapelyWKB_dealloc,
    .tp_repr      = (reprfunc)ShapelyWKB_repr,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "Wrap raw WKB bytes to be stored and deserialized as a shapely geometry.\n"
                    "Requires shapely on the reading side: pip install shapely",
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
// .wkt нужен duck-typing'у geoalchemy2 — возвращаем None (атрибут существует)
static PyObject* GeoAlchemyWKB_get_wkt(GeoAlchemyWKBObject*, void*) { Py_RETURN_NONE; }
static PyMemberDef GeoAlchemyWKB_members[] = {
    {"data", T_OBJECT_EX, offsetof(GeoAlchemyWKBObject, data), READONLY, "WKB bytes"},
    {nullptr}
};
static PyGetSetDef GeoAlchemyWKB_getset[] = {
    {"wkt", (getter)GeoAlchemyWKB_get_wkt, nullptr, "sentinel for duck-typing", nullptr},
    {nullptr}
};
static PyTypeObject GeoAlchemyWKB_Type = {
    .ob_base      = PyVarObject_HEAD_INIT(nullptr, 0)
    .tp_name      = "mod_dict.GeoAlchemyWKB",
    .tp_basicsize = sizeof(GeoAlchemyWKBObject),
    .tp_dealloc   = (destructor)GeoAlchemyWKB_dealloc,
    .tp_repr      = (reprfunc)GeoAlchemyWKB_repr,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "Wrap raw WKB bytes to be stored and deserialized as a geoalchemy2.WKBElement.\n"
                    "Requires geoalchemy2 on the reading side: pip install geoalchemy2",
    .tp_members   = GeoAlchemyWKB_members,
    .tp_getset    = GeoAlchemyWKB_getset,
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

static PyMethodDef module_methods[] = {
    {"register_converter", py_register_converter, METH_VARARGS,
     "register_converter(type, callable)\n"
     "Register a converter for a custom type.\n"
     "callable(obj) must return a value ModDict can store natively.\n"
     "Subclasses are matched via MRO walk."},
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
