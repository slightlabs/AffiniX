add_executable(afx_tests
    unit/test_main.cpp
    unit/test_result.cpp
    unit/test_inline_fn.cpp
    unit/test_context.cpp
    unit/test_io_buffer.cpp
    unit/test_handle_table.cpp
    unit/test_pool_arena.cpp
    unit/test_rings.cpp
    unit/test_mailbox.cpp
    unit/test_timer_wheel.cpp
    unit/test_timer_group_lifecycle.cpp
    unit/test_event_manager.cpp
    unit/test_framing.cpp
    unit/test_channel.cpp
    unit/test_timestamping.cpp
    unit/test_uring_backend.cpp
    model/test_rings_model.cpp
    model/test_timers_model.cpp
    fuzz/test_framer_fuzz.cpp
    integration/test_echo.cpp
    integration/test_client_server.cpp
    integration/test_runtime.cpp
    integration/test_shutdown_drain.cpp
)
target_link_libraries(afx_tests PRIVATE afx::afx doctest::doctest)
target_compile_options(afx_tests PRIVATE -Wall -Wextra)

include(doctest)
doctest_discover_tests(afx_tests)

# Invariant tests live in their own binary: the no-alloc test replaces the
# global operator new, which would pollute the other suites if colocated.
add_executable(afx_invariant_tests
    invariant/main.cpp
    invariant/no_alloc_steady_state.cpp
    invariant/close_exactly_once.cpp
    invariant/no_blocking_in_em.cpp
)
target_link_libraries(afx_invariant_tests PRIVATE afx::afx doctest::doctest)
target_compile_options(afx_invariant_tests PRIVATE -Wall -Wextra)
doctest_discover_tests(afx_invariant_tests
    PROPERTIES LABELS invariant)

# Concurrency stress tests: bounded for PR CI, scaled up nightly via
# AFX_STRESS_SCALE. Kept in a separate binary so the label can exclude them
# from a quick local run (ctest -L '!stress').
add_executable(afx_stress_tests
    stress/main.cpp
    stress/test_lost_wakeup.cpp
)
target_link_libraries(afx_stress_tests PRIVATE afx::afx doctest::doctest)
target_compile_options(afx_stress_tests PRIVATE -Wall -Wextra)
doctest_discover_tests(afx_stress_tests
    PROPERTIES LABELS stress)

# ---------------------------------------------------------------------------
# Must-not-compile suite (M1-02, ADR-0009). Each affinity_negative/*.cpp case
# listed in AFX_NEGATIVE_CASES must FAIL to compile under Clang with
# -Wthread-safety; the positive control must COMPILE. The cases are ctest
# entries that invoke the compiler directly: a negative case passing (i.e.
# compiling) means an affinity violation slipped through the analysis.
#
# Clang-only by design (DESIGN.md §6.3): GCC has no equivalent analysis and
# the AFX_* macros expand to nothing there, so the cases would "fail" for the
# wrong reason. Skipping loudly is the documented behaviour.
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(AFX_AFFINITY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/affinity_negative)
  set(AFX_TSA_FLAGS -std=c++20 -fsyntax-only -Wthread-safety -Werror
                    -DAFX_THREAD_SAFETY_ANALYSIS=1
                    -I${CMAKE_SOURCE_DIR}/include
                    -I${AFX_AFFINITY_DIR})

  foreach(case IN ITEMS
      call_without_capability
      write_guarded_field
      excludes_while_held)
    add_test(NAME affinity_negative_${case}
             COMMAND ${CMAKE_CXX_COMPILER} ${AFX_TSA_FLAGS}
                     ${AFX_AFFINITY_DIR}/${case}.cpp)
    set_tests_properties(affinity_negative_${case}
        PROPERTIES WILL_FAIL TRUE LABELS affinity_negative)
  endforeach()

  add_test(NAME affinity_positive_call_with_capability
           COMMAND ${CMAKE_CXX_COMPILER} ${AFX_TSA_FLAGS}
                   ${AFX_AFFINITY_DIR}/call_with_capability.cpp)
  set_tests_properties(affinity_positive_call_with_capability
      PROPERTIES LABELS affinity_negative)
else()
  message(WARNING
      "test/affinity_negative is Clang-only (-Wthread-safety): the "
      "must-not-compile suite is INACTIVE under ${CMAKE_CXX_COMPILER_ID}. "
      "Run the clang-tsa preset (or CI job) to exercise it — ADR-0009.")
endif()
