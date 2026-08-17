# mod_dict

A Python extension (C++) that stores nested dictionaries by reference and provides indexed filtering, sorting, grouping and merging — without converting to a DataFrame or a database. Also provides live, mutation-aware **cursors** for backing reactive GUI tables (Qt or otherwise) without hand-rolling index bookkeeping.

**[Русская версия](https://github.com/grey-pre-server/mod_dict/blob/master/README_RU.md)** · **[Benchmarks](https://github.com/grey-pre-server/mod_dict/blob/master/BENCHMARK.md)** · **[API reference (type stubs)](https://github.com/grey-pre-server/mod_dict/blob/master/src/mod_dict.pyi)** · **[PyPI](https://pypi.org/project/mod_dict/)**

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
slim   = mn.select_mass(["age", "score"])             # → new ModDict
cols   = mn.select_mass(["age", "score"], returns="values")  # → [[age,...], [score,...]] (columnar)
ages   = mn.select("age", returns="values")      # → [age, age, ...] — single field, already flat

# Dot-notation paths in sort / group / select
mn.sort_by("meta.details.rank")
mn.group_by("meta.level")
mn.select_mass(["meta.details.rank", "score"])

# Links between rows — declare, traverse, JOIN-in-WHERE, ON DELETE
mn.link("orders.?.customer_id", "customers.?")
mn.follow("orders.?.customer_id")                # → ModDict of resolved customers
mn.filter("orders.?.customer_id->name").eq("Alice")  # JOIN in WHERE, chainable ("->name->city")

# Cursors — live, mutation-aware views backing a reactive GUI table
# (anchored at an existing nested {key: row} table — see "Cursors" below)
orders = mn.cursor("some_table")
orders.set_sort("amount")
new_index, row = orders.insert("o9", {"amount": 15})  # → (int | None, dict) — landing position, row
orders.connect("insert", lambda payload: qt_model.apply_insert(payload))

# Update from another collection
mn.update(other)                                 # bulk insert (like dict.update)
mn.update(other, "*", "*")                       # key-to-key merge (only updates existing)
mn.update(other, "?", "?")                       # key-to-key: also inserts new keys from other
mn.update(other, "user_id", "id")               # field-to-field
mn.update(other, "*.geo.lat", "*.geo.lat")      # deep field only

# Build from a list of rows or any Mapping
mn3 = md.ModDict.from_rows(users, key="id")      # {r["id"]: r for r in users}
mn4 = md.ModDict(other_mn)                       # copy from ModDict
mn5 = md.ModDict(OrderedDict(...))               # any Mapping accepted

# Deep copy (7.8× faster than deepcopy)
backup = mn.copy()

# Index access by insertion order
mn.at(0)    # first value
mn.at(-1)   # last value

# Binary serialization (full Python type set — date, bytes, bytearray, tuple, uuid, Decimal, Path, …)
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

A **cursor** (`mn.cursor(path)`) is a `ModDict` instance anchored at an existing nested table inside another `ModDict`'s storage — it has no storage of its own; every read/write routes straight through to the parent's raw dict. It exists to back reactive GUI tables: stateful sort/filter/group flags maintained incrementally on each mutation, point-mutation methods (`insert`/`update_row`/`delete`/`insert_batch`) that return exactly the diff a GUI needs, and a framework-agnostic `connect()` for push-based reactivity. See [Cursors](#cursors-reactive-views-for-gui-tables) below.

## When it fits

- Collection of records with a fixed (or semi-fixed) schema.
- You need indexed filter, sort, group, or field-level merge without pandas/SQL.
- Write once, query many times.
- In-process cache shared across asyncio coroutines — zero-copy, no GC pressure on reads.
- Backing a GUI table (Qt or otherwise) that needs to react to inserts/updates/deletes without hand-rolled index/order bookkeeping — see [Cursors](#cursors-reactive-views-for-gui-tables).

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
len(mn)

# Iteration
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

# "->" — JOIN a declared link() mid-path, chainable, index-accelerated on .eq()
mn.filter("orders.?.customer_id->name").eq("Alice")
mn.filter("orders.?.customer_id->company_id->name").eq("Acme")   # 2 hops

# Sort / select / group — support dot-notation paths
mn.sort_by("age", reverse=False, returns="rows")        # default → [row, ...]
mn.sort_by("age", returns="parent_keys")                # → [key, ...]
mn.sort_by("age", inplace=True)                         # reorders mn in-place, returns None
mn.sort_by("meta.details.rank", returns="values")       # → [val, ...]


mn.select("age")                                        # → {pk: age, ...} — single field, flat, no per-row wrapper
mn.select("age", returns="values")                      # → [age, ...]

mn.select_mass(["age", "name"])                         # → new ModDict, keyed by each path's last segment
mn.select_mass({"user_age": "age"})                     # explicit labels — also disambiguates collisions
mn.select_mass(["age", "meta.level"], returns="values") # → [[age,...], [meta.level,...]] (columnar)
mn.select_mass(["orders.?.customer_id->name"])                # wildcard/"->" default: lands on target table → {"customers": {100: {...}, ...}}
mn.select_mass(["orders.?.customer_id->name"], returns="rows_here")  # flat extraction instead → {order_pk: {"name": ...}}

mn.group_by("active")                                   # → {value: ModDict, ...}
mn.group_by("meta.level")

# Links between rows (self-references allowed) — see "Links between rows" below
mn.link("orders.?.customer_id", "customers.?", on_delete="restrict")  # restrict|cascade|set_null
mn.follow("orders.?.customer_id")                        # → ModDict of resolved target rows

# Cursors — live views for GUI tables, see "Cursors" below
orders = mn.cursor("u1.orders")                          # anchor must already exist
orders.set_sort("amount") / orders.set_group("status")   # each -> list[(old_index, new_index)] of what moved, also fired as "reorder"
orders.set_filter("status").eq("shipped")   # same operators as filter(): eq/ne/lt/lte/gt/gte/between/in_/text_search — evaluated in C++ per row
orders.set_filter("?").predicate(lambda r: r["amount"] > 100 and r["status"] == "shipped")  # "?" = the whole row; predicate() for what no operator expresses
orders.set_sort(None) / orders.set_group(None) / orders.set_filter(None)   # one reset form for all three — back to what the others still impose
orders.insert(key, row)          # -> (int | None, dict) = (new_index, row)
orders.delete(key)               # -> int | None (old_index)
orders.update_row(key, changes)  # -> ((old_index, new_index), changes) — changes: {field: new_value}
orders.insert_batch({key: row, ...})  # -> list[(int|None, dict)] = [(new_index, row), ...], one write pass, one connect() event
orders.insert_batch([row, ...], key="id")  # same, key= extracts each row's outer key instead of a pre-keyed dict
orders.insert_batch([row, ...], key=("user_id", "group_id"))  # tuple of field names -> composite tuple key per row
orders.connect("insert" | "update" | "delete" | "reorder", callback)
orders.disconnect(event?, callback?)  # -> int removed; (event,cb) / (event) / () — one, the event's, or all listeners
orders.view_keys() / orders.view_values() / orders.view_items()  # current sort/filter VIEW — [key] / in / del stay raw, unaffected by it

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
mn.load_rows(rows, key="id", path="users")       # writes into an EXISTING mn: mn["users"] = {r["id"]: r for r in rows}
# key= as a tuple of field names builds a composite tuple key (DB composite PK) —
# same for from_rows/load_rows/cursor.insert_batch:
md.ModDict.from_rows(rows, key=("user_id", "group_id"))   # {(r["user_id"], r["group_id"]): r for r in rows}

# Serialize — see "Serialization" below for the type table
mn.serialize() / mn.deserialize(data)          # data → self, returned for chaining
mn.to_dict()                                   # → plain dict (bypasses RowProxy)
md.dumps(obj) / md.loads(data)                 # serialize any object; ModDict round-trips as ModDict
md.register_converter(MyType, fn)              # custom types, converted at insert time
md.set_geo_backend("shapely"|"geoalchemy2"|"wkb_bytes")     # what a WKB geometry deserializes into
md.set_row_backend("row"|"dict"|"tuple"|"list")               # what a SQLAlchemy Row deserializes into
md.set_rowset_backend("list"|"tuple"|"dict"|"mod_dict")       # what a fetchall()/mappings().all() deserializes into

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

### Space is an alias for `.` in path strings

Every path-accepting API (`filter`, `sort_by`, `group_by`, `select`, `update`,
`create_index`, ...) treats whitespace as a literal alias for `.` — `"meta.level"`
and `"meta level"` are identical. No collapsing: `"meta   level"` (extra spaces)
is equivalent to `"meta...level"` (extra dots) — both produce empty segments
in the middle, matching nothing. Same strictness for both separators.

```python
mn.filter("meta level").eq(5)          # same as mn.filter("meta.level").eq(5)
mn.filter("g1 ? user_id").eq(1)        # same as mn.filter("g1.?.user_id").eq(1)
```

A field name that itself contains a literal `.` or space can't be written as
a string path — pass a **tuple/list** instead, where each element is taken
as one exact segment with no splitting at all:

```python
mn.filter(("first name",)).eq("alice")   # field literally named "first name"
mn.filter(("a.b",)).eq(1)                # field literally named "a.b"
```

Also new: `mn.to_dict()` returns a plain `dict` (bypasses RowProxy — useful
for libraries like Pydantic that require an actual `dict`), and module-level
`md.dumps(obj)` / `md.loads(data)` serialize *any* supported object, not just
a whole `ModDict` — a `ModDict` round-trips back as a `ModDict`, everything
else as itself. No implicit `ModDict` → `dict` conversion; call `mn.to_dict()`
first if that's what you want serialized.

## Links between rows

Declare a foreign-key-style relationship between rows in the same `ModDict` —
including self-references, e.g. an employee hierarchy via `manager_id`:

```python
mn = md.ModDict({
    "orders":    {1: {"customer_id": 100}, 2: {"customer_id": 200}},
    "customers": {100: {"name": "Alice"},  200: {"name": "Bob"}},
})
mn.link("orders.?.customer_id", "customers.?")            # pk-based
mn.link("orders.?.customer_id", "customers.?.email")      # or match by a non-pk field

mn.follow("orders.?.customer_id")                         # → ModDict of resolved customers
mn.follow("orders.?.customer_id", keys=[1])                # only for order 1
mn.follow("orders.?.customer_id", values=[100, 200])        # resolve ids directly, no table scan

# ON DELETE — SQL-style, self-reference-safe (a cycle breaks itself on the first delete)
mn.link("employees.?.manager_id", "employees.?", on_delete="cascade")
del mn["employees"][1]                                      # cascades to every direct/indirect report

# JOIN in WHERE — "->" resolves a declared link mid-path, chainable across hops
mn.filter("orders.?.customer_id->name").eq("Alice")         # orders whose customer's name is "Alice"
mn.filter("orders.?.customer_id->company_id->name").eq("Acme")  # 2 hops
mn.select("orders.?.customer_id->name")                     # default returns="rows": lands on target table → {"customers": {100: {...}, ...}}
```

`on_delete` is `"restrict"` *(default — refuses if still referenced)*,
`"cascade"` *(deletes referencing rows too, recursively)*, or `"set_null"`
*(clears the reference field on referencing rows)*. A `None`/missing FK is
never a dangling-reference error — same as a nullable SQL foreign key. Both
`link()` (at declaration, against existing data) and every later write to the
source table validate references immediately — not just at delete time.

`follow()`'s `keys=` composes multi-hop walks of *unknown* depth (e.g.
walking a hierarchy to its root) via an explicit Python loop:

```python
managers      = mn.follow("employees.?.manager_id")                       # 1 hop
skip_managers = mn.follow("employees.?.manager_id", keys=managers.keys()) # 2 hops
```

`->`, by contrast, is for a *statically known* number of hops in one
expression and works inside `filter()`/`select()` directly — `.eq()` is
index-accelerated (no table scan); `.ne()/.lt()/.between()/.in_()/...` fall
back to a scan of the anchor table. `returns="rows_here"`/`"values"` report
data from the **anchor** row (e.g. `orders`, not wherever the hop lands),
one entry per matching anchor row, not deduplicated by target.

`select()`/`select_mass()`'s default (`returns="rows"`) works differently
from `filter()`'s: a wildcard/`->` field doesn't extract a value — it **lands
on and keeps the table** the field resolves to (multi-hop chains `follow()`
under the hood; a field with no `->` hop just keeps its own anchor table,
unchanged). With `select_mass()`, fields resolve independently and merge
into one ModDict — mixing hop and non-hop fields, or fields anchored on
different tables, is fine:

```python
mn.select_mass(["workgroup.?.group_id->name"])                              # → {"user_group": {100: {...}, ...}}
mn.select_mass(["workgroup.?.group_id->name", "workgroup.?.status"])        # → {"user_group": {...}, "workgroup": {...}}
mn.select_mass(["workgroup.?.group_id->name"], returns="rows_here")         # → {1: {"name": "Engineering"}, ...} (old flat extraction)
```

Both a `->`-containing `filter()` and a table-landing `select()`/
`select_mass()` result are themselves chainable — a further
`.filter("user_group.?.field->...")` or `.select()`/`.select_mass()` call
relays the whole call up to the root ModDict and
intersects with the already-narrowed rows, so multi-step chains across
tables work as expected. Reverse traversal (landing on a table that
*references* the current one, rather than one it points to) isn't
supported — only the direction a `link()` was declared in.

### Many-to-many via a junction table

A junction table (`users_groups` below) is nothing special — just an
ordinary table with two `link()` declarations on it instead of one:

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
mn.link("users_groups.?.user_id", "users.?")     # matches against users' own KEY ("u1") — "id" being equal is a convention, not a requirement
mn.link("users_groups.?.group_id", "groups.?")

# every group alice belongs to: filter narrows users_groups by a JOIN
# through user_id, select projects each match's group name through group_id
mn.filter("users_groups.?.user_id->name").eq("alice") \
  .select("users_groups.?.group_id->name", returns="values")
# → ["engineering", "support"]

# a different field, through the SAME hop the filter already used
mn.filter("users_groups.?.user_id->name").eq("alice") \
  .select("users_groups.?.user_id->note", returns="values")
# → ["vip", "vip"] — one per matching membership row, not deduplicated by user
```

`link()`'s pk-based form matches a foreign-key field against the target
table's own **outer dict key** — the row's `id` field is a plain field the
link never looks at directly, so it's free to duplicate the key (as above,
matching SQL's `SELECT *` returning the id column too) or be entirely
unrelated to it — mod_dict never strips or requires it. If your foreign
key instead matches a non-pk field, `link()`'s second form covers that:
`mn.link(..., "users.?.id")` would match against the `id` *field* instead
of the key.

## Cursors (reactive views for GUI tables)

A **cursor** is a live, mutation-aware `ModDict` handle anchored at an
*existing* nested table — built to back a reactive GUI table (a Qt model,
or anything else) without hand-rolling index/order bookkeeping. It has no
storage of its own: every read and write routes straight through to the
parent's raw dict, zero-copy.

```python
mn = md.ModDict()
mn["u1"] = {"orders": {
    "o1": {"amount": 30, "status": "shipped"},
    "o2": {"amount": 10, "status": "pending"},
}}

orders = mn.cursor("u1.orders")      # anchor must already exist — raises otherwise
orders["o1"]["amount"]                # 30 — plain dict protocol, no copy
len(orders)                           # 2

orders.set_sort("amount")             # maintained incrementally from here on
orders.set_group("status")            # rows grouped, sorted by amount within each group

new_index, row = orders.insert("o3", {"amount": 20, "status": "new"})
# → (int | None, dict) — THIS row's own landing position (not a list of
#   every sibling that shifted — feed it straight into Qt's
#   beginInsertRows(pos, pos), Qt's own downstream stack already knows
#   everything after `pos` moved) paired with the row itself, so a
#   connect() listener elsewhere doesn't need a separate lookup to reach
#   it. None means an active filter excludes the row (it's still written,
#   just not visible; `row` is still returned either way).

(old_index, new_index), changes = orders.update_row("o1", {"amount": 99})
# unlike insert/delete, BOTH endpoints are returned — a field-driven move is
# a Qt beginMoveRows(old, new), and there's no way to infer "from where" on
# the GUI side the way an insert/remove's implicit shift can be inferred.
# changes: {field: new_value} only for fields whose value actually changed.

old_index = orders.delete("o2")       # → int | None — the row's former position

results = orders.insert_batch({"o4": {"amount": 5, "status": "new"},
                                "o5": {"amount": 40, "status": "new"}})
# → list[(int | None, dict)], one (index, row) pair per new row, in the
# same order the batch was given — one write pass, one connect() event,
# existing rows displaced by the batch aren't individually reported
# (same reasoning as insert() above)

# same effect from a plain list, key= extracts each row's own outer key —
# saves building the {key: row} mapping yourself in a Python loop first
orders.insert_batch([{"id": "o4", "amount": 5, "status": "new"}], key="id")

orders.connect("insert", lambda payload: qt_model.apply_insert(payload))
orders.connect("update", lambda payload: qt_model.apply_update(payload))
orders.connect("delete", lambda old_index: qt_model.apply_delete(old_index))
orders.connect("reorder", lambda diff: qt_model.apply_reorder(diff))
# "reorder" is the one event that DOES carry a full list[(old,new)] diff —
# it fires whenever this view changed by anything other than THIS cursor's
# own insert/update_row/delete: its own set_sort()/set_group()/set_filter()
# (same diff they return), a sibling cursor, a root-side
# mn["u1"]["orders"][...] write, a raw cursor[key] = row — and the listener
# has no way to know which single row triggered the change. Only rows that
# moved, positions as at(i)/len() count them (dense over the visible rows
# under a filter), None on the side a row isn't visible on.

orders.disconnect("insert", handler)  # -> how many registrations were removed (0 = nothing matched)
orders.disconnect("reorder")          # drop all of one event's listeners
orders.disconnect()                   # drop everything
# matching is ==, not identity — self.method / signal.emit pair up naturally
# across connect/disconnect; a listener may disconnect itself mid-dispatch
# (takes effect from the NEXT event — the current dispatch runs on a snapshot)
```

Multiple independent cursors can point at the same anchor — each keeps its
own private sort/filter/group state and its own `connect()` listeners, but
they all see the same live data and notify each other on mutation:

```python
grid_view = mn.cursor("u1.orders")
grid_view.set_sort("amount")

summary_view = mn.cursor("u1.orders")
summary_view.set_group("status")

grid_view.insert("o9", {"amount": 5, "status": "new"})
# summary_view gets a "reorder" event too — it doesn't know insert() caused
# it, only that its own view changed

mn["u1"]["orders"]["o10"] = {"amount": 7, "status": "new"}
# root-side writes through mn[key] reach cursors as well: while any cursor is
# alive, mn[key] hands out a RowProxy (same mechanism field indices use), so
# both views resync and fire "reorder". Rows taken via at()/values()/to_dict()
# are raw dicts — writes through those need mn.reindex(key) afterwards.
```

**What a cursor supports:** the full raw dict protocol (`cursor[key]`,
`cursor[key] = row`, `del cursor[key]`, `in`, `get`, `pop`, `keys`/`values`/
`items`, `to_dict`, `copy`, `serialize`, `len()`, iteration, `.at(i)`),
`set_sort()`/`set_filter()`/`set_group()`, `connect()`, and the point-mutation
methods `insert()`/`update_row()`/`delete()`/`insert_batch()` shown above —
each returns exactly the diff a GUI model needs, computed against whatever
the cursor's own state was immediately before the call.

Everything in that raw group reads/writes the anchored table directly and is
**blind to `set_sort()`/`set_filter()`** — `keys()` returns every row in the
table's own order even under an active filter. `view_keys()`/`view_values()`/
`view_items()` are the sort/filter-aware counterparts (see below); the split
is by name so neither meaning is ever silently implied. `copy()`/`serialize()`
act on the anchored *table*, producing a standalone root `ModDict`/its bytes —
a cursor's sort/filter/connect state is presentation, not data, and is never
part of that payload.

**What a cursor does not support:** genuinely root-only operations —
`link`/`follow` (links are declared on root-level table paths; a cursor lives
*inside* one table), `reindex` (ambiguous: key relative to the root or the
anchor?), `deserialize` (replaces content wholesale). Separately,
`filter`/`sort_by`/`group_by`/`select`/`select_mass`/`update` and
`create_index`/`drop_index`/`has_index` raise because they read the root's own
storage, which a cursor never populates — call them on the root. Those are
one-shot queries that return data, and don't overlap with a cursor's
`set_sort`/`set_filter`/`set_group`, which are persistent presentation state.

`set_filter()` is fully composed into reads: with an active filter,
`len()`/iteration/`.at(i)` only see the passing rows, densely indexed
(`.at(0)` is the first *visible* row, not necessarily the first row under
the anchor) — and `insert()`/`update_row()`/`delete()`'s returned
position(s) agree with that same numbering. The whole raw group (`cursor[key]`,
`in`, `del`, `get`, `pop`, `keys`/`values`/`items`, `to_dict`, ...) is
unaffected by the filter either way — a filtered-out row is still fully
present in the underlying data, just absent from the positional/iteration view.

```python
orders.set_sort("amount")
orders.set_filter("status").eq("shipped")

for key in orders:                 # keys only, same visible/sorted order as .at(i)
    ...
orders.view_values()               # → [row, ...] — rows in that same order, no per-row cursor[key] lookup
orders.view_items()                # → [(key, row), ...] — when you need both

# the raw group doesn't know about the filter/sort at all — a filtered-out
# row is still reachable directly:
orders["o2"]["amount"]             # works even if "o2" fails the active filter
orders.get("o2")                   # same, non-raising
"o2" in orders                     # True regardless of the filter
orders.keys()                      # EVERY key, in the table's own order
"o2" in orders.view_keys()         # False — filtered out of the view
```

`view_keys()`/`view_values()`/`view_items()` are named apart from
`keys()`/`values()`/`items()` on purpose: the raw group on a cursor means
exactly what it means on a plain dict (filter/sort blind), so a method whose
name reads like plain dict access must never silently mean something else
depending on whether a filter happens to be active. `view_*` says up front
that the current sort/filter is being honored — same rows, same order, that
`__iter__`/`len()`/`.at()` already agree on.

Full method docs (return shapes, event payloads, edge cases) are in
`src/mod_dict.pyi`.

## Serialization

`mn.serialize()` → `bytes` and `md.ModDict().deserialize(data)` round-trip a
whole ModDict in a compact binary format; `md.dumps(obj)` / `md.loads(data)`
do the same for any single supported value (a plain dict, a list, a scalar…
— a ModDict comes back as a ModDict, anything else as itself).

```python
data = mn.serialize()
mn2  = md.ModDict().deserialize(data)          # returns self, chainable

blob = md.dumps({"rows": [1, 2.5, "x", None]})  # any supported value
md.loads(blob)                                  # → {"rows": [1, 2.5, "x", None]}
```

**Supported types** — every one round-trips exactly, type included:

| Group | Types |
|-------|-------|
| scalars | `None` `bool` `int` (int64 range) `float` (NaN/±inf/-0.0 preserved) `str` `bytes` `bytearray` |
| containers | `dict` `list` `tuple` `set` `frozenset` — nested to any depth; dict keys may be any hashable supported type (str, int, bytes, tuples, …) |
| temporal | `datetime.datetime` — naive comes back **as written** (no local-timezone interpretation on either side); tz-aware keeps its UTC offset; any year 1..9999, microsecond precision · `datetime.date` · `datetime.time` (keeps `tzinfo`) · `datetime.timedelta` |
| stdlib | `decimal.Decimal` `uuid.UUID` `pathlib.Path` / `PurePosixPath` / `PureWindowsPath` |
| geometry | shapely geometries, `geoalchemy2.WKBElement` — stored as WKB, SRID preserved (see below) |
| SQL results | SQLAlchemy `Row`, `RowMapping`, and lists of them (`fetchall()`, `mappings().all()`, …) — column names and primary key recorded (see below) |
| custom | anything you register via `md.register_converter()` (see below) |

Anything else raises `TypeError` at `serialize()`/`dumps()` time — a value
is never silently dropped or written as `None`. Non-value objects such as
`datetime.timezone` / `tzinfo` are in that category too.

### Custom type converters

Converters are applied **at insert time** — values are converted before
storage, so they survive `serialize()` and everything else that reads the
row. MRO is walked: a converter for a base class also applies to subclasses.

```python
md.register_converter(MyType, lambda obj: obj.to_dict())
mn["key"] = {"value": MyType(...)}   # → stored as the dict, serializable
```

The converter's output must itself be a supported type (any of the table
above). Converters are the extension point for domain types; the built-in
geometry and SQL handling below are the two cases where mod_dict knows the
type well enough to do better than a one-way conversion — they are
reconstructed on read into a *chosen* type, not flattened.

### Geometry (WKB) — shapely / geoalchemy2

A `shapely` geometry or `geoalchemy2.WKBElement` value serializes as raw WKB
bytes regardless of which library produced it. If the geometry carries an
SRID as an attribute (rather than inside the bytes), it is folded into the
bytes as EWKB on write, so it survives the round-trip. On the reading side,
what it turns back into is controlled by `md.set_geo_backend(...)`, not by
which library wrote it:

```python
md.set_geo_backend("shapely")       # -> shapely geometry
md.set_geo_backend("geoalchemy2")   # -> WKBElement
md.set_geo_backend("wkb_bytes")     # -> the raw WKB bytes, unparsed
md.set_geo_backend(None)            # clear -> auto-detect (see below)
```

`"wkb_bytes"` needs neither library installed and skips the per-value
parse/allocate cost entirely — the right choice when the geometry is only
being relayed onward (to another service, straight back into a database) and
never inspected geometrically. It also makes the result independent of what
happens to be `pip install`ed in a given environment.

Without an explicit choice, reconstruction depends on the reading side:

- Only one of the two libraries installed → that one is used automatically,
  no call needed.
- Both installed → **required** — deserializing a geometry without calling
  this first raises `ValueError`. Deliberately an error rather than a guess:
  the choice decides the returned *type*, so guessing would be a source of
  silent wrong results.
- Neither installed → the raw WKB `bytes` come back, no data loss. Prefer
  stating `"wkb_bytes"` outright if that's what you actually want, rather
  than relying on a library being absent.

`md.ShapelyWKB(raw_bytes)` / `md.GeoAlchemyWKB(raw_bytes)` let you tag raw
WKB bytes for storage without either library installed on the writing side —
same reconstruction rules apply on read.

### SQL query results — SQLAlchemy Row / RowMapping

Whatever a query hands back can be serialized **as is** — no `dict(row)` /
`._mapping` conversion first — in any of the forms SQLAlchemy produces:

| You pass | What it is | Serialized as |
|----------|------------|---------------|
| `fetchone()`, `first()`, `one()`, `one_or_none()` | a `Row` | one row |
| `mappings().first()` / `.one()` | a `RowMapping` | one row |
| `fetchall()`, `all()`, `fetchmany(n)`, `list(result)`, `tuples().all()` | `list[Row]` | a rowset |
| `mappings().all()` / `.fetchall()` / `.fetchmany(n)` / `list(mappings())` | `list[RowMapping]` | a rowset |
| the live `Result` / `MappingResult` / `ScalarResult` itself, un-fetched | a result cursor | drained on the spot (`.all()`), then as above — consumed either way, exactly as your own fetch would |
| `scalar()`, `scalar_one()`, `scalars().all()` | plain values / a list of them | as the plain values they are — no rowset involved |

```python
with engine.connect() as c:
    one  = c.execute(select(users).where(users.c.id == 1)).fetchone()   # Row
    many = c.execute(select(users)).fetchall()                          # list[Row]
    maps = c.execute(select(users)).mappings().all()                    # list[RowMapping]
    live = c.execute(select(users))                                     # un-fetched Result

blob = md.dumps({"one": one, "many": many, "maps": maps, "live": live})
```

A single row is written with its **column names, values, and primary-key
columns**; a list of rows from one query is written as a **rowset** —
names and pk once, then only values per row (a 3-column table costs
~36 bytes/row vs ~71 for the same rows as a plain list of dicts, and writes
~3× faster). The primary key is taken **automatically** from the `Column`
metadata SQLAlchemy attaches to a query built from `Table`/ORM objects — you
never name it; a composite key becomes a tuple key in column order. A
`text()` query, or columns from no table (aggregates), carry no pk.

What comes back is a reading-side choice, mirroring `set_geo_backend()`:

```python
md.set_row_backend("row")       # default — a real sqlalchemy.engine.Row (._fields, ._mapping, attribute access);
                                # raises ImportError if sqlalchemy isn't importable on the reading side
md.set_row_backend("dict")      # {column: value}
md.set_row_backend("tuple")     # positional values, names dropped
md.set_row_backend("list")      # positional, mutable

md.set_rowset_backend("list")   # default — list of rows shaped per set_row_backend()
md.set_rowset_backend("tuple")
md.set_rowset_backend("dict")   # {pk: row} keyed by the auto-detected primary key
md.set_rowset_backend("mod_dict")  # a ModDict keyed by pk, rows as dicts — filter()/cursor() straight off the wire
```

`"dict"` / `"mod_dict"` raise `TypeError` on a rowset that carries no primary
key (`text()`, aggregates) — deliberately, since the pk is what makes the keyed
form meaningful; use `"list"`, or key it yourself with
`md.from_rows(rows, key=...)`. `scalar()` / `scalars().all()` results are
plain values and serialize as such — no rowset involved.

Serialization itself is not parametrized: a row is always written complete,
so any backend can be chosen at read time. Plain `dict`/`list`/`tuple`
values you built yourself are never touched by any of this. Values *inside*
a row (temporal columns, geometry columns, …) go through the same type
handling as everywhere else — a PostgreSQL `INTERVAL` arrives as
`timedelta` and round-trips, a PostGIS geometry as WKB with its SRID.

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

See [BENCHMARK.md](https://github.com/grey-pre-server/mod_dict/blob/master/BENCHMARK.md) for detailed numbers.
