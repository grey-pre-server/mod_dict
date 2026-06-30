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
| `d[key]` vs `m[key]` | 187ms | 190ms | **~equal** |
| `d.get(key)` vs `m.get(key)` | 199ms | 244ms | 0.81× slower |
| `key in d` vs `key in m` | 170ms | 183ms | 0.93× slower |
| `d[key] = val` vs `m[key] = val` | 265ms | 267ms | **~equal** |
| bulk insert 1M | 244ms | 275ms | 0.89× slower |

For pure key→value scalar workloads, dict has a slight edge — ModDict adds a refcount and index-check overhead per insert. Use dict if your data is entirely scalar.

---

## Structured rows — read (100 000 rows)

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| `d[key]` vs `mn[key]` (full row) | 9ms | 14ms | 0.61× slower |
| `d[key]["age"]` vs `mn[key]["age"]` | 14ms | 18ms | 0.79× slower |
| `d[k]["meta"]["details"]["rank"]` vs same on mn | 29ms | 32ms | 0.90× slower |

`mn[key]` returns a **RowProxy** when field indices are active — a thin wrapper that intercepts writes to keep indices consistent. Read access delegates transparently to the stored dict.

---

## Structured rows — write (100 000 rows)

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| bulk insert 100k rows | 25ms | 27ms | **~equal** |
| `d[k]["age"] = 99` vs `mn[k]["age"] = 99` | 19ms | 26ms | 0.75× slower |
| `d[k]["meta"]["details"]["rank"] = 99` vs same on mn | 31ms | 36ms | 0.85× slower |

`mn[k]["age"] = 99` goes through RowProxy which updates FieldIndex in O(1) automatically. The overhead vs plain dict is small and buys you index consistency for free.

---

## Filter (100 000 rows)

Index is built automatically on the **first** `filter()` call and reused after.

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| `age >= 40` scan vs `filter("age").gte(40)` — 1st call | 20ms | 28ms | 0.70× |
| same — 2nd+ call (index reuse) | 20ms | 26ms | 0.78× |
| `active == True` scan vs `filter("active").eq(True)` — 1st call | 17ms | 12ms | **1.45×** faster |
| same — 2nd+ call | 17ms | 10ms | **1.63×** faster |

Boolean index lookup is O(1). Numeric range scan (`age >= 40`) approaches linear scan speed when most rows pass the predicate (~60% here); ModDict wins at higher selectivity and on larger datasets.

---

## Sort, group, select (100 000 rows)

**This is where ModDict's indexed structure delivers the largest wins.**

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| `sorted(d, key=lambda k: d[k]["age"])` vs `mn.sort_by("age")` | 52ms | 15ms | **3.54×** faster |
| `groupby active` (2 groups) | 43ms | 19ms | **2.27×** faster |
| `groupby age` (~63 unique values) | 1033ms | 19ms | **55×** faster |
| `{k: {f: d[k][f] for f in fields} for k in d}` vs `mn.select([...])` | 201ms | 93ms | **2.15×** faster |

Index is built automatically on the first `sort_by` / `group_by` call and reused on subsequent calls. The group-by win grows with cardinality — the dict approach scales O(n·groups), ModDict is O(n).

---

## Update / merge (100 000 rows)

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| key→key `*,*` (update existing only) | 49ms | 38ms | **1.29×** faster |
| key→key `?,?` (update existing + insert new) | — | 37ms | — |
| update by field value (`user_id` → `*`) | 60ms | 26ms | **2.29×** faster |

`mn.update(other, "*", "*")` — updates only keys already present in self.  
`mn.update(other, "?", "?")` — same, plus **inserts** keys from `other` missing in self.  
Both skip rebuilding indices for untouched fields.

---

## Serialization (100 000 rows)

Supported types: `None` `bool` `int` `float` `str` `bytes` `list` `set` `frozenset` `dict`
`datetime.datetime` `datetime.date` `datetime.time`
`decimal.Decimal` `pathlib.Path` `pathlib.PurePosixPath` `pathlib.PureWindowsPath`
plus custom types via `md.register_converter(MyType, encoder)`

| format | serialize | deserialize | size |
|--------|-----------|-------------|------|
| **ModDict binary** | 260ms | 410ms | 32.5 MB |
| json | 252ms | 341ms | 23.7 MB |
| pickle | 756ms | 488ms | 25.4 MB |

ModDict binary supports the full Python type set (date, bytes, Decimal, Path, …) without a custom encoder — unlike json. Compared to pickle: **2.9× faster serialize, 1.2× faster deserialize**.

---

## Iteration (100 000 rows)

| dict | ModDict | ratio |
|------|---------|-------|
| `for k in d` 2ms | `for k in mn` 3ms | 0.67× slower |

ModDict scans a flat hash table; dict uses a compact split-index design. The gap closes as entry size decreases — ModDict's 24-byte `OuterEntry` matches CPython dict entry size exactly. If iteration is your bottleneck, collect keys once with `mn.keys()`.

---

## New in this release (100 000 rows)

| Operation | dict equivalent | dict | ModDict | ratio |
|-----------|-----------------|------|---------|-------|
| `ModDict.from_rows(rows, key="id")` | `{r["id"]: r for r in rows}` | 17ms | 86ms | 0.20× slower |
| `mn.copy()` (deep copy) | `copy.deepcopy(d)` | 1313ms | 169ms | **7.79×** faster |
| `mn.at(0)` (first by insertion order) | `list[0]` | ~0ms | ~0ms | **2.37×** faster |
| `mn.at(-1)` (last by insertion order) | `list[-1]` | ~0ms | ~0ms | **2.27×** faster |

`from_rows` is slower than a dict comprehension because it indexes each row at insert time. `copy()` wins by **7.8×** over `deepcopy` — ModDict's deep copy recurses only into dict values and skips Python's general-purpose copier overhead.

---

## Wildcard filter (1 000 outer × 100 inner rows)

Dataset: `{group_key: {row_key: {user_id, score, ...}}}` — two-level nesting.

| Operation | dict equivalent | dict | ModDict 1st | ModDict 2nd+ |
|-----------|-----------------|------|-------------|--------------|
| `filter("?.user_id").eq(5)` | `{gk: gv for gk,gv in d.items() if any(r["user_id"]==5 for r in gv.values())}` | 2ms | 0.1ms (**13×**) | 0.1ms (**16×**) |
| `filter("?").eq("r1")` — inner key exists | `{gk: gv for gk,gv in d.items() if "r1" in gv}` | ~0ms | ~0ms (**2.2×**) | ~0ms (**3.2×**) |
| `filter("g1.?.user_id").eq(5)` — anchor | `{rk: rv for rk,rv in d["g1"].items() if rv["user_id"]==5}` | ~0ms | ~0ms (**2.0×**) | ~0ms (**4.8×**) |
| `.eq(5, returns="rows_here")` | `[r for gv in d.values() for r in gv.values() if r["user_id"]==5]` | 7ms | 9ms | 9ms |
| `.eq(5, returns="values", value_field="score")` | `[r["score"] for gv in d.values() for r in gv.values() if r["user_id"]==5]` | 7ms | 9ms | 9ms |

**Path semantics:**
- `"?.user_id"` — `?` skips one key level, filters by `user_id` in the value. Builds an index → **16× faster** on repeated calls.
- `"?"` (terminal) — checks if the value equals an inner **key**. Useful for "does this group contain row X?"
- `"g1.?.user_id"` — anchor: first segment is a known outer key, scan scoped to that row only.
- `returns="rows_here"` — returns the inner dicts at the level where the field lives (no index, linear scan).
- `returns="values", value_field="score"` — extracts one field from each matching inner dict.

---

## Summary

| Use case | Recommendation |
|----------|----------------|
| Scalar store only | dict — ModDict adds unnecessary overhead |
| Nested dict field reads | dict — marginally faster |
| Bulk insert rows | **~equal** |
| Sort by field | **ModDict** — **3.5×+** faster (grows with dataset size) |
| Group by field | **ModDict** — up to **55×** faster |
| Filter (boolean fields) | **ModDict** — **1.6×** faster |
| Filter (numeric range, dense) | ~equal; ModDict wins at high selectivity |
| Select (field projection) | **ModDict** — **2.1×** faster |
| Update / merge by key | **ModDict** — **1.3–2.3×** faster |
| Deep copy | **ModDict** — **7.8×** faster than `deepcopy` |
| Index access `at(i)` | **ModDict** — O(1) via insertion-order vector |
| Serialization | ModDict if you need date/bytes/Decimal; json for smallest file |
| Asyncio shared cache | **ModDict** — zero-copy `PyObject*` refs, no GC pressure on reads |
| Init from dict / ModDict / Mapping | dict is fastest; ModDict copy ~2× slower (full re-index) |
| Wildcard filter (nested 2-level) | **ModDict** — up to **16×** faster on repeated calls |
