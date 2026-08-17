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
| `d[key]` vs `m[key]` | 246ms | 222ms | 1.11× faster |
| `d.get(key)` vs `m.get(key)` | 259ms | 275ms | 0.94× slower |
| `key in d` vs `key in m` | 235ms | 209ms | 1.12× faster |
| `d[key] = val` vs `m[key] = val` | 325ms | 300ms | 1.08× faster |
| bulk insert 1M | 367ms | 348ms | 1.05× faster |

For pure key→value scalar workloads, ModDict is at least on par with dict, and ahead on read/write/`in`/bulk insert — `.get()` is the one op still behind.

---

## Structured rows — read (100 000 rows)

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| `d[key]` vs `mn[key]` (full row) | 16ms | 20ms | 0.79× slower |
| `d[key]["age"]` vs `mn[key]["age"]` | 24ms | 25ms | 0.97× slower |
| `d[k]["meta"]["details"]["rank"]` vs same on mn | 41ms | 38ms | 1.09× faster |

`mn[key]` returns a **RowProxy** when field indices are active — a thin wrapper that intercepts writes to keep indices consistent. Read access delegates transparently to the stored dict.

---

## Structured rows — write (100 000 rows)

| Operation | dict | ModDict | ratio |
|-----------|------|---------|-------|
| bulk insert 100k rows | 34ms | 33ms | 1.03× faster |
| `d[k]["age"] = 99` vs `mn[k]["age"] = 99` | 26ms | 29ms | 0.87× slower |
| `d[k]["meta"]["details"]["rank"] = 99` vs same on mn | 35ms | 41ms | 0.86× slower |

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

Supported types: `None` `bool` `int` `float` `str` `bytes` `bytearray` `list` `tuple` `set` `frozenset` `dict`
`datetime.datetime` (naive as written; tz-aware keeps its offset) `datetime.date` `datetime.time` `datetime.timedelta`
`decimal.Decimal` `uuid.UUID` `pathlib.Path` `pathlib.PurePosixPath` `pathlib.PureWindowsPath`
WKB geometry (shapely / geoalchemy2, SRID preserved — `set_geo_backend()`)
SQLAlchemy `Row` / `RowMapping` / result lists with the primary key recorded (`set_row_backend()` / `set_rowset_backend()`)
plus custom types via `md.register_converter(MyType, encoder)`

Per-type cost is normalized against a small-int baseline in
`tests/bench_serialize_types.py`; after the per-value stdlib/geo import
probes were removed (v0.8.12+) the remaining per-type multiples are the
Python constructors themselves. A SQL rowset writes column names once per
set: 0.31µs/row and 36 bytes/row for a 3-column table vs 0.97µs and 71
bytes for the same rows as a plain list of dicts.

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
`filter()` on a non-terminal wildcard is **pruned**: only the inner keys that
actually matched survive in the result, so the dict comparisons below build
the same pruned structure (not an `any()` existence check) for a fair,
apples-to-apples comparison.

| Operation | dict equivalent | dict | ModDict 1st | ModDict 2nd+ |
|-----------|-----------------|------|-------------|--------------|
| `filter("?.user_id").eq(5)` | pruned `{gk: {rk: rv for rk,rv in gv.items() if rv["user_id"]==5}}` | 9ms | 1ms (**10.8×**) | 1ms (**15.7×**) |
| `filter("?").eq("r1")` — terminal, key exists | pruned `{gk: {"r1": gv["r1"]}}` for matching groups | ~0ms | ~0ms (**1.5×**) | ~0ms (**1.6×**) |
| `filter("g1.?.user_id").eq(5)` — anchor | `{rk: rv for rk,rv in d["g1"].items() if rv["user_id"]==5}` | ~0ms | ~0ms (**2.1×**) | ~0ms (**3.6×**) |
| `filter("?.?.status").eq("Active")` — 2 wildcard levels | pruned 3-level nested dict comprehension | 12ms | 7ms (**1.6×**) | 7ms (**1.7×**) |
| `.eq(5, returns="rows_here")` | `[r for gv in d.values() for r in gv.values() if r["user_id"]==5]` | 7ms | 10ms | 10ms |
| `.eq(5, returns="values", value_field="score")` | `[r["score"] for gv in d.values() for r in gv.values() if r["user_id"]==5]` | 7ms | 10ms | 10ms |

**Path semantics:**
- `"?.user_id"` — `?` skips one key level, filters by `user_id` in the value. Builds an index → **15.7× faster** on repeated calls.
- `"?"` (terminal) — checks if the value equals an inner **key**. Useful for "does this group contain row X?"
- `"g1.?.user_id"` — anchor: first segment is a known outer key, scan scoped to that row only.
- `"?.?.status"` — one `?` per nesting level (no implicit multi-level skip — chain wildcards explicitly for deeper structures). Uses the same index-backed reconstruction as a single `?`, no rescan.
- `returns="rows_here"` — returns the inner dicts at the level where the field lives (no index, linear scan).
- `returns="values", value_field="score"` — extracts one field from each matching inner dict.

Non-terminal wildcard EQ (any number of `?` levels) and terminal `?` reconstruct
the pruned result directly from the index — no rescanning the row. `ne()` and
range ops (`lt`/`gt`/...) on wildcard paths have no index shortcut yet and fall
back to a full scan on every call.

---

## to_dict / dumps / loads (100 000 rows)

| Operation | Notes | Time |
|-----------|-------|------|
| `mn.to_dict()` | plain dict, bypasses RowProxy | 23ms |
| `dict(mn)` | keys()+getitem, may return RowProxy if any index exists | 24ms (1.06× slower) |
| `md.dumps(plain_dict)` | generic single-value format | 169ms |
| `md.dumps(mn)` | ModDict's native container format (same as `mn.serialize()`) | 271ms |
| `md.loads(dumps(dict))` | → `dict` | 488ms |
| `md.loads(dumps(mn))` | → `ModDict` | 576ms |

`md.dumps()`/`md.loads()` are module-level functions for serializing **any**
supported object, not just a whole `ModDict`. A `ModDict` round-trips back as
a `ModDict`; everything else round-trips as itself. There's no implicit
`ModDict` → `dict` conversion — call `mn.to_dict()` first if that's what you
want serialized.

---

## Links: link() / follow() / "->" filter (100 000 orders × 10 000 customers)

Two-table `ModDict`: `orders: {pk: {customer_id}}`, `customers: {pk: {name}}`,
declared with `mn.link("orders.?.customer_id", "customers.?")`.

| Operation | dict equivalent | dict | ModDict | ratio |
|-----------|-----------------|------|---------|-------|
| `link()` declare + validate (100k orders) | — one-time cost, no dict equivalent | — | 51ms | — |
| full join: `{ok: custs[o["customer_id"]] for ok,o in orders.items()}` vs `mn.follow(...)` | 15ms | 12ms | **1.29×** faster |
| join+filter by target name (`customers[o["customer_id"]]["name"]==target`) vs `filter("orders.?.customer_id->name").eq(target)` — 1st call | 17453µs | 13.7µs | **1274×** faster |
| same — 2nd+ call (target index reused too) | 17453µs | 3.5µs | **5056×** faster |

`link()`'s one-time cost validates every existing row resolves to a real
target row — same class of cost as building a `FieldIndex`, paid once no
matter how many `follow()`/`filter()` calls come after.

`follow()` still visits every source row, same shape as the manual join — its
win over the dict version is the usual per-row overhead difference, not a
different algorithm.

`filter("...->name").eq(target)` **is** a different algorithm: it never scans
the orders table. It resolves "which customers match" first (indexed lookup
on 10 000 customers), then reverse-looks-up "which orders point at those
customers" through the index `link()` already built — O(matches), not
O(orders). With 1 target customer out of 10 000 (≈10 matching orders out of
100 000 total), that touches on the order of 10 rows regardless of table
size — the microsecond-scale timings above (500-rep average, not a single
noisy ms-rounded sample) confirm it: **the fast path's cost tracks the number
of matches, not the size of the orders table**.

---

## Cursors (reactive views)

Measured against the closest hand-rolled analog: a Python `list` kept sorted
by hand (`bisect.insort` per insert, `append`+`sort()` for a batch) — what a
GUI table model does without a cursor. Setup calls (`set_sort`/`set_filter`/
`set_group`) run once; the mutation rows are the per-keystroke path.

| Operation | list | cursor | ratio |
|-----------|------|--------|-------|
| `cursor()` creation (any anchor size, 1k–100k rows) | — | 1.4–1.9µs | constant, doesn't scan the table |
| `bisect.insort` vs `insert()` (1k / 10k / 50k / 100k rows, sort active) | 1.8 / 5.7 / 15.6 / 26.3µs | 4.2 / 7.3 / 16.7 / 21.6µs | slower at 1k (both sub-5µs), ~equal by 50k, **1.22×** faster at 100k |
| `bisect.insort` × 5 000 in a loop vs `append`+`sort()` once (list, its own batch strategy) | 10ms | 1ms | 8.2× faster |
| `append`+`sort()` (list) vs `insert_batch()` (cursor), both batch-style | 1ms | 1ms | **1.27×** faster |
| `insert()` × 5 000 in a loop vs `insert_batch()` once | 366ms | 1ms | **374×** faster |
| `sorted(list)` vs `set_sort()` bootstrap (1k / 10k / 50k / 100k rows) | 0 / 2 / 12 / 29ms | 0 / 6 / 40 / 101ms | ~3.0–3.4× slower — one-time |
| list comprehension vs `set_filter("status").eq(...)` bootstrap (1k / 10k / 50k / 100k rows) | 0 / 1 / 4 / 7ms | 0 / 3 / 20 / 49ms | ~3–7× slower — one-time |
| `list.sort(key=group)` vs `set_group()` bootstrap (1k / 10k / 50k / 100k rows) | 0 / 2 / 8 / 17ms | 0 / 3 / 22 / 56ms | ~1.2–3.3× slower — one-time |
| `insert()` with 0 / 1 / 5 / 20 sibling cursors on the same anchor (sort active) | — | 2.7µs / 13.9µs / 44.5µs / 265µs | each sibling updates incrementally from the mutation |
| find-by-key+`del` vs `delete()` under active sort (1k / 10k / 50k / 100k rows) | 25 / 368 / 4 099 / 8 879µs | 1.5 / 8.2 / 37.4 / 71.6µs | **17–124×** faster |
| find-by-key+`pop`+`insort` vs `update_row()` (sort field changes, 1k / 10k / 50k / 100k rows) | 31 / 369 / 4 304 / 8 901µs | 4.1 / 18.7 / 58.4 / 115µs | **8–78×** faster |
| `bisect.insort`+`set.add` vs `insert()` (sort **and** filter active, 1k / 10k / 50k / 100k rows) | 2.9 / 7.8 / 15.8 / 25.8µs | 4.2 / 14.3 / 25.5 / 45.7µs | ~1.4–1.8× slower |
| per-row `mn[k]=v` × 20 000 into an indexed `ModDict` vs `mn.update()` (batched `FieldIndex` rebuild) | 258ms | 72ms | **3.56×** faster |

**Why the shape is what it is:**

- **Mutations are `O(log n)`.** A single insert/update/delete bisects the one
  affected row into/out of `sort_index` (and `visible_index` under a filter);
  every other row's relative order is unaffected, so nothing is re-sorted.
  `update_row()`/`delete()` locate the row's *old* position by bisecting on
  its sort-field values captured before the write — which is why they pull
  away from the list (whose scan-by-identity is linear) as the table grows.
- **Sibling cursors get the mutation, not a "something changed" signal** —
  each applies the same `O(log n)` step to its own view (one filter-condition
  evaluation per sibling, not one per row).
- **Bootstrap builds structures a list doesn't have** — `sort_index`
  (INCREF'd keys), `filter_membership` (hash set), `visible_index` — the
  price of `O(1)` `.at(i)` and the `O(log n)` mutations above. Paid once per
  `set_*` call, never per mutation.
- **`insert()` under sort *and* filter** pays two bisects plus one condition
  evaluation, against the list's one insort plus one set-add — the ~1.5×
  gap is that second vector, and it buys dense visible positions.
- **`insert_batch()`** writes all rows in one pass and rebuilds the view once
  — the same reason `append`+`sort()` beats an insort loop, applied on the
  cursor side.

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
| Wildcard filter (`?.field`, any number of levels) | **ModDict** — up to **15.7×** faster on repeated calls |
| `to_dict()` vs `dict(mn)` | **~equal**; `to_dict()` always bypasses RowProxy |
| `dumps`/`loads` (any object, not just ModDict) | see dedicated section — comparable to `serialize()`/`deserialize()` |
| `follow()` (declared link, resolve every source row) | **ModDict** — **1.4×** faster than a manual join |
| `filter("...->field").eq(x)` (JOIN in WHERE) | **ModDict** — O(matches) via index, not O(table size) — **1000×+** faster at low selectivity |
| Cursor `insert()` vs `bisect.insort` on a sorted list | **cursor** — roughly equal by 50k rows, **1.22× faster** at 100k |
| Cursor `insert_batch()` vs list `append`+`sort()` | **cursor** — **1.27×** faster |
| Cursor `delete()`/`update_row()` under active sort vs find-by-key + list mutation | **cursor** — **8–124×** faster, growing with table size (bisect on captured sort values vs a linear identity scan) |
| Cursor `insert()` under active sort+filter vs `bisect.insort`+`set.add` | list — ~1.4–1.8× faster |
| Cursor `set_sort()` bootstrap vs `list.sort()` | list — ~3× faster, one-time cost |
| Cursor `set_filter()` bootstrap vs list comprehension | list — ~3–7× faster, one-time cost |
| Cursor `set_group()` bootstrap vs `list.sort(key=group)` | list — ~1.2–3.3× faster, one-time cost |
