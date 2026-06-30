"""
bench_report.py — produces numbers for BENCHMARK.md
Run after building: python tests/bench_report.py
"""
import sys, os, time, random, string, datetime, json, pickle

import _env  # noqa — adds cmake-build-release to sys.path
import mod_dict as md

random.seed(42)

N_SCALAR = 1_000_000
N_ROWS   = 100000

# ── helpers ───────────────────────────────────────────────────────────────────

def rstr(n=8): return ''.join(random.choices(string.ascii_lowercase, k=n))
def rtags():
    pool = ["python","cpp","rust","go","java","fast","slow","new","old"]
    return random.sample(pool, k=random.randint(1, 3))

def make_row():
    return {
        "age":    random.randint(18, 80),
        "score":  round(random.random() * 10, 4),
        "name":   rstr(10),
        "active": random.choice([True, False]),
        "tags":   rtags(),
        "joined": datetime.date(random.randint(2000, 2024),
                                random.randint(1, 12),
                                random.randint(1, 28)),
        "meta": {
            "level":    random.randint(1, 10),
            "badge":    rstr(6),
            "score_v2": round(random.random() * 10, 4),
            "details": {
                "region": rstr(5),
                "rank":   random.randint(1, 1000),
            }
        }
    }

def tm(fn, reps=3):
    fn()
    t = time.perf_counter()
    for _ in range(reps): fn()
    return (time.perf_counter() - t) / reps * 1000  # ms

def sep(title): print(f"\n{'─'*62}\n  {title}\n{'─'*62}")

def row(label, d_ms, m_ms):
    ratio = d_ms / m_ms if m_ms else float('inf')
    arrow = "faster" if ratio >= 1 else "slower"
    print(f"  {label:<50} dict={d_ms:6.0f}ms  mod={m_ms:6.0f}ms  {ratio:.2f}× {arrow}")

def row1(label, ms, note=""):
    print(f"  {label:<50} {ms:6.0f}ms  {note}")

def pr(label, ms, ratio=None):
    r = f"  {ratio:.2f}×" if ratio else ""
    print(f"  {label:<52} {ms:6.0f}ms{r}")

# ── data setup ────────────────────────────────────────────────────────────────
print("Generating data...")
scalar_keys = [rstr(12) for _ in range(N_SCALAR)]
scalar_vals = list(range(N_SCALAR))
row_keys    = [rstr(10) for _ in range(N_ROWS)]
rows        = [make_row() for _ in range(N_ROWS)]

# ── 1. SCALAR STORE ───────────────────────────────────────────────────────────
sep("1. Scalar store (1 000 000 int entries)")

d = dict(zip(scalar_keys, scalar_vals))
m = md.ModDict()
for k, v in zip(scalar_keys, scalar_vals): m[k] = v

row("d[key]  vs  m[key]",
    tm(lambda: [d[k] for k in scalar_keys]),
    tm(lambda: [m[k] for k in scalar_keys]))

row("d.get(key)  vs  m.get(key)",
    tm(lambda: [d.get(k) for k in scalar_keys]),
    tm(lambda: [m.get(k) for k in scalar_keys]))

row("key in d  vs  key in m",
    tm(lambda: [k in d for k in scalar_keys]),
    tm(lambda: [k in m for k in scalar_keys]))

row("d[key]=val  vs  m[key]=val",
    tm(lambda: [d.__setitem__(k, 99) for k in scalar_keys]),
    tm(lambda: [m.__setitem__(k, 99) for k in scalar_keys]))

def d_bulk():
    d2 = {}
    for k, v in zip(scalar_keys, scalar_vals): d2[k] = v

def m_bulk():
    m2 = md.ModDict()
    for k, v in zip(scalar_keys, scalar_vals): m2[k] = v

row("bulk insert (1M from scratch)", tm(d_bulk, 1), tm(m_bulk, 1))

# ── 2. STRUCTURED ROWS — READ ─────────────────────────────────────────────────
sep("2. Structured rows — read (100 000 rows)")

dr = dict(zip(row_keys, rows))
mn = md.ModDict()
for k, r in zip(row_keys, rows): mn[k] = r

row("d[key]  vs  mn[key]  (full row)",
    tm(lambda: [dr[k] for k in row_keys]),
    tm(lambda: [mn[k] for k in row_keys]))

row("d[key]['age']  vs  mn[key]['age']",
    tm(lambda: [dr[k]['age'] for k in row_keys]),
    tm(lambda: [mn[k]['age'] for k in row_keys]))

row("d[k]['meta']['details']['rank']  vs  mn[k]['meta']['details']['rank']",
    tm(lambda: [dr[k]['meta']['details']['rank'] for k in row_keys]),
    tm(lambda: [mn[k]['meta']['details']['rank'] for k in row_keys]))

# ── 3. STRUCTURED ROWS — WRITE ────────────────────────────────────────────────
sep("3. Structured rows — write (100 000 rows)")

wkeys = [rstr(10) for _ in range(N_ROWS)]
wrows = [make_row() for _ in range(N_ROWS)]

d_ins = tm(lambda: [dr.__setitem__(k, r) for k, r in zip(wkeys, wrows)])
mn_w  = md.ModDict()
m_ins = tm(lambda: [mn_w.__setitem__(k, r) for k, r in zip(wkeys, wrows)])

print(f"  {'dict bulk insert (100k rows)':<50} {d_ins:6.0f}ms")
print(f"  {'ModDict bulk insert':<50} {m_ins:6.0f}ms  {d_ins/m_ins:.2f}× {'faster' if d_ins/m_ins>1 else 'slower'}")

# field write
t_d_age_w = tm(lambda: [dr[k].__setitem__('age', 99) for k in row_keys])
t_mn_age_c = tm(lambda: [mn[k].__setitem__('age', 99) for k in row_keys])
print()
pr("d[k]['age']=99  (dict)",                   t_d_age_w)
pr("mn[k]['age']=99  (chained on PyObject*)",   t_mn_age_c,  t_d_age_w/t_mn_age_c)

t_d_rank_w  = tm(lambda: [dr[k]['meta']['details'].__setitem__('rank', 99) for k in row_keys])
t_mn_rank_c = tm(lambda: [mn[k]['meta']['details'].__setitem__('rank', 99) for k in row_keys])
print()
pr("d[k]['meta']['details']['rank']=99  (dict)",       t_d_rank_w)
pr("mn[k]['meta']['details']['rank']=99  (chained)",   t_mn_rank_c, t_d_rank_w/t_mn_rank_c)

# ── 4. FILTER ─────────────────────────────────────────────────────────────────
sep("4. Filter")

t_d_age = tm(lambda: [k for k, r in dr.items() if r['age'] >= 40])
t_d_act = tm(lambda: [k for k, r in dr.items() if r['active']])

mn_f = md.ModDict()
for k, r in zip(row_keys, rows): mn_f[k] = r
t_f1 = tm(lambda: mn_f.filter("age").gte(40), 1)
t_f2 = tm(lambda: mn_f.filter("age").gte(40))

mn_a = md.ModDict()
for k, r in zip(row_keys, rows): mn_a[k] = r
t_a1 = tm(lambda: mn_a.filter("active").eq(True), 1)
t_a2 = tm(lambda: mn_a.filter("active").eq(True))

print(f"  {'dict age>=40 scan':<50} {t_d_age:6.0f}ms")
print(f"  {'mn.filter(age).gte(40) — 1st call (builds index)':<50} {t_f1:6.0f}ms  {t_d_age/t_f1:.2f}×")
print(f"  {'mn.filter(age).gte(40) — 2nd+ call (reuse)':<50} {t_f2:6.0f}ms  {t_d_age/t_f2:.2f}×")
print(f"  {'dict active==True scan':<50} {t_d_act:6.0f}ms")
print(f"  {'mn.filter(active).eq(True) — 1st call':<50} {t_a1:6.0f}ms  {t_d_act/t_a1:.2f}×")
print(f"  {'mn.filter(active).eq(True) — 2nd+ call':<50} {t_a2:6.0f}ms  {t_d_act/t_a2:.2f}×")

# ── 5. SORT / GROUP / SELECT ──────────────────────────────────────────────────
sep("5. Sort, group, select  (auto-builds index on 1st call)")

row("sorted(d,key=d[k]['age'])  vs  mn.sort_by('age')",
    tm(lambda: sorted(dr, key=lambda k: dr[k]['age'])),
    tm(lambda: mn.sort_by('age')))

row("groupby active (2 groups)",
    tm(lambda: {v: [k for k,r in dr.items() if r['active']==v] for v in [True,False]}),
    tm(lambda: mn.group_by('active')))

row("groupby age (~63 groups)",
    tm(lambda: {v: [k for k,r in dr.items() if r['age']==v] for v in range(18,81)}),
    tm(lambda: mn.group_by('age')))

row("select ['age','name','score']",
    tm(lambda: {k:{f:dr[k][f] for f in ['age','name','score']} for k in dr}),
    tm(lambda: mn.select(['age','name','score'])))

# ── 6. MERGE ──────────────────────────────────────────────────────────────────
sep("6. Merge")

updates_py = {k: {"score": round(random.random()*10,4)} for k in row_keys}
updates_mn = md.ModDict()
for k, v in updates_py.items(): updates_mn[k] = v

row("dict key→key update  vs  mn.update(*,*)",
    tm(lambda: [dr[k].update(updates_py[k]) for k in dr if k in updates_py]),
    tm(lambda: mn.update(updates_mn, "*", "*")))

# update by field key
keyed_rows  = [{"user_id": k, "score": round(random.random()*10,4)} for k in row_keys]
mn_keyed    = md.ModDict()
for i, r in enumerate(keyed_rows): mn_keyed[i] = r
t_d_kk = tm(lambda: {r["user_id"]: dr[r["user_id"]].update({"score": r["score"]})
                     for r in keyed_rows if r["user_id"] in dr})
t_mn_kk = tm(lambda: mn.update(mn_keyed, "user_id", "*"))
row("update by field: manual vs mn.update(key,*)", t_d_kk, t_mn_kk)

# ── 7. SERIALIZATION ──────────────────────────────────────────────────────────
sep("7. Serialization (100 000 rows)")

def to_json_row(r):
    r2 = dict(r); r2['joined'] = r['joined'].isoformat()
    r2['meta'] = dict(r['meta']); r2['meta']['details'] = dict(r['meta']['details'])
    return r2

dr_json = {k: to_json_row(r) for k, r in zip(row_keys, rows)}

t_js = tm(lambda: json.dumps(dr_json), 1)
js_b = json.dumps(dr_json).encode()
t_jd = tm(lambda: json.loads(js_b), 1)

t_ps = tm(lambda: pickle.dumps(dr, protocol=5), 1)
pk_b = pickle.dumps(dr, protocol=5)
t_pd = tm(lambda: pickle.loads(pk_b), 1)

mn_ser = md.ModDict()
for k, r in zip(row_keys, rows): mn_ser[k] = r
t_ms = tm(lambda: mn_ser.serialize(), 1)
md_b = mn_ser.serialize()
t_md = tm(lambda: md.ModDict().deserialize(md_b), 1)

print(f"\n  {'format':<18} {'serialize':>12} {'deserialize':>14} {'size':>10}")
print(f"  {'':-<18} {'':-<12} {'':-<14} {'':-<10}")
print(f"  {'ModDict binary':<18} {t_ms:>11.0f}ms {t_md:>13.0f}ms {len(md_b)/1e6:>8.1f}MB")
print(f"  {'json':<18} {t_js:>11.0f}ms {t_jd:>13.0f}ms {len(js_b)/1e6:>8.1f}MB")
print(f"  {'pickle':<18} {t_ps:>11.0f}ms {t_pd:>13.0f}ms {len(pk_b)/1e6:>8.1f}MB")

# ── 8. ITERATION ──────────────────────────────────────────────────────────────
sep("8. Iteration")

row("for k in d  vs  for k in mn",
    tm(lambda: list(dr)),
    tm(lambda: list(mn)))

# ── 9. CONVERTERS ────────────────────────────────────────────────────────────
sep("9. Converters")

class Temp:
    def __init__(self, v): self.v = v

plain_rows = [{"age": i, "score": float(i), "name": rstr(6)} for i in range(N_ROWS)]
temp_rows  = [{"age": Temp(i), "score": float(i), "name": rstr(6)} for i in range(N_ROWS)]

mn_nc = md.ModDict()
t_no = tm(lambda: [mn_nc.__setitem__(k, r) for k, r in zip(row_keys, plain_rows)])

md.register_converter(Temp, lambda t: t.v)
mn_c = md.ModDict()
t_cv = tm(lambda: [mn_c.__setitem__(k, r) for k, r in zip(row_keys, temp_rows)])

print(f"  {'insert — no converters (fast path O(1) check)':<50} {t_no:6.0f}ms")
print(f"  {'insert — Temp→int converter active':<50} {t_cv:6.0f}ms")

# ── 10. NEW FEATURES ──────────────────────────────────────────────────────────
sep("10. from_rows / copy / at  (100 000 rows)")

rows_list = [{"id": k, **r} for k, r in zip(row_keys, rows)]

# from_rows vs dict comprehension
t_d_from  = tm(lambda: {r["id"]: r for r in rows_list}, 1)
t_mn_from = tm(lambda: md.ModDict.from_rows(rows_list, key="id"), 1)
row("from_rows: {r[id]:r} vs ModDict.from_rows", t_d_from, t_mn_from)

# copy (deep) vs copy.deepcopy
import copy as _copy
mn_copy_src = md.ModDict()
for k, r in zip(row_keys, rows): mn_copy_src[k] = r
dr_copy_src = dict(zip(row_keys, rows))
t_d_copy  = tm(lambda: _copy.deepcopy(dr_copy_src), 1)
t_mn_copy = tm(lambda: mn_copy_src.copy(), 1)
row("copy (deep): deepcopy(dict) vs mn.copy()", t_d_copy, t_mn_copy)

# at() vs list index
mn_at_src = md.ModDict()
for k, r in zip(row_keys, rows): mn_at_src[k] = r
row_list_src = list(zip(row_keys, rows))
t_d_at_first = tm(lambda: row_list_src[0])
t_mn_at_first = tm(lambda: mn_at_src.at(0))
t_d_at_last  = tm(lambda: row_list_src[-1])
t_mn_at_last  = tm(lambda: mn_at_src.at(-1))
row("at(0) first: list[0] vs mn.at(0)", t_d_at_first, t_mn_at_first)
row("at(-1) last: list[-1] vs mn.at(-1)", t_d_at_last, t_mn_at_last)

# ── 11. INIT ──────────────────────────────────────────────────────────────────
sep("11. ModDict.__init__  (100 000 rows)")

from collections import OrderedDict as OD

# dict init
t_d_init  = tm(lambda: dict(zip(row_keys, rows)), 1)
t_mn_init = tm(lambda: md.ModDict(dict(zip(row_keys, rows))), 1)
row("dict init: dict() vs ModDict(dict)", t_d_init, t_mn_init)

# ModDict copy via __init__
mn_init_src = md.ModDict(dict(zip(row_keys, rows)))
t_mn_cp = tm(lambda: md.ModDict(mn_init_src), 1)
print(f"  {'ModDict(other_mn) shallow copy':<50} {t_mn_cp:6.0f}ms")

# OrderedDict init
od_src = OD(zip(row_keys, rows))
t_od = tm(lambda: md.ModDict(od_src), 1)
print(f"  {'ModDict(OrderedDict)':<50} {t_od:6.0f}ms")

# ── 12. UPDATE ────────────────────────────────────────────────────────────────
sep("12. update  (100 000 rows)")

mn_upd = md.ModDict(dict(zip(row_keys, rows)))
upd_same = md.ModDict({k: {"score": round(random.random()*10,4)} for k in row_keys})
upd_dict = {k: {"score": round(random.random()*10,4)} for k in row_keys}

t_d_upd = tm(lambda: [dr[k].update(upd_dict[k]) for k in row_keys if k in upd_dict])
t_mn_star = tm(lambda: mn_upd.update(upd_same, "*", "*"))
row("key-to-key (*,*): manual vs mn.update(*,*)", t_d_upd, t_mn_star)

t_mn_q = tm(lambda: mn_upd.update(upd_same, "?", "?"))
print(f"  {'mn.update(?,?) — same data, insert+update':<50} {t_mn_q:6.0f}ms  {t_d_upd/t_mn_q:.2f}×")

upd_deep = md.ModDict({k: {"meta": {"level": random.randint(1,10)}} for k in row_keys})
t_d_deep = tm(lambda: [dr[k]["meta"].__setitem__("level", upd_dict[k]["score"]) for k in row_keys])
t_mn_deep = tm(lambda: mn_upd.update(upd_deep, "*.meta.level", "*.meta.level"))
row("deep field (*.meta.level): manual vs mn.update", t_d_deep, t_mn_deep)

# ── 13. WILDCARD FILTER ───────────────────────────────────────────────────────
sep("13. Wildcard filter  (groups: 1000 outer × 100 inner rows)")

N_G = 1000
N_R = 100
g_keys = [rstr(8) for _ in range(N_G)]
r_keys = [rstr(6) for _ in range(N_R)]
g_rows = [[{"_id": rk, "user_id": random.randint(1, 20), "score": round(random.random()*10,2)}
           for rk in r_keys] for _ in range(N_G)]

# build dict-of-dicts and ModDict
dd = {gk: {r["_id"]: r for r in grp} for gk, grp in zip(g_keys, g_rows)}
mn_g = md.ModDict()
for gk, grp in zip(g_keys, g_rows):
    mn_g[gk] = {r["_id"]: r for r in grp}

TARGET_UID = 5

# filter('?.user_id').eq(5) — scan all outer rows, find inner rows where user_id=5
t_d_wc = tm(lambda: {gk: gv for gk, gv in dd.items()
                     if any(r["user_id"] == TARGET_UID for r in gv.values())})
t_mn_wc1 = tm(lambda: mn_g.filter("?.user_id").eq(TARGET_UID), 1)
t_mn_wc2 = tm(lambda: mn_g.filter("?.user_id").eq(TARGET_UID))
print(f"  {'dict: {gk:gv for gk,gv if any(r[uid]==5)}':<50} {t_d_wc:6.0f}ms")
print(f"  {'mn.filter(?.user_id).eq(5) 1st call':<50} {t_mn_wc1:6.0f}ms  {t_d_wc/t_mn_wc1:.2f}×")
print(f"  {'mn.filter(?.user_id).eq(5) 2nd+ call':<50} {t_mn_wc2:6.0f}ms  {t_d_wc/t_mn_wc2:.2f}×")

# filter('?').eq(rk) — find outer rows containing a specific inner key
TARGET_KEY = r_keys[N_R // 2]
t_d_key = tm(lambda: {gk: gv for gk, gv in dd.items() if TARGET_KEY in gv})
t_mn_key1 = tm(lambda: mn_g.filter("?").eq(TARGET_KEY), 1)
t_mn_key2 = tm(lambda: mn_g.filter("?").eq(TARGET_KEY))
print()
print(f"  {'dict: {gk:gv for gk,gv if key in gv}':<50} {t_d_key:6.0f}ms")
print(f"  {'mn.filter(?).eq(key) 1st call':<50} {t_mn_key1:6.0f}ms  {t_d_key/t_mn_key1:.2f}×")
print(f"  {'mn.filter(?).eq(key) 2nd+ call':<50} {t_mn_key2:6.0f}ms  {t_d_key/t_mn_key2:.2f}×")

# returns="rows_here" — collect inner dicts directly
t_d_here = tm(lambda: [r for gv in dd.values() for r in gv.values()
                        if r["user_id"] == TARGET_UID])
t_mn_here = tm(lambda: mn_g.filter("?.user_id").eq(TARGET_UID, returns="rows_here"))
print()
row("rows_here: [r for gv in dd.values()...] vs eq(5,returns=rows_here)",
    t_d_here, t_mn_here)

# returns="values" — extract one field
t_d_val = tm(lambda: [r["score"] for gv in dd.values() for r in gv.values()
                       if r["user_id"] == TARGET_UID])
t_mn_val = tm(lambda: mn_g.filter("?.user_id").eq(TARGET_UID, returns="values", value_field="score"))
row("values: [r[score]...] vs eq(5,returns=values,value_field=score)",
    t_d_val, t_mn_val)

# anchor: filter("gk.?.user_id").eq(5) — only scan one outer row
ANCHOR_KEY = g_keys[N_G // 2]
t_d_anch = tm(lambda: {rk: rv for rk, rv in dd[ANCHOR_KEY].items()
                        if rv["user_id"] == TARGET_UID})
t_mn_anch1 = tm(lambda: mn_g.filter(f"{ANCHOR_KEY}.?.user_id").eq(TARGET_UID), 1)
t_mn_anch2 = tm(lambda: mn_g.filter(f"{ANCHOR_KEY}.?.user_id").eq(TARGET_UID))
print()
print(f"  {'dict: {rk:rv for rk,rv in dd[gk].items() if uid==5}':<50} {t_d_anch:6.2f}ms")
print(f"  {f'mn.filter({ANCHOR_KEY}.?.user_id).eq(5) 1st':<50} {t_mn_anch1:6.2f}ms  {t_d_anch/t_mn_anch1:.2f}×")
print(f"  {f'mn.filter({ANCHOR_KEY}.?.user_id).eq(5) 2nd+':<50} {t_mn_anch2:6.2f}ms  {t_d_anch/t_mn_anch2:.2f}×")

print("\n" + "═"*62)
print("  Done.")
print("═"*62)
