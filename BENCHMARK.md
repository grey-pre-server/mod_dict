# Benchmark: ModDict vs plain Python dict

All results measured on **100 000 structured rows** (or 1 000 000 scalar entries).
Absolute timings are machine-specific; ratios are stable across platforms.

Row schema: `age int | score float | name str | active bool | tags list | joined date`
`| meta.level int | meta.badge str | meta.score_v2 float`
`| meta.details.region str | meta.details.rank int`
(3 levels of nesting, 11 leaf fields per row)

> **Architecture:** rows stored as `PyObject*` references — the same dict the caller
> passed in, with `Py_INCREF`. No deep copy. Field access via Python chaining on the
> stored reference: `mn[key]["field"]`. Writes via `mn[key]["field"] = val` go through
> **RowProxy**, which keeps FieldIndex in sync automatically.

---

## Scalar store (1 000 000 int entries)

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| `d[key]` vs `m[key]` | 175ms | 200ms | 0.88× slower |
| `d.get(key)` vs `m.get(key)` | 184ms | 254ms | 0.72× slower |
| `key in d` vs `key in m` | 214ms | 200ms | **1.07×** faster |
| `d[key] = val` vs `m[key] = val` | 262ms | 279ms | 0.94× slower |
| bulk insert 1M | 226ms | 298ms | 0.76× slower |

For pure key→value scalar workloads, dict has a slight edge — ModDict adds a refcount and index-check overhead per insert. Use dict if your data is entirely scalar.

---

## Structured rows — read (100 000 rows)

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| `d[key]` vs `mn[key]` (full row) | 8ms | 16ms | 0.50× slower |
| `d[key]["age"]` vs `mn[key]["age"]` | 12ms | 18ms | 0.67× slower |
| `d[k]["meta"]["details"]["rank"]` vs same on mn | 26ms | 31ms | 0.83× slower |

`mn[key]` returns a **RowProxy** when field indices are active — a thin wrapper that intercepts writes to keep indices consistent. Read access delegates transparently to the stored dict.

---

## Structured rows — write (100 000 rows)

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| bulk insert 100k rows | 27ms | 28ms | **~equal** |
| `d[k]["age"] = 99` vs `mn[k]["age"] = 99` | 20ms | 28ms | 0.73× slower |
| `d[k]["meta"]["details"]["rank"] = 99` vs same on mn | 30ms | 37ms | 0.82× slower |

`mn[k]["age"] = 99` goes through RowProxy which updates FieldIndex in O(1) automatically. The overhead vs plain dict is small and buys you index consistency for free.

---

## Filter (100 000 rows)

Index is built automatically on the **first** `filter()` call and reused after.

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| `age >= 40` scan vs `filter("age").gte(40)` — 1st call | 20ms | 22ms | 0.92× |
| same — 2nd+ call (index reuse) | 20ms | 23ms | 0.86× |
| `active == True` scan vs `filter("active").eq(True)` — 1st call | 17ms | 10ms | **1.67×** faster |
| same — 2nd+ call | 17ms | 10ms | **1.67×** faster |

Boolean index lookup is O(1). Numeric range scan (`age >= 40`) approaches linear scan speed when most rows pass the predicate (~60% here); ModDict wins at higher selectivity and on larger datasets.

---

## Sort, group, select (100 000 rows)

**This is where ModDict's indexed structure delivers the largest wins.**

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| `sorted(d, key=lambda k: d[k]["age"])` vs `mn.sort_by("age")` | 48ms | 15ms | **3.25×** faster |
| `groupby active` (2 groups) | 43ms | 21ms | **2.03×** faster |
| `groupby age` (~63 unique values) | 1050ms | 21ms | **49.8×** faster |
| `{k: {f: d[k][f] for f in fields} for k in d}` vs `mn.select([...])` | 187ms | 128ms | **1.46×** faster |

Index is built automatically on the first `sort_by` / `group_by` call and reused on subsequent calls. The group-by win grows with cardinality — the dict approach scales O(n·groups), ModDict is O(n).

---

## Update (100 000 rows)

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| key→key update (iterate source, find in target, call update) | 46ms | 37ms | **1.25×** faster |

`mn.update(updates, "*", "*")` skips rebuilding indices for fields not touched — existing indices are preserved as-is.

---

## Serialization (100 000 rows)

Supported types: `None` `bool` `int` `float` `str` `bytes` `list` `set` `frozenset` `dict`
`datetime.datetime` `datetime.date` `datetime.time`
`decimal.Decimal` `pathlib.Path` `pathlib.PurePosixPath` `pathlib.PureWindowsPath`
plus custom types via `md.register_converter(MyType, encoder)`

| format | serialize | deserialize | size |
|--------|-----------|-------------|------|
| **ModDict binary** | 267ms | 422ms | 32.5 MB |
| json | 257ms | 337ms | 23.7 MB |
| pickle | 746ms | 517ms | 25.4 MB |

ModDict binary supports the full Python type set (date, bytes, Decimal, Path, …) without a custom encoder — unlike json. Compared to pickle: **2.8× faster serialize, 1.2× faster deserialize**.

---

## Iteration (100 000 rows)

| dict | ModDict | ratio |
|------|---------|-------|
| `for k in d` 2ms | `for k in mn` 5ms | 0.43× slower |

ModDict iteration wraps each key in a Python string object on the fly; dict iterates over cached `PyObject*` directly. If iteration is your bottleneck, collect keys once with `mn.keys()`.

---

## Summary

| Use case | Recommendation |
|----------|----------------|
| Scalar store only | dict — ModDict adds unnecessary overhead |
| Nested dict field reads | dict — marginally faster at small scale |
| Bulk insert rows | **~equal** |
| `key in mn` | **ModDict** — 1.07× faster |
| Sort by field | **ModDict** — **3×+** faster (grows with dataset size) |
| Group by field | **ModDict** — up to **50×** faster |
| Filter (boolean fields) | **ModDict** — **1.7×** faster |
| Filter (numeric range, dense) | ~equal; ModDict wins at high selectivity |
| Select (field projection) | **ModDict** — **1.5×** faster |
| Update / merge by key | **ModDict** — **1.25×** faster |
| Serialization | ModDict if you need date/bytes/Decimal; json for smallest file |
| Asyncio shared cache | **ModDict** — zero-copy `PyObject*` refs, no GC pressure on reads |
