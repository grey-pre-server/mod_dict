//
// Created by grey on 15.05.2026.
//

#ifndef MOD_DICT_NATIVE_CODECS_H
#define MOD_DICT_NATIVE_CODECS_H

#include "codec_base.h"
#include <string>

/* ============================================================================
   Встроенные типы
   ============================================================================ */

// None
template<> struct Codec<void> {
    static constexpr TypeId id = TypeId::NONE;
    static void encode(std::vector<uint8_t>& buf) {
        buf.push_back(to_byte(id));
        write_u32(buf, 0);
    }
    static void decode(const uint8_t*& ptr) {
        read_u32(ptr); // длина = 0
    }
};

// Bool
template<> struct Codec<bool> {
    static constexpr TypeId id = TypeId::BOOL;
    static void encode(std::vector<uint8_t>& buf, bool v) {
        buf.push_back(to_byte(id));
        write_u32(buf, 1);
        buf.push_back(v ? 1 : 0);
    }
    static bool decode(const uint8_t*& ptr) {
        read_u32(ptr); // длина = 1
        return *ptr++ != 0;
    }
};

// Int
template<> struct Codec<int64_t> {
    static constexpr TypeId id = TypeId::INT;
    static void encode(std::vector<uint8_t>& buf, int64_t v) {
        buf.push_back(to_byte(id));
        write_u32(buf, 8);
        write_u64(buf, static_cast<uint64_t>(v));
    }
    static int64_t decode(const uint8_t*& ptr) {
        read_u32(ptr); // длина = 8
        return static_cast<int64_t>(read_u64(ptr));
    }
};

// Float
template<> struct Codec<double> {
    static constexpr TypeId id = TypeId::FLOAT;
    static void encode(std::vector<uint8_t>& buf, double v) {
        buf.push_back(to_byte(id));
        write_u32(buf, 8);
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        write_u64(buf, bits);
    }
    static double decode(const uint8_t*& ptr) {
        read_u32(ptr); // длина = 8
        uint64_t bits = read_u64(ptr);
        double v;
        std::memcpy(&v, &bits, 8);
        return v;
    }
};

// String
template<> struct Codec<std::string> {
    static constexpr TypeId id = TypeId::STRING;
    static void encode(std::vector<uint8_t>& buf, const std::string& v) {
        buf.push_back(to_byte(id));
        write_u32(buf, static_cast<uint32_t>(v.size()));
        write_bytes(buf, reinterpret_cast<const uint8_t*>(v.data()), v.size());
    }
    static std::string decode(const uint8_t*& ptr) {
        uint32_t len = read_u32(ptr);
        std::string s(reinterpret_cast<const char*>(ptr), len);
        ptr += len;
        return s;
    }
};

#endif
