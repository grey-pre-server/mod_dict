#ifndef MOD_DICT_MOD_DICT_H
#define MOD_DICT_MOD_DICT_H

#include "mod_value.h"
#include "flat_hash_map.h"
#include <Python.h>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>

enum class MergeConflict { KEEP_LEFT, KEEP_RIGHT, MERGE, CONCAT };
enum class FilterOp      { EQ, NE, LT, LE, GT, GE };
enum class LinkOnDelete  { RESTRICT, CASCADE, SET_NULL };

// ──────────────────────────────────────────────────────────────────────────────
// LinkDecl — a declared relationship between rows within one ModDict.
//
// v1 scope (deliberately narrow, see project memory project-link-feature-design):
//   source_pattern     = [table, "__pass_key__", field]   e.g. "employees.?.manager_id"
//   references_pattern = [table, "__pass_key__"]          pk-based, e.g. "employees.?"
//                      or [table, "__pass_key__", field]  non-pk,   e.g. "customers.?.email"
// Self-reference (source table == target table) is allowed — cycles are safe
// under CASCADE because deleting a row scrubs its own reverse-index entry
// before cascade looks up who referenced it (no separate visited-set needed).
// ──────────────────────────────────────────────────────────────────────────────
struct LinkDecl {
    std::vector<std::string> source_pattern;
    std::vector<std::string> references_pattern;
    LinkOnDelete on_delete = LinkOnDelete::RESTRICT;
};

// ──────────────────────────────────────────────────────────────────────────────
// OuterEntry — one record in the outer FlatHashMap.
//   key_py  = the outer key Python object (Py_INCREF'd)
//   val_py  = scalar PyObject* or deep-copied row dict (Py_INCREF'd)
//   is_row  = true → val_py is a Python dict (the stored row)
//
// Lifecycle is manual:
//   insert new: Py_INCREF key_py + val_py before outer.insert
//   replace:    Py_XDECREF old refs, Py_INCREF new refs, mutate in place
//   erase:      Py_XDECREF refs, then outer.erase
//   ~ModDict:   Py_XDECREF all occupied entries
//
// NO destructor — FlatHashMap resize() memcpy's entries; a destructor on
// OuterEntry would double-Py_DECREF after the copy.
// ──────────────────────────────────────────────────────────────────────────────
struct OuterEntry {
    PyObject* key_py = nullptr;
    PyObject* val_py = nullptr;  // nullptr = alias entry (val always >= Py_None for real entries)
    bool      is_row = false;
};

class ModDict {
public:
    FlatHashMap<uint64_t, OuterEntry> outer;
    std::vector<uint64_t> order;  // outer hashes in insertion order, no aliases

    // Alias side-tables (keep main outer entries small)
    FlatHashMap<uint64_t, uint64_t> alias_to_orig;  // alias_hash → orig_hash
    FlatHashMap<uint64_t, uint64_t> orig_to_alias;  // orig_hash  → alias_hash

    // own_indices is the storage this object actually owns (freed in
    // ~ModDict() only when root==nullptr). `indices` is a REFERENCE that
    // aliases own_indices for a genuine root, or root->own_indices for a
    // cursor — bound once in the constructor. Every existing `indices.
    // by_field...` call site across mod_dict.cpp keeps compiling and working
    // completely unchanged either way (reference members use the same `.`
    // syntax as value members) — this is what gives a cursor shared-by-
    // reference field-indices without touching ~30 call sites individually.
    struct IndexStore { FlatHashMap<std::string, class FieldIndex*> by_field; };
    IndexStore  own_indices;
    IndexStore& indices;

    std::vector<LinkDecl> links;

    PyObject* on_change_cb = nullptr;
    PyObject* on_merge_cb  = nullptr;
    struct ModDictObject* py_wrapper = nullptr;

    // Cursor support (see project memory: mod_dict_gui_observability_design).
    // root==nullptr => this is a genuine root (owns outer/order/indices).
    // A cursor has root pointing at the true top-level ModDict (flattened —
    // cursor-of-cursor collapses to one hop at creation time) and a
    // non-empty, fully literal anchor_path naming a descent from root's
    // outer down through nested raw dicts to the {pk: row} table this
    // cursor addresses. A cursor's own outer/order/alias_to_orig/
    // orig_to_alias/links are never populated — all row reads/writes go
    // through root + anchor_path instead (see resolve_anchor_path()).
    ModDict* root = nullptr;
    std::vector<std::string> anchor_path;

    // The true top-level ModDict — self for a root, root for a cursor.
    // Convenience for code that needs "where does the live_cursors registry/
    // notification live", not required for indices (see IndexStore above).
    ModDict* true_root() const { return root ? root : const_cast<ModDict*>(this); }

    // Live-cursor notification registry — only meaningful/populated on a
    // true root (true_root() == this). Keyed by pattern_key(anchor_path);
    // each value is a vector of owned PyWeakref objects (PyObject* — the
    // weakref object itself is owned/Py_DECREF'd, what it *points at* is a
    // non-owning weak reference to a live cursor ModDictObject wrapper).
    FlatHashMap<std::string, std::vector<PyObject*>> live_cursors;

    // Returns a new cursor ModDict anchored at `path`, descending from
    // *this* (flattened onto true_root() + this->anchor_path + path if
    // *this* is itself a cursor — a nested cursor() call is just a longer
    // anchor path, not a new indirection layer). Raises ValueError if any
    // segment is a wildcard ("?") or the resolved path isn't dict-shaped /
    // doesn't exist (no lazy creation).
    ModDict* cursor(const std::vector<std::string>& path) const;

    // Registers a weak reference into this cursor's true root's
    // live_cursors registry, keyed by this object's own anchor_path. The
    // weakref itself must already be constructed by the binding layer
    // (creating a PyWeakref needs the fully-built Python wrapper object,
    // which doesn't exist yet inside cursor() itself) — call this
    // immediately after a cursor's ModDictObject wrapper is created.
    void register_live_cursor(PyObject* weakref);

    // ──────────────────────────────────────────────────
    // Per-cursor derived state — only meaningful when root != nullptr.
    // Private to each cursor instance (NOT shared, unlike indices above).
    // ──────────────────────────────────────────────────
    PyObject* cached_anchor_dict = nullptr;      // borrowed; last-resolved anchor dict, for rebind detection
    uint64_t  cached_top_hash = 0;               // path[0]'s outer hash, set alongside cached_anchor_dict

    // Row identity here is the actual KEY PyObject* (owned/INCREF'd), not a
    // hash — a cursor's data lives in a raw Python dict, and every operation
    // needs the real key object to call PyDict_GetItem/PyDict_SetItem, so a
    // hash would just need a second hash->key map for no benefit at this
    // (private, per-cursor, typically small) scale.
    // Always holds the CURRENT full presentation order once has_derived_order
    // is true — natural PyDict order when sort_field/group_field are both
    // empty, else the computed sort/group order. Emptiness of sort_index
    // itself is NOT the "inactive" sentinel (an active sort over zero rows
    // is legitimately empty too) — has_derived_order is.
    std::vector<PyObject*> sort_index;
    bool has_derived_order = false;              // true once rebuild_sort_index() has run at least once
    std::vector<std::string> sort_field;         // empty = unsorted (dotted path, pre-split into segments)
    bool sort_reverse = false;
    std::vector<std::string> group_field;        // empty = grouping inactive (dotted path, pre-split)
    FlatHashMap<uint64_t, char> filter_membership;  // set of currently-passing key hashes (value unused)
    PyObject* filter_predicate = nullptr;        // callable, or nullptr = inactive
    PyObject* live_connect_listeners = nullptr;  // dict of event_type -> list[callback]

    // (old_index,new_index) pairs — the vocabulary every diff-returning
    // cursor method shares (set_sort/set_filter/set_group/insert/update_row/
    // delete/insert_batch). old==-1 means "newly appeared", new==-1 means
    // "disappeared" (filtered out or removed).
    using IndexDiff = std::vector<std::pair<Py_ssize_t,Py_ssize_t>>;

    // Full O(n log n) rebuild — the "rare, explicit reconfigure" cost the
    // design deliberately allows (see set_sort/set_filter/set_group in the
    // project's design memory). Per-mutation maintenance is a separate,
    // cheaper incremental path (see insert()/insert_batch()). `field`/
    // `group_by_field` are dotted paths already split into literal segments
    // by the binding layer (reuses the same parser filter()/create_index()
    // use) — a value missing on a given row sorts as "incomparable, pushed
    // to the end" rather than raising. Diffs against whatever presentation
    // order (sort/group if already active, else natural dict order) existed
    // before the call. Only meaningful when root != nullptr.
    IndexDiff set_sort(const std::vector<std::string>& field, bool reverse);
    IndexDiff set_filter(PyObject* predicate);              // nullptr clears the filter
    IndexDiff set_group(const std::vector<std::string>& group_by_field);  // empty clears

    // Rebuilds sort_index in place from cached_anchor_dict's current rows,
    // using group_field (primary key, if non-empty) + sort_field (secondary,
    // or sole key if group_field empty) as the comparator; natural PyDict
    // order if both are empty. Sets has_derived_order=true. Missing/
    // incomparable field values sort after comparable ones, never raise.
    void rebuild_sort_index();

    // The keys (borrowed) in whatever order is CURRENTLY being presented —
    // sort_index if has_derived_order, else natural PyDict iteration order
    // over `d`. Used to snapshot "old" positions before a reconfigure.
    std::vector<PyObject*> current_presentation_order(PyObject* d) const;

    // The actual ordering RULE (group value primary, sort value secondary,
    // missing/incomparable sorts last) — takes already-extracted ModValues,
    // no PyDict/field lookups of its own. Shared by sort_index_less() (looks
    // values up fresh per pair — fine for the O(log n)-call bisect path) and
    // rebuild_sort_index()'s precomputed-key sort (looks each row's values
    // up exactly ONCE, not once per comparison — see that function's own
    // comment) so the two can never silently disagree on ordering.
    bool less_by_values(const ModValue& ga, const ModValue& gb,
                         const ModValue& sa, const ModValue& sb) const;

    // true if `a` sorts before `b` under the CURRENT group_field (primary)
    // + sort_field (secondary) comparator, looking both rows' values up
    // fresh (cheap only when called O(log n) times — see
    // try_bisect_insert_sort_index() below; rebuild_sort_index() uses
    // less_by_values() directly against precomputed values instead, since it
    // calls a comparator O(n log n) times).
    bool sort_index_less(PyObject* a, PyObject* b) const;

    // O(log n) position search + O(n) vector shift (via std::vector::insert)
    // instead of rebuild_sort_index()'s full O(n log n) re-sort — the
    // performance-critical fast path for the common case: a single BRAND-NEW
    // key inserted while a sort/group is active and no filter is active.
    // Returns false (caller must fall back to a full resync_and_diff()) if
    // any of that doesn't hold: !has_derived_order, filter_predicate set, or
    // `key` already existed before this write (an overwrite can change an
    // existing row's sort position, which isn't an O(log n) update — call
    // this only for a genuinely new key, checked by the caller BEFORE the
    // write via PyDict_Contains). On success, sets out_new_pos to the row's
    // landing position and returns true — does NOT enumerate every existing
    // row shifted by the insertion (a GUI's beginInsertRows(pos,pos) already
    // implies that shift for everything after `pos`, same as Qt itself never
    // needs an explicit list of renumbered siblings — see set_sort()/
    // set_group() for the genuinely-scattered-reorder case, which still
    // needs the full picture). Requires the write (PyDict_SetItem) to have
    // already happened, since it reads the new row's field value(s) via `key`.
    bool try_bisect_insert_sort_index(PyObject* key, Py_ssize_t& out_new_pos);

    // Finds `key`'s current position in sort_index (after it's been rebuilt
    // to reflect a mutation), or -1 if the key isn't present / has_derived_
    // order is false. O(n) — used only on the rarer fallback paths (an
    // overwritten existing key, or an active filter) where a full rebuild
    // already happened anyway; this only adds a linear scan, not the O(n)
    // Python-object marshaling a full diff enumeration would cost.
    Py_ssize_t find_sort_index_position(PyObject* key) const;

    // Rebuilds filter_membership from filter_predicate against
    // cached_anchor_dict's current rows. Requires filter_predicate to be
    // non-null. Leaves PyErr set and returns early if the predicate raises —
    // caller must check PyErr_Occurred() afterward.
    void rebuild_filter_membership();

    // Re-resolves this cursor's anchored nested dict (fresh every call —
    // never cache the raw pointer across calls, see anchor_path comment
    // above). If the resolved PyObject* differs from cached_anchor_dict
    // (the anchor was rebound wholesale, e.g. `data["u1"]["orders"]={}`,
    // bypassing every mutation hook that would otherwise notify this
    // cursor), forces a resync of derived state before returning. Returns
    // nullptr (PyErr set) if the anchor no longer resolves at all (e.g. an
    // ancestor row was deleted). Only meaningful when root != nullptr.
    PyObject* resolve_cursor_dict();

    // Recomputes sort_index (if has_derived_order) and filter_membership (if
    // filter_predicate set) against cached_anchor_dict's CURRENT contents,
    // and returns the diff versus what they were before this call. Used both
    // by a cursor reacting to a mutation someone else made through a sibling
    // handle (see notify_live_cursors) and — for now, as the simple/correct
    // baseline, see project design memory's "simple first" iteration
    // philosophy — by this cursor's own insert()/update_row()/delete(), full
    // O(n) recompute rather than an incremental bisect/merge (a later,
    // purely-performance change, see insert_batch). Only meaningful when
    // root != nullptr; assumes resolve_cursor_dict()/cached_anchor_dict are
    // already current (call resolve_cursor_dict() first).
    IndexDiff resync_and_diff();

    // Dispatches `payload` to every callback registered via connect() for
    // event_type on THIS object. Only meaningful on a cursor
    // (live_connect_listeners is cursor-private). A listener's exception
    // propagates (fails loud) rather than being swallowed — connect() fires
    // synchronously on the calling thread, per the design; no thread
    // marshaling or error isolation is the library's job.
    void dispatch_event(const char* event_type, PyObject* payload);

    // Notifies every LIVE cursor (across the whole tree, not just this one)
    // whose anchor's top-level segment hash matches `changed_top_hash` — the
    // row that was just inserted/updated/reindexed/removed at the true root.
    // Prunes dead weakrefs it encounters along the way. For each affected
    // cursor OTHER than `originator` (siblings on the same anchor, reacting
    // to a mutation made through a different handle — pass the mutating
    // cursor itself as `originator` to exclude it, since it already has or
    // is about to compute its own precise diff via resync_and_diff();
    // without this it would get silently resynced here first, leaving
    // nothing left for its own explicit resync_and_diff() call to find),
    // calls resync_and_diff() and fires a "reorder" event with the result —
    // from a sibling's perspective the precise operation (insert/update/
    // delete) that caused the change isn't known, only that the view
    // changed. Only meaningful on a true root (live_cursors is only
    // populated there).
    void notify_live_cursors(uint64_t changed_top_hash, ModDict* originator = nullptr);

    // Point-mutation API for a cursor: writes through to the anchored raw
    // dict (PyDict_SetItem/DelItem, same as __setitem__/__delitem__), then
    // reindexes the top-level row, notifies sibling cursors, and returns
    // THIS cursor's own structural diff (via resync_and_diff()) so the
    // binding layer can compute changed_fields and fire the precise typed
    // connect() event (insert/update/delete) on this cursor itself. A row
    // failing an active filter contributes no reported position at all — see
    // set_filter(). Only meaningful when root != nullptr.
    //
    // Return values report ONLY this row's own position(s) — never every
    // sibling row that structurally shifted as a side effect. A GUI's
    // beginInsertRows(pos,pos)/beginRemoveRows(pos,pos)/beginMoveRows(old,new)
    // already implies that shift for Qt's whole downstream stack (selection,
    // persistent indices, delegates); enumerating every renumbered sibling
    // explicitly would be both redundant and, at scale, the dominant cost
    // (O(shift distance) Python-object marshaling for what's structurally an
    // O(log n) search + O(shift distance) pointer-only C++ vector shift — a
    // real, benchmark-measured regression this design deliberately avoids).
    // set_sort()/set_group()'s full-reconfigure diffs are a different case —
    // there, many rows genuinely move to unrelated positions, and the full
    // {old_index: new_index} picture is unavoidable (matches Qt's own
    // changePersistentIndexList need).
    //
    // -1 means "not applicable" (no old position because the row is new /
    // wasn't visible before; no new position because it was removed / an
    // active filter now excludes it) — surfaced as None at the Python layer.
    Py_ssize_t cursor_insert(PyObject* key, PyObject* row);                       // -> new_index
    std::pair<Py_ssize_t,Py_ssize_t> cursor_update_row(PyObject* key, PyObject* changes);  // -> (old_index, new_index); changes merged via PyDict_Update
    Py_ssize_t cursor_delete(PyObject* key);                                      // -> old_index

    // Bulk insert: `rows` is {key: row_dict, ...}, merged into the anchored
    // dict via one PyDict_Update() call (not a per-row Python-level loop),
    // then reindexed and resynced ONCE for the whole batch — O(n log n)
    // total (one sort-index rebuild) rather than O(n²) from calling
    // cursor_insert() once per row (which would resync after each one).
    // Fires a single "insert" connect() event for the whole batch. Returns
    // only the NEW rows' own landing positions, in the same key-iteration
    // order as `rows` — same "don't enumerate shifted siblings" principle
    // as cursor_insert() above, extended to a batch of new rows.
    std::vector<Py_ssize_t> cursor_insert_batch(PyObject* rows);

    // ──────────────────────────────────────────────────
    // Constructors
    // ──────────────────────────────────────────────────
    ModDict();                                                     // genuine root
    ModDict(ModDict* anchor_root, std::vector<std::string> path);  // cursor, anchored at anchor_root+path
    ~ModDict();

    // ──────────────────────────────────────────────────
    // Insert
    // ──────────────────────────────────────────────────
    bool insert(const ModValue& key, const ModValue& value);
    // skip_field_index=true skips this row's per-FieldIndex on_insert_row()
    // call — for bulk-loading code that will instead call FieldIndex::
    // rebuild() once at the end for every existing index (O(N log N) total
    // rather than O(N) shift-per-row × k rows — see ModDict_update's simple
    // bulk-insert mode). Default false: every other caller is unaffected.
    void insert_row(const ModValue& outer_key, PyObject* dict_obj, bool skip_field_index = false);

    // ──────────────────────────────────────────────────
    // Read
    // ──────────────────────────────────────────────────
    bool     contains(const ModValue& key) const;
    ModDict* deep_copy() const;

    size_t len()   const { return order.size(); }
    bool   empty() const { return order.empty(); }

    // Access by insertion-order index (negative = from end). Returns nullptr if out of range.
    // Returns borrowed key_py and val_py via out params; returns false if OOB.
    bool at(Py_ssize_t i, uint64_t& out_hash) const {
        Py_ssize_t n = (Py_ssize_t)order.size();
        if (i < 0) i += n;
        if (i < 0 || i >= n) return false;
        out_hash = order[(size_t)i];
        return true;
    }

    // Full row → Py_INCREF'd dict, O(1).
    PyObject* get_row(uint64_t outer_hash) const;

    // Sub-dict or scalar at path. path = "\x01"-joined segments.
    PyObject* get_subrow(uint64_t outer_hash, const std::string& path) const;

    // Borrowed ref to the stored row dict (for FieldIndex, merges). No Py_INCREF.
    PyObject* get_row_ref(uint64_t outer_hash) const;

    bool remove(const ModValue& key);
    void reindex_row(uint64_t outer_hash);

    // Same field-index rebuild as reindex_row(), but skips the link-validation
    // pass. Used internally by delete_with_link_semantics() for the reindex
    // steps that happen BETWEEN cascade steps, where the table is expected to
    // be transiently inconsistent (a referrer not yet cascade-deleted still
    // pointing at a just-deleted row) — validating there would reject states
    // the cascade is already in the middle of fixing. The cascade's own logic
    // guarantees the table is fully consistent once it returns.
    // originator: pass `this` when called from a cursor's own point-mutation
    // method (cursor_insert/cursor_update_row/cursor_delete) so the
    // resulting notify_live_cursors() call excludes the cursor that already
    // has (or is about to compute) its own precise diff via resync_and_diff()
    // — without this, the cursor's own state would get silently resynced
    // here first, leaving nothing left to diff when it asks explicitly.
    // Default nullptr (no exclusion) for every other caller, where the
    // mutation didn't go through any specific cursor.
    void reindex_row_no_validate(uint64_t outer_hash, ModDict* originator = nullptr);

    // ──────────────────────────────────────────────────
    // Operations
    // ──────────────────────────────────────────────────
    int merges(ModDict* target,
               const std::vector<const char*>& on_source,
               const std::vector<const char*>& on_target,
               MergeConflict conflict = MergeConflict::KEEP_LEFT);

    ModDict* filter(const std::string& field,
                    FilterOp op, const ModValue& value) const;
    ModDict* filter(const std::vector<std::string>& pattern,
                    FilterOp op, const ModValue& value) const;

    // EQ-only fast path for a pattern containing one or more "__follow_link__"
    // hops (e.g. "orders.?.customer_id->name") — chains FieldIndex::
    // find_wildcard_leaf_eq lookups backwards from the rightmost (compared)
    // segment through each hop's already-built link index, never falling back
    // to a per-row scan. Called by filter(pattern,...) when op==EQ and the
    // pattern contains a hop; ValueError (ptr set) if any hop's link wasn't
    // declared via link() first.
    ModDict* filter_linked_eq(const std::vector<std::string>& pattern, const ModValue& value) const;

    using SortResult  = std::vector<PyObject*>;
    SortResult sort_by(const std::string& field, bool reverse) const;

    // paths[i] is looked up on each row; labels[i] is the key it's stored
    // under in the result row (result key naming — default "last segment of
    // the path" vs. explicit user labels — is a binding-layer concern; this
    // just requires paths.size()==labels.size(), no uniqueness check of its
    // own — a later paths[i] silently overwrites an earlier one under the
    // same label, same as any PyDict_SetItemString would).
    ModDict* select(const std::vector<std::string>& paths, const std::vector<std::string>& labels) const;

    // Anchored/wildcard select — one or more patterns of shape "table.?...",
    // optionally containing "__follow_link__" hops, all sharing the same
    // anchor table (segment 0). Result is a FLAT ModDict keyed by each
    // matched anchor row's own key (same convention as the plain select()
    // above), one projected {field_labels[i]: value} dict per row — not
    // filter()'s {table: {...}} nesting. Rows missing every field are
    // skipped, same as plain select().
    ModDict* select_anchored(const std::vector<std::vector<std::string>>& patterns,
                              const std::vector<std::string>& field_labels) const;

    using GroupResult = std::vector<std::pair<ModValue, ModDict*>>;
    GroupResult group_by(const std::string& field) const;

    void create_index(const std::string& field_name);
    void create_index(const std::vector<std::string>& pattern);
    void drop_index(const std::string& field_name);
    void drop_index(const std::vector<std::string>& pattern);
    bool has_index(const std::string& field_name) const;
    bool has_index(const std::vector<std::string>& pattern) const;

    // ──────────────────────────────────────────────────
    // Links (see LinkDecl above)
    // ──────────────────────────────────────────────────

    // Declares source_pattern -> references_pattern. Validates every existing
    // match resolves to a real target row (raises ValueError on any dangling
    // reference — ptr set on error). No-op (silently ignored) if an identical
    // link is already declared.
    void link(const std::vector<std::string>& source_pattern,
              const std::vector<std::string>& references_pattern,
              LinkOnDelete on_delete = LinkOnDelete::RESTRICT);

    // Resolves one "->" hop: fk_val is the value just read from `field` on a
    // row of `*current_table`. Looks up the link declared for exactly
    // {*current_table, "?", field}. On success, updates *current_table to the
    // hop's target table (for chaining further hops) and returns the
    // (borrowed) target row. Returns nullptr for a nullable/dangling FK (not
    // an error — *no_link stays false) or for no declared link (*no_link set
    // true — caller should raise). Public because the "->"-aware scan_here()
    // engine for filter()'s returns="rows_here"/"values" lives in the Python
    // binding layer (mod_dict_type.cpp), not core, and needs this without
    // reaching into core-internal statics (resolve_link_hop/resolve_table
    // stay file-local to mod_dict.cpp).
    PyObject* resolve_hop(std::string& current_table, const std::string& field,
                           PyObject* fk_val, bool* no_link) const;

    // Finds the declared link matching source_pattern exactly (raises
    // ValueError if none). Returns a new ModDict of the resolved target rows
    // for every current match of source_pattern (outer keys = target keys).
    //
    // key_filter (optional): only scan source rows whose own key hashes into
    // one of these — lets a caller chain hops by passing the previous hop's
    // result keys, e.g. org.follow(path, &prev_result_key_hashes).
    //
    // value_filter (optional, mutually exclusive with key_filter): skip
    // scanning the source table entirely and resolve these values directly
    // against the target — for values obtained from somewhere other than a
    // source-table scan (e.g. an external list of ids).
    ModDict* follow(const std::vector<std::string>& source_pattern,
                     const std::vector<uint64_t>* key_filter = nullptr,
                     const std::vector<PyObject*>* value_filter = nullptr) const;

    const LinkDecl* find_link(const std::vector<std::string>& source_pattern) const;

    // Deletes `key` from `table`'s nested row, applying on_delete semantics
    // for any declared link that targets this table:
    //   restrict  — refuses (raises ValueError, table left untouched) if any
    //               row still references `key`.
    //   cascade   — deletes referencing rows too, recursively. Safe under
    //               cycles: deletes `key` (and reindexes) BEFORE looking up
    //               who referenced it, so a cycle's own back-reference is
    //               already scrubbed by the time recursion would return to
    //               it — no separate visited-set needed.
    //   set_null  — clears the reference field on referencing rows.
    // Returns false (PyErr set) on restrict-refusal or a real Python error;
    // true if the deletion (and any cascade/set_null side effects) completed.
    bool delete_with_link_semantics(const std::string& table, PyObject* key);

    PyObject* to_python_dict() const;
    std::string to_string() const;
    PyObject* to_json() const;
    PyObject* to_python() const;
    void dump() const;

    std::vector<uint8_t> serialize() const;
    void deserialize(const uint8_t* data, size_t len);

    // True if data starts with the ModDict container magic (produced by serialize()).
    // Lets module-level dumps()/loads() distinguish a whole-ModDict blob from a
    // single arbitrary-value blob without duplicating the magic constant.
    static bool has_container_magic(const uint8_t* data, size_t len);
};

#endif
