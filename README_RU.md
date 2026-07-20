# mod_dict

Python-расширение на C++, которое хранит вложенные словари по ссылке и предоставляет индексированную фильтрацию, сортировку, группировку и обновление данных — без конвертации в DataFrame или базу данных. А также живые, реагирующие на мутации **курсоры** для реактивных GUI-таблиц (Qt или любых других) — без ручного ведения индексов и порядка.

```python
import mod_dict as md

mn = md.ModDict()
mn["alice"] = {"age": 30, "score": 9.5, "active": True,
               "meta": {"level": 5, "details": {"rank": 42}}}

# Доступ к полям через хранимый PyObject* — без копирования
age  = mn["alice"]["age"]                        # 30
rank = mn["alice"]["meta"]["details"]["rank"]    # 42
mn["alice"]["age"] = 31                          # обновление на месте

# Индексированная фильтрация / сортировка / группировка — индекс строится автоматически
adults = mn.filter("age").gte(18)
active = mn.filter("active").eq(True)
rows   = mn.sort_by("age")                        # → [row, row, ...]
keys   = mn.sort_by("age", returns="parent_keys") # → [key, key, ...]
ages   = mn.sort_by("age", returns="values")      # → [18, 25, 30, ...]
groups = mn.group_by("age")                       # → {value: ModDict, ...}
slim   = mn.select_mass(["age", "score"])         # → новый ModDict
cols   = mn.select_mass(["age", "score"], returns="values")  # → [[age,...], [score,...]] (по колонкам)
ages   = mn.select("age", returns="values")       # → [age, age, ...] — одно поле, уже плоский список

# Пути через точку в sort / group / select
mn.sort_by("meta.details.rank")
mn.group_by("meta.level")
mn.select_mass(["meta.details.rank", "score"])

# Связи между строками — объявление, обход, JOIN в WHERE, ON DELETE
mn.link("orders.?.customer_id", "customers.?")
mn.follow("orders.?.customer_id")                # → ModDict разрешённых клиентов
mn.filter("orders.?.customer_id->name").eq("Alice")  # JOIN в WHERE, можно цепочкой ("->name->city")

# Курсоры — живые, реагирующие на мутации виды для реактивной GUI-таблицы
# (привязаны к уже существующей вложенной таблице {key: row} — см. "Курсоры" ниже)
orders = mn.cursor("some_table")
orders.set_sort("amount")
new_index, row = orders.insert("o9", {"amount": 15})  # → (int | None, dict) — позиция и сама строка
orders.connect("insert", lambda payload: qt_model.apply_insert(payload))

# Обновление из другой коллекции
mn.update(other)                                 # массовая вставка (как dict.update)
mn.update(other, "*", "*")                       # по внешнему ключу (только существующие)
mn.update(other, "?", "?")                       # по ключу + вставка новых ключей из other
mn.update(other, "user_id", "id")               # по значению поля
mn.update(other, "*.geo.lat", "*.geo.lat")      # только одно глубокое поле

# Построение из списка строк или любого Mapping
mn3 = md.ModDict.from_rows(users, key="id")      # {r["id"]: r for r in users}
mn4 = md.ModDict(other_mn)                       # копия из ModDict
mn5 = md.ModDict(OrderedDict(...))               # принимается любой Mapping

# Глубокое копирование (в 7.8× быстрее deepcopy)
backup = mn.copy()

# Доступ по индексу в порядке вставки
mn.at(0)    # первое значение
mn.at(-1)   # последнее значение

# Бинарная сериализация (полный набор типов Python — date, bytes, bytearray, tuple, uuid, Decimal, Path, …)
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
```

На внешней хэш-таблице строится **FieldIndex** для каждого поля — O(1) для равенства и O(log n) для диапазонных запросов. Индексы строятся автоматически при первом вызове `filter()` / `sort_by()` / `group_by()` и переиспользуются.

**Курсор** (`mn.cursor(path)`) — это экземпляр `ModDict`, привязанный к уже существующей вложенной таблице внутри хранилища другого `ModDict` — у него нет собственного хранилища, каждое чтение/запись идёт напрямую в сырой dict родителя. Он существует для реактивных GUI-таблиц: флаги сортировки/фильтра/группировки, поддерживаемые инкрементально при каждой мутации, точечные методы мутации (`insert`/`update_row`/`delete`/`insert_batch`), возвращающие ровно тот diff, что нужен GUI, и независимый от фреймворка `connect()` для push-реактивности. См. [Курсоры](#курсоры-реактивные-виды-для-gui-таблиц) ниже.

## Когда подходит

- Коллекция записей с фиксированной (или полуфиксированной) схемой.
- Нужна индексированная фильтрация, сортировка, группировка или обновление полей — без pandas/SQL.
- Пишем один раз, читаем много раз.
- Внутрипроцессный кэш, разделяемый между asyncio-корутинами — zero-copy, без GC-давления при чтении.
- Нужно держать GUI-таблицу (Qt или любую другую), реагирующую на вставки/обновления/удаления, без ручного ведения индексов и порядка — см. [Курсоры](#курсоры-реактивные-виды-для-gui-таблиц).

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
mn.pop(key)                              # удалить и вернуть значение
mn.pop(key, default)                     # вернуть default если ключ не найден

# Принадлежность / размер
key in mn
len(mn)

# Итерация
for key in mn: ...
mn.keys() / mn.values() / mn.items()

# Фильтрация (индекс строится автоматически, далее переиспользуется)
mn.filter("age").eq(18)                          # age == 18
mn.filter("age").ne(18)                          # age != 18
mn.filter("age").lt(18)                          # age <  18
mn.filter("age").lte(18)                         # age <= 18
mn.filter("age").gt(18)                          # age >  18
mn.filter("age").gte(18)                         # age >= 18
mn.filter("age").between(18, 30)                 # 18 <= age <= 30
mn.filter("city").in_(["NY", "LA"])              # city в списке
mn.filter("orders.?.status").eq("shipped")       # ? пропускает один уровень ключей
mn.filter("?").eq("orders")                      # терминальный ?: строки с ключом "orders"
mn.filter("g1.?.status").eq("shipped")           # anchor: сканирование ограничено ключом "g1"
mn.filter("region.?.?.status").eq("Active")      # один ? на уровень — для вложенности глубже цепочкой

# нетерминальный wildcard возвращает PRUNED-результат: остаются только
# совпавшие inner-ключи, поэтому цепочка фильтров работает как AND, не OR
mn.filter("a.?.age").eq(30).filter("a.?.name").eq("alice")

# параметр returns: получить внутренние результаты без построения нового ModDict
mn.filter("age").gte(18, returns="rows_here")                    # → [row, ...]
mn.filter("age").gte(18, returns="values", value_field="name")   # → [name, ...]

# "->" — JOIN объявленной связи прямо в пути, можно цепочкой, .eq() ускорен индексами
mn.filter("orders.?.customer_id->name").eq("Alice")
mn.filter("orders.?.customer_id->company_id->name").eq("Acme")   # 2 хопа

# Сортировка / проекция / группировка — поддержка путей через точку
mn.sort_by("age", reverse=False, returns="rows")        # по умолчанию → [row, ...]
mn.sort_by("age", returns="parent_keys")                # → [key, ...]
mn.sort_by("age", inplace=True)                         # переупорядочивает mn на месте, возвращает None
mn.sort_by("meta.details.rank", returns="values")       # → [val, ...]

mn.select("age")                                        # → {pk: age, ...} — одно поле, без обёртки на строку
mn.select("age", returns="values")                      # → [age, ...]

mn.select_mass(["age", "name"])                         # → новый ModDict, ключ — последний сегмент пути
mn.select_mass({"user_age": "age"})                     # явные метки — также разрешают коллизии
mn.select_mass(["age", "meta.level"], returns="values") # → [[age,...], [meta.level,...]] (по колонкам)
mn.select_mass(["orders.?.customer_id->name"])                # wildcard/"->" по умолчанию: приземляется на целевую таблицу → {"customers": {100: {...}, ...}}
mn.select_mass(["orders.?.customer_id->name"], returns="rows_here")  # вместо этого плоское извлечение → {order_pk: {"name": ...}}

mn.group_by("active")                                   # → {value: ModDict, ...}
mn.group_by("meta.level")

# Связи между строками (self-reference допустим) — см. "Связи между строками" ниже
mn.link("orders.?.customer_id", "customers.?", on_delete="restrict")  # restrict|cascade|set_null
mn.follow("orders.?.customer_id")                        # → ModDict разрешённых целевых строк

# Курсоры — живые виды для GUI-таблиц, см. "Курсоры" ниже
orders = mn.cursor("u1.orders")                          # anchor должен уже существовать
orders.set_sort("amount") / orders.set_filter(pred) / orders.set_group("status")
orders.insert(key, row)          # -> (int | None, dict) = (новая позиция, row)
orders.delete(key)               # -> int | None (прежняя позиция)
orders.update_row(key, changes)  # -> ((old_index, new_index), changes) — changes: {поле: новое_значение}
orders.insert_batch({key: row, ...})  # -> list[(int|None, dict)] = [(новая позиция, row), ...], одна запись, одно событие connect()
orders.insert_batch([row, ...], key="id")  # то же, key= извлекает ключ строки вместо готового {key: row}
orders.connect("insert" | "update" | "delete" | "reorder", callback)
orders.view_keys() / orders.view_values() / orders.view_items()  # текущий вид с учётом sort/filter — [key] / in / del остаются raw, фильтра не видят

# Обновление из другой коллекции
mn.update(other)                                      # массовая вставка
mn.update(other, from_path, to_path, conflict="keep_right")

# Глубокое копирование (в 7.8× быстрее copy.deepcopy)
mn.copy()                                        # → новый ModDict, строки скопированы глубоко

# Доступ по индексу в порядке вставки (O(1), поддержка отрицательных индексов)
mn.at(0)                                         # значение первого вставленного ключа
mn.at(-1)                                        # значение последнего вставленного ключа

# Построение из списка словарей
md.ModDict.from_rows(rows, key="id")             # {r["id"]: r for r in rows}
md.ModDict.from_row(row)                         # нормализует Mapping → обычный dict
mn.load_rows(rows, key="id", path="users")       # пишет в СУЩЕСТВУЮЩИЙ mn: mn["users"] = {r["id"]: r for r in rows}

# Сериализация
mn.serialize() / mn.deserialize(data)          # data → self, возвращает self для чейнинга
mn.to_dict()                                   # → обычный dict (минует RowProxy)
md.dumps(obj) / md.loads(data)                 # сериализация любого объекта; ModDict возвращается как ModDict

# Управление индексами (опционально — авто-индекс покрывает большинство случаев)
mn.create_index("field") / mn.drop_index("field") / mn.has_index("field")
```

### Синтаксис путей для `update` и `filter`

| Токен | Значение |
|-------|---------|
| `*`   | **scan_key** — совпадение по внешнему ключу (update: только существующие ключи) |
| `?`   | **pass_key** — wildcard (update: + вставка новых; filter нетерминальный: пропускает ключ; filter терминальный: проверяет наличие ключа) |
| `key` | **anchor** (filter) — первый сегмент ограничивает сканирование одним внешним ключом |

```python
mn.update(updates, "*", "*")                         # объединение по ключу (только существующие)
mn.update(updates, "?", "?")                         # объединение + вставка новых ключей
mn.update(prices, "*.meta.score", "*.meta.score")   # обновить одно глубокое поле

mn.filter("orders.?.status").eq("shipped")           # ? пропускает любой id заказа
mn.filter("?").eq("orders")                          # терминальный ?: строка имеет ключ "orders"
mn.filter("g1.?.status").eq("shipped")               # anchor "g1": сканирование ограничено одной строкой
mn.filter("region.?.?.status").eq("Active")          # один ? на уровень, для вложенности — цепочкой
mn.filter("age").gte(18, returns="rows_here")        # → плоский список совпадающих dict
mn.filter("age").gte(18, returns="values", value_field="name")  # → список значений поля
```

Нетерминальные wildcard-совпадения **обрезаются (pruned)** — в результате
остаются только совпавшие inner-ключи, а не вся строка целиком, поэтому цепочка
`.filter(...)` на wildcard-путях работает как AND, а не OR. `eq()` на
wildcard-путях (любой глубины) и терминальный `?` восстанавливаются напрямую
из индекса без пересканирования; `ne()` и range-операции (`lt`/`gt`/...) на
wildcard-путях пока падают на полный скан при каждом вызове — быстрого пути
для них нет.

### Пробел — алиас точки в строковых путях

Любой метод, принимающий путь (`filter`, `sort_by`, `group_by`, `select`,
`update`, `create_index`, ...) воспринимает пробел как буквальный алиас `.` —
`"meta.level"` и `"meta level"` эквивалентны. Никакого схлопывания:
`"meta   level"` (лишние пробелы) эквивалентно `"meta...level"` (лишние
точки) — оба дают пустые сегменты в середине и не находят совпадений.
Строгость одинаковая для обоих разделителей.

```python
mn.filter("meta level").eq(5)          # то же, что mn.filter("meta.level").eq(5)
mn.filter("g1 ? user_id").eq(1)        # то же, что mn.filter("g1.?.user_id").eq(1)
```

Если имя поля само содержит буквальную точку или пробел — строковым путём
это не выразить. Передайте **tuple/list**, где каждый элемент берётся как
один точный сегмент, без разбиения:

```python
mn.filter(("first name",)).eq("alice")   # поле буквально называется "first name"
mn.filter(("a.b",)).eq(1)                # поле буквально называется "a.b"
```

Также новое: `mn.to_dict()` возвращает обычный `dict` (минуя RowProxy —
полезно для библиотек вроде Pydantic, которым нужен именно `dict`), и
модульные `md.dumps(obj)` / `md.loads(data)` сериализуют **любой**
поддерживаемый объект, не только целиком `ModDict` — `ModDict` возвращается
обратно как `ModDict`, всё остальное как есть. Неявной конвертации
`ModDict` → `dict` нет; вызови `mn.to_dict()` сначала, если нужно
сериализовать именно plain dict.

## Связи между строками

Объявление foreign-key-style связи между строками одного `ModDict` —
включая self-reference, например иерархию сотрудников через `manager_id`:

```python
mn = md.ModDict({
    "orders":    {1: {"customer_id": 100}, 2: {"customer_id": 200}},
    "customers": {100: {"name": "Alice"},  200: {"name": "Bob"}},
})
mn.link("orders.?.customer_id", "customers.?")            # по первичному ключу
mn.link("orders.?.customer_id", "customers.?.email")      # или по значению другого поля

mn.follow("orders.?.customer_id")                         # → ModDict разрешённых клиентов
mn.follow("orders.?.customer_id", keys=[1])                 # только для заказа 1
mn.follow("orders.?.customer_id", values=[100, 200])         # разрешить id напрямую, без скана таблицы

# ON DELETE — в стиле SQL, безопасно для self-reference (цикл разрывается на первом же удалении)
mn.link("employees.?.manager_id", "employees.?", on_delete="cascade")
del mn["employees"][1]                                       # каскадно удалит всех прямых и косвенных подчинённых

# JOIN в WHERE — "->" резолвит объявленную связь прямо в пути, можно цепочкой через несколько хопов
mn.filter("orders.?.customer_id->name").eq("Alice")          # заказы, чей клиент по имени "Alice"
mn.filter("orders.?.customer_id->company_id->name").eq("Acme")  # 2 хопа
mn.select("orders.?.customer_id->name")                      # returns="rows" по умолчанию: приземляется на целевую таблицу → {"customers": {100: {...}, ...}}
```

`on_delete` — `"restrict"` *(по умолчанию — отказ, если есть ссылки)*,
`"cascade"` *(каскадно удаляет и ссылающиеся строки)* или `"set_null"`
*(обнуляет поле-ссылку у ссылающихся строк)*. `None`/отсутствующий FK
никогда не считается висячей ссылкой — как nullable foreign key в SQL. И
`link()` (в момент объявления, по уже существующим данным), и каждая
последующая запись в исходную таблицу валидируют ссылки сразу — не только
при удалении.

`keys=` у `follow()` собирает обходы **неизвестной** заранее глубины
(например, подъём по иерархии до корня) явным циклом на Python:

```python
managers      = mn.follow("employees.?.manager_id")                       # 1 хоп
skip_managers = mn.follow("employees.?.manager_id", keys=managers.keys()) # 2 хопа
```

`->`, наоборот, — для **заранее известного** числа хопов в одном выражении
и работает прямо внутри `filter()`/`select()`: `.eq()` использует быстрый
путь по индексам (без скана таблицы); `.ne()/.lt()/.between()/.in_()/...`
падают на скан таблицы-якоря. `returns="rows_here"`/`"values"` отдают
данные **якорной** строки (например `orders`, а не там, где приземлился
хоп), по одной записи на каждую совпавшую якорную строку, без дедупликации
по цели.

`select()`/`select_mass()`'s дефолт (`returns="rows"`) работает иначе, чем у
`filter()`: wildcard/`->`-поле не извлекает значение — оно **приземляется на
и оставляет таблицу**, на которую указывает поле (мультихоп цепочки
используют `follow()` под капотом; поле без `->`-хопа просто оставляет свою
якорную таблицу как есть). У `select_mass()` поля резолвятся независимо и
мёржатся в один ModDict — смешивать хоп- и не-хоп-поля, или поля с разными
якорными таблицами, можно:

```python
mn.select_mass(["workgroup.?.group_id->name"])                              # → {"user_group": {100: {...}, ...}}
mn.select_mass(["workgroup.?.group_id->name", "workgroup.?.status"])        # → {"user_group": {...}, "workgroup": {...}}
mn.select_mass(["workgroup.?.group_id->name"], returns="rows_here")         # → {1: {"name": "Engineering"}, ...} (старое плоское извлечение)
```

И `filter()` с `->`, и `select()`/`select_mass()` с приземлением на таблицу —
сами по себе chainable: дальнейший вызов `.filter("user_group.?.field->...")`
или `.select()`/`.select_mass()` ретранслируется до корневого ModDict и пересекается с уже
сузившимися строками, так что многошаговые цепочки между таблицами работают
как ожидается. Обратный обход (приземление на таблицу, которая *ссылается*
на текущую, а не на ту, куда она указывает) не поддерживается — только то
направление, в котором был объявлен `link()`.

### Многие-ко-многим через связующую таблицу

Связующая таблица (`users_groups` ниже) — ничего особенного, обычная
таблица с двумя объявлениями `link()` вместо одного:

```python
mn = md.ModDict({
    "users": {
        "u1": {"id": "u1", "name": "alice", "note": "vip"},
        "u2": {"id": "u2", "name": "bob",   "note": "trial"},
    },
    "users_groups": {
        1: {"user_id": "u1", "group_id": 1},
        2: {"user_id": "u1", "group_id": 2},
    },
    "groups": {
        1: {"name": "engineering"},
        2: {"name": "support"},
    },
})
mn.link("users_groups.?.user_id", "users.?")     # сверяется с собственным КЛЮЧОМ users ("u1") — совпадение с "id" это соглашение, а не требование
mn.link("users_groups.?.group_id", "groups.?")

# все группы alice: filter сужает users_groups через JOIN по user_id,
# select проецирует имя группы каждого совпадения через group_id
mn.filter("users_groups.?.user_id->name").eq("alice") \
  .select("users_groups.?.group_id->name", returns="values")
# → ["engineering", "support"]

# другое поле через ТОТ ЖЕ хоп, что уже использовал filter
mn.filter("users_groups.?.user_id->name").eq("alice") \
  .select("users_groups.?.user_id->note", returns="values")
# → ["vip", "vip"] — по одному на совпавшую строку членства, без дедупликации по пользователю
```

pk-based форма `link()` сверяет поле внешнего ключа с собственным **внешним
ключом словаря** целевой таблицы — поле `id` у строки связь напрямую вообще
не смотрит, так что оно вольно дублировать ключ (как выше, аналогично
`SELECT *` в SQL, который возвращает и колонку id тоже) или вовсе с ним не
совпадать — mod_dict его не срезает и не требует. Если внешний ключ у вас
сверяется не с pk, а с другим полем — для этого вторая форма `link()`:
`mn.link(..., "users.?.id")` сверялась бы с полем `id`, а не с ключом.

## Курсоры (реактивные виды для GUI-таблиц)

**Курсор** — живой, реагирующий на мутации handle типа `ModDict`,
привязанный к *уже существующей* вложенной таблице — создан для того, чтобы
питать реактивную GUI-таблицу (Qt-модель или любую другую) без ручного
ведения индексов и порядка. У него нет собственного хранилища: каждое
чтение и запись идёт напрямую в сырой dict родителя, без копирования.

```python
mn = md.ModDict()
mn["u1"] = {"orders": {
    "o1": {"amount": 30, "status": "shipped"},
    "o2": {"amount": 10, "status": "pending"},
}}

orders = mn.cursor("u1.orders")      # anchor должен уже существовать — иначе исключение
orders["o1"]["amount"]                # 30 — обычный dict-протокол, без копирования
len(orders)                           # 2

orders.set_sort("amount")             # дальше поддерживается инкрементально
orders.set_group("status")            # строки сгруппированы, внутри группы — по amount

new_index, row = orders.insert("o3", {"amount": 20, "status": "new"})
# → (int | None, dict) — позиция ИМЕННО этой строки (а не список всех
#   сдвинутых соседей — подавай прямо в Qt-шный beginInsertRows(pos, pos),
#   сам Qt уже знает, что всё после pos сдвинулось) плюс сама строка, чтобы
#   connect()-слушателю не пришлось делать отдельный lookup. None — значит
#   активный фильтр строку не показывает (записана, но не видна; row всё
#   равно возвращается).

(old_index, new_index), changes = orders.update_row("o1", {"amount": 99})
# в отличие от insert/delete здесь возвращаются ОБА индекса — сдвиг из-за
# смены поля это Qt-шный beginMoveRows(old, new), и "откуда" нельзя вывести
# так же, как неявный сдвиг при insert/remove. changes: {поле: новое_значение}
# только для полей, значение которых реально изменилось.

old_index = orders.delete("o2")       # → int | None — прежняя позиция строки

results = orders.insert_batch({"o4": {"amount": 5, "status": "new"},
                                "o5": {"amount": 40, "status": "new"}})
# → list[(int | None, dict)], по одной паре (индекс, row) на новую строку,
# в порядке батча — одна запись, одно событие connect(), сдвинутые
# существующие строки поштучно не репортятся (та же логика, что у insert() выше)

# тот же эффект из обычного list, key= извлекает собственный ключ каждой
# строки — не нужно самому собирать {key: row} в цикле на Python
orders.insert_batch([{"id": "o4", "amount": 5, "status": "new"}], key="id")

orders.connect("insert", lambda payload: qt_model.apply_insert(payload))
orders.connect("update", lambda payload: qt_model.apply_update(payload))
orders.connect("delete", lambda old_index: qt_model.apply_delete(old_index))
orders.connect("reorder", lambda diff: qt_model.apply_reorder(diff))
# "reorder" — единственное событие, которое отдаёт полный list[(old,new)] —
# оно летит СИБЛИНГ-курсору в ответ на чужую мутацию, а сиблинг не знает,
# какая именно строка стала причиной изменения
```

Несколько независимых курсоров могут указывать на один и тот же anchor —
каждый держит своё приватное состояние sort/filter/group и свои
`connect()`-слушатели, но все видят одни и те же живые данные и уведомляют
друг друга при мутации:

```python
grid_view = mn.cursor("u1.orders")
grid_view.set_sort("amount")

summary_view = mn.cursor("u1.orders")
summary_view.set_group("status")

grid_view.insert("o9", {"amount": 5, "status": "new"})
# summary_view тоже получит событие "reorder" — он не знает, что причиной
# был именно insert(), только то, что его собственный вид изменился
```

**Что курсор поддерживает:** обычный dict-протокол (`cursor[key]`,
`cursor[key] = row`, `del cursor[key]`, `in`, `len()`, итерация, `.at(i)`),
`set_sort()`/`set_filter()`/`set_group()`, `connect()`, и точечные методы
мутации `insert()`/`update_row()`/`delete()`/`insert_batch()` из примера
выше — каждый возвращает ровно тот diff, который нужен GUI-модели,
посчитанный относительно того состояния курсора, что было непосредственно
перед вызовом.

**Что курсор пока не поддерживает:** индексацию полей (`create_index`,
`filter`, `sort_by` — их нужно звать на корневом `ModDict`) и большинство
операций над коллекцией целиком (`link`, `follow`, `select`/`select_mass`,
`copy`, `serialize`, `group_by`, `keys`/`values`/`items`, `pop`, ...) — эти
методы намеренно кидают `NotImplementedError` на курсоре: курсор — это
живой позиционный вид для GUI, а не второй полноценный `ModDict`.
`keys`/`values`/`items` заблокированы намеренно, а не временно — они не
должны на курсоре незаметно поменять смысл; их учитывающие
sort/filter аналоги — `view_keys`/`view_values`/`view_items` ниже.

`set_filter()` полностью учитывается в чтениях: при активном фильтре
`len()`/итерация/`.at(i)` видят только проходящие строки, плотно
проиндексированные (`.at(0)` — первая *видимая* строка, не обязательно
первая строка под anchor'ом) — и позиции, возвращаемые
`insert()`/`update_row()`/`delete()`, согласованы с этой же нумерацией.
Доступ по ключу (`cursor[key]`, `in`, `del cursor[key]`) от фильтра не
зависит в любом случае — отфильтрованная строка всё ещё полностью
присутствует в данных, просто отсутствует в позиционном/итерационном виде.

```python
orders.set_sort("amount")
orders.set_filter(lambda r: r["status"] == "shipped")

for key in orders:                 # только ключи, тот же видимый/отсортированный порядок, что и .at(i)
    ...
orders.view_values()               # → [row, ...] — строки в том же порядке, без отдельного cursor[key] на каждую
orders.view_items()                # → [(key, row), ...] — когда нужен и ключ, и строка

# [key]/in/del — raw, о фильтре/сортировке ничего не знают: отфильтрованная
# строка всё равно достижима напрямую:
orders["o2"]["amount"]             # работает, даже если "o2" не проходит активный фильтр
"o2" in orders                     # True вне зависимости от фильтра
"o2" in orders.view_keys()         # False — исключена из вида
```

`view_keys()`/`view_values()`/`view_items()` названы отдельно от
`keys()`/`values()`/`items()` намеренно: `[key]`/`in`/`del` на курсоре
остаются raw (как у обычного словаря, фильтр/сортировку не видят) — значит
метод, чьё имя читается как обычный dict-доступ, не должен незаметно
означать что-то другое в зависимости от того, активен ли сейчас фильтр.
`view_*` сразу говорит, что учитывается текущий sort/filter — те же строки,
тот же порядок, что уже согласован между `__iter__`/`len()`/`.at()`.

Полная документация методов (формы возврата, payload событий, крайние
случаи) — в `src/mod_dict.pyi`.

### Конвертеры типов

Конвертеры применяются **при вставке** — значения конвертируются до сохранения, поэтому они переживают `serialize()`.
MRO обходится: конвертер для базового класса применяется и к подклассам.

```python
md.register_converter(MyType, lambda obj: obj.to_dict())
mn["key"] = {"value": MyType(...)}   # → хранится как dict, сериализуется
```

Тип без зарегистрированного конвертера и без встроенной поддержки кидает
`TypeError` при `serialize()`/`dumps()` — он никогда не отбрасывается молча
и не превращается в `None`.

### Геометрия (WKB) — shapely / geoalchemy2

Значение типа `shapely`-геометрии или `geoalchemy2.WKBElement` сериализуется
как сырые байты WKB независимо от того, какая библиотека его создала. На
стороне чтения то, в какую библиотеку это восстановится, управляется
`md.set_geo_backend(...)`, а не тем, кто это записал:

```python
md.set_geo_backend("shapely")       # или "geoalchemy2", или None чтобы сбросить
```

- Установлена только одна из двух библиотек на стороне чтения → используется
  автоматически, вызов не нужен.
- Установлены обе → **обязателен вызов** — десериализация геометрии без
  предварительного вызова кидает `ValueError` (неоднозначно, во что
  восстанавливать).
- Не установлена ни одна → возвращаются сырые байты WKB, данные не теряются.

`md.ShapelyWKB(raw_bytes)` / `md.GeoAlchemyWKB(raw_bytes)` позволяют
пометить сырые байты WKB для хранения без установленной библиотеки на
стороне записи — те же правила восстановления действуют при чтении.

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
