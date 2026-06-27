#include "elastic_pool.h"
#include <Python.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>

ElasticPool::ElasticPool(size_t chunk_size)
    : chunk_size_(std::max(chunk_size, size_t(4096)))
{
    add_chunk(chunk_size_);
}

ElasticPool::~ElasticPool() {
    for (auto& c : chunks_) std::free(c.data);
}

ElasticPool::ElasticPool(ElasticPool&& other) noexcept
    : chunks_(std::move(other.chunks_))
    , chunk_size_(other.chunk_size_)
    , used_(other.used_)
    , peak_(other.peak_)
{
    other.used_ = 0;
    other.peak_ = 0;
}

ElasticPool& ElasticPool::operator=(ElasticPool&& other) noexcept {
    if (this != &other) {
        for (auto& c : chunks_) std::free(c.data);
        chunks_     = std::move(other.chunks_);
        chunk_size_ = other.chunk_size_;
        used_       = other.used_;
        peak_       = other.peak_;
        other.used_ = 0;
        other.peak_ = 0;
    }
    return *this;
}

void* ElasticPool::allocate(size_t size) {
    if (size == 0) return nullptr;

    // ищем чанк с достаточным местом (сначала последний — горячий путь)
    if (!chunks_.empty()) {
        Chunk& c = chunks_.back();
        if (c.used + size <= c.cap) {
            void* ptr = c.data + c.used;
            c.used += size;
            used_  += size;
            if (used_ > peak_) peak_ = used_;
            return ptr;
        }
    }

    // нужен новый чанк
    size_t prev_count = chunks_.size();
    add_chunk(std::max(size, chunk_size_));
    if (chunks_.size() == prev_count) return nullptr;  // OOM: PyErr уже выставлен
    Chunk& c = chunks_.back();
    void* ptr = c.data + c.used;
    c.used += size;
    used_  += size;
    if (used_ > peak_) peak_ = used_;
    return ptr;
}

void ElasticPool::deallocate(void* /*ptr*/) {
    // no-op: bump allocator не освобождает по одному
}

void ElasticPool::shrink_if_idle() {
    // no-op
}

size_t ElasticPool::capacity() const {
    size_t total = 0;
    for (auto& c : chunks_) total += c.cap;
    return total;
}

void ElasticPool::trim() {
    // удаляем пустые чанки (кроме первого)
    for (size_t i = 1; i < chunks_.size(); ) {
        if (chunks_[i].used == 0) {
            std::free(chunks_[i].data);
            chunks_.erase(chunks_.begin() + i);
        } else {
            ++i;
        }
    }
}

void ElasticPool::add_chunk(size_t min_size) {
    size_t cap = std::max(min_size, chunk_size_);
    uint8_t* data = static_cast<uint8_t*>(std::malloc(cap));
    if (!data) {
        PyErr_NoMemory();
        return;
    }
    chunks_.push_back({data, cap, 0});
}
