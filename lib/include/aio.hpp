#pragma once

#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <termios.h>
#include <unistd.h>
#include <utility>

#include "utility.hpp"

namespace coro {

struct AsyncFile {
  friend class AsyncFileStream;

  AsyncFile() : fd_(-1) {}

  explicit AsyncFile(int fd, bool unblock = true, bool borrow = false) noexcept
      : fd_(fd), borrow_(borrow) {
    if (fd != -1 && unblock) {
      set_nonblock();
    }
  }

  AsyncFile(AsyncFile &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  AsyncFile &operator=(AsyncFile &&other) noexcept {
    std::swap(fd_, other.fd_);
    return *this;
  }

  ~AsyncFile() {
    if (fd_ != -1 && !borrow_) {
      CHECK_SYSCALL(close(fd_));
    }
  }

  int release() noexcept { return std::exchange(fd_, -1); }

  int fd_{-1};
  bool borrow_{};

private:
  void set_nonblock() {
    int read_fd = fd_;
    int flags = CHECK_SYSCALL(fcntl(read_fd, F_GETFL, 0));
    flags = flags | O_NONBLOCK;
    CHECK_SYSCALL(fcntl(read_fd, F_SETFL, flags));
  }
};

inline AsyncFile dup_std_file(int fd) {
  if (!(fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)) {
    throw std::invalid_argument(
        "fd must be one of STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO");
  }
  AsyncFile f(CHECK_SYSCALL(dup(fd)));
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

struct AsyncFileStream {
  AsyncFileStream(AsyncFile f, const char *mode) : af_(std::move(f)) {
    int fd = af_.fd_;
    FILE *fp = fdopen(fd, mode);
    if (!fp) {
      THROW_SYSCALL("fdopen");
    }
    handle_.reset(fp);
    af_.borrow_ = true;
  }

  operator FILE *() const { return handle_.get(); }

  operator AsyncFile &() { return af_; }

  struct FileCloser {
    void operator()(FILE *f) const {
      if (f) {
        CHECK_SYSCALL(fclose(f));
      }
    }
  };

  AsyncFile af_;
  std::unique_ptr<FILE, FileCloser> handle_;
};

template <typename T> struct IOResult {
  T result{};
  bool hup{}; // This means cannot write/read anymore.
              // Operations on both AsyncFile and AsyncFileStream can have this
              // flag.
  bool partial{}; // A result can be not partial even when there's a hup flag.
                  // Operations on AsyncFileStream can have this flag.
};
} // namespace coro
