set(TESTS await_task http_parse_uri http_route when_all when_any)

foreach(t IN LISTS TESTS)
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} PRIVATE coro gtest_main)
  gtest_discover_tests(${t})
endforeach()

set(MANUAL_TESTS monitor_multiple_files)
foreach(t IN LISTS MANUAL_TESTS)
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} PRIVATE coro)
endforeach()
