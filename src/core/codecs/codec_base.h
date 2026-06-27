//
// Created by grey on 15.05.2026.
//
#ifndef MOD_DICT_CODEC_BASE_H
#define MOD_DICT_CODEC_BASE_H

#include <cstdint>
#include <vector>
#include <cstring>

/* ============================================================================
   ID типов в бинарном формате (0-31, 5 бит)
   ============================================================================ */
enum class TypeId : uint8_t {
    NONE      = 0,
    BOOL      = 1,
    INT       = 2,
    FLOAT     = 3,
    STRING    = 4,
    LIST      = 5,
    MODDICT   = 6,
    // Кастомные типы (7-31)
    DATETIME  = 7,
    DATE      = 8,
    TIME      = 9,
    TIMEDELTA = 10,
    DECIMAL   = 11,
    UUID      = 12,
    BYTES     = 13,
    SET       = 14,
    FROZENSET = 15,
    PATH      = 16,
    ENUM      = 17,
    NDARRAY   = 18,
    DATAFRAME = 19,
    GEOMETRY   = 20,  // deprecated — при чтении десериализуется как BYTES
    STRING_REF          = 21,  // индекс в таблице строк (интернирование)
    GEOMETRY_SHAPELY    = 22,  // WKB → shapely.wkb.loads
    GEOMETRY_GEOALCHEMY = 23,  // WKB → geoalchemy2.WKBElement
    PATH_POSIX          = 24,  // UTF-8 → pathlib.PurePosixPath
    PATH_WINDOWS        = 25,  // UTF-8 → pathlib.PureWindowsPath
};

static constexpr uint8_t INTERNED_MAGIC = 0x49;  // 'I'

inline uint8_t to_byte(TypeId id) { return static_cast<uint8_t>(id); }
inline TypeId from_byte(uint8_t b) { return static_cast<TypeId>(b); }

/* ============================================================================
   Хелперы записи/чтения (little-endian)
   ============================================================================ */
inline void write_u32(std::vector<uint8_t>& buf, uint32_t val) {
    uint8_t b[4]; memcpy(b, &val, 4);
    buf.insert(buf.end(), b, b + 4);
}

inline void write_i32(std::vector<uint8_t>& buf, int32_t val) {
    write_u32(buf, static_cast<uint32_t>(val));
}

inline void write_u64(std::vector<uint8_t>& buf, uint64_t val) {
    uint8_t b[8]; memcpy(b, &val, 8);
    buf.insert(buf.end(), b, b + 8);
}

inline void write_i64(std::vector<uint8_t>& buf, int64_t val) {
    write_u64(buf, static_cast<uint64_t>(val));
}

inline void write_bytes(std::vector<uint8_t>& buf, const uint8_t* data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

inline uint32_t read_u32(const uint8_t*& ptr) {
    uint32_t v = ptr[0] | (ptr[1]<<8) | (ptr[2]<<16) | (ptr[3]<<24);
    ptr += 4; return v;
}

inline int32_t read_i32(const uint8_t*& ptr) {
    return static_cast<int32_t>(read_u32(ptr));
}

inline uint64_t read_u64(const uint8_t*& ptr) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)ptr[i] << (i*8);
    ptr += 8; return v;
}

inline int64_t read_i64(const uint8_t*& ptr) {
    return static_cast<int64_t>(read_u64(ptr));
}

/* ============================================================================
   Базовый шаблон кодека (специализируется для каждого типа)
   ============================================================================ */
template<typename T>
struct Codec;

/* ============================================================================
   Ротатор: вызывает Codec<T> по TypeId (compile-time диспетчеризация)
   ============================================================================ */
// template<typename Func>
// void dispatch(TypeId id, Func&& func) {
//     switch (id) {
//         case TypeId::NONE:      func(Codec<void>{}); break;
//         case TypeId::BOOL:      func(Codec<bool>{}); break;
//         case TypeId::INT:       func(Codec<int64_t>{}); break;
//         case TypeId::FLOAT:     func(Codec<double>{}); break;
//         case TypeId::STRING:    func(Codec<std::string>{}); break;
//         case TypeId::LIST:      func(Codec<void*>{}); break;  // placeholder
//         case TypeId::MODDICT:   func(Codec<void*>{}); break;  // placeholder
//         default: break;  // неизвестный тип — пропускаем
//     }
// }

#endif
