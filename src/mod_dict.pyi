"""
mod_dict — Nested dictionary with indexed field queries and merge operations.

A C++ backed dictionary for structured (JSON-like) data. Designed for
field-level access, indexed filters, multi-row merge operations, and
compact binary serialization.

Typical use cases:
  - In-memory processing of JSON/API responses
  - ETL pipelines with nested update operations
  - Fast group-by / filter / sort on structured records
  - Merging datasets by key or field value

Quick start::

    import mod_dict as md

    mn = md.ModDict()
    mn["alice"] = {"age": 30, "score": 9.5, "geo": {"city": "Moscow"}}
    mn["bob"]   = {"age": 25, "score": 7.2, "geo": {"city": "Berlin"}}

    # Filter with chaining
    adults = mn.filter("age").gte(18)
    top    = mn.filter("score").between(8.0, 10.0)

    # Merge nested field
    updates = md.ModDict()
    updates["alice"] = {"score": 10.0}
    mn.merge(updates, "*", "*", conflict="keep_right")

    # Index for repeated queries
    mn.create_index("age")
    by_city = mn.group_by("geo.city")

Path strings (used by filter/sort_by/group_by/select/update/create_index/...):
  A dot ``.`` separates nesting levels; whitespace is a literal alias for
  ``.`` (normalized before splitting, no collapsing — ``"a  b"`` behaves
  exactly like ``"a..b"``: an empty segment in the middle, matches nothing) —
  ``"geo.city"`` and ``"geo city"`` are equivalent. A field name that itself
  contains a literal ``.`` or space can't be expressed as a string path;
  pass a tuple/list of exact segments instead: ``("geo.city",)`` means one
  field literally named ``"geo.city"``, not nesting into ``geo`` then ``city``.
"""
from __future__ import annotations
from typing import Any, Callable, Iterator, Literal, Sequence, overload


class FilterBuilder:
    """
    Intermediate object returned by ``ModDict.filter(field)``.

    Provides chainable comparison methods that execute the actual filter
    and return a new ``ModDict`` view sharing the same underlying store.

    All methods return a ``ModDict`` — results can be further filtered,
    sorted, or passed to other operations::

        mn.filter("age").gte(18).filter("active").eq(True)

    Path syntax for nested fields (whitespace is an alias for '.' — see the
    module docstring)::

        mn.filter("meta.score").gte(8.0)          # dot-notation
        mn.filter("orders.?.status").eq("shipped") # ? = any one key level
    """

    def eq(
        self, value: Any,
        returns: Literal["rows", "rows_here", "values"] = "rows",
        value_field: Any | None = None,
    ) -> ModDict | list[Any]:
        """
        Return rows where field **equals** value.

        Works for all types: str, int, float, bool, date, datetime, etc.

        Args:
            value:       The value to compare against.
            returns:     What to return:

                         - ``"rows"`` — new ModDict at the outer level *(default)*
                         - ``"rows_here"`` — list of dicts at the level where the
                           field lives (useful with wildcard paths)
                         - ``"values"`` — list of values extracted from each
                           matching dict (requires *value_field*)

            value_field: Field name to extract when ``returns="values"``.

        Examples::

            active_users = mn.filter("active").eq(True)
            shipped      = mn.filter("status").eq("shipped")

            # wildcard: rows at the inner level where user_id==1
            inner = mn.filter("?.user_id").eq(1, returns="rows_here")

            # extract "name" from each matching inner row
            names = mn.filter("?.user_id").eq(1, returns="values", value_field="name")

            # anchor: find outer rows that contain inner key "r1"
            has_r1 = mn.filter("?").eq("r1")
        """
        ...

    def ne(
        self, value: Any,
        returns: Literal["rows", "rows_here", "values"] = "rows",
        value_field: Any | None = None,
    ) -> ModDict | list[Any]:
        """Return rows where field **does not equal** value.

        See :meth:`eq` for the ``returns`` / ``value_field`` parameters.

        Example::

            not_cancelled = mn.filter("status").ne("cancelled")
        """
        ...

    def lt(
        self, value: Any,
        returns: Literal["rows", "rows_here", "values"] = "rows",
        value_field: Any | None = None,
    ) -> ModDict | list[Any]:
        """Return rows where field is **less than** value.

        Supported for numeric types (int, float). Uses sorted index
        when available for O(log n) performance.

        See :meth:`eq` for the ``returns`` / ``value_field`` parameters.

        Example::

            juniors = mn.filter("age").lt(25)
        """
        ...

    def lte(
        self, value: Any,
        returns: Literal["rows", "rows_here", "values"] = "rows",
        value_field: Any | None = None,
    ) -> ModDict | list[Any]:
        """Return rows where field is **less than or equal** to value.

        See :meth:`eq` for the ``returns`` / ``value_field`` parameters.

        Example::

            mn.filter("score").lte(5.0)
        """
        ...

    def gt(
        self, value: Any,
        returns: Literal["rows", "rows_here", "values"] = "rows",
        value_field: Any | None = None,
    ) -> ModDict | list[Any]:
        """Return rows where field is **greater than** value.

        See :meth:`eq` for the ``returns`` / ``value_field`` parameters.

        Example::

            seniors    = mn.filter("age").gt(60)
            high_score = mn.filter("meta.score").gt(9.0)
        """
        ...

    def gte(
        self, value: Any,
        returns: Literal["rows", "rows_here", "values"] = "rows",
        value_field: Any | None = None,
    ) -> ModDict | list[Any]:
        """Return rows where field is **greater than or equal** to value.

        See :meth:`eq` for the ``returns`` / ``value_field`` parameters.

        Example::

            adults = mn.filter("age").gte(18)
        """
        ...

    def between(
        self, lo: Any, hi: Any,
        returns: Literal["rows", "rows_here", "values"] = "rows",
        value_field: Any | None = None,
    ) -> ModDict | list[Any]:
        """Return rows where ``lo <= field <= hi`` (inclusive on both ends).

        See :meth:`eq` for the ``returns`` / ``value_field`` parameters.

        Example::

            working_age = mn.filter("age").between(18, 65)
            mid_score   = mn.filter("score").between(6.0, 9.0)
        """
        ...

    def in_(
        self, values: Sequence[Any],
        returns: Literal["rows", "rows_here", "values"] = "rows",
        value_field: Any | None = None,
    ) -> ModDict | list[Any]:
        """Return rows where field matches **any** value in the sequence.

        Performs one ``eq`` lookup per value and unions the results.
        An index on the field makes each lookup O(1).

        See :meth:`eq` for the ``returns`` / ``value_field`` parameters.

        Example::

            gold_silver = mn.filter("meta.badge").in_(["gold", "silver"])
            ages        = mn.filter("age").in_([25, 30, 35, 40])
        """
        ...


class ModDict:
    """
    High-performance nested dictionary with indexed queries and merge operations.

    **Storage model**

    Rows are stored as ``PyObject*`` references — the same dict the caller passed in,
    with ``Py_INCREF``. No deep copy. Field access via Python chaining::

        mn["alice"]["age"]          # returns stored PyObject*, O(1)
        mn["alice"]["geo"]["city"]  # any depth, same cost

    Zero-copy sharing across asyncio coroutines — no GC pressure on reads.

    **When to use ModDict vs plain dict**

    Prefer ModDict when you need:
    - Repeated ``filter`` / ``group_by`` / ``sort_by`` on large datasets (index pays off)
    - ``merge`` that updates specific nested fields across thousands of rows
    - Serialization with full Python type set (date, bytes, bytearray, tuple, uuid, Decimal, Path, …)
    - In-process cache shared across asyncio coroutines (zero-copy refs)

    Prefer plain dict when:
    - Data is flat with no repeated field queries
    - Dataset is small (< 1 000 rows)

    **Path notation**

    Nested paths use dot-notation throughout the API::

        "meta.score"           # two levels deep
        "geo.coords.lat"       # three levels deep
        "orders.?.status"      # ? matches any single intermediate key

    **Index**

    Create an index before repeated ``filter``, ``group_by``, or field-based
    ``merge`` calls. Without an index these operations still work but scan
    all rows::

        mn.create_index("age")              # simple field
        mn.create_index("orders.?.status")  # wildcard path

    **Quick example**::

        import mod_dict as md

        mn = md.ModDict()
        mn["u1"] = {"age": 30, "name": "Alice", "meta": {"level": 5}}
        mn["u2"] = {"age": 25, "name": "Bob",   "meta": {"level": 2}}
        mn["u3"] = {"age": 40, "name": "Carol",  "meta": {"level": 8}}

        mn.create_index("age")

        adults   = mn.filter("age").gte(18)          # all 3
        top      = mn.filter("meta.level").gte(5)    # u1, u3
        by_level = mn.group_by("meta.level")         # {5: ..., 2: ..., 8: ...}
        sorted_  = mn.sort_by("age")                 # [("u2", ...), ("u1", ...), ("u3", ...)]
    """

    # ──────────────────────────────────────────────────
    # Constructors
    # ──────────────────────────────────────────────────

    def __init__(self, data: dict | ModDict | None = None) -> None:
        """Create a ModDict, optionally populated from *data*.

        *data* may be a plain ``dict``, another ``ModDict``, or any
        ``Mapping``-compatible object (e.g. ``OrderedDict``, ``defaultdict``).

        Example::

            mn = md.ModDict()                          # empty
            mn = md.ModDict({"a": {"x": 1}})           # from dict
            mn = md.ModDict(other_mn)                  # shallow copy of ModDict
            mn = md.ModDict(OrderedDict([("a", {})]))  # any Mapping
        """
        ...

    @classmethod
    def from_dict(cls, d: dict) -> ModDict:
        """
        Create a ModDict from a plain Python dict.

        Top-level values that are dicts become rows (nested storage).
        Other values are stored as flat scalars.

        Example::

            mn = ModDict.from_dict({
                "alice": {"age": 30, "score": 9.5},
                "bob":   {"age": 25, "score": 7.2},
            })
        """
        ...

    @classmethod
    def from_json(cls, json_str: str) -> ModDict:
        """
        Create a ModDict from a JSON string.

        Equivalent to ``ModDict.from_dict(json.loads(json_str))``.

        Example::

            mn = ModDict.from_json('{"alice": {"age": 30}}')
        """
        ...

    @classmethod
    def from_rows(cls, rows: list[dict], key: Any) -> ModDict:
        """
        Build a ModDict from a list of row dicts using one field as the outer key.

        Typically used to index SQL result sets or any list of records by primary key.

        Args:
            rows: Iterable of dicts (or Mapping-like objects).
            key:  Field name whose value becomes the outer key.

        Example::

            rows = [
                {"id": 1, "name": "alice", "age": 30},
                {"id": 2, "name": "bob",   "age": 25},
            ]
            mn = ModDict.from_rows(rows, key="id")
            mn[1]["name"]   # "alice"
        """
        ...

    @classmethod
    def from_row(cls, row: Any) -> dict:
        """
        Convert a single Mapping-like row to a plain Python dict.

        If *row* is already a dict it is returned as-is (same object).
        Useful when you receive cursor rows or ORM objects that quack like dicts.

        Example::

            d = ModDict.from_row(cursor.fetchone())
        """
        ...

    # ──────────────────────────────────────────────────
    # Dict protocol
    # ──────────────────────────────────────────────────

    def __setitem__(self, key: Any, value: Any) -> None:
        """Insert or update a record.

        Assigns a full row (dict) or scalar::

            mn["alice"] = {"age": 30, "meta": {"level": 5}}  # row
            mn["count"] = 42                                   # scalar

        To update a specific field, chain access on the returned row::

            mn["alice"]["age"] = 99
            mn["alice"]["geo"]["city"] = "Paris"
        """
        ...

    def __getitem__(self, key: Any) -> Any:
        """Return the stored row dict or scalar. Raises ``KeyError`` if not found.

        Returns the stored ``PyObject*`` directly — no copy, O(1)::

            row = mn["alice"]           # the original dict passed to __setitem__
            age = mn["alice"]["age"]    # chain into Python dict as usual
        """
        ...

    def __delitem__(self, key: Any) -> None:
        """Remove a record by key. Raises ``KeyError`` if not found."""
        ...

    def __contains__(self, key: Any) -> bool:
        """Return True if key exists::

            if "alice" in mn: ...
        """
        ...

    def __len__(self) -> int:
        """Return the number of records (rows + scalars)."""
        ...

    def __iter__(self) -> Iterator[Any]:
        """Iterate over keys (same order as insertion)."""
        ...

    def __repr__(self) -> str: ...

    def get(self, key: Any, default: Any = None) -> Any:
        """
        Return the record for *key*, or *default* if not found.

        Example::

            row = mn.get("alice", {})
        """
        ...

    def keys(self) -> list[Any]:
        """Return list of all keys."""
        ...

    def values(self) -> list[Any]:
        """Return list of all values (rows reconstructed as dicts)."""
        ...

    def items(self) -> list[tuple[Any, Any]]:
        """Return list of ``(key, value)`` pairs."""
        ...

    # ──────────────────────────────────────────────────
    # Merge
    # ──────────────────────────────────────────────────

    def update(
        self,
        target: ModDict | dict,
        from_path: str | tuple[str, ...] | None = None,
        to_path: str | tuple[str, ...] | None = None,
        conflict: str = "keep_right",
    ) -> int | None:
        """
        Update *self* with data from *target* in-place.

        **Three calling modes:**

        ``mn.update(other)``
            Plain bulk insert — inserts or overwrites every key from *other*,
            exactly like ``dict.update()``. Returns ``None``.

        ``mn.update(other, to_path=...)``
            Path-based update where *from_path* defaults to *to_path*.
            Useful when both collections share the same field layout.
            Returns the number of matched rows.

        ``mn.update(other, from_path, to_path)``
            Full control: *from_path* navigates *self* to find the join key,
            *to_path* navigates *target* to find the matching value.
            Returns the number of matched rows.

        **Path tokens**

        +---------------------+-----------------------------------------------+
        | Token               | Meaning                                       |
        +=====================+===============================================+
        | ``"*"``             | Match by outer key                            |
        +---------------------+-----------------------------------------------+
        | ``"field"``         | Match by field value                          |
        +---------------------+-----------------------------------------------+
        | ``"*.field.sub"``   | Outer key match, copy only that nested field  |
        +---------------------+-----------------------------------------------+
        | ``"?.field"``       | ``?`` — wildcard for one nesting level        |
        +---------------------+-----------------------------------------------+

        **Conflict resolution** (path mode only)

        +---------------+---------------------------------------------------+
        | Value         | Behaviour on field conflict                       |
        +===============+===================================================+
        | ``keep_right``| Overwrite with incoming value **(default)**       |
        +---------------+---------------------------------------------------+
        | ``keep_left`` | Keep existing value                               |
        +---------------+---------------------------------------------------+
        | ``merge``     | Deep-merge nested dicts                           |
        +---------------+---------------------------------------------------+
        | ``concat``    | Concatenate lists                                 |
        +---------------+---------------------------------------------------+

        Examples::

            # Bulk insert — plain dict.update() behaviour
            mn.update({"alice": {"age": 31}})
            mn.update(other_moddict)

            # Match by outer key, copy all fields (from_path defaults to to_path)
            mn.update(other, to_path="*")

            # Match by outer key, full explicit form
            mn.update(other, from_path="*", to_path="*")

            # Match self[?] with other[?].geo  (from_path defaults to to_path="?.geo")
            mn.update(other, to_path="?.geo")

            # Match self["user_id"] field with other["id"] field
            mn.update(other, from_path="user_id", to_path="id")

            # Copy only geo.lat where outer keys match
            mn.update(other, to_path="*.geo.lat")

            # Self outer key matched against target field "ref_id"
            mn.update(ref_data, from_path="*", to_path="ref_id")
        """
        ...

    # ──────────────────────────────────────────────────
    # Filter (chaining API)
    # ──────────────────────────────────────────────────

    @overload
    def filter(self, field: str) -> FilterBuilder: ...
    @overload
    def filter(self, field: tuple[str, ...]) -> FilterBuilder: ...
    def filter(self, field: str | tuple[str, ...]) -> FilterBuilder:
        """
        Return a ``FilterBuilder`` for the given field path.

        The actual filter is executed when you call a comparison method
        on the returned builder (``.eq()``, ``.gte()``, etc.).

        For simple fields and terminal ``?`` the result is a **view** — outer
        rows are shared with the original ModDict (no copy), so mutating a
        row in the result mutates the original too.

        For non-terminal wildcard paths (``?`` skipping a key, not the last
        segment) the result is **pruned**: each outer row in the result only
        contains the inner keys that actually matched, wrapped in freshly
        built dicts — not a view of the original row. This makes chained
        wildcard filters behave as AND, not OR::

            mn.filter("a.?.age").eq(30)                    # -> {"a": {123: {...}}}  (only key 123 kept)
            mn.filter("a.?.age").eq(30).filter("a.?.name").eq("alice")
            # -> only entries matching BOTH conditions survive

        **Path syntax**

        Simple field (any nesting level via dot-notation; whitespace is an
        accepted alias for ``.`` — see the module docstring for details and
        the tuple-path escape hatch for field names containing ``.``/``  ``)::

            mn.filter("age").gte(18)
            mn.filter("meta.score").between(7.0, 10.0)
            mn.filter("geo.coords.lat").lt(0)
            mn.filter("geo coords lat").lt(0)             # same as above

        Wildcard path (``?`` = any single key at that level — one ``?`` is
        exactly one level; for deeper wildcards chain them explicitly,
        e.g. ``"?.?.status"``, not a single ``?`` that skips several levels)::

            # non-terminal ?: skip key, look at field in the value
            mn.filter("orders.?.status").eq("shipped")   # any order id
            mn.filter("orders.?.amount").gte(100)

            # multiple wildcard levels — one "?" per level
            mn.filter("region.?.?.status").eq("Active")   # region -> group -> row

            # terminal ?: check if that key EXISTS at the inner level
            mn.filter("?").eq("r1")   # outer rows whose inner dict has key "r1"

        Anchor path (first segment is a known outer key)::

            # only scan rows inside the "g1" outer key
            mn.filter("g1.?.user_id").eq(1)
            mn.filter("g1.?.user_id").eq(1, returns="rows_here")

        **Performance**

        - With ``create_index("field")``: EQ is O(1), range is O(log n + k)
        - Without index: auto-builds index on first call, O(n) build + O(1) lookup
        - Wildcard paths: auto-builds wildcard index on first call. EQ on
          non-terminal wildcards (any number of ``?``) and on terminal ``?``
          reconstructs pruned results directly from the index, no rescan.
          ``ne()`` and range ops (``lt``/``gt``/...) on wildcard paths have
          no index shortcut and fall back to a full scan every call.

        **Mixed / unnormalized types**

        A field with incompatible types across rows (e.g. some rows have
        ``age: 30``, others ``age: None``) never raises. ``eq()``/``ne()``
        use normal equality, so ``None`` only matches ``eq(None)``. Range
        ops (``lt``/``lte``/``gt``/``gte``/``between``) treat a value that
        can't be ordered against the query value (e.g. ``None`` vs an int)
        as excluded — it matches none of them, rather than being silently
        treated as equal.

        **Link hops ("->")**

        A path can hop across a declared ``link()`` mid-pattern with ``->``
        — a JOIN-in-WHERE. Each hop's left side must be the *exact* path
        passed to a prior ``link()`` call; the right side continues reading
        from the resolved target row, and can itself contain another ``->``
        for a multi-hop chain (each hop has statically known depth, so this
        composes — unlike ``follow()``, which needs ``keys=`` for hierarchy
        walks of *unknown* depth)::

            mn.link("orders.?.customer_id", "customers.?")
            mn.filter("orders.?.customer_id->name").eq("Alice")
            # -> {"orders": {pk: row, ...}} for every order whose customer's name is "Alice"

            mn.link("customers.?.company_id", "companies.?")
            mn.filter("orders.?.customer_id->company_id->name").eq("Acme")
            # 2 hops: orders -> customers -> companies

        Supported on ``.eq()/.ne()/.lt()/.lte()/.gt()/.gte()/.between()/.in_()``,
        with every ``returns`` mode. Raises ``ValueError`` if any hop's link
        wasn't declared via ``link()`` first. ``.eq()`` is index-accelerated
        (chains each hop's already-built link index — no full scan); the
        other operators fall back to a linear scan of the anchor table.

        ``returns="rows_here"``/``"values"`` report data from the **anchor**
        row (``orders``, e.g.) — not wherever the ``->`` chain lands — since
        the comparison value is one you already supplied via ``.eq(...)``
        (or similar), so echoing back the target row/field wouldn't tell you
        anything new; the useful, non-redundant data is on the row you're
        actually filtering, same as ``returns="rows"`` already returns (its
        ``{table: {...}}`` nesting is keyed by that same anchor table) — this
        just flattens it instead of nesting it. One entry per matching
        **anchor** row, not deduplicated by target — if 5 orders share the
        same customer, the matching order appears 5 times, mirroring what
        ``returns="rows_here"`` already does for an ordinary (non-``->``)
        wildcard path::

            mn.link("orders.?.customer_id", "customers.?")
            mn.filter("orders.?.customer_id->name").eq("Alice", returns="rows_here")
            # -> [order_row, order_row, ...] — one per order whose customer is named Alice
            mn.filter("orders.?.customer_id->name").eq("Alice",
                                                          returns="values", value_field="total")
            # -> [order_total, order_total, ...] — a field read off each matching ORDER

        Examples::

            mn.create_index("age")

            adults   = mn.filter("age").gte(18)
            teens    = mn.filter("age").between(13, 17)
            specific = mn.filter("age").in_([25, 30, 35])
            active   = mn.filter("active").eq(True)

            # Chaining filters
            result = mn.filter("age").gte(18).filter("active").eq(True)

            # Wildcard path
            mn.filter("orders.?.status").eq("shipped")
        """
        ...

    # ──────────────────────────────────────────────────
    # Index management
    # ──────────────────────────────────────────────────

    @overload
    def create_index(self, field: str) -> None: ...
    @overload
    def create_index(self, field: tuple[str, ...]) -> None: ...
    def create_index(self, field: str | tuple[str, ...]) -> None:
        """
        Build a field index for fast filter / group_by / sort_by / merge.

        Call once before repeated queries on the same field. The index is
        maintained automatically on subsequent inserts and deletes.

        An index stores two structures:
        - Hash index: ``field_value_hash → [outer_keys]`` for O(1) EQ lookup
        - Sorted index: ordered list of numeric values for O(log n) range queries

        **Simple field**::

            mn.create_index("age")
            mn.create_index("meta.level")   # nested field via dot-notation

        **Wildcard path** (dynamic intermediate keys)::

            # {user: {orders: {<dynamic_order_id>: {status: ...}}}}
            mn.create_index("orders.?.status")

        Note: calling ``create_index`` on an already-indexed field is a no-op.
        """
        ...

    @overload
    def drop_index(self, field: str) -> None: ...
    @overload
    def drop_index(self, field: tuple[str, ...]) -> None: ...
    def drop_index(self, field: str | tuple[str, ...]) -> None:
        """
        Remove a previously created field index.

        Frees memory. After dropping, queries on this field fall back to
        linear scan (or auto-rebuild index on first ``filter`` call).

        Example::

            mn.drop_index("age")
            mn.drop_index("orders.?.status")
        """
        ...

    @overload
    def has_index(self, field: str) -> bool: ...
    @overload
    def has_index(self, field: tuple[str, ...]) -> bool: ...
    def has_index(self, field: str | tuple[str, ...]) -> bool:
        """
        Return ``True`` if an index exists for this field.

        Example::

            if not mn.has_index("age"):
                mn.create_index("age")
        """
        ...

    # ──────────────────────────────────────────────────
    # Links — declared relationships between rows in one ModDict
    # ──────────────────────────────────────────────────

    def link(
        self,
        source_path: str | tuple[str, ...],
        references_path: str | tuple[str, ...],
        on_delete: Literal["restrict", "cascade", "set_null"] = "restrict",
    ) -> None:
        """
        Declare that ``source_path`` is a foreign-key-style reference to
        ``references_path``, within this ModDict. ``follow()`` (and nothing
        else — there's no implicit resolution) only works on paths declared
        here first.

        **v1 shape (deliberately narrow):**

        - ``source_path``: exactly ``"table.?.field"`` — one anchor (an
          outer key of this ModDict), one wildcard, one literal field.
        - ``references_path``: ``"table.?"`` (match by key — pk-based) or
          ``"table.?.field"`` (match by a field's value — non-pk).

        Self-reference is allowed — ``source_path`` and ``references_path``
        can share the same table (e.g. an employee hierarchy where
        ``manager_id`` points to another row of the same ``employees``
        table). Cycles are safe under ``cascade``: deleting a row scrubs its
        own reverse-index entry before the cascade looks up who referenced
        it, so a cycle breaks itself on the first deletion — no separate
        cycle detection needed.

        A ``None``/missing field value is treated as "no reference" (like a
        nullable SQL foreign key) — it's never a dangling reference.
        Anything else that doesn't resolve to a real row in the target
        raises immediately, at declaration time (existing data) and at
        write time (future writes) — never silently.

        Args:
            source_path: path to the referencing field, e.g. ``"orders.?.customer_id"``.
            references_path: path identifying the target, e.g. ``"customers.?"``.
            on_delete: what happens to referencing rows when the target row
                is deleted — ``"restrict"`` *(default)* refuses the delete;
                ``"cascade"`` deletes referencing rows too; ``"set_null"``
                clears the reference field on referencing rows.

        Examples::

            mn.link("orders.?.customer_id", "customers.?")
            mn.link("orders.?.customer_id", "customers.?.email")   # non-pk
            mn.link("employees.?.manager_id", "employees.?")       # self-reference
        """
        ...

    def follow(
        self,
        source_path: str | tuple[str, ...],
        keys: Sequence[Any] | None = None,
        values: Sequence[Any] | None = None,
    ) -> ModDict:
        """
        Traverse a link declared with ``link()``.

        For every current row matching ``source_path``, resolves the field
        value against the declared target and collects the matched target
        rows into a new ``ModDict`` (keyed by the target's own key — results
        are naturally de-duplicated if multiple source rows resolve to the
        same target row). A ``None``/missing field value contributes nothing
        (not an error).

        Raises ``ValueError`` if no link was declared for this exact
        ``source_path`` — call ``link()`` first.

        Args:
            source_path: the exact path passed to a prior ``link()`` call.
            keys: restrict the scan to source rows whose own key is in this
                sequence (default: scan every row of the source table).
                Mutually exclusive with ``values``.
            values: skip scanning the source table entirely and resolve
                these values directly against the target — for values that
                didn't come from a source-table scan (e.g. an external list
                of ids). Mutually exclusive with ``keys``.

        Multi-hop traversal (unbounded depth, e.g. walking a hierarchy to
        its root) isn't a single-string DSL — use ``keys`` to chain calls,
        since each hop's *depth* isn't known until you get there::

            mn.link("employees.?.manager_id", "employees.?")
            managers = mn.follow("employees.?.manager_id")             # 1 hop
            skip_managers = mn.follow("employees.?.manager_id",
                                       keys=managers.keys())            # 2 hops

            # values=: resolve ids that came from elsewhere, no source scan
            mn.follow("orders.?.customer_id", values=[100, 200])
        """
        ...

    # ──────────────────────────────────────────────────
    # Query helpers
    # ──────────────────────────────────────────────────

    def sort_by(
        self,
        field: str,
        reverse: bool = False,
        returns: Literal["rows", "parent_keys", "values"] = "rows",
        inplace: bool = False,
    ) -> list[Any] | None:
        """
        Return a sorted list by the given numeric field.

        Supports dot-notation paths: ``sort_by("meta.details.rank")``.
        Uses the sorted index if available (O(n) copy), otherwise builds it first.

        Args:
            field:   Field name or dot-notation path.
            reverse: If True, sort descending.
            returns: What each list element contains (ignored when inplace=True):

                     - ``"rows"``        — the full row dict *(default)*
                     - ``"parent_keys"`` — the outer key (string)
                     - ``"values"``      — the sorted field value (int/float)

            inplace: If True, reorder the ModDict's insertion-order vector in-place
                     and return ``None``. After this, ``mn.at(0)`` returns the
                     smallest element, iteration follows sorted order, etc.
                     Cannot be combined with ``returns`` — raises ``ValueError``.

        Examples::

            rows  = mn.sort_by("score", reverse=True)           # [{"score":9.5,...}, ...]
            keys  = mn.sort_by("age", returns="parent_keys")    # ["alice", "bob", ...]
            ages  = mn.sort_by("age", returns="values")         # [17, 25, 30, ...]
            mn.sort_by("age", inplace=True)                     # reorders mn itself
        """
        ...

    def select(
        self,
        fields: list[str] | dict[str, str],
        returns: Literal["rows", "rows_here", "values"] = "rows",
    ) -> ModDict | list[Any]:
        """
        Project rows to the specified fields — or (default, for wildcard/
        ``->`` fields) land on and keep the tables those fields refer to.

        Supports dot-notation paths. Ignore the rest of this docstring's
        label discussion when ``returns="rows"`` and any field is wildcard-
        shaped — table-landing mode (below) doesn't use labels at all. For
        every other case, the result-row key for each field is its **last
        segment** — the text after the last ``"->"`` (if any), then after
        the last ``"."`` in that (e.g. ``"meta.level"`` → key ``"level"``,
        ``"orders.?.customer_id->name"`` → key ``"name"``) — not the full
        path string. If two fields default to the same key, this raises
        ``ValueError``; pass ``fields`` as a ``{label: path}`` dict instead
        to give explicit, collision-free result keys::

            mn.select(["age", "meta.level"])                       # -> {"age": ..., "level": ...}
            mn.select({"user_age": "age", "lvl": "meta.level"})     # explicit labels
            mn.select(["orders.?.customer_id->name", "orders.?.vendor_id->name"], returns="rows_here")
            # ValueError: both default to "name" -- use the dict form:
            mn.select({"customer": "orders.?.customer_id->name", "vendor": "orders.?.vendor_id->name"}, returns="rows_here")

        Args:
            fields:  List of paths, or a ``{label: path}`` dict (explicit
                     labels — irrelevant for table-landing mode, see below).
            returns: ``"rows"`` *(default)* — new ModDict. If any field is a
                     table-anchored wildcard path (``"table.?..."``), lands
                     on and keeps that field's table instead of extracting a
                     value — see "Table-landing" below. Otherwise, a flat
                     projection identical to ``"rows_here"``.
                     ``"rows_here"`` — always the flat projection (one
                     projected row per matched source row, keyed by that
                     row's own key), even for wildcard/``->`` fields — use
                     this to force value-extraction instead of table-landing.
                     ``"values"`` — columnar: one flat list per requested
                     field, in the same order as ``fields``. A row missing
                     a field contributes ``None`` at that field's position,
                     keeping all columns the same length.

        **Wildcard fields and link hops ("->"), with ``returns="rows_here"``/``"values"``**

        A field can also be a table-anchored wildcard path, ``"table.?..."``
        — the first time ``select()`` looks past a single flat collection —
        optionally with a ``->`` hop across a declared ``link()``, same
        syntax and semantics as ``filter()``'s. Every field must be
        wildcard-shaped if any is (mixing plain and wildcard fields in one
        call raises ``ValueError``), and all wildcard fields must share the
        same anchor table. The result is flat, keyed by each matched anchor
        row's own key — not nested under the table name::

            mn.link("orders.?.customer_id", "customers.?")
            mn.select(["orders.?.customer_id->name", "orders.?.customer_id->email"], returns="rows_here")
            # -> {order_pk: {"name": ..., "email": ...}, ...}

        **Table-landing (default for wildcard/``->`` fields)**

        With the default ``returns="rows"``, a table-anchored wildcard field
        doesn't extract a value — it resolves to the table it refers to and
        keeps that table's rows instead. A plain field (``"group_id"``, no
        ``?``) resolves via a declared ``link()`` on the anchor table's FK
        field, hopping to the target (multi-hop supported, chaining
        ``follow()`` under the hood exactly like the manual ``keys=`` loop
        shown on ``follow()``'s own docstring). A field with **no** ``->``
        hop just contributes its own anchor table's current rows, unchanged.
        Every field is resolved this way and the results are merged into one
        ModDict — two fields landing on the same table get their rows
        unioned together, not overwritten. The trailing part of a hop field
        is still written (it's what identifies the hop chain — the same
        path you'd use to extract that value with ``returns="rows_here"``),
        just unused for this mode's output::

            mn.link("workgroup.?.group_id", "user_group.?")
            mn.select(["workgroup.?.group_id->name"])
            # -> {"user_group": {100: {...}, 200: {...}}} -- "name" is unused here
            mn.select(["workgroup.?.group_id->name", "workgroup.?.status"])
            # -> {"user_group": {...}, "workgroup": {...}} -- mixed: one hops, one doesn't
            mn.select(["workgroup.?.group_id->name"], returns="rows_here")
            # -> {1: {"name": "Engineering"}, 2: {"name": "Sales"}, ...} -- force value-extraction instead

        The result is chainable — since each landed table is shaped like an
        anchored ``filter()`` result, a further ``.filter("user_group.?.field->...")``
        or another ``.select(...)`` relays through to the root and
        intersects, same as any other derived result this library produces.
        Reverse traversal (landing back on a table that *references* the
        current one, rather than one the current table's FK points to) is
        not supported — only the direction a ``link()`` was actually
        declared in.

        Examples::

            slim = mn.select(["name", "age", "meta.level"])
            # -> {pk: {"name": ..., "age": ..., "level": ...}, ...}
            cols = mn.select(["name", "score"], returns="values")
            # → [["alice", "bob", ...], [9.5, 6.0, ...]]
            names, scores = mn.select(["name", "score"], returns="values")
        """
        ...

    def group_by(self, field: str) -> dict[Any, ModDict]:
        """
        Group rows by field value.

        Returns a plain Python dict mapping each distinct field value
        to a ModDict view containing the rows with that value.

        Uses the field index if available (fast path), otherwise
        scans all rows.

        Args:
            field: Field name or dot-notation path.

        Returns:
            ``{field_value: ModDict}``

        Example::

            groups = mn.group_by("meta.level")
            for level, rows in groups.items():
                print(f"Level {level}: {len(rows)} users")

            # Group by nested field
            by_region = mn.group_by("meta.details.region")
        """
        ...

    # ──────────────────────────────────────────────────
    # Cursors — live, mutation-aware views for reactive GUI tables
    # ──────────────────────────────────────────────────

    def cursor(self, path: str | tuple[str, ...]) -> ModDict:
        """
        Return a live cursor anchored at an existing nested table.

        A cursor is a ``ModDict`` instance whose data lives in the *parent's*
        storage — reads and writes route straight through to the anchored
        raw dict (``PyDict``-level, no copy). It is the backing primitive for
        reactive GUI tables (Qt models, etc.): stateful ``set_sort()``/
        ``set_filter()``/``set_group()`` flags maintained incrementally,
        point-mutation methods that return exactly the diff a GUI needs, and
        ``connect()`` for push-based reactivity.

        **The anchor must already exist** — a cursor never creates structure.
        Calling ``cursor()`` on a missing path raises immediately, the same
        way indexing a missing key does.

        Multiple independent cursors can point at the same anchor — each
        gets its own private sort/filter/group state and its own
        ``connect()`` listeners, but all of them see the same live
        underlying data and notify each other when any one of them mutates
        it (a "reorder" event — see ``connect()``).

        A cursor is **not** a full second ``ModDict``: most root-only
        methods (``link``, ``follow``, ``select``, ``copy``, ``serialize``,
        ``group_by``, ``keys``/``values``/``items``, ``alias``, ``pop``, and
        more) raise ``NotImplementedError`` on a cursor by design — call
        them on the root instead. Field-indexing (``create_index``,
        ``filter``, ``sort_by``) is likewise not yet cursor-aware and also
        raises; a cursor's own ``set_sort``/``set_filter``/``set_group``
        below are unrelated to that machinery.

        Args:
            path: Dot-notation string or tuple/list of exact segments,
                  identifying a nested ``{key: row}`` table to anchor on.
                  Must be fully literal — no ``?`` wildcard segments
                  (a cursor addresses one deterministic location, never a
                  flatten across many dynamic parents; for that, use
                  ``filter()``/``select()`` instead, unchanged).

        Raises:
            ValueError: path is empty, contains a wildcard segment, or
                        doesn't resolve to an existing dict-shaped value.

        Example::

            mn = md.ModDict()
            mn["u1"] = {"orders": {"o1": {"amount": 30, "status": "shipped"}}}

            orders = mn.cursor("u1.orders")   # single segment: mn.cursor("u1") also valid
            orders["o1"]["amount"]             # 30 — normal dict protocol
            len(orders)                        # 1

            mn.cursor("u1.missing")            # ValueError — doesn't exist
            mn.cursor("u1.orders.?.status")    # ValueError — wildcard segment
        """
        ...

    def set_sort(
        self, field: str | tuple[str, ...], reverse: bool = False,
    ) -> list[tuple[int | None, int]]:
        """
        Activate (or reconfigure) incremental sort on a cursor.

        Maintains a private ordered view of this cursor's rows by `field`'s
        value, kept incrementally in sync by ``insert()``/``update_row()``/
        ``delete()``/``insert_batch()`` afterward (and by mutations made
        through a *different* cursor on the same anchor — see ``connect()``
        "reorder"). This call itself does a full, explicit rebuild — cheap
        and rare relative to per-mutation maintenance.

        If ``set_group()`` is also active, sort is the **secondary** key
        (tie-breaker) within each group, not the primary order.

        A field value missing on a given row, or not comparable to another
        row's value for that field (e.g. ``None`` vs ``int``), sorts after
        every comparable value — never raises.

        Args:
            field:   Field name or dot-notation path (see the module
                     docstring for path syntax) — a literal path within each
                     row, no ``?`` wildcard.
            reverse: Descending order if True.

        Returns:
            A list of ``(old_index, new_index)`` pairs describing exactly
            what moved, versus whatever presentation order (a prior
            sort/group, or natural insertion order if this is the first
            call) existed immediately before — ``old_index=None`` means the
            row had no defined position before (not applicable here on a
            reconfigure of an already-populated cursor, but shared
            vocabulary with ``insert()``/``insert_batch()``). Feed straight
            into Qt's ``changePersistentIndexList``.

        Only valid on a cursor — raises ``NotImplementedError`` on the root
        ``ModDict``.

        Example::

            orders = mn.cursor("u1.orders")
            orders.set_sort("amount")               # ascending by amount
            orders.at(0)                              # smallest amount
            orders.set_sort("amount", reverse=True)  # re-sort descending
        """
        ...

    def set_group(
        self, field: str | tuple[str, ...] | None,
    ) -> list[tuple[int | None, int]]:
        """
        Activate (or clear) incremental grouping on a cursor.

        Rows sharing the same `field` value become contiguous — the sort
        order (if ``set_sort()`` is also active) becomes secondary, applied
        *within* each group; without an active sort, each group's internal
        order is plain insertion order. Groups themselves are ordered by the
        same value-comparison logic used for sort, applied to the distinct
        group-key values rather than to rows.

        There is no separate group-bucket structure to query directly — a
        cursor stays one flat, positionally-addressable sequence
        (``at(i)``/iteration/``len()``); a GUI wanting group headers reads
        each row's own group field to detect a boundary.

        Args:
            field: Field name or dot-notation path, or ``None`` to clear
                   grouping (falls back to plain sort, or natural order).

        Returns:
            Same ``(old_index, new_index)`` diff vocabulary as ``set_sort()``.

        Only valid on a cursor — raises ``NotImplementedError`` on the root
        ``ModDict``.

        Example::

            orders = mn.cursor("u1.orders")
            orders.set_sort("amount")
            orders.set_group("status")   # grouped by status, sorted by amount within each group
            orders.set_group(None)       # clear grouping, sort_by("amount") still active
        """
        ...

    def set_filter(
        self, predicate: Callable[[dict], bool] | None,
    ) -> list[tuple[int | None, int]]:
        """
        Activate (or clear) a row-visibility predicate on a cursor.

        `predicate` is called with each row's dict; rows it rejects are
        tracked as excluded — maintained incrementally afterward, same as
        ``set_sort()``/``set_group()``.

        Filter membership is currently tracked and diffed correctly, but is
        **not yet composed into ``len()``/iteration/``at()``** — those still
        reflect every row under the anchor regardless of filter state. Only
        the diff returned here (and the payload delivered to a matching
        mutation's ``connect()`` event) currently reflects visibility.

        Args:
            predicate: Callable taking a row dict, returning truthy to keep
                       the row visible. ``None`` clears the filter (every
                       row becomes visible again).

        Returns:
            ``(old_index, new_index)`` pairs — for a reconfigure, a row that
            was hidden by the *previous* filter (or wasn't tracked because
            no filter was active) and becomes visible under the new one
            reports ``old_index=None`` even though the row itself already
            existed; a formerly-visible row that the new filter now excludes
            reports ``new_index=None``. Multiple rows can legitimately share
            ``old_index=None`` in the same call — that's exactly why this is
            a **list**, not a ``{old: new}`` dict (a dict would collapse
            them).

        Only valid on a cursor — raises ``NotImplementedError`` on the root
        ``ModDict``.

        Example::

            orders = mn.cursor("u1.orders")
            orders.set_filter(lambda row: row["status"] == "shipped")
            orders.set_filter(None)   # clear
        """
        ...

    def connect(self, event_type: Literal["insert", "update", "delete", "reorder"],
                callback: Callable[[Any], None]) -> None:
        """
        Register a callback for one event type on a cursor.

        Framework-agnostic — no Qt/GUI dependency. Fires synchronously on
        the calling thread; thread marshaling across a Qt event loop is the
        caller's job (e.g. register a Qt Signal's bound ``.emit`` as the
        callback, then connect the Signal normally for an automatic
        ``QueuedConnection`` across threads). Multiple listeners per event
        are supported — each ``connect()`` call appends, none replace.

        There is no path/wildcard parameter — a listener is always scoped to
        the exact anchor the cursor it's registered on was created for.

        Events:

        - ``"insert"`` — fired by this cursor's own ``insert()`` (payload:
          the same ``int | None`` new-position it returns) or
          ``insert_batch()`` (payload: the same ``list[int | None]``).
        - ``"update"`` — fired by this cursor's own ``update_row()``.
          Payload: the same ``((old_index, new_index), changed_fields)``
          2-tuple it returns.
        - ``"delete"`` — fired by this cursor's own ``delete()``. Payload:
          the same ``int | None`` former-position it returns.
        - ``"reorder"`` — fired on a cursor when a *different* cursor
          anchored at the same location mutates the data (a sibling doesn't
          know the precise operation that changed its view, only that it
          did, so this is the one event that DOES carry the full picture).
          Payload: ``list[(old_index, new_index)]`` — every row whose
          position or visibility actually changed, computed by re-diffing
          this cursor's own state from scratch.

        A listener's exception propagates normally when it's this cursor's
        own direct mutation call; during a "reorder" broadcast to several
        sibling cursors, one listener raising doesn't stop the others from
        being notified.

        Args:
            event_type: One of ``"insert"``/``"update"``/``"delete"``/``"reorder"``.
            callback:   Called with that event's payload (see above).

        Only valid on a cursor — raises ``NotImplementedError`` on the root
        ``ModDict``.

        Example::

            orders = mn.cursor("u1.orders")
            orders.connect("insert", lambda diff: model.apply_insert(diff))
            orders.connect("reorder", lambda diff: model.apply_reorder(diff))
        """
        ...

    def insert(self, key: Any, row: dict) -> int | None:
        """
        Insert (or overwrite) one row through a cursor.

        Writes straight into the parent's raw anchored dict (same effect as
        ``cursor[key] = row``), then updates any active sort/filter/group
        state and notifies sibling cursors on the same anchor. Fires this
        cursor's own ``"insert"`` ``connect()`` event with the same value
        this method returns.

        Returns only this row's own landing position — **not** a list of
        every sibling row that structurally shifted as a side effect (e.g.
        every row after the insertion point, under an active sort). A GUI's
        ``beginInsertRows(parent, pos, pos)`` already implies that shift for
        Qt's whole downstream stack (selection, persistent indices,
        delegates) — enumerating every renumbered sibling explicitly would
        be both redundant and, at scale, far more expensive than the actual
        O(log n) position search + pointer-only vector shift underneath (a
        real, benchmark-measured cost this return shape avoids: a single
        insert into a 50 000-row sorted cursor is ~16us, on par with
        ``bisect.insort`` on a plain list — a full-diff-list return of every
        shifted sibling cost over 100x more, almost entirely in marshaling
        that list to Python objects nobody needed).

        A row that an active filter rejects still gets written (it's still
        in the underlying data — see ``set_filter()``) but has no defined
        position, so this returns ``None`` for it and fires no event at all.

        Args:
            key: The row's key within the anchored table.
            row: The row dict to store.

        Returns:
            The row's new position (``int``), or ``None`` if an active
            filter excludes it.

        Only valid on a cursor — raises ``NotImplementedError`` on the root
        ``ModDict``.

        Example::

            orders = mn.cursor("u1.orders")
            orders.set_sort("amount")
            new_index = orders.insert("o9", {"amount": 15, "status": "new"})
        """
        ...

    def update_row(self, key: Any, changes: dict) -> tuple[tuple[int | None, int | None], list[str]]:
        """
        Merge field changes into one existing row through a cursor.

        Equivalent to ``cursor[key].update(changes)`` plus incremental
        sort/filter/group maintenance and sibling notification — merges
        `changes` into the row via a plain dict update (existing fields not
        named in `changes` are left untouched).

        Unlike ``insert()``/``delete()``, this reports **both** the row's old
        and new position explicitly, as a pair — not just the one, and not a
        list of every other shifted sibling either. A field change that
        moves a row is structurally a *move*, and a GUI's
        ``beginMoveRows(sourceParent, oldIndex, oldIndex, destParent, newIndex)``
        genuinely needs both endpoints; unlike a plain insert/remove, there's
        no way for the GUI to infer where a specific row moved *from* and
        *to* on its own.

        Args:
            key:     The row's key within the anchored table. Raises
                     ``KeyError`` if not found.
            changes: Fields to merge into the row.

        Returns:
            A 2-tuple:

            - ``(old_index, new_index)`` — either may be ``None`` if the row
              wasn't visible before / isn't visible after (an active filter
              excludes it); both are the same non-``None`` value if the
              change didn't move the row at all.
            - ``changed_fields`` — the subset of `changes`' keys whose value
              actually differs from what was there before (not just "was
              present in `changes`" — a value written as identical to what
              was already there is excluded).

        Fires this cursor's own ``"update"`` ``connect()`` event with this
        same 2-tuple as the payload.

        Only valid on a cursor — raises ``NotImplementedError`` on the root
        ``ModDict``.

        Example::

            orders = mn.cursor("u1.orders")
            (old_i, new_i), changed = orders.update_row("o1", {"amount": 99, "status": "shipped"})
            # changed == ["amount"] if status was already "shipped"
        """
        ...

    def delete(self, key: Any) -> int | None:
        """
        Delete one row through a cursor.

        Equivalent to ``del cursor[key]`` plus incremental sort/filter/group
        maintenance and sibling notification. Fires this cursor's own
        ``"delete"`` ``connect()`` event with the same value this method
        returns.

        Returns only the deleted row's own former position — **not** a list
        of every remaining row that shifted as a result. A GUI's
        ``beginRemoveRows(parent, pos, pos)`` already implies that shift for
        everything after `pos` — same reasoning as ``insert()``, see there
        for the benchmark numbers behind this shape.

        Args:
            key: The row's key within the anchored table. Raises
                 ``KeyError`` if not found.

        Returns:
            The row's former position (``int``), or ``None`` if it wasn't
            visible under an active filter at the time of deletion.

        Only valid on a cursor — raises ``NotImplementedError`` on the root
        ``ModDict``.

        Example::

            orders = mn.cursor("u1.orders")
            old_index = orders.delete("o2")
        """
        ...

    def insert_batch(self, rows: dict[Any, dict]) -> list[int | None]:
        """
        Insert (or overwrite) many rows through a cursor in one call.

        Writes every ``{key: row}`` pair in one pass (not a Python-level
        loop of individual ``insert()`` calls — one incremental-state
        rebuild for the whole batch, not one per row), then notifies sibling
        cursors and fires a single ``"insert"`` ``connect()`` event covering
        every inserted row.

        Returns only the NEW rows' own landing positions — same "don't
        enumerate shifted siblings" reasoning as ``insert()``, extended to a
        batch. Existing rows displaced by the batch aren't reported.

        Args:
            rows: ``{key: row_dict, ...}`` — every value must be a dict.

        Returns:
            ``list[int | None]``, one entry per row in `rows` (in the same
            order `rows` iterates), each either that row's new position or
            ``None`` if an active filter excludes it.

        Only valid on a cursor — raises ``NotImplementedError`` on the root
        ``ModDict``.

        Example::

            orders = mn.cursor("u1.orders")
            positions = orders.insert_batch({
                "o10": {"amount": 5,  "status": "new"},
                "o11": {"amount": 40, "status": "new"},
            })
        """
        ...

    # ──────────────────────────────────────────────────
    # Serialization
    # ──────────────────────────────────────────────────

    def serialize(self) -> bytes:
        """
        Serialize the entire ModDict to a compact binary format.

        The format preserves all supported types including datetime, date,
        time, bytes, bytearray, pathlib paths, set, frozenset, tuple, uuid.UUID,
        Decimal, and WKB geometry (shapely / geoalchemy2 — see
        ``set_geo_backend()``).

        Any other type (arbitrary Python objects with no registered converter)
        is **not** serializable and raises ``TypeError`` — register a
        converter first via ``md.register_converter()``, or convert the value
        to a supported type before storing/serializing it.

        Returns:
            Bytes object. Can be stored to disk or transferred over network.

        Example::

            data = mn.serialize()
            with open("cache.bin", "wb") as f:
                f.write(data)
        """
        ...

    def aliases(self) -> dict[Any, Any]:
        """
        Return a mapping of all active aliases to their original keys.

        Alias entries are hidden from ``keys()``, ``iter``, and ``len()`` —
        this method is the only way to enumerate them.

        Example::

            mn.alias("alice", "al")
            mn.aliases()  # {"al": "alice"}
        """
        ...

    def pop(self, key: Any, *default: Any) -> Any:
        """
        Remove *key* and return its value.

        If *key* is not found and *default* is given, return *default*.
        If *key* is not found and no default is given, raise ``KeyError``.

        Example::

            val = mn.pop("alice")           # removes "alice", returns its row
            val = mn.pop("missing", None)   # → None (no KeyError)
        """
        ...

    def copy(self) -> ModDict:
        """
        Return a deep copy of this ModDict.

        All row dicts are recursively copied — mutations on the copy do not
        affect the original and vice versa. Aliases are not copied.

        Example::

            c = mn.copy()
            c["alice"]["age"] = 99   # original unchanged
        """
        ...

    def at(self, i: int) -> Any:
        """
        Return the value at insertion-order position *i*.

        Supports negative indices (``-1`` = last inserted).
        Raises ``IndexError`` if out of range.

        Example::

            mn["a"] = {"x": 1}
            mn["b"] = {"x": 2}
            mn.at(0)   # {"x": 1}
            mn.at(-1)  # {"x": 2}
        """
        ...

    def alias(self, key: Any, alias: Any) -> None:
        """Create a transparent alias for an existing key.

        Both the original key and the alias point to the same row dict.
        Mutations via either key update the same object and keep indices in sync.
        Deletion is symmetric: deleting either the alias or the original key
        removes both from the ModDict.

        Args:
            key:   Existing key in the ModDict.
            alias: New key to register as an alias. Must not already exist.

        Example::

            mn["alice"] = {"age": 30}
            mn.alias("alice", "al")
            mn["al"]["age"] = 31   # same row — mn["alice"]["age"] == 31
        """
        ...

    def reindex(self, key: Any) -> None:
        """Rebuild field indices for one row after a deep nested write.

        `mn[k]["age"] = 99` and `mn[k].update({...})` update the index
        automatically via RowProxy. For deeper writes that bypass RowProxy::

            mn[k]["meta"]["details"]["rank"] = 99  # RowProxy only sees "meta"
            mn.reindex(k)                           # explicitly resync indices

        Calling this when no indices exist is a no-op.
        """
        ...

    def deserialize(self, data: bytes) -> ModDict:
        """
        Deserialize bytes produced by ``serialize()`` into this ModDict.

        Clears any existing data before loading. Mutates self in place and
        returns self, so the call can be chained.

        Args:
            data: Bytes produced by ``ModDict.serialize()``.

        Example::

            mn2 = ModDict().deserialize(open("cache.bin", "rb").read())
        """
        ...

    def to_dict(self) -> dict[Any, Any]:
        """
        Return a shallow copy of this ModDict as a plain ``dict``.

        Row values are the same underlying dict objects (not copied), but
        unlike ``mn[key]`` this bypasses RowProxy entirely — useful when
        handing data to code that requires an actual ``dict`` (e.g. Pydantic's
        ``model_validate``, which only accepts ``dict`` or a model instance,
        not arbitrary Mapping-like objects).

        Example::

            LoginRequest.model_validate(mn.to_dict())
        """
        ...


class ShapelyWKB:
    """
    Wrapper to explicitly tag raw WKB bytes as a Shapely geometry.

    Use when you want to store geometry data without having Shapely
    installed on the writing side. On the reading side, reconstructs as a
    ``shapely`` geometry object if Shapely is importable there — see
    ``set_geo_backend()`` for what happens if both Shapely and GeoAlchemy2
    are installed on the reading side, or neither is.

    Example::

        import mod_dict as md

        wkb_bytes = b"\\x01\\x01..."  # raw WKB
        mn["point"] = {"geom": md.ShapelyWKB(wkb_bytes)}
    """
    def __init__(self, data: bytes) -> None: ...


class GeoAlchemyWKB:
    """
    Wrapper to explicitly tag raw WKB bytes as a GeoAlchemy2 geometry.

    Analogous to ``ShapelyWKB`` but reconstructs as ``geoalchemy2.WKBElement``
    — see ``set_geo_backend()`` for the reading-side reconstruction rules.

    Example::

        mn["row"] = {"geom": md.GeoAlchemyWKB(wkb_bytes)}
    """
    def __init__(self, data: bytes) -> None: ...


def set_geo_backend(name: Literal["shapely", "geoalchemy2"] | None) -> None:
    """
    Choose which library a deserialized geometry reconstructs into.

    A serialized geometry only ever remembers that it *is* WKB geometry data
    — reconstructing it back into a ``shapely`` object or a
    ``geoalchemy2.WKBElement`` depends on what's importable on the reading
    side, regardless of which one wrote it:

    - Only one of Shapely / GeoAlchemy2 installed: that one is used
      automatically, no call to this function needed.
    - Both installed: **required** — deserializing a geometry without
      calling this first raises ``ValueError`` (ambiguous which to use).
    - Neither installed: the raw WKB ``bytes`` are returned instead — no
      data loss, just no reconstruction.

    Raises ``ValueError`` if *name* isn't ``"shapely"``/``"geoalchemy2"``/
    ``None``, or ``ImportError`` if that library isn't actually installed.
    Pass ``None`` to clear a previously set preference.

    Example::

        md.set_geo_backend("shapely")   # both libs installed -> always shapely
        mn2 = md.loads(md.dumps(mn))    # geometries come back as shapely objects
    """
    ...


def dumps(obj: Any) -> bytes:
    """
    Serialize any supported object to bytes — not just a ModDict.

    A ``ModDict`` is serialized with its own container format (the same one
    ``ModDict.serialize()`` uses), so ``loads()`` reconstructs a ``ModDict``
    back. Any other supported value (dict, list, tuple, str, int, float,
    bytes, bytearray, set, frozenset, datetime, Decimal, Path, uuid.UUID, …)
    uses a lighter single-value format and round-trips back as itself.

    There is **no** implicit ``ModDict`` → ``dict`` conversion — call
    ``mn.to_dict()`` first if you specifically want the plain-dict form
    serialized.

    An unsupported type (no registered converter, not one of the above)
    raises ``TypeError`` rather than silently losing data.

    Example::

        data = md.dumps({"age": 30, "name": "alice"})
        row  = md.loads(data)          # -> dict

        data2 = md.dumps(mn)           # mn: ModDict
        mn2   = md.loads(data2)        # -> ModDict
    """
    ...


def loads(data: bytes) -> Any:
    """
    Deserialize bytes produced by ``dumps()`` (or ``ModDict.serialize()``).

    Returns a ``ModDict`` if the bytes were produced from one, otherwise
    the plain Python value that was serialized.
    """
    ...
