#pragma once
#include <Python.h>

void converter_registry_init();
void converter_registry_clear();
void converter_registry_register(PyObject* type, PyObject* callable);
// Авто-регистрация duck-typing: callable = operator.attrgetter(attr_name)
void converter_registry_register_attr(PyObject* type_obj, const char* attr_name);
bool converter_registry_has(PyObject* type_obj);
PyObject* converter_registry_apply(PyObject* obj);        // new ref or nullptr
// Рекурсивно применяет конвертеры к dict/list. Возвращает new ref (converted copy или obj+INCREF).
// Short-circuit O(1) если реестр пуст — нет накладных расходов для обычных строк.
PyObject* converter_registry_convert_deep(PyObject* obj);  // new ref always
