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

Closest analog for a sorted, mutation-aware cursor view: a plain Python
`list` kept sorted by hand — `bisect.insort` for single inserts, `append`+
`sort()` for batches. That's what a hand-rolled GUI table model does without
a cursor (see `app_copy`'s `RecordTableModel`, the thing this feature
replaces) — every row below is measured against it, not in isolation.

| Operation | list | cursor | ratio |
|-----------|------|--------|-------|
| `cursor()` creation (any anchor size, 1k–50k rows) | — | 1.7–2.0µs | constant, doesn't scan the table |
| `bisect.insort` vs `insert()` (1 000 rows, sort active) | 2.1µs | 3.5µs | 0.61× (i.e. 1.7× slower) |
| `bisect.insort` vs `insert()` (10 000 rows) | 4.8µs | 8.2µs | 0.59× (1.71× slower) |
| `bisect.insort` vs `insert()` (50 000 rows) | 13.8µs | 19.7µs | 0.70× (1.43× slower) |
| `bisect.insort` × 5 000 in a loop vs `append`+`sort()` once (list, its own batch strategy) | 9ms | 2ms | 5.57× faster |
| `append`+`sort()` (list) vs `insert_batch()` (cursor), both batch-style | 2ms | 1ms | **2.59×** faster — cursor batch beats even list's own best strategy |
| `insert()` × 5 000 in a loop vs `insert_batch()` once | 250ms | 1ms | **401×** faster |
| `sorted(list)` vs `set_sort()` bootstrap (1k / 10k / 50k rows) | 0 / 2 / 10ms | 0 / 6 / 39ms | ~2.8–3.9× slower — see below |
| list comprehension vs `set_filter()` bootstrap (1k / 10k / 50k rows) | 0 / 1 / 3ms | 0 / 2 / 15ms | ~3.7–5× slower — see below |
| `list.sort(key=group)` vs `set_group()` bootstrap (1k / 10k / 50k rows) | 0 / 1 / 8ms | 0 / 4 / 26ms | ~2.1–3.2× slower — see below |
| `insert()` with 0 / 1 / 5 / 20 sibling cursors registered (sort active) | — | 1.8µs / 1.1ms / 5.5ms / 22.3ms | scales linearly with live siblings |
| per-row `mn[k]=v` × 20 000 into an indexed `ModDict` vs `mn.update()` (batched `FieldIndex` rebuild) | 237ms | 62ms | **3.83×** faster |

**`insert()` lands within ~1.4–1.7× of a bare Python list `bisect.insort`** —
close to the theoretical floor once you account for what a cursor does that
a list doesn't: write through to the parent's real dict, maintain a field
index hook, and check for registered sibling cursors. Getting here took two
rounds of fixing: a bisect fast path (`O(log n)` search + a pointer-only
`vector::insert` shift, replacing an `O(n log n)` full re-sort per single
insert), and then discovering the fast path *itself* was still reporting
every row shifted by the insertion as an explicit `(old_index, new_index)`
pair — cheap in C++, but each pair meant allocating 2 Python ints + a tuple,
which dominated the cost at scale (~25 000 pairs for a single insert into
50 000 rows). `insert()`/`delete()`/`insert_batch()` now report only the
row(s) actually being written, not every sibling a GUI's own
`beginInsertRows`/`beginRemoveRows` already re-numbers implicitly —
`update_row()` is the one exception, returning both old and new index,
because a field-driven move is a `beginMoveRows` and there's no way for the
GUI to infer the "from" side on its own.

**`set_sort()` bootstrap was originally 20–33× slower than `list.sort()`,
now ~2.8–3.9×.** The root cause wasn't "C++ vs Python" — it was that the
sort comparator re-extracted and re-converted each row's field value from
scratch on *every pairwise comparison* (`O(n log n)` extractions for a
50 000-row sort — roughly 1.5 million dict/field lookups for the same
50 000 values). Python's own `list.sort(key=...)` avoids exactly this via
decorate-sort-undecorate: compute each element's key once, then sort the
cheap precomputed keys. `rebuild_sort_index()` now does the same —
precompute every row's sort/group `ModValue`s in one `O(n)` pass, then sort
by the precomputed values. The remaining ~3–4× gap is real, inherent
`ModValue`/`PyObject` comparison overhead a raw Python `sort(key=...)` never
pays — no longer an avoidable algorithmic difference.

**`set_filter()` bootstrap is ~3.7–5× slower than a Python list
comprehension** — every row's own predicate call (`PyObject_CallOneArg`
into user Python code) plus a hash-map insert per surviving row is
inherently heavier than an interpreter-level `if` inside a comprehension,
and unlike sort there's no per-comparison redundancy to eliminate — each
row is only ever evaluated once either way. Same "rare, explicit
reconfigure" cost profile as `set_sort()`.

**`set_group()` bootstrap is ~2.1–3.2× slower than `list.sort(key=group)`**
— it shares `rebuild_sort_index()`'s precompute-then-sort path with
`set_sort()` (group value takes priority in the comparator, sort value is
the tiebreaker), so it benefited from the same optimization; the gap here
is narrower than plain `set_sort()`'s because the benchmark's list-side
comparator also does real work (a string comparison per element) rather
than being a trivial no-op.

**Sibling notification cost is linear in the number of live cursors** on
the same anchor with an active sort — each one gets its own full resync when
a *different* cursor mutates the shared data (this also got faster from the
`set_sort()` fix above, since resync uses the same `rebuild_sort_index()`).
This is an accepted tradeoff, not a bug: a GUI backed by cursors typically
has a handful of views on one table, not hundreds.

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
| Wildcard filter (`?.field`, any number of levels) | **ModDict** — up to **15.7×** faster on repeated calls |
| `to_dict()` vs `dict(mn)` | **~equal**; `to_dict()` always bypasses RowProxy |
| `dumps`/`loads` (any object, not just ModDict) | see dedicated section — comparable to `serialize()`/`deserialize()` |
| `follow()` (declared link, resolve every source row) | **ModDict** — **1.4×** faster than a manual join |
| `filter("...->field").eq(x)` (JOIN in WHERE) | **ModDict** — O(matches) via index, not O(table size) — **1000×+** faster at low selectivity |
| Cursor `insert()` vs `bisect.insort` on a sorted list | **~equal** — within 1.4–1.7× at 1k–50k rows |
| Cursor `insert_batch()` vs list `append`+`sort()` | **cursor** — **2.59×** faster |
| Cursor `set_sort()` bootstrap vs `list.sort()` | list — ~2.8–3.9× faster, but a rare one-time cost, not per-mutation |
| Cursor `set_filter()` bootstrap vs list comprehension | list — ~3.7–5× faster, but a rare one-time cost, not per-mutation |
| Cursor `set_group()` bootstrap vs `list.sort(key=group)` | list — ~2.1–3.2× faster, but a rare one-time cost, not per-mutation |
