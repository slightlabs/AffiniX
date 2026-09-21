foreach(ex IN ITEMS echo_server timer_zoo itc_pingpong custom_framing
                    session_coro)
  add_executable(${ex} ${ex}.cpp)
  target_link_libraries(${ex} PRIVATE afx::afx)
endforeach()
