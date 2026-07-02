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
from typing import Any, Iterator, Literal, Sequence, overload


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
    - Serialization with full Python type set (date, bytes, Decimal, Path, …)
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
        fields: list[str],
        returns: Literal["rows", "values"] = "rows",
    ) -> ModDict | list[Any]:
        """
        Project rows to the specified fields.

        Supports dot-notation paths; the full path string is used as the key
        in the result row (e.g. ``"meta.level"`` → ``row["meta.level"]``).

        Args:
            fields:  List of field names or dot-notation paths.
            returns: ``"rows"`` — new ModDict *(default)*;
                     ``"values"`` — columnar: one flat list per requested
                     field, in the same order as ``fields``. A row missing
                     a field contributes ``None`` at that field's position,
                     keeping all columns the same length.

        Examples::

            slim = mn.select(["name", "age", "meta.level"])
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
    # Serialization
    # ──────────────────────────────────────────────────

    def serialize(self) -> bytes:
        """
        Serialize the entire ModDict to a compact binary format.

        The format preserves all supported types including datetime, date,
        time, bytes, pathlib paths, set, frozenset, and Decimal.

        ``PYOBJECT`` values (arbitrary Python objects) are **not** serializable
        and will raise ``TypeError``.

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
    installed on the writing side. On the reading side, if Shapely
    is available, the value is reconstructed as a ``shapely`` geometry
    object automatically.

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
    when GeoAlchemy2 is available on the reading side.

    Example::

        mn["row"] = {"geom": md.GeoAlchemyWKB(wkb_bytes)}
    """
    def __init__(self, data: bytes) -> None: ...


def dumps(obj: Any) -> bytes:
    """
    Serialize any supported object to bytes — not just a ModDict.

    A ``ModDict`` is serialized with its own container format (the same one
    ``ModDict.serialize()`` uses), so ``loads()`` reconstructs a ``ModDict``
    back. Any other supported value (dict, list, str, int, float, bytes,
    set, frozenset, datetime, Decimal, Path, …) uses a lighter single-value
    format and round-trips back as itself.

    There is **no** implicit ``ModDict`` → ``dict`` conversion — call
    ``mn.to_dict()`` first if you specifically want the plain-dict form
    serialized.

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
