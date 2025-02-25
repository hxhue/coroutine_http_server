set(TESTS
  await_task
  http_parse_uri
  http_router
  when_all
  when_any
)

foreach(t IN LISTS TESTS)
  add_executable(${t} ${t}.cpp)
  target_link_libraries(${t} gtest_main)
  gtest_discover_tests(${t})
endforeach()
