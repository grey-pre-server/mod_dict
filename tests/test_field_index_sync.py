"""
test_field_index_sync.py — verify FieldIndex stays consistent after in-place writes.

Covers:
  1. mn[k]["field"] = val          — RowProxy __setitem__
  2. mn[k].update({...})           — RowProxy .update()
  3. del mn[k]["field"]            — RowProxy __delitem__
  4. mn.reindex(k) after deep write — manual resync for nested paths
  5. No-index path: plain dict returned, no crash
  6. Re-filter after write: results match brute-force scan
"""
import sys, os
import _env  # noqa — adds cmake-build-release to sys.path
import mod_dict as md

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

def check(label, got, expected):
    ok = sorted(got) == sorted(expected)
    print(f"  {'PASS' if ok else 'FAIL'}  {label}")
    if not ok:
        print(f"        got:      {sorted(got)}")
        print(f"        expected: {sorted(expected)}")
    return ok

all_ok = True

def mk():
    mn = md.ModDict()
    mn["a"] = {"age": 10, "score": 1.0, "active": True,  "tag": "x"}
    mn["b"] = {"age": 20, "score": 2.0, "active": False, "tag": "y"}
    mn["c"] = {"age": 30, "score": 3.0, "active": True,  "tag": "x"}
    mn["d"] = {"age": 40, "score": 4.0, "active": False, "tag": "z"}
    return mn

print("\n── 1. __setitem__ via RowProxy ──────────────────────────────────")
mn = mk(); mn.create_index("age")
mn["a"]["age"] = 35   # was 10, now 35 — index must follow
all_ok &= check("filter(age).eq(10) empty after write",  mn.filter("age").eq(10).keys(), [])
all_ok &= check("filter(age).eq(35) finds 'a'",          mn.filter("age").eq(35).keys(), ["a"])
all_ok &= check("filter(age).gte(30) has a,c,d",         mn.filter("age").gte(30).keys(), ["a","c","d"])

print("\n── 2. RowProxy.update() ─────────────────────────────────────────")
mn = mk(); mn.create_index("age"); mn.create_index("score")
mn["b"].update({"age": 99, "score": 9.9})
all_ok &= check("filter(age).eq(20) empty",              mn.filter("age").eq(20).keys(), [])
all_ok &= check("filter(age).eq(99) finds 'b'",          mn.filter("age").eq(99).keys(), ["b"])
all_ok &= check("filter(score).gt(9.0) finds 'b'",       mn.filter("score").gt(9.0).keys(), ["b"])

print("\n── 3. __delitem__ via RowProxy ──────────────────────────────────")
mn = mk(); mn.create_index("active")
del mn["c"]["active"]          # remove field from row c
all_ok &= check("filter(active).eq(True) only 'a' now",  mn.filter("active").eq(True).keys(), ["a"])

print("\n── 4. Deep write + mn.reindex() ─────────────────────────────────")
mn = md.ModDict()
mn["a"] = {"meta": {"level": 1}}
mn["b"] = {"meta": {"level": 5}}
mn["c"] = {"meta": {"level": 3}}
mn.create_index("meta.level")
mn["a"]["meta"]["level"] = 9   # bypasses RowProxy (returns plain dict on 2nd chain)
mn.reindex("a")                # explicit resync
all_ok &= check("filter(meta.level).eq(1) empty",        mn.filter("meta.level").eq(1).keys(), [])
all_ok &= check("filter(meta.level).eq(9) finds 'a'",    mn.filter("meta.level").eq(9).keys(), ["a"])

print("\n── 4b. NE filter with numeric index ─────────────────────────────")
mn_ne = mk(); mn_ne.create_index("age")
all_ok &= check("filter(age).ne(10) excludes 'a'", mn_ne.filter("age").ne(10).keys(), ["b","c","d"])
all_ok &= check("filter(age).ne(99) all rows",     mn_ne.filter("age").ne(99).keys(), ["a","b","c","d"])

print("\n── 4c. del mn[key] with index ───────────────────────────────────")
mn_del = mk(); mn_del.create_index("age")
del mn_del["a"]
all_ok &= check("key removed from outer",          mn_del.filter("age").eq(10).keys(), [])
all_ok &= check("other rows untouched",            mn_del.filter("age").gte(20).keys(), ["b","c","d"])

print("\n── 5. No index — returns raw dict, no crash ─────────────────────")
mn = mk()                      # no create_index
row = mn["a"]
try:
    row["age"] = 99
    brute = [k for k in mn.keys() if mn[k]["age"] == 99]
    all_ok &= check("no-index: raw write reflected in data", brute, ["a"])
except Exception as e:
    print(f"  FAIL  exception: {e}")
    all_ok = False

print("\n── 6. Re-filter results match brute-force scan ──────────────────")
import random; random.seed(0)
mn = md.ModDict()
keys = [f"k{i}" for i in range(500)]
for k in keys:
    mn[k] = {"age": random.randint(1, 100), "score": round(random.random()*10, 2)}
mn.create_index("age")

# mutate 50 random rows through RowProxy
for k in random.sample(keys, 50):
    mn[k]["age"] = random.randint(1, 100)

# compare indexed filter vs brute-force
threshold = 60
indexed = set(mn.filter("age").gte(threshold).keys())
brute   = {k for k in mn.keys() if mn[k]["age"] >= threshold}
all_ok &= check(f"500-row random mutations: filter(age).gte({threshold}) == brute", indexed, brute)

print()
print("══════════════════════════════════")
print(f"  {'ALL PASS' if all_ok else 'FAILURES DETECTED'}")
print("══════════════════════════════════")
if not all_ok:
    sys.exit(1)
