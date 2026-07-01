# mod_dict

A Python extension (C++) that stores nested dictionaries by reference and provides indexed filtering, sorting, grouping and merging — without converting to a DataFrame or a database.

```python
import mod_dict as md

mn = md.ModDict()
mn["alice"] = {"age": 30, "score": 9.5, "active": True,
               "meta": {"level": 5, "details": {"rank": 42}}}

# Field access via chaining on stored PyObject* — no copy
age  = mn["alice"]["age"]                        # 30
rank = mn["alice"]["meta"]["details"]["rank"]    # 42
mn["alice"]["age"] = 31                          # update in-place

# Indexed filter / sort / group — auto-builds index on first call
adults = mn.filter("age").gte(18)
active = mn.filter("active").eq(True)
rows   = mn.sort_by("age")                       # → [row, row, ...]
keys   = mn.sort_by("age", returns="parent_keys")# → [key, key, ...]
ages   = mn.sort_by("age", returns="values")     # → [18, 25, 30, ...]
groups = mn.group_by("age")                      # → {value: ModDict, ...}
slim   = mn.select(["age", "score"])             # → new ModDict
cols   = mn.select(["age", "score"], returns="values")  # → [[age,...], [score,...]] (columnar)

# Dot-notation paths in sort / group / select
mn.sort_by("meta.details.rank")
mn.group_by("meta.level")
mn.select(["meta.details.rank", "score"])

# Update from another collection
mn.update(other)                                 # bulk insert (like dict.update)
mn.update(other, "*", "*")                       # key-to-key merge (only updates existing)
mn.update(other, "?", "?")                       # key-to-key: also inserts new keys from other
mn.update(other, "user_id", "id")               # field-to-field
mn.update(other, "*.geo.lat", "*.geo.lat")      # deep field only

# Aliases — transparent second key for the same row
mn.alias("alice", "al")
mn["al"]["age"] = 32          # same row — mn["alice"]["age"] == 32
mn["al"] = {"age": 33}        # replaces row via alias
del mn["al"]                  # removes both alias and original
print(mn.aliases())           # {"al": "alice"}

# Build from a list of rows or any Mapping
mn3 = md.ModDict.from_rows(users, key="id")      # {r["id"]: r for r in users}
mn4 = md.ModDict(other_mn)                       # copy from ModDict
mn5 = md.ModDict(OrderedDict(...))               # any Mapping accepted

# Deep copy (7.8× faster than deepcopy)
backup = mn.copy()

# Index access by insertion order
mn.at(0)    # first value
mn.at(-1)   # last value

# Binary serialization (full Python type set — date, bytes, Decimal, Path, …)
data = mn.serialize()
mn2  = md.ModDict(); mn2.deserialize(data)

# Custom type converters — applied at insert time
md.register_converter(Temperature, lambda t: t.celsius)
```

## Architecture

Rows are stored as `PyObject*` references — the same dict object the caller passed in, with `Py_INCREF`. No deep copy.

```
mn["alice"] = row_dict          # Py_INCREF(row_dict), store pointer
mn["alice"]["age"]              # outer hash → PyObject* → PyDict_GetItemString
```

On top of the outer hash table, a **FieldIndex** per field powers O(1) equality and O(log n) range queries. Indices are built automatically on the first `filter()` / `sort_by()` / `group_by()` call and reused after.

## When it fits

- Collection of records with a fixed (or semi-fixed) schema.
- You need indexed filter, sort, group, or field-level merge without pandas/SQL.
- Write once, query many times.
- In-process cache shared across asyncio coroutines — zero-copy, no GC pressure on reads.

## When it does not fit

- Truly write-heavy, latency-sensitive workloads — `mn[k] = row` is slightly slower than dict (refcount + hash).
- You need concurrent writes from multiple threads.
- Schema is fully dynamic with no repeating field names.

## API at a glance

```python
# Write
mn[key] = value                          # scalar or nested dict
mn[key]["field"] = value                 # update field in-place
del mn[key]

# Read
mn[key]                                  # full row — O(1), returns stored dict ref
mn[key]["field"]                         # field via Python chaining
mn.get(key, default)
mn.pop(key)                              # remove and return value
mn.pop(key, default)                     # return default if not found

# Membership / size
key in mn
len(mn)                                  # aliases are not counted

# Iteration — aliases are hidden
for key in mn: ...
mn.keys() / mn.values() / mn.items()

# Filter (auto-builds index on first call, reused after)
mn.filter("age").eq(18)                          # age == 18
mn.filter("age").ne(18)                          # age != 18
mn.filter("age").lt(18)                          # age <  18
mn.filter("age").lte(18)                         # age <= 18
mn.filter("age").gt(18)                          # age >  18
mn.filter("age").gte(18)                         # age >= 18
mn.filter("age").between(18, 30)                 # 18 <= age <= 30
mn.filter("city").in_(["NY", "LA"])              # city in [...]
mn.filter("orders.?.status").eq("shipped")       # ? skips one key, checks value
mn.filter("?").eq("orders")                      # terminal ?: outer rows that HAVE key "orders"
mn.filter("g1.?.status").eq("shipped")           # anchor: first segment scopes scan to key "g1"
mn.filter("region.?.?.status").eq("Active")      # one ? per level — chain for deeper nesting

# non-terminal wildcard results are PRUNED: only matching inner keys survive,
# so chained filters behave as AND (not OR)
mn.filter("a.?.age").eq(30).filter("a.?.name").eq("alice")

# returns parameter: collect inner-level results without rebuilding a new ModDict
mn.filter("age").gte(18, returns="rows_here")                    # → [row, ...]
mn.filter("age").gte(18, returns="values", value_field="name")   # → [name, ...]

# Sort / select / group — support dot-notation paths
mn.sort_by("age", reverse=False, returns="rows")        # default → [row, ...]
mn.sort_by("age", returns="parent_keys")                # → [key, ...]
mn.sort_by("age", inplace=True)                         # reorders mn in-place, returns None
mn.sort_by("meta.details.rank", returns="values")       # → [val, ...]


mn.select(["age", "name"])                              # → new ModDict
mn.select(["age", "meta.level"], returns="values")      # → [[age,...], [meta.level,...]] (columnar)

mn.group_by("active")                                   # → {value: ModDict, ...}
mn.group_by("meta.level")

# Aliases
mn.alias(key, alias)                     # create alias (1 per key)
mn.aliases()                             # → {alias: original_key, ...}
del mn[alias]                            # removes alias and original

# Update from another collection
mn.update(other)                                      # bulk insert
mn.update(other, from_path, to_path, conflict="keep_right")

# Deep copy (7.8× faster than copy.deepcopy)
mn.copy()                                        # → new ModDict, rows deep-copied

# Index access by insertion order (O(1), supports negative indices)
mn.at(0)                                         # first inserted key's value
mn.at(-1)                                        # last inserted key's value

# Build from a list of dicts
md.ModDict.from_rows(rows, key="id")             # {r["id"]: r for r in rows}
md.ModDict.from_row(row)                         # normalize Mapping → plain dict

# Serialize
mn.serialize() / mn.deserialize(data)          # data → self, returned for chaining
mn.to_dict()                                   # → plain dict (bypasses RowProxy)
md.dumps(obj) / md.loads(data)                 # serialize any object; ModDict round-trips as ModDict

# Index management (optional — auto-index handles most cases)
mn.create_index("field") / mn.drop_index("field") / mn.has_index("field")
```

### Path syntax for `update` and `filter`

| Token | Meaning |
|-------|---------|
| `*`   | **scan_key** — match by outer key (update: only updates existing keys) |
| `?`   | **pass_key** — wildcard one nesting level (update: also inserts new keys; filter non-terminal: skips any key; filter terminal: checks if KEY exists at this level) |
| `key` | **anchor** (filter) — first segment matched against outer keys to scope the scan |

```python
mn.update(updates, "*", "*")                         # join by outer key (existing only)
mn.update(updates, "?", "?")                         # join + insert new keys from other
mn.update(prices, "*.meta.score", "*.meta.score")   # update one deep field

mn.filter("orders.?.status").eq("shipped")           # ? skips any order id key
mn.filter("?").eq("orders")                          # terminal ?: outer row HAS key "orders"
mn.filter("g1.?.status").eq("shipped")               # anchor "g1": scan scoped to one row
mn.filter("region.?.?.status").eq("Active")          # one ? per level, chain for deeper nesting
mn.filter("age").gte(18, returns="rows_here")        # → flat list of matching inner dicts
mn.filter("age").gte(18, returns="values", value_field="name")  # → list of field values
```

Non-terminal wildcard matches are **pruned** — the result only keeps the inner
keys that actually matched (not the whole row), so chaining `.filter(...)`
calls on wildcard paths behaves as AND, not OR. `eq()` on wildcard paths
(any depth) and terminal `?` reconstruct directly from the index with no
rescan; `ne()` and range ops (`lt`/`gt`/...) on wildcard paths fall back to a
full scan every call — there's no index shortcut for those yet.

Also new: `mn.to_dict()` returns a plain `dict` (bypasses RowProxy — useful
for libraries like Pydantic that require an actual `dict`), and module-level
`md.dumps(obj)` / `md.loads(data)` serialize *any* supported object, not just
a whole `ModDict` — a `ModDict` round-trips back as a `ModDict`, everything
else as itself. No implicit `ModDict` → `dict` conversion; call `mn.to_dict()`
first if that's what you want serialized.

### Custom type converters

Converters are applied **at insert time** — values are converted before storage, so they survive `serialize()`.
MRO is walked: a converter for a base class also applies to subclasses.

```python
md.register_converter(MyType, lambda obj: obj.to_dict())
mn["key"] = {"value": MyType(...)}   # → stored as dict, serializable
```

Built-in converters for `shapely` geometry (WKB) and `geoalchemy2` (WKBElement) activate automatically when the library is installed.

Full type stubs with docstrings are in `src/mod_dict.pyi` — visible in IDE on hover and via `help()`.

## Installation

```bash
pip install mod_dict
```

Requires Python ≥ 3.11. Pre-built wheels for Windows / Linux / macOS.
To build from source: `pip wheel .` (requires CMake ≥ 3.15 and a C++17 compiler).

## Asyncio

ModDict is safe for concurrent reads in a single-threaded event loop. Rows are stored as `PyObject*` references — no copy between coroutines, no GC pressure during reads.

```python
cache = md.ModDict()

async def handler(request):
    row = cache[request.user_id]
    return Response(row["meta"]["details"]["rank"])

async def startup():
    for key, row in data:
        cache[key] = row
```

See [BENCHMARK.md](BENCHMARK.md) for detailed numbers.
