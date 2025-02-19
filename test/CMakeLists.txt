set(EXECUTABLES when_all when_any)

foreach(exe IN LISTS EXECUTABLES)
  add_executable(${exe} ${exe}.cpp)
  target_link_libraries(${exe} gtest)
  gtest_discover_tests(${exe})
endforeach()
