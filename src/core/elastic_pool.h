//
// Created by grey on 12.05.2026.
//

#ifndef MOD_DICT_ELASTIC_POOL_H
#define MOD_DICT_ELASTIC_POOL_H

#include <cstddef>
#include <cstdint>
#include <vector>

// Chunk-based bump allocator: новые чанки добавляются, старые не перемещаются.
// Гарантирует стабильность всех выданных указателей при росте.
class ElasticPool {
public:
    explicit ElasticPool(size_t chunk_size = 65536);
    ~ElasticPool();

    ElasticPool(const ElasticPool&) = delete;
    ElasticPool& operator=(const ElasticPool&) = delete;

    ElasticPool(ElasticPool&& other) noexcept;
    ElasticPool& operator=(ElasticPool&& other) noexcept;

    void* allocate(size_t size);
    void  deallocate(void* ptr);  // no-op (bump allocator)

    size_t capacity() const;
    size_t used()     const { return used_; }
    size_t peak()     const { return peak_; }

    void trim();            // освобождает пустые чанки
    void shrink_if_idle();  // no-op, для совместимости

private:
    struct Chunk {
        uint8_t* data;
        size_t   cap;
        size_t   used;
    };

    std::vector<Chunk> chunks_;
    size_t chunk_size_;
    size_t used_  = 0;
    size_t peak_  = 0;

    void add_chunk(size_t min_size);
};

#endif // MOD_DICT_ELASTIC_POOL_H
