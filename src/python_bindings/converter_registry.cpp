#include "converter_registry.h"

// {type: callable} — Python dict for O(1) exact-type lookup
static PyObject* s_registry = nullptr;

void converter_registry_init() {
    if (!s_registry) s_registry = PyDict_New();
}

void converter_registry_clear() {
    Py_CLEAR(s_registry);
}

void converter_registry_register(PyObject* type_obj, PyObject* callable) {
    if (!s_registry) converter_registry_init();
    PyDict_SetItem(s_registry, type_obj, callable);
}

bool converter_registry_has(PyObject* type_obj) {
    if (!s_registry) return false;
    return PyDict_GetItem(s_registry, type_obj) != nullptr;
}

// Авто-регистрация через атрибут-строку: callable = lambda obj: getattr(obj, attr)
void converter_registry_register_attr(PyObject* type_obj, const char* attr_name) {
    if (!s_registry) converter_registry_init();
    if (PyDict_GetItem(s_registry, type_obj)) return;  // уже зарегистрирован

    // Строим: operator.attrgetter(attr_name)
    PyObject* op = PyImport_ImportModule("operator");
    if (!op) { PyErr_Clear(); return; }
    PyObject* ag = PyObject_GetAttrString(op, "attrgetter");
    Py_DECREF(op);
    if (!ag) { PyErr_Clear(); return; }
    PyObject* callable = PyObject_CallFunction(ag, "s", attr_name);
    Py_DECREF(ag);
    if (!callable) { PyErr_Clear(); return; }

    PyDict_SetItem(s_registry, type_obj, callable);
    Py_DECREF(callable);
}

PyObject* converter_registry_convert_deep(PyObject* obj) {
    if (!obj) { Py_RETURN_NONE; }
    // Fast path: no converters registered → return original ref
    if (!s_registry || PyDict_Size(s_registry) == 0) {
        Py_INCREF(obj); return obj;
    }

    // Try converter on this object directly
    PyObject* converted = converter_registry_apply(obj);
    if (converted) return converted;  // new ref

    // Recurse into dict: create a copy only if any value actually changes
    if (PyDict_Check(obj)) {
        PyObject* new_dict = nullptr;
        PyObject *k, *v; Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &k, &v)) {
            PyObject* cv = converter_registry_convert_deep(v);
            if (cv != v) {
                // First change: lazily allocate new_dict and backfill already-visited pairs
                if (!new_dict) {
                    new_dict = PyDict_New();
                    PyObject *k2, *v2; Py_ssize_t pos2 = 0;
                    while (PyDict_Next(obj, &pos2, &k2, &v2) && k2 != k)
                        PyDict_SetItem(new_dict, k2, v2);
                }
            }
            if (new_dict) PyDict_SetItem(new_dict, k, cv);
            Py_DECREF(cv);
        }
        if (new_dict) return new_dict;
        Py_INCREF(obj); return obj;
    }

    // Recurse into list
    if (PyList_Check(obj)) {
        Py_ssize_t n = PyList_GET_SIZE(obj);
        PyObject* new_list = nullptr;
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject* item = PyList_GET_ITEM(obj, i);
            PyObject* ci   = converter_registry_convert_deep(item);
            if (ci != item && !new_list) {
                new_list = PyList_New(n);
                for (Py_ssize_t j = 0; j < i; j++) {
                    PyObject* prev = PyList_GET_ITEM(obj, j);
                    Py_INCREF(prev);
                    PyList_SET_ITEM(new_list, j, prev);
                }
            }
            if (new_list) PyList_SET_ITEM(new_list, i, ci);
            else          Py_DECREF(ci);
        }
        if (new_list) return new_list;
        Py_INCREF(obj); return obj;
    }

    Py_INCREF(obj); return obj;
}

PyObject* converter_registry_apply(PyObject* obj) {
    if (!obj) return nullptr;
    PyObject* type_obj = (PyObject*)Py_TYPE(obj);

    // 1. Exact type lookup
    if (s_registry) {
        PyObject* fn = PyDict_GetItem(s_registry, type_obj);
        if (fn) {
            PyObject* result = PyObject_CallOneArg(fn, obj);
            return result;
        }

        // 2. MRO walk — ближайший зарегистрированный базовый класс
        PyObject* mro = ((PyTypeObject*)type_obj)->tp_mro;
        if (mro) {
            Py_ssize_t n = PyTuple_GET_SIZE(mro);
            for (Py_ssize_t i = 1; i < n; i++) {
                PyObject* base = PyTuple_GET_ITEM(mro, i);
                fn = PyDict_GetItem(s_registry, base);
                if (fn) {
                    // Кэшируем exact type → тот же callable, MRO-walk больше не нужен
                    PyDict_SetItem(s_registry, type_obj, fn);
                    PyObject* result = PyObject_CallOneArg(fn, obj);
                    return result;
                }
            }
        }
    }

    return nullptr;
}
