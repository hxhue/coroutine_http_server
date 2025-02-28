#pragma once

#include <fcntl.h>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <termios.h>
#include <unistd.h>
#include <utility>

#include "task.hpp"
#include "utility.hpp"

namespace coro {

struct AsyncFile {
  friend class AsyncFileStream;

  AsyncFile() : fd_(-1) {}

  explicit AsyncFile(int fd, bool nonblock = true, bool borrow = false) noexcept
      : fd_(fd), borrow_(borrow) {
    if (fd != -1 && nonblock) {
      set_nonblock();
    }
  }

  AsyncFile(AsyncFile &&other) noexcept
      : fd_(other.fd_), borrow_(other.borrow_) {
    other.fd_ = -1;
    other.borrow_ = false;
  }

  AsyncFile &operator=(AsyncFile &&other) noexcept {
    if (fd_ != -1) {
      assert(fd_ != other.fd_);
    }
    AsyncFile temp(std::move(other));
    std::swap(temp, *this);
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
        "fd must be one of STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO\n" +
        SOURCE_LOCATION());
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

// AsyncFileStream does not have a "borrow" mode. We may have to duplicate the
// fd sometimes.
// 2025/2/27: I think AsyncFileStream is unreliable because you cannot get the
// direct return value (for error-checking) from syscalls. It may also be slow
// if we wrap fgetc and fputc.
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
    void operator()(FILE *f) const { CHECK_SYSCALL(fclose(f)); }
  };

  AsyncFile af_;
  std::unique_ptr<FILE, FileCloser> handle_;
};

template <typename T> struct IOResult {
  T result{};
  bool hup{}; // This means cannot write/read anymore.
};

struct FileDescriptor {
  friend class FileStream;

  explicit FileDescriptor(int fd) : fd(fd) {}

  ~FileDescriptor() {
    if (fd != -1) {
      CHECK_SYSCALL(close(fd));
      fd = -1;
    }
  }

  FileDescriptor(FileDescriptor const &) = delete;
  FileDescriptor(FileDescriptor &&other) : fd(std::exchange(other.fd, -1)) {}

  [[nodiscard]] int release() { return std::exchange(fd, -1); }

  int fd = -1;
};

struct FileStream {
  FileStream(FileDescriptor fd, const char *mode) {
    stream = fdopen(fd.fd, mode);
    if (!stream) {
      THROW_SYSCALL("fdopen");
    }
    fd.fd = -1;
  }

  ~FileStream() {
    if (stream) {
      if (EOF == fclose(stream)) {
        THROW_SYSCALL("fclose");
      }
      stream = nullptr;
    }
  }

  FileStream(FileStream const &) = delete;
  FileStream(FileStream &&other)
      : stream(std::exchange(other.stream, nullptr)) {}

  FILE *stream = nullptr;
};

// These two concepts require an object to implement asynchronous, syscall-like
// read/write operations, which are inherently best-effort.
//
// You can inherit std::streambuf to customize the behavior of an
// std::ostream (this std::ostream should be constructed with a pointer
// to such a std::streambuf object). That's where this idea of comes
// from. See
// https://hxhue.github.io/posts/programming/cpp/%E8%87%AA%E5%AE%9A%E4%B9%89-ostream/.
//
// Concepts bring headache to CRTP.
template <typename T>
concept AsyncReader = requires(T t, std::span<char> buffer) {
  // { t.read(buffer) } -> std::same_as<Task<std::size_t>>;
  { 0 };
};

template <typename T>
concept AsyncWriter = requires(T t, std::span<char const> buffer) {
  // { t.write(buffer) } -> std::same_as<Task<std::size_t>>;
  { 0 };
};

template <typename T>
concept AsyncReadWriter = (AsyncReader<T> && AsyncWriter<T>);

struct EOFException : std::runtime_error {
  using std::runtime_error::runtime_error;
};

template <AsyncReader Reader> struct AsyncIStreamBase {
  explicit AsyncIStreamBase(std::size_t buffer_size = 8192)
      : buffer_(new char[buffer_size]), capacity_(buffer_size) {}

  Task<char> getchar() {
    if (empty()) [[unlikely]] {
      co_await refill();
    };
    char ch = buffer_[start_++];
    co_return ch;
  }

  Task<std::string> getn(std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n; i++) {
      char ch = co_await getchar();
      s.push_back(ch);
    }
    co_return s;
  }

  Task<std::string> getline(std::string_view eol) {
    std::string s;
    while (true) {
      char ch = co_await getchar();
      if (ch == eol[0]) {
        std::size_t i;
        for (i = 1; i < eol.size(); ++i) {
          char ch = co_await getchar();
          if (ch != eol[i]) {
            break;
          }
        }
        // eol is not included.
        if (i == eol.size()) {
          break;
        }
        // Mismatch: put back delayed characters.
        for (std::size_t j = 0; j < i; ++j) {
          s.push_back(eol[j]);
        }
        continue;
      }
      s.push_back(ch);
    }
    co_return s;
  }

private:
  bool empty() { return start_ == end_; }

  Task<> refill() {
    auto &derived = static_cast<Reader &>(*this);
    auto sz = co_await derived.read(std::span{buffer_.get(), capacity_});
    assert(sz <= capacity_);
    if (sz != 0) {
      start_ = 0;
      end_ = sz;
    } else {
      throw EOFException("Read EOF\n" + SOURCE_LOCATION());
    }
  }

  std::unique_ptr<char[]> buffer_;
  std::size_t capacity_{};
  std::size_t start_{};
  std::size_t end_{};
};

template <AsyncWriter Writer> struct AsyncOStreamBase {
  explicit AsyncOStreamBase(std::size_t buffer_size = 8192)
      : buffer_(new char[buffer_size]), capacity_(buffer_size) {}

  Task<> putchar(char ch) {
    if (full()) {
      co_await flush();
    }
    buffer_[end_++] = ch;
  }

  Task<> flush() {
    if (end_) [[likely]] {
      auto &derived = static_cast<Writer &>(*this);
      auto span_ = std::span{buffer_.get(), end_};
      auto sz = co_await derived.write(span_);
      while (sz > 0 && sz != span_.size()) [[unlikely]] {
        // Write the later half. (But why did the first write failed?)
        span_ = span_.subspan(sz);
        sz = co_await derived.write(span_);
      }
      if (sz == 0) {
        throw EOFException("Write EOF\n" + SOURCE_LOCATION());
      }
      end_ = 0;
    }
  }

  Task<> puts(std::string_view sv) {
    for (char ch : sv) {
      co_await putchar(ch);
    }
  }

private:
  bool full() { return end_ == capacity_; }

  std::unique_ptr<char[]> buffer_;
  std::size_t capacity_{};
  std::size_t end_{};
};

template <AsyncReadWriter ReadWriter>
struct AsyncIOStreamBase : AsyncIStreamBase<ReadWriter>,
                           AsyncOStreamBase<ReadWriter> {
  AsyncIOStreamBase(std::size_t buffer_size = 8192)
      : AsyncIStreamBase<ReadWriter>(buffer_size),
        AsyncOStreamBase<ReadWriter>(buffer_size) {}
};

// template <AsyncReader Reader>
// struct AsyncIStream : AsyncIStreamBase<AsyncIStream<Reader>>, Reader {
//   template <typename... Args>
//   AsyncIStream(Args &&...args)
//       : AsyncIStreamBase<AsyncIStream<Reader>>(),
//         Reader(std::forward<Args>(args)...) {}
// };

// // template <AsyncWriter Writer>
// // struct AsyncOStream : AsyncOStreamBase<Writer>, Writer {
// //   template <typename... Args>
// //   AsyncOStream(Args &&...args)
// //       : AsyncOStreamBase<Writer>(), Writer(std::forward<Args>(args)...) {}
// // };
// template <AsyncWriter Writer>
// struct AsyncOStream : AsyncOStreamBase<AsyncOStream<Writer>>, Writer {
//   template <typename... Args>
//   AsyncOStream(Args &&...args)
//       : AsyncOStreamBase<AsyncOStream<Writer>>(),
//         Writer(std::forward<Args>(args)...) {}
// };

// template <AsyncReadWriter ReadWriter>
// struct AsyncIOStream : AsyncIOStreamBase<AsyncIOStream<ReadWriter>>,
//                        ReadWriter {
//   template <typename... Args>
//   AsyncIOStream(Args &&...args)
//       : AsyncIOStreamBase<AsyncIOStream<ReadWriter>>(),
//         ReadWriter(std::forward<Args>(args)...) {}
// };

} // namespace coro
