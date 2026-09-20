# Operator tools. afx-flight is a Python script (no build step); afx-load is
# a networked C++ load generator — built, never registered as a ctest.

add_executable(afx-load afx_load.cpp)
target_link_libraries(afx-load PRIVATE afx::afx)
target_compile_options(afx-load PRIVATE -Wall -Wextra)
