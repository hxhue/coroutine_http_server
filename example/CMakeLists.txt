set(EXECUTABLES epoll_multiple_files)

foreach(exe IN LISTS EXECUTABLES)
  add_executable(${exe} ${exe}.cpp)
endforeach()
