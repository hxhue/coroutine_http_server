#pragma once

#include <sys/epoll.h>
#include <unistd.h>

#include "task.hpp"
#include "utility.hpp"

struct EpollFilePromise : Promise<void> {
  auto get_return_object() {
    return std::coroutine_handle<EpollFilePromise>::from_promise(*this);
  }

  EpollFilePromise &operator=(EpollFilePromise &&) = delete;

  inline ~EpollFilePromise();

  int fd_;
  uint32_t events_;
};

struct EpollScheduler {
  void add_listener(EpollFilePromise &promise) {
    struct epoll_event event;
    event.events = promise.events_;
    event.data.ptr = &promise;
    check_syscall(epoll_ctl(epoll_, EPOLL_CTL_ADD, promise.fd_, &event));
  }

  void remove_listener(EpollFilePromise &promise) {
    check_syscall(epoll_ctl(epoll_, EPOLL_CTL_DEL, promise.fd_, NULL));
  }

  // Call epoll_wait once and run callbacks(coroutines).
  void try_run(int timeout = -1) {
    struct epoll_event ebuf[10];
    int res = check_syscall(epoll_wait(epoll_, ebuf, 10, timeout));
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
      std::coroutine_handle<EpollFilePromise>::from_promise(promise).resume();
    }
  }

  template <typename... Ts> void run(Task<Ts...> &task) {
    task.coro_.resume();
    while (!task.coro_.done()) {
      try_run();
    }
  }

  EpollScheduler &operator=(EpollScheduler &&) = delete;

  ~EpollScheduler() { check_syscall(close(epoll_)); }

  static EpollScheduler &get() {
    static thread_local EpollScheduler instance;
    return instance;
  }

  int epoll_ = check_syscall(epoll_create1(STDIN_FILENO));
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

inline Task<void, EpollFilePromise> wait_file(EpollScheduler &sched, int fd,
                                              uint32_t events) {
  co_await EpollFileAwaiter(sched, fd, events);
}
