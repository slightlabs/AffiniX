cmake_minimum_required(VERSION 3.24)
project(AffiniX VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options (DESIGN.md §23)
# ---------------------------------------------------------------------------
option(AFX_BUILD_TESTS       "Build unit/integration tests"   ON)
option(AFX_BUILD_BENCH       "Build benchmarks"               OFF)
option(AFX_BUILD_EXAMPLES    "Build examples"                 ON)
option(AFX_WITH_URING        "Build the io_uring backend"     OFF)
option(AFX_WITH_TIMESTAMPING "SO_TIMESTAMPING support"        OFF)
option(AFX_FLIGHT_RECORDER   "Always-on per-EM flight recorder" ON)
option(AFX_DEBUG_CHAOS       "Randomise unspecified order in debug builds" OFF)
option(AFX_THREAD_SAFETY_ANALYSIS "Clang -Wthread-safety affinity checks" OFF)
set(AFX_SANITIZE "" CACHE STRING "Sanitizer: asan|ubsan|tsan")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# afx compiled library (non-template parts) + afx::afx alias
# ---------------------------------------------------------------------------
add_library(afx STATIC
  src/sys/error.cpp
  src/sys/clock.cpp
  src/sys/topology.cpp
  src/sys/affinity.cpp
  src/backend/epoll.cpp
  src/backend/sim.cpp
  src/net/socket.cpp
  src/net/sock_addr.cpp
  src/runtime.cpp
)
add_library(afx::afx ALIAS afx)

target_compile_features(afx PUBLIC cxx_std_20)
target_include_directories(afx PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_options(afx PUBLIC -Wall -Wextra -Wpedantic)
target_link_libraries(afx PUBLIC pthread)

target_compile_definitions(afx PUBLIC
  $<$<BOOL:${AFX_FLIGHT_RECORDER}>:AFX_FLIGHT_RECORDER=1>
  $<$<BOOL:${AFX_DEBUG_CHAOS}>:AFX_DEBUG_CHAOS=1>
  $<$<BOOL:${AFX_WITH_TIMESTAMPING}>:AFX_WITH_TIMESTAMPING=1>
  $<$<CONFIG:Debug>:AFX_DEBUG=1>)

if(AFX_THREAD_SAFETY_ANALYSIS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(afx PUBLIC -Wthread-safety)
  target_compile_definitions(afx PUBLIC AFX_THREAD_SAFETY_ANALYSIS=1)
endif()

if(AFX_SANITIZE STREQUAL "asan")
  target_compile_options(afx PUBLIC -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(afx PUBLIC -fsanitize=address)
elseif(AFX_SANITIZE STREQUAL "ubsan")
  target_compile_options(afx PUBLIC -fsanitize=undefined -fno-omit-frame-pointer)
  target_link_options(afx PUBLIC -fsanitize=undefined)
elseif(AFX_SANITIZE STREQUAL "tsan")
  target_compile_options(afx PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(afx PUBLIC -fsanitize=thread)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(AFX_BUILD_TESTS)
  # doctest 2.4.x predates cmake_minimum_required 3.5 deprecation in CMake 4.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  include(FetchContent)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(doctest)
  list(APPEND CMAKE_MODULE_PATH ${doctest_SOURCE_DIR}/scripts/cmake)

  enable_testing()
  add_subdirectory(test)
endif()

if(AFX_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(AFX_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
