#pragma once

#include <cerrno>
#include <chrono>
#include <coroutine>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <utility>

#include "task.hpp"
#include "utility.hpp"

struct AsyncFile {
  AsyncFile() : fd_(-1) {}

  explicit AsyncFile(int fd) noexcept : fd_(fd) {}

  AsyncFile(AsyncFile &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  AsyncFile &operator=(AsyncFile &&other) noexcept {
    std::swap(fd_, other.fd_);
    return *this;
  }

  ~AsyncFile() {
    if (fd_ != -1) {
      close(fd_);
    }
  }

  int release() noexcept { return std::exchange(fd_, -1); }

  void set_nonblock() {
    int read_fd = fd_;
    int flags = CHECK_SYSCALL(fcntl(read_fd, F_GETFL, 0));
    flags = flags | O_NONBLOCK;
    CHECK_SYSCALL(fcntl(read_fd, F_SETFL, flags));
  }

  int fd_;
};

using EpollEventMask = std::uint32_t;

struct EpollScheduler;

struct EpollFileAwaiter;

struct EpollFilePromise : Promise<EpollEventMask> {
  auto get_return_object() {
    return std::coroutine_handle<EpollFilePromise>::from_promise(*this);
  }

  EpollFilePromise &operator=(EpollFilePromise &&) = delete;

  inline ~EpollFilePromise();

  EpollFileAwaiter *awaiter_;
};

// If the fds point to the same file, there will be an exception.
// Don't add the same file multiple times!
struct EpollScheduler {
  inline bool add_listener(EpollFilePromise &promise, int epoll_op);

  inline void remove_listener(EpollFilePromise &promise);

  // Call epoll_wait once and run callbacks(coroutines).
  void run(std::optional<Clock::duration> timeout_opt = std::nullopt) {
    int timeout = -1;
    if (timeout_opt) {
      using namespace std::chrono;
      timeout = duration_cast<milliseconds>(*timeout_opt).count();
    }
    struct epoll_event ebuf[10];
    int res = CHECK_SYSCALL(epoll_wait(epoll_, ebuf, 10, timeout));
    for (int i = 0; i < res; i++) {
      auto &event = ebuf[i];

      // The pointer comes from add_listener.
      auto &promise = *(EpollFilePromise *)event.data.ptr;

      // When epoll gives us an event, we get a coroutine handle from data.ptr
      // and resume it.
      //
      // What's the difference between it and a normal function?
      // The coroutine can return early without finishing, the semantic is being
      // launched instead of having finished.
      auto h = std::coroutine_handle<EpollFilePromise>::from_promise(promise);
      h.resume();
    }
  }

  template <typename... Ts> void run(Task<Ts...> &task) {
    task.coro_.resume();
    while (!task.coro_.done()) {
      run();
    }
  }

  bool have_registered_events() {
    assert(registered_cnt_ >= 0);
    return registered_cnt_ != 0;
  }

  EpollScheduler &operator=(EpollScheduler &&) = delete;

  ~EpollScheduler() { CHECK_SYSCALL(close(epoll_)); }

  int epoll_{CHECK_SYSCALL2(epoll_create1(STDIN_FILENO))};
  int registered_cnt_{};
};

struct EpollFileAwaiter {
  bool await_ready() const noexcept { return false; }

  bool await_suspend(std::coroutine_handle<EpollFilePromise> coroutine) {
    auto &promise = coroutine.promise();
    bool suspend = true;
    promise.awaiter_ = this;
    if (!sched_.add_listener(promise, epoll_op_)) {
      suspend = false;
      promise.awaiter_ = nullptr;
      // DEBUG() << "add_listener failed\n";
    }
    return suspend;
  }

  auto await_resume() const noexcept { return resume_events_; }

  EpollScheduler &sched_;
  int fd_{-1};
  EpollEventMask events_{0};
  EpollEventMask resume_events_{0};
  int epoll_op_{EPOLL_CTL_ADD};
};

EpollFilePromise::~EpollFilePromise() {
  if (awaiter_) {
    awaiter_->sched_.remove_listener(*this);
  }
}

bool EpollScheduler::add_listener(EpollFilePromise &promise, int epoll_op) {
  struct epoll_event event;
  event.events = promise.awaiter_->events_;
  event.data.ptr = &promise;
  auto ret = epoll_ctl(epoll_, epoll_op, promise.awaiter_->fd_, &event);
  if (ret == -1) {
    return false;
  }
  if (epoll_op == EPOLL_CTL_ADD) {
    registered_cnt_++;
  }
  return true;
}

void EpollScheduler::remove_listener(EpollFilePromise &promise) {
  CHECK_SYSCALL(epoll_ctl(epoll_, EPOLL_CTL_DEL, promise.awaiter_->fd_, NULL));
  --registered_cnt_;
}

inline Task<EpollEventMask, EpollFilePromise>
wait_file_event(EpollScheduler &sched, AsyncFile &file, EpollEventMask events) {
  co_return co_await EpollFileAwaiter(sched, file.fd_, events);
}

inline std::size_t read_file_sync(AsyncFile &file, std::span<char> buffer) {
  auto ret = read(file.fd_, buffer.data(), buffer.size());
  if (ret == -1 && errno == EAGAIN) {
    ret = 0;
  }
  if (ret == -1) {
    THROW_SYSCALL("read");
  }
  assert(ret >= 0);
  return static_cast<std::size_t>(ret);
}

inline std::size_t write_file_sync(AsyncFile &file,
                                   std::span<char const> buffer) {
  auto ret = write(file.fd_, buffer.data(), buffer.size());
  if (ret == -1 && errno == EAGAIN) {
    ret = 0;
  }
  if (ret == -1) {
    THROW_SYSCALL("write");
  }
  return ret;
}

inline Task<std::size_t> read_file(EpollScheduler &sched, AsyncFile &file,
                                   std::span<char> buffer) {
  co_await wait_file_event(sched, file, EPOLLIN | EPOLLRDHUP);
  auto len = read_file_sync(file, buffer);
  co_return len;
}

inline Task<std::size_t> write_file(EpollScheduler &sched, AsyncFile &file,
                                    std::span<char const> buffer) {
  // DEBUG() << "waiting for file to be ready\n";
  co_await wait_file_event(sched, file, EPOLLOUT | EPOLLHUP);
  // DEBUG() << "file is ready\n";
  auto len = write_file_sync(file, buffer);
  co_return len;
}
inline Task<std::string> read_string(EpollScheduler &sched, AsyncFile &file) {
  co_await wait_file_event(sched, file, EPOLLIN | EPOLLET);
  std::string s;
  size_t chunk = 64;
  while (true) {
    char c;
    std::size_t exist = s.size();
    s.resize(exist + chunk);
    std::span<char> buffer(s.data() + exist, chunk);
    auto len = co_await read_file(sched, file, buffer);
    if (len != chunk) {
      s.resize(exist + len);
      break;
    }
    if (chunk < 65536)
      chunk *= 4;
  }
  co_return s;
}