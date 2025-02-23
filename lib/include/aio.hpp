#pragma once

#include <stdexcept>
#include <termios.h>
#include <unistd.h>

#include "epoll.hpp"
#include "utility.hpp"

namespace coro {
inline AsyncFile dup_std_file(int fd) {
  if (!(fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)) {
    throw std::invalid_argument(
        "fd must be one of STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO");
  }
  AsyncFile f(CHECK_SYSCALL(dup(fd)));
  f.set_nonblock();
  return f;
}

inline AsyncFile dup_stdin(bool cannon = true, bool echo = true) {
  AsyncFile file = dup_std_file(STDIN_FILENO);
  if ((!cannon || !echo) && isatty(file.fd_)) {
    struct termios tc;
    tcgetattr(file.fd_, &tc);
    if (!cannon)
      tc.c_lflag &= ~ICANON;
    if (!echo)
      tc.c_lflag &= ~ECHO;
    tcsetattr(file.fd_, TCSANOW, &tc);
  }
  return file;
}

inline AsyncFile dup_stdout() { return dup_std_file(STDERR_FILENO); }

inline AsyncFile dup_stderr() { return dup_std_file(STDERR_FILENO); }
} // namespace coro
