set(TESTS when_all when_any await_task)

foreach(t IN LISTS TESTS)
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} gtest_main)
  gtest_discover_tests(${t})
endforeach()
