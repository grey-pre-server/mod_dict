//
// Created by grey on 15.05.2026.
//

#ifndef MOD_DICT_DATETIME_CODEC_H
#define MOD_DICT_DATETIME_CODEC_H

#include <pytypedefs.h>
#include "codec_base.h"

template<>
struct DateTimeCodec {
    static constexpr TypeId type_id = TypeId::DATETIME;
    static constexpr const char* name = "datetime";

    // Проверить, является ли PyObject datetime
    static bool can_handle(PyObject* obj) {
        return PyDateTime_Check(obj);
    }

    // Извлечь timestamp из Python datetime
    static int64_t from_pyobject(PyObject* obj) {
        // PyDateTime_GET_EPOCH нет в публичном API. Используем альтернативу:
        // Преобразуем в POSIX timestamp
        struct tm tm = {};
        tm.tm_year = PyDateTime_GET_YEAR(obj) - 1900;
        tm.tm_mon  = PyDateTime_GET_MONTH(obj) - 1;
        tm.tm_mday = PyDateTime_GET_DAY(obj);
        tm.tm_hour = PyDateTime_DATE_GET_HOUR(obj);
        tm.tm_min  = PyDateTime_DATE_GET_MINUTE(obj);
        tm.tm_sec  = PyDateTime_DATE_GET_SECOND(obj);

        time_t ts = mktime(&tm);
        int usec = PyDateTime_DATE_GET_MICROSECOND(obj);

        // Упаковываем: timestamp в секундах + микросекунды
        return (static_cast<int64_t>(ts) * 1000000) + usec;
    }

    // Сериализовать
    static void encode(std::vector<uint8_t>& buf, int64_t val) {
        buf.push_back(to_byte(type_id));
        write_i64(buf, val);  // 8 байт
    }

    // Десериализовать
    static PyObject* decode(const uint8_t*& ptr) {
        int64_t ts_us = read_i64(ptr);
        time_t sec = static_cast<time_t>(ts_us / 1000000);
        int usec = static_cast<int>(ts_us % 1000000);

        struct tm* gmt = gmtime(&sec);
        return PyDateTime_FromDateAndTime(
            gmt->tm_year + 1900, gmt->tm_mon + 1, gmt->tm_mday,
            gmt->tm_hour, gmt->tm_min, gmt->tm_sec, usec);
    }
};

#endif