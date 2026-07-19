#ifndef MOD_DICT_FIELD_INDEX_H
#define MOD_DICT_FIELD_INDEX_H

#include "flat_hash_map.h"
#include "mod_value.h"
#include <string>
#include <vector>
#include <utility>
#include <cstddef>

class ModDict;
enum class FilterOp : int;

/* ============================================================================
   SortedEntry — запись в отсортированном индексе (только числовые типы)
   ============================================================================ */
struct SortedEntry {
    ValueType type;           // INT или FLOAT
    union {
        int64_t int_val;
        double  float_val;
    };
    uint64_t outer_key_hash;  // хэш внешнего ключа — для инкрементальных обновлений
    size_t   slot = SIZE_MAX; // прямой слот в data.entries_ (SIZE_MAX = неизвестен)

    bool less_than(const SortedEntry& other) const {
        if (type == ValueType::INT && other.type == ValueType::INT)
            return int_val < other.int_val;
        double a = (type == ValueType::FLOAT) ? float_val : (double)int_val;
        double b = (other.type == ValueType::FLOAT) ? other.float_val : (double)other.int_val;
        return a < b;
    }

    bool equal_to(const SortedEntry& other) const {
        if (type == ValueType::INT && other.type == ValueType::INT)
            return int_val == other.int_val;
        double a = (type == ValueType::FLOAT) ? float_val : (double)int_val;
        double b = (other.type == ValueType::FLOAT) ? other.float_val : (double)other.int_val;
        return a == b;
    }
};

/* ============================================================================
   FieldIndex — двойной индекс по одному полю вложенного словаря:
     hash_index  → O(1) для EQ / NE
     sorted_index → O(log n) для LT / LE / GT / GE (только числовые поля)

   Поддерживает два режима:
     - простой:   field_name = "age" → один path_hash
     - wildcard:  pattern = ["orders","__pass_key__","status"] → N path_hashes
   ============================================================================ */
class FieldIndex {
public:
    std::string field_name;               // простой режим: "age", wildcard: пустая
    bool is_wildcard = false;
    std::vector<std::string> pattern;     // wildcard: сегменты пути

    // Set once by build_wildcard() — whether pattern[0] resolved to a real
    // top-level ModDict key at build time (the "g1.?.field" anchor-path
    // case). on_insert_row()/on_remove_row() must reuse this cached decision
    // instead of re-deriving it per mutation: re-deriving via find_anchor()
    // and bailing out when it comes back empty silently stops maintaining
    // the index for the (much more common) unanchored case, e.g.
    // create_index("meta.level") or create_index("orders.?.status") where
    // pattern[0] is just an ordinary per-row field, not a table key.
    bool     has_anchor  = false;
    uint64_t anchor_hash = 0;

    // field_value_hash → [outer_key_hash, ...]
    // Для wildcard-паттернов один outer_key_hash может встречаться несколько раз
    // (по разу на каждый совпавший inner-ключ этой внешней строки).
    FlatHashMap<uint64_t, std::vector<uint64_t>> hash_index;

    // отсортированный список для range-запросов (числовые типы)
    std::vector<SortedEntry> sorted_index;

    // Для wildcard с нетерминальным "__pass_key__" (т.е. НЕ терминальный "?",
    // у которого своя отдельная логика через hash_index — matched key = value).
    // Один "?" = один уровень вложенности (для нескольких уровней путь пишется
    // явно как "?.?.field" — по одному "?" на уровень). Хранит для каждого
    // совпадения ПОЛНУЮ цепочку ключей, выбранных каждым "?" в пути, по порядку.
    // Литеральные сегменты пути НЕ хранятся здесь — они статически известны из
    // `pattern` и разрешаются напрямую при восстановлении (см. insert_pruned_path
    // в mod_dict.cpp). Позволяет filter() восстановить pruned-результат прямыми
    // PyDict_GetItem без повторного скана внешней строки — для ЛЮБОГО числа "?".
    FlatHashMap<uint64_t, std::vector<std::pair<uint64_t, std::vector<PyObject*>>>> wildcard_leaf_index;

    // ──────────────────────────────────────────────────
    // Построение / сброс
    // ──────────────────────────────────────────────────
    void build(ModDict* owner, const std::string& fname);
    void build_wildcard(ModDict* owner, const std::vector<std::string>& pat);
    void clear();

    // Rebuilds from scratch using whatever field_name/pattern this index was
    // ORIGINALLY built with (already stored as members) — for bulk-load
    // callers that skip per-row on_insert_row()/on_remove_row() (O(n) shift
    // each) during a loop and rebuild every existing index ONCE at the end
    // instead (O(N log N) total, not O(N) × k rows). Reuses build()/
    // build_wildcard() verbatim, no new indexing logic.
    void rebuild(ModDict* owner) { if (is_wildcard) build_wildcard(owner, pattern); else build(owner, field_name); }

    // ──────────────────────────────────────────────────
    // Инкрементальные обновления
    // ──────────────────────────────────────────────────
    void on_insert(uint64_t outer_key_hash, const ModValue& field_val);
    void on_remove(uint64_t outer_key_hash, const ModValue& field_val);

    // Wildcard-aware версии: сами достают нужные значения из owner
    void on_insert_row(uint64_t outer_key_hash, ModDict* owner);
    void on_remove_row(uint64_t outer_key_hash, ModDict* owner);

    // Value-agnostic removal: scan all buckets and drop outer_key_hash.
    // Use when the row has already been mutated so on_remove_row would read wrong value.
    void remove_outer_key(uint64_t outer_key_hash);

    // ──────────────────────────────────────────────────
    // Запросы
    // ──────────────────────────────────────────────────
    std::vector<uint64_t>* find_eq(uint64_t field_val_hash) const;
    std::vector<uint64_t> find_range(FilterOp op, const ModValue& val) const;
    bool is_numeric_range_supported(const ModValue& val) const;

    // Valid whenever is_wildcard is true and the pattern's last segment is not
    // "__pass_key__" (terminal-key patterns don't use this — see comment above).
    const std::vector<std::pair<uint64_t,std::vector<PyObject*>>>* find_wildcard_leaf_eq(uint64_t field_val_hash) const;

    size_t size() const { return hash_index.size(); }
};

#endif
