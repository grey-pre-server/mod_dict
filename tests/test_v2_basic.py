import datetime
import sys, os
import _env  # noqa — adds cmake-build-release to sys.path
import mod_dict

def ok(label): print(f"  OK  {label}")
def fail(label, msg): raise AssertionError(f"FAIL {label}: {msg}")

print("=" * 60)
print("ModDict — basic tests")
print("=" * 60)

# ── 1. Insert / get scalar & dict ────────────────────────────────
print("\n[1] Insert / get")
d = mod_dict.ModDict()
d["a"] = 1
d["b"] = "hello"
d["c"] = 3.14
d["dt"] = {"ts": datetime.datetime(2024, 1, 1)}
d["by"] = b'\x00\xff'
assert d["a"] == 1
assert d["b"] == "hello"
assert abs(d["c"] - 3.14) < 1e-9
assert d["dt"]["ts"] == datetime.datetime(2024, 1, 1)
assert d["by"] == b'\x00\xff'
assert len(d) == 5
ok("scalar types + len")

# ── 2. Nested rows ────────────────────────────────────────────────
print("\n[2] Nested rows")
d2 = mod_dict.ModDict({
    "row1": {"id": 5, "name": "John"},
    "row2": {"id": 7, "name": "Jane"},
})
assert d2["row1"]["id"] == 5
assert d2["row2"]["name"] == "Jane"
assert isinstance(d2["row1"], dict)
ok("nested dict rows")

# ── 3. update (key → key) ──────────────────────────────────────────
print("\n[3] update key→key")
a = mod_dict.ModDict({"x": {"v": 1}, "y": {"v": 2}, "z": {"v": 3}})
b = mod_dict.ModDict({"y": {"v": 20}, "w": {"v": 4}})
n = a.update(b, "*", "*")
assert a["y"]["v"] == 20, f"expected 20, got {a['y']['v']}"
assert a["x"]["v"] == 1
assert n == 1  # only "y" matched
ok(f"updated {n} row(s), values correct")

# ── 4. update (field → field) ─────────────────────────────────────
print("\n[4] update field→field")
users = mod_dict.ModDict({
    "u1": {"id": 5, "name": "John"},
    "u2": {"id": 7, "name": "Jane"},
})
orders = mod_dict.ModDict({
    "o1": {"user_id": 5, "product": "apple"},
    "o2": {"user_id": 9, "product": "pear"},
})
users.update(orders, "id", "user_id")
assert users["u1"]["product"] == "apple"
assert users["u2"].get("product") is None  # no matching order
ok("field→field update correct")

# ── 5. filter ─────────────────────────────────────────────────────
print("\n[5] filter")
f = mod_dict.ModDict({
    "a": {"age": 10},
    "b": {"age": 17},
    "c": {"age": 30},
    "d": {"age": 25},
})
assert sorted(f.filter("age").eq(17).keys())  == ["b"]
assert sorted(f.filter("age").gte(18).keys()) == ["c", "d"]
assert sorted(f.filter("age").lt(20).keys())  == ["a", "b"]
assert sorted(f.filter("age").ne(17).keys())  == ["a", "c", "d"]
ok("eq / gte / lt / ne")

# auto-index on 2nd call
f.filter("age").gte(18)
assert f.has_index("age")
ok("auto-index built after 2nd filter call")

# incremental index update
f["e"] = {"age": 22}
assert sorted(f.filter("age").eq(22).keys()) == ["e"]
ok("incremental index update after insert")

# ── 6. sort_by / group_by / select ───────────────────────────────
print("\n[6] sort_by / group_by / select")
s = mod_dict.ModDict({
    "a": {"age": 30, "active": True,  "score": 9.0},
    "b": {"age": 10, "active": False, "score": 5.0},
    "c": {"age": 20, "active": True,  "score": 7.0},
})
assert s.sort_by("age") == ["b", "c", "a"]
assert s.sort_by("age", reverse=True) == ["a", "c", "b"]
ok("sort_by asc / desc")

groups = s.group_by("active")
assert sorted(groups[True].keys())  == ["a", "c"]
assert sorted(groups[False].keys()) == ["b"]
ok("group_by bool field")

sel = s.select(["age", "score"])
assert set(sel["a"].keys()) == {"age", "score"}
assert "active" not in sel["a"]
ok("select field projection")

# ── 7. keys / values / items / iteration ─────────────────────────
print("\n[7] Iteration")
assert sorted(d2.keys()) == ["row1", "row2"]
assert len(list(d2.values())) == 2
assert len(list(d2.items())) == 2
assert sorted(k for k in d2) == ["row1", "row2"]
ok("keys / values / items / iter")

# ── 8. contains / get / delete ───────────────────────────────────
print("\n[8] contains / get / delete")
assert "a" in d
assert "zzz" not in d
assert d.get("a") == 1
assert d.get("zzz") is None
assert d.get("zzz", 42) == 42
del d["a"]
assert "a" not in d
ok("in / get / del")

# ── 9. Serialization ─────────────────────────────────────────────
print("\n[9] Serialization")
src = mod_dict.ModDict({
    "alice": {"age": 30, "joined": datetime.date(2020, 6, 1)},
    "bob":   {"age": 25, "joined": datetime.date(2021, 3, 15)},
})
data = src.serialize()
assert len(data) > 0

dst = mod_dict.ModDict()
dst.deserialize(data)
assert dst["alice"]["age"] == 30
assert dst["alice"]["joined"] == datetime.date(2020, 6, 1)
assert dst["bob"]["age"] == 25
ok(f"round-trip {len(data)} bytes")

# ── 10. from_dict / from_json ────────────────────────────────────
print("\n[10] from_dict / from_json")
fd = mod_dict.ModDict.from_dict({"x": 1, "y": {"z": 2}})
assert fd["x"] == 1
assert fd["y"]["z"] == 2
ok("from_dict")

fj = mod_dict.ModDict.from_json('{"p": 1, "q": {"r": 3}}')
assert fj["p"] == 1
assert fj["q"]["r"] == 3
ok("from_json")

# ── 11. Converters ───────────────────────────────────────────────
print("\n[11] Converters")
class Celsius:
    def __init__(self, v): self.v = v

mod_dict.register_converter(Celsius, lambda c: c.v * 9/5 + 32)
cv = mod_dict.ModDict()
cv["city"] = {"temp": Celsius(100)}
assert abs(cv["city"]["temp"] - 212.0) < 1e-9
ok("converter Celsius→Fahrenheit")

# ── 12. FieldIndex sync via RowProxy ────────────────────────────
print("\n[12] FieldIndex sync (RowProxy)")
rx = mod_dict.ModDict()
rx["a"] = {"age": 10, "score": 1.0, "active": True}
rx["b"] = {"age": 20, "score": 2.0, "active": False}
rx["c"] = {"age": 30, "score": 3.0, "active": True}
rx.create_index("age"); rx.create_index("score"); rx.create_index("active")

rx["a"]["age"] = 35
assert sorted(rx.filter("age").eq(10).keys()) == []
assert sorted(rx.filter("age").eq(35).keys()) == ["a"]
ok("__setitem__ updates index")

rx["b"].update({"age": 99, "score": 9.9})
assert sorted(rx.filter("age").eq(20).keys()) == []
assert sorted(rx.filter("age").eq(99).keys()) == ["b"]
assert sorted(rx.filter("score").gt(9.0).keys()) == ["b"]
ok("update() updates index")

del rx["c"]["active"]
assert sorted(rx.filter("active").eq(True).keys()) == ["a"]
ok("__delitem__ updates index")

nested = mod_dict.ModDict()
nested["x"] = {"meta": {"level": 1}}
nested.create_index("meta.level")
nested["x"]["meta"]["level"] = 9
nested.reindex("x")
assert sorted(nested.filter("meta.level").eq(1).keys()) == []
assert sorted(nested.filter("meta.level").eq(9).keys()) == ["x"]
ok("reindex() after deep write")

print("\n" + "=" * 60)
print("All tests passed.")
print("=" * 60)
