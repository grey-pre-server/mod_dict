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
// The three TEXT_* ops are substring predicates over str fields — always a
// scan (no index can serve them), case-insensitive via casefold, and a
// non-str field value simply doesn't match. Exposed as
// filter(path).text_search(needle, mode=...).
// IN / BETWEEN / PREDICATE exist for the cursor's set_filter() condition only
// (root filter() composes in_/between from EQ/GE/LE calls and has no
// callable form) — root code never sees them.
enum class FilterOp      { EQ, NE, LT, LE, GT, GE, TEXT_CONTAINS, TEXT_STARTSWITH, TEXT_ENDSWITH,
                           IN, BETWEEN, PREDICATE };
inline bool is_text_op(FilterOp op) { return op == FilterOp::TEXT_CONTAINS || op == FilterOp::TEXT_STARTSWITH || op == FilterOp::TEXT_ENDSWITH; }
// The text-op predicate (defined in mod_dict.cpp): `needle` must already be
// casefolded by the caller; a non-str `field` never matches.
bool text_match(PyObject* field, FilterOp op, PyObject* needle);
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
    PyObject* val_py = nullptr;
    bool      is_row = false;
};

class ModDict {
public:
    FlatHashMap<uint64_t, OuterEntry> outer;
    std::vector<uint64_t> order;  // outer hashes in insertion order

    // `indices` is a reference bound in the ctor to own_indices (root) or
    // root->indices (cursor) — gives a cursor shared field-indices without
    // touching every existing indices.by_field call site.
    struct IndexStore { FlatHashMap<std::string, class FieldIndex*> by_field; };
    IndexStore  own_indices;
    IndexStore& indices;

    std::vector<LinkDecl> links;

    PyObject* on_change_cb = nullptr;
    PyObject* on_merge_cb  = nullptr;
    struct ModDictObject* py_wrapper = nullptr;

    // root==nullptr => genuine root (owns outer/order/indices). A cursor has
    // root pointing at the true top-level ModDict (flattened — cursor-of-
    // cursor collapses to one hop) and a non-empty, literal anchor_path
    // descending from root's outer to the {pk: row} table it addresses. A
    // cursor's own outer/order/links are never populated — reads/
    // writes go through root + anchor_path (resolve_anchor_path()).
    ModDict* root = nullptr;
    std::vector<std::string> anchor_path;

    ModDict* true_root() const { return root ? root : const_cast<ModDict*>(this); }

    // Weakrefs to live cursors, keyed by pattern_key(anchor_path); only
    // populated on a true root. Used to notify siblings on mutation.
    FlatHashMap<std::string, std::vector<PyObject*>> live_cursors;

    // New cursor anchored at `path` from *this (flattened onto true_root()
    // if *this is itself a cursor). Raises if `path` has a wildcard segment
    // or doesn't resolve to an existing dict (no lazy creation).
    ModDict* cursor(const std::vector<std::string>& path) const;

    // Called after the binding layer builds the cursor's Python wrapper
    // (register_live_cursor can't build the weakref itself — needs that
    // wrapper to already exist).
    void register_live_cursor(PyObject* weakref);

    // True while at least one cursor on this root is alive. Decides whether a
    // row handed out by root [key]/get() must be wrapped in RowProxy so a
    // nested write through it reaches reindex_row() → notify_live_cursors():
    // without that hook a root-side write under a cursor's anchor never
    // reaches the cursor, whose maintained sort_index/visible_index then
    // silently drift from the dict (len() one thing, at()/view_keys()
    // another). Prunes dead weakrefs it meets, so a long-gone cursor stops
    // costing anything; O(1) once the first live one is found.
    bool has_live_cursors();

    // Per-cursor derived state — only meaningful when root != nullptr.
    PyObject* cached_anchor_dict = nullptr;      // borrowed; last-resolved anchor dict, for rebind detection
    uint64_t  cached_top_hash = 0;               // path[0]'s outer hash, set alongside cached_anchor_dict

    // Owned/INCREF'd keys, not hashes — ops need the real key object for
    // PyDict_GetItem/SetItem. has_derived_order (not emptiness) is the
    // "active" sentinel, since a sort over zero rows is legitimately empty.
    std::vector<PyObject*> sort_index;
    bool has_derived_order = false;              // true once rebuild_sort_index() has run at least once
    std::vector<std::string> sort_field;         // empty = unsorted (dotted path, pre-split into segments)
    bool sort_reverse = false;
    std::vector<std::string> group_field;        // empty = grouping inactive (dotted path, pre-split)
    // Owned PyUnicode segments mirroring sort_field/group_field, rebuilt in
    // set_sort()/set_group() — lets read_field_path use PyDict_GetItem
    // instead of PyDict_GetItemString, which allocates a temporary PyUnicode
    // from the C string on every single call.
    std::vector<PyObject*> sort_field_py;
    std::vector<PyObject*> group_field_py;
    FlatHashMap<uint64_t, char> filter_membership;  // set of currently-passing key hashes (value unused)
    // The active filter condition — set_filter(path).<op>(value). Non-null
    // filter_predicate means "a filter is active" (every `if (filter_predicate)`
    // check keys off it) and holds the OPERAND: the comparison value for
    // eq/lt/..., the casefolded needle for text_search, the callable for
    // predicate(). filter_op says how to apply it, filter_path (already
    // split, as PyUnicode segments for PyDict_GetItem) says to WHAT — the
    // whole row when the path is the single "?" segment (filter_path empty),
    // else the field the path reaches. One evaluation routine,
    // filter_row_passes(), serves bootstrap and incremental maintenance alike.
    PyObject* filter_predicate = nullptr;        // operand (see above), or nullptr = inactive
    FilterOp  filter_op = FilterOp::EQ;
    std::vector<PyObject*> filter_path_py;      // owned PyUnicode segments; empty = "?" (row itself)
    PyObject* filter_operand2 = nullptr;        // BETWEEN's `hi` (filter_predicate is `lo`); nullptr otherwise
    // True when the row (or the field value at filter_path) passes the
    // active condition. Leaves PyErr set (predicate() raised, or a
    // comparison failed hard) and returns false in that case — callers that
    // already check PyErr_Occurred() after the membership routines keep
    // working unchanged.
    bool filter_row_passes(PyObject* row) const;
    // Owned/INCREF'd keys — the filtered subsequence of sort_index (same
    // order), populated/maintained ONLY while filter_predicate is set;
    // empty and unused otherwise. Lets len()/iter()/.at() work against a
    // ready-made O(1)-indexable sequence instead of recomputing which rows
    // pass on every read.
    std::vector<PyObject*> visible_index;
    PyObject* live_connect_listeners = nullptr;  // dict of event_type -> list[callback]

    // (old_index, new_index) pairs — the ONE vocabulary set_sort()/set_group()/
    // set_filter() return and the "reorder" event carries. Positions are
    // PRESENTATION positions: exactly the coordinates len()/at()/iteration/
    // view_keys() and insert()/update_row()/delete()'s own return values use
    // — dense over the visible rows while a filter is active (position 0 is
    // the first VISIBLE row), raw order otherwise. -1 (None in Python) on a
    // side the row isn't visible on: old==-1 newly appeared, new==-1
    // disappeared (filtered out/removed). Rows whose position didn't change
    // are omitted — an empty diff means "nothing moved".
    using IndexDiff = std::vector<std::pair<Py_ssize_t,Py_ssize_t>>;

    // Bootstraps/reconfigures; each is an O(n log n) rebuild (rare, explicit
    // reconfigure cost — per-mutation maintenance is the separate, cheaper
    // incremental path in insert()/insert_batch()). Diffs against whatever
    // presentation order existed before the call.
    IndexDiff set_sort(const std::vector<std::string>& field, bool reverse);
    // Installs the condition (path already split into segments; empty =
    // "?" = the row itself), bootstraps membership, returns the diff. A null
    // `operand` with op==EQ and empty path clears the filter (set_filter(None)).
    // If bootstrapping raises (a predicate() that throws), the exception
    // propagates and the cursor is left UNFILTERED — never half-installed.
    IndexDiff set_filter(const std::vector<std::string>& path, FilterOp op,
                         PyObject* operand, PyObject* operand2 = nullptr);
    void clear_filter_condition();  // drops the stored operands/path (not membership)
    IndexDiff set_group(const std::vector<std::string>& group_by_field);  // empty clears

    // Rebuilds sort_index from cached_anchor_dict using group_field (primary)
    // + sort_field (secondary); natural PyDict order if both empty. Missing/
    // incomparable values sort last, never raise. Stable: rows that tie keep
    // their dict (insertion) order — the same order the incremental
    // bisect-insert produces (a new row lands after the rows it ties with).
    void rebuild_sort_index();

    // Keys in RAW order — sort_index when a maintained snapshot exists, else
    // the dict's own order. Filter-blind: this is the sequence
    // rebuild_visible_index() filters down to visible_index.
    std::vector<PyObject*> raw_order(PyObject* d) const;

    // Content hashes of the keys in PRESENTATION order (visible_index under
    // an active filter, else raw_order()) — the "before" side of every diff.
    // Hashes rather than pointers on purpose: a key freed by the mutation in
    // between (deleted row, rebuilt sort_index dropping its last reference)
    // must never be dereferenced again.
    std::vector<uint64_t> presentation_snapshot(PyObject* d) const;

    // Diffs two presentation snapshots into IndexDiff (see its comment for
    // the vocabulary). O(n) with two hash maps.
    static IndexDiff diff_snapshots(const std::vector<uint64_t>& before,
                                    const std::vector<uint64_t>& after);

    // The ordering rule (group primary, sort secondary) over already-
    // extracted values — shared by sort_index_less() and rebuild_sort_index()
    // so the O(log n) bisect path and the O(n) precomputed sort can never
    // silently disagree.
    bool less_by_values(const ModValue& ga, const ModValue& gb,
                         const ModValue& sa, const ModValue& sb) const;

    // Same rule as less_by_values(), but looks both rows' values up fresh —
    // only cheap when called O(log n) times (see bisect_insert_sort_index).
    bool sort_index_less(PyObject* a, PyObject* b) const;

    // Bisect-inserts `key` (caller guarantees it isn't already in sort_index)
    // — O(log n) search + O(shift) pointer-only vector::insert. Requires the
    // write to have already happened, since it reads the row's field
    // value(s). Lands AFTER every row it ties with (upper_bound): ties keep
    // insertion order, and a cursor with no sort/group field — where every
    // row ties — appends, i.e. natural insertion order (lower_bound would
    // put every new row at position 0). Deliberately does NOT also maintain a key->position hash map
    // during the shift: touching a hashmap entry per shifted element is far
    // more expensive than the memmove vector::insert already does (an
    // earlier attempt at this regressed insert() 100x+ at 50k-100k rows).
    Py_ssize_t bisect_insert_sort_index(PyObject* key);

    // Erases sort_index[pos] — O(shift distance) pointer-only vector::erase.
    void erase_from_sort_index(Py_ssize_t pos);

    // Removes `key` from sort_index at `old_pos` (skipped if < 0, i.e. a
    // genuinely new key) and bisect-inserts it back in based on its current
    // field value(s). Used whenever only THIS row's position may have moved
    // (insert of a new or overwritten key, or update_row) — every other row's
    // relative order is unaffected, so no full rebuild_sort_index() is
    // needed. O(log n) search + O(shift distance), vs. a full O(n log n) rebuild.
    // Stays put when it can: if the old slot is still inside the run of rows
    // equal to the row's current values, the row goes back exactly there —
    // an update to an unrelated field never reports a move.
    Py_ssize_t reposition_in_sort_index(PyObject* key, Py_ssize_t old_pos);

    // A row's sort-relevant field values, captured BEFORE a mutation so the
    // row's OLD position can still be located afterwards (once the row dict
    // is updated in place, its old values are gone).
    struct SortKeyValues { ModValue group_val; ModValue sort_val; };
    SortKeyValues capture_sort_key_values(PyObject* row) const;

    // Position of `key` in sort_index, -1 if absent.
    //
    // With a sort/group field active AND `old_vals` supplied, this is an
    // O(log n) bisect on the values (sort_index is ordered by exactly those)
    // followed by a walk of the equal-valued run to find the key — the
    // OLD-value lookup update_row()/delete() need. Without a comparator
    // (natural order, no set_sort/set_group) or without old_vals it falls
    // back to a linear scan — but a cheap one: pointer identity first
    // (sort_index holds the very key objects the anchored dict holds), then
    // rich-equality only on a pointer miss; the previous scan re-hashed EVERY
    // key on EVERY call, which is what made update_row() ~20x slower than
    // insert() at 50k rows (bench_cursor_vs_root.py, 2026-08-17).
    Py_ssize_t find_sort_index_position(PyObject* key, const SortKeyValues* old_vals = nullptr) const;

    // Rebuilds filter_membership from filter_predicate; leaves PyErr set and
    // returns early if the predicate raises.
    void rebuild_filter_membership();

    // Evaluates filter_predicate against `row` and updates filter_membership
    // for `key_hash` accordingly (insert if passing, erase if not) — the
    // single-row incremental counterpart to rebuild_filter_membership(),
    // used by insert()/update_row()/delete() so a single mutation doesn't
    // re-run the predicate against every row. Requires filter_predicate to
    // be non-null. Returns false (PyErr set) if the predicate raises.
    bool update_filter_membership_one(uint64_t key_hash, PyObject* row);

    // Rebuilds visible_index from raw_order() filtered down
    // to filter_membership — O(n). Call wherever rebuild_filter_membership()
    // is called for a full reconfigure (set_filter/resync/rebind), and
    // wherever rebuild_sort_index() runs while a filter is active (the
    // *order* changed even though membership didn't). No-op (leaves
    // visible_index empty) when filter_predicate is null.
    void rebuild_visible_index();

    // visible_index counterparts to bisect_insert_sort_index()/
    // erase_from_sort_index()/reposition_in_sort_index()/
    // find_sort_index_position() above — same shape (shifting a second
    // *vector* is cheap, same cost class as sort_index's own memmove) and,
    // with a sort/group comparator, the same bisect rules (O(log n); a
    // reposition stays in its old slot while that slot is still inside its
    // tie run). Without a comparator (natural order) every row "ties", so a
    // value bisect would put a newly visible row at the END of the list —
    // wrong: visible_index is the filtered SUBSEQUENCE of sort_index, and a
    // row revealed by a filter change must reappear in its dict-order place.
    // Natural order therefore derives the slot from the row's (already
    // final) position `raw_pos` in sort_index: right after the nearest
    // visible row that precedes it there (visible_position_from_raw). A
    // sorted cursor uses the same look-back, bounded, only for a row
    // REVEALED mid-run by a filter flip — see place_in_visible_index() in
    // the .cpp for the exact rules.
    Py_ssize_t visible_position_from_raw(PyObject* key, Py_ssize_t raw_pos, Py_ssize_t max_walk) const;
    Py_ssize_t bisect_insert_visible_index(PyObject* key, Py_ssize_t raw_pos);
    void erase_from_visible_index(Py_ssize_t pos);
    Py_ssize_t reposition_in_visible_index(PyObject* key, Py_ssize_t old_pos, Py_ssize_t raw_pos);
    Py_ssize_t find_visible_index_position(PyObject* key, const SortKeyValues* old_vals = nullptr) const;

    // Re-resolves the anchored dict fresh every call (never cache the raw
    // pointer). If it differs from cached_anchor_dict, the anchor was
    // rebound wholesale (e.g. `data["u1"]["orders"]={}`, bypassing the usual
    // mutation hooks) — forces a resync before returning. nullptr (PyErr
    // set) if the anchor no longer resolves at all.
    PyObject* resolve_cursor_dict();

    // Recomputes sort_index/filter_membership/visible_index against
    // cached_anchor_dict's current contents and diffs against their prior
    // state — the full-O(n) fallback for sibling notification when no
    // mutation hint is available (RowProxy writes, wholesale rebinds,
    // insert_batch). Assumes resolve_cursor_dict() has already been called.
    // Only meaningful with a maintained snapshot (has_derived_order or an
    // active filter): without one there is no "before" to diff against.
    IndexDiff resync_and_diff();

    // A mutation's identity, carried from the originating cursor's method to
    // the sibling notification — the whole reason siblings can update
    // incrementally instead of full-rebuilding: without it a sibling only
    // learns "something under this top-level key changed" and its only
    // correct move is resync_and_diff() (O(n log n) + one filter-predicate
    // call PER ROW, measured 1137us vs the originator's own 1.8us bisect
    // path for the same insert). `key` is borrowed for the duration of the
    // notify call. `key_existed` matters for Insert only: it decides whether
    // the sibling must O(n)-scan for the old position (overwrite) or can
    // pure-bisect (new key).
    struct CursorMutationHint {
        enum class Kind : uint8_t { Insert, Update, Remove };
        Kind kind;
        PyObject* key;
        bool key_existed;
        // The row AS IT WAS before the mutation (borrowed for the notify
        // call; nullptr for a brand-new Insert, or when the originator has
        // no snapshot — a sibling then falls back to a linear scan for the
        // old position, still far cheaper than a full resync). Siblings need
        // it to locate the row's OLD position by bisect on their OWN sort
        // fields — which the originator can't precompute for them, since
        // every cursor sorts by its own field. Update mutates the row dict
        // in place, so the originator hands over a shallow snapshot; Remove
        // keeps the removed row alive across the notify with an INCREF.
        PyObject* old_row;
        // The dict `key` lives in (borrowed). A sibling applies the hint only
        // if this is its OWN anchored dict — a mutation under the same
        // top-level key but at another level (a different sub-table, or a
        // field write inside a row) is not this table's row insert/update/
        // remove, and such a sibling takes the full resync instead.
        PyObject* dict;
    };

    // Applies one hinted mutation to THIS (sibling) cursor's derived state —
    // same primitives, same order as the originator's own cursor_insert()/
    // cursor_update_row()/cursor_delete() post-write maintenance, so both
    // cursors converge on identical index states. Requires has_derived_order
    // (a maintained snapshot to update); the anchored dict already contains
    // the mutation. Returns the same diff resync_and_diff() would (IndexDiff
    // vocabulary: presentation positions, only what moved) — built only when
    // want_diff (someone listens for "reorder"); index maintenance happens
    // either way. May leave PyErr set (filter predicate raised) — caller
    // decides whether to clear.
    IndexDiff sibling_apply_hint(const CursorMutationHint& hint, bool want_diff);

    void dispatch_event(const char* event_type, PyObject* payload);

    // True when at least one connect() listener is registered for the event
    // — lets notify_live_cursors() skip building a diff nobody would see.
    bool has_listener(const char* event_type) const;

    // Notifies every live cursor whose anchor's top segment hash matches
    // `changed_top_hash`, excluding `originator` (the cursor that made the
    // mutation, which already maintains its own state inline — without the
    // exclusion it would get silently resynced here first, leaving nothing
    // for its own maintenance to do). With a `hint`, siblings holding a
    // maintained snapshot update incrementally via sibling_apply_hint();
    // hint-less notifications (RowProxy writes, rebinds, batches) and
    // snapshot-less siblings fall back to resync_and_diff(). Prunes dead
    // weakrefs along the way. Only meaningful on a true root.
    void notify_live_cursors(uint64_t changed_top_hash, ModDict* originator = nullptr,
                             const CursorMutationHint* hint = nullptr);

    // Point-mutation API for a cursor: writes through to the anchored dict,
    // reindexes, notifies siblings, and returns only THIS row's own
    // position(s) — never every sibling shifted as a side effect, since a
    // GUI's beginInsertRows/beginRemoveRows/beginMoveRows already implies
    // that shift (enumerating it was a real, benchmark-measured regression).
    // -1 means not applicable (new/removed/filtered-out) — None in Python.
    Py_ssize_t cursor_insert(PyObject* key, PyObject* row);                       // -> new_index
    std::pair<Py_ssize_t,Py_ssize_t> cursor_update_row(PyObject* key, PyObject* changes);  // -> (old_index, new_index)
    Py_ssize_t cursor_delete(PyObject* key);                                      // -> old_index

    // `rows` = {key: row_dict, ...}, merged in one PyDict_Update() call and
    // resynced once for the whole batch. Returns only the new rows' landing
    // positions, in `rows`' own iteration order.
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
    // skip_field_index=true skips the per-row FieldIndex update, for bulk
    // loaders that call FieldIndex::rebuild() once at the end instead.
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
    // `hint`: set by a RowProxy write that inserted/overwrote/removed ONE key
    // of ONE dict under this row (see CursorMutationHint) — lets cursors
    // anchored on exactly that dict update incrementally; nullptr elsewhere.
    void reindex_row(uint64_t outer_hash, const CursorMutationHint* hint = nullptr);

    // Same field-index rebuild as reindex_row(), but skips the link-validation
    // pass. Used internally by delete_with_link_semantics() for the reindex
    // steps that happen BETWEEN cascade steps, where the table is expected to
    // be transiently inconsistent (a referrer not yet cascade-deleted still
    // pointing at a just-deleted row) — validating there would reject states
    // the cascade is already in the middle of fixing. The cascade's own logic
    // guarantees the table is fully consistent once it returns.
    // originator: the cursor making the mutation (excluded from its own
    // notify_live_cursors() resync — see that function). nullptr elsewhere.
    void reindex_row_no_validate(uint64_t outer_hash, ModDict* originator = nullptr,
                                 const CursorMutationHint* hint = nullptr);

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
