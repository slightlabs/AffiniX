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

foreach(bench_name IN ITEMS itc_pingpong channel_throughput timer_ops)
  add_executable(bench_${bench_name} ${bench_name}.cpp)
  target_link_libraries(bench_${bench_name}
      PRIVATE afx::afx nanobench::nanobench)
  target_include_directories(bench_${bench_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(bench_${bench_name} PRIVATE -Wall -Wextra)
endforeach()

# Smoke-run each benchmark under ctest so CI catches build/runtime breakage
# without treating runner noise as a regression.
if(AFX_BUILD_TESTS)
  foreach(bench_name IN ITEMS itc_pingpong channel_throughput timer_ops)
    add_test(NAME bench_smoke_${bench_name}
             COMMAND bench_${bench_name})
    set_tests_properties(bench_smoke_${bench_name} PROPERTIES
        ENVIRONMENT "AFX_BENCH_SMOKE=1" LABELS "bench")
  endforeach()
endif()
