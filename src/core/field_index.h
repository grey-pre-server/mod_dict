#ifndef MOD_DICT_FIELD_INDEX_H
#define MOD_DICT_FIELD_INDEX_H

#include "flat_hash_map.h"
#include "mod_value.h"
#include <string>
#include <vector>
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

    // field_value_hash → [outer_key_hash, ...]
    FlatHashMap<uint64_t, std::vector<uint64_t>> hash_index;

    // отсортированный список для range-запросов (числовые типы)
    std::vector<SortedEntry> sorted_index;

    // ──────────────────────────────────────────────────
    // Построение / сброс
    // ──────────────────────────────────────────────────
    void build(ModDict* owner, const std::string& fname);
    void build_wildcard(ModDict* owner, const std::vector<std::string>& pat);
    void clear();

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

    size_t size() const { return hash_index.size(); }
};

#endif
