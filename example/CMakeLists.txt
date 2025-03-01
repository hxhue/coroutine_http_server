set(EXECUTABLES server_epoll_coro server_blocking)

foreach(exe IN LISTS EXECUTABLES)
  add_executable(${exe} ${exe}.cpp)
  target_link_libraries(${exe} PRIVATE coro)
  target_compile_options(${exe} PRIVATE -O2 -g)
endforeach()
