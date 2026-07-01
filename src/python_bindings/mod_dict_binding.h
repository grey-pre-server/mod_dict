//
// Created by grey on 14.05.2026.
//

#ifndef MOD_DICT_MOD_DICT_TYPE_H
#define MOD_DICT_MOD_DICT_TYPE_H
#include "Python.h"

struct ModDict;

// Wraps a freshly heap-allocated ModDict* with no other owner (deleted on
// dealloc). Use for filter/select/copy/loads() results.
PyObject* ModDict_wrap_owned(ModDict* internal);

// Returns the internal ModDict* if obj is a ModDict instance, else nullptr.
ModDict* ModDict_unwrap(PyObject* obj);

#endif //MOD_DICT_MOD_DICT_TYPE_H
