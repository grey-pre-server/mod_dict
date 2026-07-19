//
// Created by grey on 14.05.2026.
//

#ifndef MOD_DICT_MOD_DICT_TYPE_H
#define MOD_DICT_MOD_DICT_TYPE_H
#include "Python.h"
#include "../core/mod_dict.h"

// Wraps a freshly heap-allocated ModDict* with no other owner (deleted on
// dealloc). Use for filter/select/copy/loads() results.
PyObject* ModDict_wrap_owned(ModDict* internal);

// Returns the internal ModDict* if obj is a ModDict instance, else nullptr.
ModDict* ModDict_unwrap(PyObject* obj);

// Converts a ModDict::IndexDiff (old==-1 means "newly appeared") into a
// Python list of (old_index_or_None, new_index) tuples — the shared
// vocabulary for set_sort/set_filter/set_group/insert/update_row/delete/
// insert_batch and the "reorder" event notify_live_cursors() fires on
// sibling cursors. Implemented in mod_dict.cpp (core), declared here so
// both mod_dict.cpp and mod_dict_type.cpp can use it.
PyObject* index_diff_to_pylist(const ModDict::IndexDiff& diff);

// -1 -> None, else a PyLong — the single-index return vocabulary for
// cursor_insert()/cursor_delete() (and each element of cursor_update_row()'s
// pair, and each entry of cursor_insert_batch()'s list): report ONLY this
// row's own position, never every sibling shifted as a structural side
// effect (see mod_dict.h's comment on cursor_insert() for why).
PyObject* py_index_or_none(Py_ssize_t idx);

// list[int|None] — cursor_insert_batch()'s return: one entry per row in the
// batch, in the same order PyDict_Next visits the input `rows` dict.
PyObject* py_index_list(const std::vector<Py_ssize_t>& positions);

#endif //MOD_DICT_MOD_DICT_TYPE_H
