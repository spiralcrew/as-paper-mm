cmake_minimum_required(VERSION 3.25)

project(as_paper_mm
    VERSION 0.1.0
    DESCRIPTION "Avellaneda-Stoikov paper market making on live Binance BTCUSDT data"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ASMM_BUILD_TESTS "Build unit tests (GoogleTest)" ON)
option(ASMM_BUILD_BENCHMARKS "Build benchmarks (Google Benchmark)" ON)
option(ASMM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Записанные 90 секунд BTCUSDT с Binance: используются тестами и бенчмарком.
set(ASMM_SAMPLE_DATA "${PROJECT_SOURCE_DIR}/data/sample_btcusdt.jsonl")

# --- Предупреждения компилятора --------------------------------------------
add_library(asmm_warnings INTERFACE)
if(MSVC)
    target_compile_options(asmm_warnings INTERFACE /W4 /permissive-)
    if(ASMM_WARNINGS_AS_ERRORS)
        target_compile_options(asmm_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(asmm_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
    if(ASMM_WARNINGS_AS_ERRORS)
        target_compile_options(asmm_warnings INTERFACE -Werror)
    endif()
endif()

# --- Библиотека ядра --------------------------------------------------------
add_library(asmm_core
    src/avellaneda_stoikov.cpp
    src/volatility.cpp
    src/paper_exchange.cpp
    src/binance_parser.cpp
    src/engine.cpp)
add_library(asmm::core ALIAS asmm_core)
target_include_directories(asmm_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(asmm_core PUBLIC cxx_std_20)
target_link_libraries(asmm_core PRIVATE asmm_warnings)

# --- CLI-приложение ---------------------------------------------------------
add_executable(asmm app/main.cpp)
target_link_libraries(asmm PRIVATE asmm::core asmm_warnings)

# --- Тесты и бенчмарки ------------------------------------------------------
include(FetchContent)

if(ASMM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ASMM_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

# --- Документация (опционально, если установлен Doxygen) --------------------
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_SOURCE_DIR}/build/doxygen
        COMMAND ${DOXYGEN_EXECUTABLE} ${PROJECT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Generating Doxygen HTML into build/doxygen/html"
        VERBATIM)
endif()
