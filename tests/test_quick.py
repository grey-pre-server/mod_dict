import _env  # noqa
import mod_dict as md
from mod_dict import ModDict

mn = md.ModDict({
    "alice": {"age": {'a':35}, "score": 9.5, "active": True,  "city": "NY"},
    "bob":   {"age": {'a':30}, "score": 6.0, "active": False, "city": "LA"},
    "carol": {"age": {'a':30}, "score": 8.1, "active": True,  "city": "NY"},
    "dave":  {"age": {'a':33}, "score": 7.3, "active": False, "city": "SF"},
})

print("─── sort_by age.a asc (rows) ───")
print(mn.sort_by("age.a"))

print("\n─── sort_by score desc (parent_keys) ───")
print(mn.sort_by("score", reverse=True, returns="rows"))

print("\n─── sort_by score desc (values) ───")
print(mn.sort_by("score", reverse=True, returns="values"))

print("\n─── group_by age.a ───")
groups = mn.group_by("age.a")
for val, grp in groups.items():
    print(f"  {val}: {sorted(grp.keys())}")

print("\n─── group_by city ───")
for val, grp in mn.group_by("city").items():
    print(f"  {val}: {sorted(grp.keys())}")

print("\n─── select [age.a, score] (rows→ModDict) ───")
sel = mn.select(["age.a", "score"])
for k in sorted(sel.keys()):
    print(f"  {k}: {sel[k]}")

print("\n─── select [age.a, score] (values→list) ───")
print(mn.select(["age.a", "score"], returns="values"))



# ── alias tests ───────────────────────────────────────────────────────────────
print("\n─── alias: basic access ───")
mn2 = md.ModDict()
mn2["alice"] = {"age": 30, "score": 9.5}
mn2.alias("alice", "al")
print(mn2["al"])                          # same row as alice
print(mn2["al"] is mn2["alice"])          # True

print("\n─── alias: mutation via alias updates original ───")
mn2["al"]["age"] = 31
print(mn2["alice"]["age"])               # 31

print("\n─── alias: mn2.update(other) via alias key ───")
other = md.ModDict()
other["al"] = {"age": 99, "score": 8.0}
mn2.update(other)
print(mn2["alice"])                      # age=99, score=8.0
print(mn2["al"])                         # same — age=99, score=8.0

print("\n─── alias: index still works after mutation ───")
mn2.create_index("age")
print(mn2.filter("age").eq(99).keys())   # ["alice"] or ["al"] depending on which key is in index

print("\n─── alias: keys/iter hide aliases ───")
print(mn2.keys())                        # only ['alice']
print(list(mn2))                         # only ['alice']
print(f"len={len(mn2)}")                 # 1

print("\n─── alias: aliases() mapping ───")
print(mn2.aliases())                     # {'al': 'alice'}

print("\n─── alias: del alias removes both ───")
del mn2["al"]
print("al" in mn2)                       # False
print("alice" in mn2)                    # False

print("\n─── alias: del original removes alias too ───")
mn2["alice"] = {"age": 30, "score": 9.5}
mn2.alias("alice", "al")
del mn2["alice"]
print("alice" in mn2)                    # False
print("al" in mn2)                       # False

# ── copy / pop / | / |= ──────────────────────────────────────────────────────
print("\n─── pop ───")
mn_pop = md.ModDict({"a": {"x": 1}, "b": {"x": 2}})
val = mn_pop.pop("a")
print(val)                    # {"x": 1}
print("a" in mn_pop)          # False
print(mn_pop.pop("missing", 42))  # 42 (default)
try:
    mn_pop.pop("missing")
    print("ERROR: should have raised")
except KeyError:
    print("KeyError raised correctly")

print("\n─── copy (deep) ───")
base = md.ModDict({"a": {"x": 1}, "b": {"x": 2}})
c = base.copy()
print(c.keys())                          # ['a', 'b']
c["a"]["x"] = 99
print(c["a"]["x"])                       # 99
print(base["a"]["x"])                    # 1 — оригинал не тронут (deep copy)

print("\n─── at() ───")
mn_at = md.ModDict({"a": {"x": 1}, "b": {"x": 2}, "c": {"x": 3}})
print(mn_at.at(0))    # {"x": 1}
print(mn_at.at(1))    # {"x": 2}
print(mn_at.at(-1))   # {"x": 3}
print(mn_at.at(2))    # {"x": 3}

# ── from_rows / from_row ──────────────────────────────────────────────────────
print("\n─── from_rows ───")
rows = [
    {"id": 1, "name": "alice", "age": 30},
    {"id": 2, "name": "bob",   "age": 25},
    {"id": 3, "name": "carol", "age": 35},
]
mn3 = md.ModDict.from_rows(rows, key="id")
print(mn3.keys())                        # [1, 2, 3]
print(mn3[1])                            # {"id":1, "name":"alice", "age":30}

print("\n─── from_row ───")
r = md.ModDict.from_row({"id": 99, "name": "dave"})
print(r)                                 # {"id":99, "name":"dave"}
print(type(r))                           # <class 'dict'>

# ── at() with from_rows ───────────────────────────────────────────────────────
print("\n─── at() with from_rows ───")
base2 = md.ModDict.from_rows(rows, key="id")
print(base2.at(0))    # first inserted row
print(base2.at(-1))   # last inserted row

print("\n─── sort(inplace=True) with from_rows ───")
mn = md.ModDict.from_rows([
    {"id": 3, "age": 40},
    {"id": 1, "age": 20},
    {"id": 2, "age": 30},
], key="id")
print(mn.at(0))   # {"id": 3, "age": 40}  — порядок вставки
mn.sort_by("age", inplace=True, returns='rows')
print(mn.at(0))

# print("\n─── filter(wildcard) ───")
groups = md.ModDict()
groups['g1'] = {
    "r1": {"_id": "r1", "user_id": 1, "data": "aaa"},
    "r2": {"_id": "r2", "user_id": 2, "data": "bbb"},
}
groups['g2'] = {
    "r3": {"_id": "r3", "user_id": 1, "data": "ccc"},
    "r4": {"_id": "r4", "user_id": 3, "data": "ddd"},
}
print("\n─── filter returns ───")
# rows_here — плоское поле
mn2 = md.ModDict({"a": {"age": 30, "name": "alice"}, "b": {"age": 25, "name": "bob"}})
print(mn2.filter("age").gte(28, returns="rows_here"))
# → [{'age': 30, 'name': 'alice'}]

# values
print(mn2.filter("age").gte(28, returns="values", value_field="name"))
# → ['alice']

# rows_here с wildcard + anchor
print(groups.filter("g1.?.user_id").eq(1, returns="rows_here"))
# → [{'_id': 'r1', 'user_id': 1, 'data': 'aaa'}]

# values с wildcard без anchor
print(groups.filter("?.user_id").eq(1, returns="values", value_field="data"))
# → ['aaa', 'ccc']
from collections import OrderedDict

# ── __init__ ──────────────────────────────────────────────────────────────────
print("\n─── __init__ ───")
mn_i1 = md.ModDict({"a": {"x": 1}, "b": {"x": 2}})
assert mn_i1.keys() == ["a", "b"], f"dict init failed: {mn_i1.keys()}"
print("dict init:      ", mn_i1.keys())       # ['a', 'b']

mn_i2 = md.ModDict(mn_i1)
assert mn_i2.keys() == ["a", "b"]
print("ModDict init:   ", mn_i2.keys())       # ['a', 'b']

od = OrderedDict([("c", {"x": 3}), ("d", {"x": 4})])
mn_i3 = md.ModDict(od)
assert mn_i3.keys() == ["c", "d"]
print("OrderedDict:    ", mn_i3.keys())       # ['c', 'd']

try:
    md.ModDict(42)
    print("ERROR: should have raised TypeError")
except TypeError as e:
    print("TypeError ok:   ", e)

# ── update ────────────────────────────────────────────────────────────────────
print("\n─── update(*,*) same keys ───")
u1 = md.ModDict({"a": {"score": 1}, "b": {"score": 2}})
u1.update(md.ModDict({"a": {"score": 9}, "b": {"score": 8}}), "*", "*")
assert u1["a"]["score"] == 9 and u1["b"]["score"] == 8
print("scores:", u1["a"]["score"], u1["b"]["score"])   # 9 8

print("\n─── update(*,*) other bigger — only matching updated ───")
u2 = md.ModDict({"a": {"score": 1}})
u2.update(md.ModDict({"a": {"score": 9}, "b": {"score": 8}}), "*", "*")
assert u2.keys() == ["a"] and u2["a"]["score"] == 9
print("keys:", u2.keys(), "score:", u2["a"]["score"])  # ['a'] 9

print("\n─── update(*,*) mn bigger — extra keys untouched ───")
u3 = md.ModDict({"a": {"score": 1}, "b": {"score": 2}})
u3.update(md.ModDict({"a": {"score": 9}}), "*", "*")
assert u3["a"]["score"] == 9 and u3["b"]["score"] == 2
print("a:", u3["a"]["score"], "b:", u3["b"]["score"])  # 9 2

print("\n─── update(?,?) — insert new + update existing ───")
u4 = md.ModDict({"a": {"score": 1}})
u4.update(md.ModDict({"a": {"score": 9}, "b": {"score": 8}}), "?", "?")
assert sorted(u4.keys()) == ["a", "b"] and u4["a"]["score"] == 9 and u4["b"]["score"] == 8
print("keys:", u4.keys(), "scores:", u4["a"]["score"], u4["b"]["score"])  # ['a','b'] 9 8

print("\n─── update deep field *.meta.level ───")
u5 = md.ModDict({"a": {"score": 1, "meta": {"level": 1}},
                 "b": {"score": 2, "meta": {"level": 2}}})
u5.update(md.ModDict({"a": {"meta": {"level": 9}}, "b": {"meta": {"level": 8}}}),
          "*.meta.level", "*.meta.level")
assert u5["a"]["meta"]["level"] == 9 and u5["b"]["meta"]["level"] == 8
print("levels:", u5["a"]["meta"]["level"], u5["b"]["meta"]["level"])  # 9 8