cmake_minimum_required(VERSION 3.15)
project(mod_dict)

if(POLICY CMP0094)
    cmake_policy(SET CMP0094 NEW)
endif()

# При сборке через CLion указываем Python явно; scikit-build-core передаёт его сам
if(NOT SKBUILD)
    set(Python3_ROOT_DIR "C:/Users/grey/conda/envs/app_env")
    set(Python3_EXECUTABLE "${Python3_ROOT_DIR}/python.exe")
    set(Python3_FIND_STRATEGY LOCATION)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    src/python_bindings/error_handling.h
    src/core/flat_hash_map.h
    src/core/mod_value.cpp
    src/core/mod_dict.cpp
    src/core/mod_dict.h
    src/core/field_index.cpp
    src/core/field_index.h
    src/python_bindings/mod_dict_type.cpp
    src/python_bindings/module.cpp
    src/python_bindings/converter_registry.cpp
    src/core/elastic_pool.cpp
    src/python_bindings/mod_dict_binding.h
    src/core/codecs/xxhash64.h
    src/core/codecs/datetime_codec.h
    src/core/codecs/codec_base.h
    src/core/codecs/native_codecs.h
    src/core/codecs/serializer.h
    src/core/codecs/serializer.cpp
)

python3_add_library(mod_dict MODULE ${SOURCES})

target_include_directories(mod_dict PRIVATE
    ${Python3_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/python_bindings
)

target_link_libraries(mod_dict PRIVATE ${Python3_LIBRARIES})

if(MSVC)
    target_compile_options(mod_dict PRIVATE "/utf-8")
endif()

# CLion не знает правильный суффикс — задаём вручную; scikit-build-core управляет этим сам
if(NOT SKBUILD)
    set_target_properties(mod_dict PROPERTIES PREFIX "" SUFFIX ".pyd")
endif()

# Копировать .pyi стаб рядом с собранным модулем
configure_file(
    ${CMAKE_SOURCE_DIR}/src/mod_dict.pyi
    ${CMAKE_CURRENT_BINARY_DIR}/mod_dict.pyi
    COPYONLY
)

install(TARGETS mod_dict DESTINATION .)
install(FILES src/mod_dict.pyi DESTINATION .)
