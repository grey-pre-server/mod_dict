#ifndef MOD_DICT_FLAT_HASH_MAP_H
#define MOD_DICT_FLAT_HASH_MAP_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <Python.h>

#ifdef key
    #undef key
#endif
#ifdef GetObject
    #undef GetObject
#endif
#ifdef LoadString
    #undef LoadString
#endif

/* ============================================================================
   Быстрый компаратор для PyObject*
   ============================================================================ */
struct PyObjectEqual {
    bool operator()(PyObject* a, PyObject* b) const {
        if (a == b) return true;

        if (PyLong_Check(a) && PyLong_Check(b)) {
            long va = PyLong_AsLong(a);
            long vb = PyLong_AsLong(b);
            return va != -1 && vb != -1 && va == vb;
        }

        if (PyUnicode_Check(a) && PyUnicode_Check(b)) {
            const char* sa = PyUnicode_AsUTF8(a);
            const char* sb = PyUnicode_AsUTF8(b);
            if (sa && sb) return strcmp(sa, sb) == 0;
        }

        if (PyFloat_Check(a) && PyFloat_Check(b)) {
            return PyFloat_AsDouble(a) == PyFloat_AsDouble(b);
        }

        return PyObject_RichCompareBool(a, b, Py_EQ) == 1;
    }
};

/* ============================================================================
   Хешер для PyObject* — хеширует ЗНАЧЕНИЕ, а не указатель
   ============================================================================ */
struct PyObjectHash {
    size_t operator()(PyObject* ptr) const noexcept {
        if (!ptr) return 0;
        return PyObject_Hash(ptr);
    }
};

/* ============================================================================
   FlatHashMap<K, V> — хеш-таблица с открытой адресацией
   K — тип ключа, V — тип значения
   ============================================================================ */
template<typename K, typename V>
class FlatHashMap {
public:
    struct Entry {
        K key;
        V value;
        bool occupied = false;
    };

    FlatHashMap(size_t initial_capacity = 8)
        : entries_(nullptr), capacity_(0), mask_(0), size_(0)
    {
        if (initial_capacity > 0) resize(initial_capacity);
    }

    ~FlatHashMap() { delete[] entries_; }

    FlatHashMap(const FlatHashMap&) = delete;
    FlatHashMap& operator=(const FlatHashMap&) = delete;

    FlatHashMap(FlatHashMap&& other) noexcept
        : entries_(other.entries_), capacity_(other.capacity_)
        , mask_(other.mask_), size_(other.size_)
    {
        other.entries_ = nullptr;
        other.capacity_ = 0;
        other.mask_ = 0;
        other.size_ = 0;
    }

    FlatHashMap& operator=(FlatHashMap&& other) noexcept {
        if (this != &other) {
            delete[] entries_;
            entries_ = other.entries_;
            capacity_ = other.capacity_;
            mask_ = other.mask_;
            size_ = other.size_;
            other.entries_ = nullptr;
            other.capacity_ = 0;
            other.mask_ = 0;
            other.size_ = 0;
        }
        return *this;
    }

    std::pair<V*, bool> insert(K key, V value) {
        if (size_ >= (size_t)(capacity_ * GROW_THRESHOLD)) {
            resize(capacity_ == 0 ? 8 : capacity_ * 2);
        }

        size_t index = hash_(key) & mask_;
        size_t start = index;

        while (entries_[index].occupied) {
            if (equal_(entries_[index].key, key)) {
                entries_[index].value = value;
                return {&entries_[index].value, false};
            }
            index = (index + 1) & mask_;
            if (index == start) break;
        }

        entries_[index].key = key;
        entries_[index].value = value;
        entries_[index].occupied = true;
        size_++;
        return {&entries_[index].value, true};
    }

    V* find(K key) {
        if (capacity_ == 0) return nullptr;

        size_t index = hash_(key) & mask_;
        size_t start = index;

        while (entries_[index].occupied) {
            if (equal_(entries_[index].key, key)) {
                return &entries_[index].value;
            }
            index = (index + 1) & mask_;
            if (index == start) break;
        }
        return nullptr;
    }

    const V* find(K key) const {
        if (capacity_ == 0) return nullptr;

        size_t index = hash_(key) & mask_;
        size_t start = index;

        while (entries_[index].occupied) {
            if (equal_(entries_[index].key, key)) {
                return &entries_[index].value;
            }
            index = (index + 1) & mask_;
            if (index == start) break;
        }
        return nullptr;
    }

    V& operator[](K key) {
        auto [ptr, inserted] = insert(key, V{});
        return *ptr;
    }

    bool erase(K key) {
        if (capacity_ == 0) return false;

        size_t index = hash_(key) & mask_;
        size_t start = index;

        while (entries_[index].occupied) {
            if (equal_(entries_[index].key, key)) {
                entries_[index].occupied = false;
                size_--;

                size_t next = (index + 1) & mask_;
                while (entries_[next].occupied) {
                    size_t ideal = hash_(entries_[next].key) & mask_;
                    if ((next > index && (ideal <= index || ideal > next)) ||
                        (next < index && (ideal <= index && ideal > next))) {
                        entries_[index] = entries_[next];
                        entries_[next].occupied = false;
                        index = next;
                    }
                    next = (next + 1) & mask_;
                }
                return true;
            }
            index = (index + 1) & mask_;
            if (index == start) break;
        }
        return false;
    }

    // Прямой доступ по слоту — O(1), используется в sort_by для indexed пути
    Entry*       entry_at(size_t slot)       { return &entries_[slot]; }
    const Entry* entry_at(size_t slot) const { return &entries_[slot]; }

    // Возвращает слот для ключа (или SIZE_MAX если не найден)
    size_t find_slot(K key) const {
        if (capacity_ == 0) return SIZE_MAX;
        size_t index = hash_(key) & mask_;
        size_t start = index;
        while (entries_[index].occupied) {
            if (equal_(entries_[index].key, key)) return index;
            index = (index + 1) & mask_;
            if (index == start) break;
        }
        return SIZE_MAX;
    }

    void reserve(size_t n) {
        size_t needed = (size_t)(n / GROW_THRESHOLD) + 1;
        if (needed > capacity_) resize(needed);
    }

    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }

    void clear() {
        for (size_t i = 0; i < capacity_; i++) {
            entries_[i].occupied = false;
        }
        size_ = 0;
    }

    Entry* begin() { return entries_; }
    Entry* end() { return entries_ + capacity_; }
    const Entry* begin() const { return entries_; }
    const Entry* end() const { return entries_ + capacity_; }

    // Итератор только по занятым слотам — пропускает пустые
    template<typename E>
    struct occupied_iterator_t {
        E* cur; E* end_;
        void skip() { while (cur != end_ && !cur->occupied) ++cur; }
        occupied_iterator_t(E* c, E* e) : cur(c), end_(e) { skip(); }
        E& operator*()  { return *cur; }
        E* operator->() { return  cur; }
        occupied_iterator_t& operator++() { ++cur; skip(); return *this; }
        bool operator!=(const occupied_iterator_t& o) const { return cur != o.cur; }
    };

    template<typename E>
    struct occupied_range_t {
        E* b; E* e;
        occupied_iterator_t<E> begin() const { return {b, e}; }
        occupied_iterator_t<E> end()   const { return {e, e}; }
    };

    occupied_range_t<Entry>       occupied()       { return {entries_, entries_ + capacity_}; }
    occupied_range_t<const Entry> occupied() const { return {entries_, entries_ + capacity_}; }

private:
    Entry* entries_;
    size_t capacity_;
    size_t mask_;
    size_t size_;

    std::hash<K> hash_;      // вместо PyObjectHash
    std::equal_to<K> equal_;  // вместо PyObjectEqual

    static constexpr double GROW_THRESHOLD = 0.70;

    void resize(size_t new_capacity) {
        size_t pow2 = 1;
        while (pow2 < new_capacity) pow2 <<= 1;

        Entry* new_entries = new Entry[pow2]();

        if (entries_) {
            for (size_t i = 0; i < capacity_; i++) {
                if (entries_[i].occupied) {
                    size_t index = hash_(entries_[i].key) & (pow2 - 1);
                    while (new_entries[index].occupied) {
                        index = (index + 1) & (pow2 - 1);
                    }
                    new_entries[index] = entries_[i];
                }
            }
            delete[] entries_;
        }

        entries_ = new_entries;
        capacity_ = pow2;
        mask_ = pow2 - 1;
    }
};

#endif