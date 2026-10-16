cmake_minimum_required(VERSION 3.16)
project(kv_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# ---- 核心库：WAL + KVStore（跳表是 header-only，随 include 进调用方）----
add_library(kvcore src/wal.cpp src/kv_store.cpp)
target_include_directories(kvcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(kvcore PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- 可选 ThreadSanitizer（仅 Clang/GCC；MSVC 不支持 -fsanitize=thread）----
option(KV_ENABLE_TSAN "Build tests with ThreadSanitizer (Clang/GCC only)" OFF)
if(KV_ENABLE_TSAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
  else()
    message(WARNING "KV_ENABLE_TSAN requested but compiler lacks support; ignored")
  endif()
endif()

# ---- 测试（无第三方依赖，手写断言见 tests/test_util.h）----
enable_testing()
foreach(t test_skiplist test_kv test_recovery test_concurrency test_sync_mode test_applied)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE kvcore)
  target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# ---- 可选 bench ----
option(KV_BUILD_BENCH "Build benchmark" OFF)
if(KV_BUILD_BENCH)
  add_executable(kv_bench bench/bench.cpp)
  target_link_libraries(kv_bench PRIVATE kvcore)
  # 缓存失效专项（只依赖 header-only 跳表，link kvcore 仅为继承 include 路径）
  add_executable(kv_bench_cache bench/bench_cache.cpp)
  target_link_libraries(kv_bench_cache PRIVATE kvcore)
endif()