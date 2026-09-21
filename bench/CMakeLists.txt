# Benchmarks (IMPLEMENTATION_PLAN.md §6). nanobench is fetched pinned, like
# doctest. Binaries run as ctest entries only in smoke mode
# (AFX_BENCH_SMOKE=1) — real numbers belong on the pinned machine with
# baselines committed under bench/baselines/.

include(FetchContent)
FetchContent_Declare(nanobench
  GIT_REPOSITORY https://github.com/martinus/nanobench.git
  GIT_TAG        v4.3.11
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(nanobench)

# M8-10: head-to-head echo bake-off (epoll vs uring vs a hand-written epoll
# loop for the ADR-0002 tripwire). A networked binary like bench_driver —
# built, never a ctest; results go to bench/baselines/ manually.
add_executable(bench_echo echo.cpp)
target_link_libraries(bench_echo PRIVATE afx::afx)
target_include_directories(bench_echo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bench_echo PRIVATE -Wall -Wextra)

# M9-08: same harness, coroutine session — the ADR-0005 layering-cost
# comparison against bench_echo.
add_executable(bench_echo_coro echo_coro.cpp)
target_link_libraries(bench_echo_coro PRIVATE afx::afx)
target_include_directories(bench_echo_coro PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bench_echo_coro PRIVATE -Wall -Wextra)

# M11-01: UDP batch-receive throughput — loopback flood between two
# UdpSockets; --batch 1 vs 32 shows the recvmmsg drain factor. Networked
# binary like bench_echo: built, never a ctest.
add_executable(bench_udp_batch udp_batch.cpp)
target_link_libraries(bench_udp_batch PRIVATE afx::afx)
target_include_directories(bench_udp_batch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bench_udp_batch PRIVATE -Wall -Wextra)

foreach(bench_name IN ITEMS itc_pingpong channel_throughput timer_ops)
  add_executable(bench_${bench_name} ${bench_name}.cpp)
  target_link_libraries(bench_${bench_name}
      PRIVATE afx::afx nanobench::nanobench)
  target_include_directories(bench_${bench_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(bench_${bench_name} PRIVATE -Wall -Wextra)
endforeach()

# M6-17: minimal load driver (grows into tools/afx-load in M8-09). Not a
# nanobench binary — it is a networked load generator, run against a live
# server, so it is built but never registered as a ctest.
add_executable(bench_driver driver.cpp)
target_link_libraries(bench_driver PRIVATE afx::afx)
target_compile_options(bench_driver PRIVATE -Wall -Wextra)

# Smoke-run each benchmark under ctest so CI catches build/runtime breakage
# without treating runner noise as a regression. RUN_SERIAL: itc_pingpong
# spins on a reply delivered by an EM thread — under `ctest -j` on a loaded
# shared runner that thread can be starved long enough to look like a hang.
if(AFX_BUILD_TESTS)
  foreach(bench_name IN ITEMS itc_pingpong channel_throughput timer_ops)
    add_test(NAME bench_smoke_${bench_name}
             COMMAND bench_${bench_name})
    set_tests_properties(bench_smoke_${bench_name} PROPERTIES
        ENVIRONMENT "AFX_BENCH_SMOKE=1" LABELS "bench" RUN_SERIAL TRUE)
  endforeach()
endif()
