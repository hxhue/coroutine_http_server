#pragma once

#include <cerrno>
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

struct EpollFilePromise : Promise<void> {
  auto get_return_object() {
    return std::coroutine_handle<EpollFilePromise>::from_promise(*this);
  }

  EpollFilePromise &operator=(EpollFilePromise &&) = delete;

  inline ~EpollFilePromise();

  int fd_;
  uint32_t events_;
};

// If the fds point to the same file, there will be an exception.
// Don't add the same file multiple times!
struct EpollScheduler {
  void add_listener(EpollFilePromise &promise) {
    struct epoll_event event;
    event.events = promise.events_;
    event.data.ptr = &promise;

    // auto h = std::coroutine_handle<EpollFilePromise>::from_promise(promise);
    // DEBUG() << "add fd " << promise.fd_ << " " << h.address() << std::endl;

    CHECK_SYSCALL(epoll_ctl(epoll_, EPOLL_CTL_ADD, promise.fd_, &event));
  }

  void remove_listener(EpollFilePromise &promise) {
    // auto h = std::coroutine_handle<EpollFilePromise>::from_promise(promise);
    // DEBUG() << "rm fd " << promise.fd_ << " " << h.address() << std::endl;

    CHECK_SYSCALL(epoll_ctl(epoll_, EPOLL_CTL_DEL, promise.fd_, NULL));
  }

  // Call epoll_wait once and run callbacks(coroutines).
  void try_run(int timeout = -1) {
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
      try_run();
    }
  }

  EpollScheduler &operator=(EpollScheduler &&) = delete;

  ~EpollScheduler() { CHECK_SYSCALL(close(epoll_)); }

  static EpollScheduler &get() {
    static thread_local EpollScheduler instance;
    return instance;
  }

  int epoll_ = CHECK_SYSCALL(epoll_create1(STDIN_FILENO));
};

EpollFilePromise::~EpollFilePromise() {
  EpollScheduler::get().remove_listener(*this);
}

struct EpollFileAwaiter {
  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<EpollFilePromise> coroutine) const {
    auto &promise = coroutine.promise();
    promise.fd_ = fd_;
    promise.events_ = events_;
    sched.add_listener(promise);
  }

  void await_resume() const noexcept {}

  EpollScheduler &sched;
  int fd_;
  uint32_t events_;
};

inline Task<void, EpollFilePromise>
wait_file(EpollScheduler &sched, AsyncFile &file, uint32_t events) {
  // DEBUG() << "before co_await EpollFileAwaiter\n";
  int fd = file.fd_;
  auto awaiter = EpollFileAwaiter(sched, fd, events);
  co_await awaiter;
  // DEBUG() << "after  co_await EpollFileAwaiter\n";
}
