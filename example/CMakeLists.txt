set(EXECUTABLES epoll_multiple_files)

foreach(exe IN LISTS EXECUTABLES)
  add_executable(${exe} ${exe}.cpp)
  target_link_libraries(${exe} PRIVATE coro)
endforeach()
