//
// Created by grey on 12.05.2026.
//

#ifndef MOD_DICT_ERROR_HANDLING_H
#define MOD_DICT_ERROR_HANDLING_H

#include <Python.h>

/* ============================================================================
   Установка ошибок
   ============================================================================ */

#define MOD_DICT_RAISE(exc, msg) do { \
    PyErr_SetString(exc, msg); \
    return NULL; \
} while(0)

#define MOD_DICT_RAISE_FMT(exc, fmt, ...) do { \
    PyErr_Format(exc, fmt, __VA_ARGS__); \
    return NULL; \
} while(0)

/* ============================================================================
   Проверки
   ============================================================================ */

#define MOD_DICT_CHECK_ARG(cond, fmt, ...) do { \
    if (!(cond)) { \
        PyErr_Format(PyExc_TypeError, fmt, __VA_ARGS__); \
        return NULL; \
    } \
} while(0)

#define MOD_DICT_CHECK_ALLOC(ptr) do { \
    if (!(ptr)) { \
        PyErr_NoMemory(); \
        return NULL; \
    } \
} while(0)

#define MOD_DICT_CHECK_BOUNDS(idx, max, name) do { \
    if ((idx) < 0 || (idx) >= (max)) { \
        PyErr_Format(PyExc_IndexError, \
            "%s index %zd out of range [0, %zd)", name, (Py_ssize_t)(idx), (Py_ssize_t)(max)); \
        return NULL; \
    } \
} while(0)

/* ============================================================================
   Типовые проверки
   ============================================================================ */

#define MOD_DICT_CHECK_DICT(obj, name) do { \
    if (!PyDict_Check(obj)) { \
        PyErr_Format(PyExc_TypeError, \
            "'%s' must be dict, got %s", name, Py_TYPE(obj)->tp_name); \
        return NULL; \
    } \
} while(0)

#define MOD_DICT_CHECK_STRING(obj, name) do { \
    if (!PyUnicode_Check(obj)) { \
        PyErr_Format(PyExc_TypeError, \
            "'%s' must be str, got %s", name, Py_TYPE(obj)->tp_name); \
        return NULL; \
    } \
} while(0)

/* ============================================================================
   Безопасный DECREF
   ============================================================================ */

#define MOD_DICT_SAFE_DECREF(obj) do { \
    Py_XDECREF(obj); \
    (obj) = NULL; \
} while(0)

/* ============================================================================
   Парсинг аргументов
   ============================================================================ */

#define MOD_DICT_PARSE_ARGS(args, kwargs, format, ...) do { \
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, format, \
        (char**)(kwlist), __VA_ARGS__)) { \
        return NULL; \
    } \
} while(0)

#endif
