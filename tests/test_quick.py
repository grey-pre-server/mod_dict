import _env  # noqa
import mod_dict as md

mn = md.ModDict({
    "alice": {"age": {'a':35}, "score": 9.5, "active": True,  "city": "NY"},
    "bob":   {"age": {'a':30}, "score": 6.0, "active": False, "city": "LA"},
    "carol": {"age": {'a':30}, "score": 8.1, "active": True,  "city": "NY"},
    "dave":  {"age": {'a':33}, "score": 7.3, "active": False, "city": "SF"},
})

print("─── sort_by age.a asc (rows) ───")
print(mn.sort_by("age.a"))

print("\n─── sort_by score desc (parent_keys) ───")
print(mn.sort_by("score", reverse=True, returns="parent_keys"))

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
