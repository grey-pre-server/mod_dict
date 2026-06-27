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
rows   = mn.select(["age", "score"], returns="values")  # → [{"age":..}, ..]

# Dot-notation paths in sort / group / select
mn.sort_by("meta.details.rank")
mn.group_by("meta.level")
mn.select(["meta.details.rank", "score"])

# Update from another collection
mn.update(other)                                 # bulk insert (like dict.update)
mn.update(other, "*", "*")                       # key-to-key merge
mn.update(other, "user_id", "id")               # field-to-field
mn.update(other, "*.geo.lat", "*.geo.lat")      # deep field only

# Aliases — transparent second key for the same row
mn.alias("alice", "al")
mn["al"]["age"] = 32          # same row — mn["alice"]["age"] == 32
mn["al"] = {"age": 33}        # replaces row via alias
del mn["al"]                  # removes both alias and original
print(mn.aliases())           # {"al": "alice"}

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

# Membership / size
key in mn
len(mn)                                  # aliases are not counted

# Iteration — aliases are hidden
for key in mn: ...
mn.keys() / mn.values() / mn.items()

# Filter (auto-builds index on first call, reused after)
mn.filter("age").gte(18)                         # → ModDict
mn.filter("age").between(30, 40)
mn.filter("active").eq(True)
mn.filter("orders.?.status").eq("shipped")       # wildcard path

# Sort / select / group — support dot-notation paths
mn.sort_by("age", reverse=False, returns="rows")        # default → [row, ...]
mn.sort_by("age", returns="parent_keys")                # → [key, ...]
mn.sort_by("meta.details.rank", returns="values")       # → [val, ...]

mn.select(["age", "name"])                              # → new ModDict
mn.select(["age", "meta.level"], returns="values")      # → [{"age":..}, ...]

mn.group_by("active")                                   # → {value: ModDict, ...}
mn.group_by("meta.level")

# Aliases
mn.alias(key, alias)                     # create alias (1 per key)
mn.aliases()                             # → {alias: original_key, ...}
del mn[alias]                            # removes alias and original

# Update from another collection
mn.update(other)                                      # bulk insert
mn.update(other, from_path, to_path, conflict="keep_right")

# Serialize
mn.serialize() / mn.deserialize(data)

# Index management (optional — auto-index handles most cases)
mn.create_index("field") / mn.drop_index("field") / mn.has_index("field")
```

### Path syntax for `update` and `filter`

| Token | Meaning |
|-------|---------|
| `*`   | **scan_key** — match by outer key |
| `?`   | **pass_key** — wildcard one nesting level |

```python
mn.update(updates, "*", "*")                        # join by outer key
mn.update(prices, "*.meta.score", "*.meta.score")  # update only one deep field
mn.filter("orders.?.status").eq("shipped")          # status inside any order id
```

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
