set(EXECUTABLES server_epoll_coro server_epoll)

foreach(exe IN LISTS EXECUTABLES)
  add_executable(${exe} ${exe}.cpp)
  target_link_libraries(${exe} PRIVATE coro)
endforeach()
