add_executable(afx_tests
    unit/test_main.cpp
    unit/test_result.cpp
    unit/test_inline_fn.cpp
    unit/test_context.cpp
    unit/test_io_buffer.cpp
    unit/test_handle_table.cpp
    unit/test_rings.cpp
    unit/test_mailbox.cpp
    unit/test_timer_wheel.cpp
    unit/test_timer_group_lifecycle.cpp
    unit/test_event_manager.cpp
    unit/test_framing.cpp
    unit/test_channel.cpp
    unit/test_uring_backend.cpp
    model/test_rings_model.cpp
    model/test_timers_model.cpp
    fuzz/test_framer_fuzz.cpp
    integration/test_echo.cpp
    integration/test_client_server.cpp
    integration/test_runtime.cpp
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
