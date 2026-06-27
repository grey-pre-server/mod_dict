# mod_dict

Python-расширение на C++, которое хранит вложенные словари по ссылке и предоставляет индексированную фильтрацию, сортировку, группировку и обновление данных — без конвертации в DataFrame или базу данных.

```python
import mod_dict as md

mn = md.ModDict()
mn["alice"] = {"age": 30, "score": 9.5, "active": True,
               "meta": {"level": 5, "details": {"rank": 42}}}

# Доступ к полям через хранимый PyObject* — без копирования
age  = mn["alice"]["age"]                        # 30
rank = mn["alice"]["meta"]["details"]["rank"]    # 42
mn["alice"]["age"] = 31                          # обновление на месте, индекс синхронизируется автоматически

# Индексированная фильтрация / сортировка / группировка — индекс строится автоматически
adults = mn.filter("age").gte(18)
active = mn.filter("active").eq(True)
rows   = mn.sort_by("age")                        # → [row, row, ...]
keys   = mn.sort_by("age", returns="parent_keys") # → [key, key, ...]
ages   = mn.sort_by("age", returns="values")      # → [18, 25, 30, ...]
groups = mn.group_by("age")                       # → {value: ModDict, ...}
slim   = mn.select(["age", "score"])              # → новый ModDict
rows   = mn.select(["age", "score"], returns="values")  # → [{"age":..}, ..]

# Пути через точку в sort / group / select
mn.sort_by("meta.details.rank")
mn.group_by("meta.level")
mn.select(["meta.details.rank", "score"])

# Обновление из другой коллекции
mn.update(other)                                 # массовая вставка (как dict.update)
mn.update(other, "*", "*")                       # по внешнему ключу
mn.update(other, "user_id", "id")               # по значению поля
mn.update(other, "*.geo.lat", "*.geo.lat")      # только одно глубокое поле

# Алиасы — прозрачное второе имя для той же строки
mn.alias("alice", "al")
mn["al"]["age"] = 32          # та же строка — mn["alice"]["age"] == 32
mn["al"] = {"age": 33}        # замена строки через алиас
del mn["al"]                  # удаляет и алиас, и оригинал
print(mn.aliases())           # {"al": "alice"}

# Бинарная сериализация (полный набор типов Python — date, bytes, Decimal, Path, …)
data = mn.serialize()
mn2  = md.ModDict(); mn2.deserialize(data)

# Конвертеры типов — применяются при вставке
md.register_converter(Temperature, lambda t: t.celsius)
```

## Архитектура

Строки хранятся как `PyObject*` — тот же объект-словарь, который передал вызывающий код, с `Py_INCREF`. Глубокого копирования нет.

```
mn["alice"] = row_dict          # Py_INCREF(row_dict), сохраняем указатель
mn["alice"]["age"]              # хэш внешнего ключа → PyObject* → PyDict_GetItemString
mn["alice"]["age"] = 31        # RowProxy перехватывает запись → reindex автоматически
```

На внешней хэш-таблице строится **FieldIndex** для каждого поля — O(1) для равенства и O(log n) для диапазонных запросов. Индексы строятся автоматически при первом вызове `filter()` / `sort_by()` / `group_by()` и переиспользуются.

## Когда подходит

- Коллекция записей с фиксированной (или полуфиксированной) схемой.
- Нужна индексированная фильтрация, сортировка, группировка или обновление полей — без pandas/SQL.
- Пишем один раз, читаем много раз.
- Внутрипроцессный кэш, разделяемый между asyncio-корутинами — zero-copy, без GC-давления при чтении.

## Когда не подходит

- Интенсивная запись с жёсткими требованиями к латентности — `mn[k] = row` немного медленнее dict (refcount + хэш).
- Нужна конкурентная запись из нескольких потоков.
- Схема полностью динамична, без повторяющихся имён полей.

## API

```python
# Запись
mn[key] = value                          # скаляр или вложенный словарь
mn[key]["field"] = value                 # обновление поля на месте (индекс синхронизируется)
del mn[key]

# Чтение
mn[key]                                  # полная строка — O(1), возвращает ссылку на dict
mn[key]["field"]                         # поле через Python chaining
mn.get(key, default)

# Принадлежность / размер
key in mn
len(mn)                                  # алиасы не учитываются

# Итерация — алиасы скрыты
for key in mn: ...
mn.keys() / mn.values() / mn.items()

# Фильтрация (индекс строится автоматически, далее переиспользуется)
mn.filter("age").gte(18)                         # → ModDict
mn.filter("age").between(30, 40)
mn.filter("active").eq(True)
mn.filter("orders.?.status").eq("shipped")       # wildcard-путь

# Сортировка / проекция / группировка — поддержка путей через точку
mn.sort_by("age", reverse=False, returns="rows")        # по умолчанию → [row, ...]
mn.sort_by("age", returns="parent_keys")                # → [key, ...]
mn.sort_by("meta.details.rank", returns="values")       # → [val, ...]

mn.select(["age", "name"])                              # → новый ModDict
mn.select(["age", "meta.level"], returns="values")      # → [{"age":..}, ...]

mn.group_by("active")                                   # → {value: ModDict, ...}
mn.group_by("meta.level")

# Алиасы
mn.alias(key, alias)                     # создать алиас (1 на ключ)
mn.aliases()                             # → {alias: original_key, ...}
del mn[alias]                            # удаляет и алиас, и оригинал

# Обновление из другой коллекции
mn.update(other)                                      # массовая вставка
mn.update(other, from_path, to_path, conflict="keep_right")

# Сериализация
mn.serialize() / mn.deserialize(data)

# Управление индексами (опционально — авто-индекс покрывает большинство случаев)
mn.create_index("field") / mn.drop_index("field") / mn.has_index("field")

# Принудительная ресинхронизация после глубокой записи (2+ уровня вложенности)
mn.reindex(key)
```

### Синтаксис путей для `update` и `filter`

| Токен | Значение |
|-------|---------|
| `*`   | **scan_key** — совпадение по внешнему ключу |
| `?`   | **pass_key** — wildcard для одного уровня вложенности |

```python
mn.update(updates, "*", "*")                        # объединение по внешнему ключу
mn.update(prices, "*.meta.score", "*.meta.score")  # обновить только одно глубокое поле
mn.filter("orders.?.status").eq("shipped")          # status внутри любого order id
```

### Конвертеры типов

Конвертеры применяются **при вставке** — значения конвертируются до сохранения, поэтому они переживают `serialize()`.
MRO обходится: конвертер для базового класса применяется и к подклассам.

```python
md.register_converter(MyType, lambda obj: obj.to_dict())
mn["key"] = {"value": MyType(...)}   # → хранится как dict, сериализуется
```

Встроенные конвертеры для `shapely` (WKB) и `geoalchemy2` (WKBElement) активируются автоматически при наличии библиотеки.

Полные type stubs с документацией находятся в `src/mod_dict.pyi` — видны в IDE при наведении и через `help()`.

## Установка

```bash
pip install mod_dict
```

Требуется Python ≥ 3.11. Готовые wheels для Windows / Linux / macOS.
Сборка из исходников: `pip wheel .` (требуется CMake ≥ 3.15 и компилятор C++17).

## Asyncio

ModDict безопасен для конкурентного чтения в однопоточном event loop. Строки хранятся как `PyObject*` — никакого копирования между корутинами, никакого GC-давления при чтении.

```python
cache = md.ModDict()

async def handler(request):
    row = cache[request.user_id]
    return Response(row["meta"]["details"]["rank"])

async def startup():
    for key, row in data:
        cache[key] = row
```

См. [BENCHMARK_RU.md](BENCHMARK_RU.md) для подробных замеров.
