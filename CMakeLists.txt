cmake_minimum_required(VERSION 3.19)
project(
        sts2_lightspeed
        VERSION 0.1
        DESCRIPTION "High-performance, RNG-faithful C++ engine for Slay the Spire 2 with MCTS battle search."
        LANGUAGES CXX)

# The STS2 engine lives entirely in src/sts2 (a port lineage of gamerpuppy's
# sts_lightspeed search structure onto an original STS2 rules implementation).
file (GLOB_RECURSE STS2_SOURCES CONFIGURE_DEPENDS "src/sts2/*.cpp")

add_library(sts2_engine STATIC ${STS2_SOURCES})
target_include_directories(sts2_engine PUBLIC include)
target_include_directories(sts2_engine PUBLIC third_party)
target_compile_features(sts2_engine PUBLIC cxx_std_20)

if (MSVC)
    target_compile_options(sts2_engine PRIVATE /W0 /O2)
else()
    target_compile_options(sts2_engine PRIVATE -Wno-shift-count-overflow -O2)
endif()

# Parity harness: replays recorded game positions and reports field-level diffs.
add_executable(parity apps/parity_main.cpp apps/rng_check.cpp)
target_link_libraries(parity PRIVATE sts2_engine)

# Battle-search harness (MCTS on recorded positions).
add_executable(search apps/search_main.cpp)
target_link_libraries(search PRIVATE sts2_engine)
if (MINGW)
    target_link_options(search PRIVATE -static-libgcc -static-libstdc++)
endif()

# Map generation and determinism checks.
add_executable(map apps/map_main.cpp)
target_link_libraries(map PRIVATE sts2_engine)

# Full-run driver: map -> rooms -> battles -> rewards -> act transitions -> bench.
add_executable(run apps/run_main.cpp)
target_link_libraries(run PRIVATE sts2_engine)

# Python bindings (used by python/train_armg.py). Requires pybind11:
#   pip install pybind11 && cmake -B build -DSTS2_BUILD_PYTHON=ON
option(STS2_BUILD_PYTHON "Build the pybind11 module" OFF)
if (STS2_BUILD_PYTHON)
    # The module is a shared object: Linux linkers reject non-PIC archive members.
    set_target_properties(sts2_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
    find_package(Python COMPONENTS Interpreter Development REQUIRED)
    execute_process(
        COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
        OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    list(APPEND CMAKE_PREFIX_PATH "${PYBIND11_CMAKE_DIR}")
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(sts2 MODULE bindings/sts2_module.cpp)
    target_link_libraries(sts2 PRIVATE sts2_engine)
    target_include_directories(sts2 PRIVATE ${Python_INCLUDE_DIRS})
endif()
