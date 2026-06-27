//
// Created by grey on 15.05.2026.
//

#ifndef MOD_DICT_XXHASH64_H
#define MOD_DICT_XXHASH64_H

#include <cstdint>
#include <cstring>

class XXHash64 {
public:
    XXHash64(uint64_t seed = 0) : seed_(seed) { reset(); }

    void reset() {
        state_[0] = seed_ + PRIME64_1 + PRIME64_2;
        state_[1] = seed_ + PRIME64_2;
        state_[2] = seed_;
        state_[3] = seed_ - PRIME64_1;
        total_len_ = 0;
        buffer_len_ = 0;
    }

    void update(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total_len_ += len;

        if (buffer_len_ + len < 32) {
            memcpy(buffer_ + buffer_len_, p, len);
            buffer_len_ += len;
            return;
        }

        if (buffer_len_ > 0) {
            size_t fill = 32 - buffer_len_;
            memcpy(buffer_ + buffer_len_, p, fill);
            process_block(buffer_);
            p += fill;
            len -= fill;
            buffer_len_ = 0;
        }

        while (len >= 32) {
            process_block(p);
            p += 32;
            len -= 32;
        }

        if (len > 0) {
            memcpy(buffer_, p, len);
            buffer_len_ = len;
        }
    }

    uint64_t digest() const {
        uint64_t h;

        if (total_len_ >= 32) {
            h = rotl64(state_[0], 1) + rotl64(state_[1], 7) +
                rotl64(state_[2], 12) + rotl64(state_[3], 18);
            h = merge_round(h, state_[0]);
            h = merge_round(h, state_[1]);
            h = merge_round(h, state_[2]);
            h = merge_round(h, state_[3]);
        } else {
            h = seed_ + PRIME64_5;
        }

        h += total_len_;

        const uint8_t* p = buffer_;
        size_t len = buffer_len_;
        while (len >= 8) {
            h ^= rotl64(read_u64(p) * PRIME64_2, 31) * PRIME64_1;
            h = rotl64(h, 27) * PRIME64_1 + PRIME64_4;
            p += 8;
            len -= 8;
        }
        if (len >= 4) {
            h ^= read_u32(p) * PRIME64_1;
            h = rotl64(h, 23) * PRIME64_2 + PRIME64_3;
            p += 4;
            len -= 4;
        }
        while (len > 0) {
            h ^= (*p) * PRIME64_5;
            h = rotl64(h, 11) * PRIME64_1;
            p++;
            len--;
        }

        h ^= h >> 33;
        h *= PRIME64_2;
        h ^= h >> 29;
        h *= PRIME64_3;
        h ^= h >> 32;
        return h;
    }

private:
    static constexpr uint64_t PRIME64_1 = 0x9E3779B185EBCA87ULL;
    static constexpr uint64_t PRIME64_2 = 0xC2B2AE3D27D4EB4FULL;
    static constexpr uint64_t PRIME64_3 = 0x165667B19E3779F9ULL;
    static constexpr uint64_t PRIME64_4 = 0x85EBCA77C2B2AE63ULL;
    static constexpr uint64_t PRIME64_5 = 0x27D4EB2F165667C5ULL;

    uint64_t seed_;
    uint64_t state_[4];
    uint64_t total_len_;
    uint8_t buffer_[32];
    size_t buffer_len_;

    static uint64_t rotl64(uint64_t x, int r) {
        return (x << r) | (x >> (64 - r));
    }

    static uint64_t read_u64(const uint8_t* p) {
        uint64_t v = 0;
        memcpy(&v, p, 8);
        return v;
    }

    static uint32_t read_u32(const uint8_t* p) {
        uint32_t v = 0;
        memcpy(&v, p, 4);
        return v;
    }

    void process_block(const uint8_t* block) {
        uint64_t k1 = read_u64(block)      * PRIME64_2;
        uint64_t k2 = read_u64(block + 8)  * PRIME64_2;
        uint64_t k3 = read_u64(block + 16) * PRIME64_2;
        uint64_t k4 = read_u64(block + 24) * PRIME64_2;

        state_[0] = rotl64(state_[0] + k1 * PRIME64_1, 31) * PRIME64_1;
        state_[1] = rotl64(state_[1] + k2 * PRIME64_1, 31) * PRIME64_1;
        state_[2] = rotl64(state_[2] + k3 * PRIME64_1, 31) * PRIME64_1;
        state_[3] = rotl64(state_[3] + k4 * PRIME64_1, 31) * PRIME64_1;
    }

    uint64_t merge_round(uint64_t h, uint64_t v) const {
        return rotl64(h ^ (v * PRIME64_1), 27) * PRIME64_1 + PRIME64_4;
    }
};

#endif
