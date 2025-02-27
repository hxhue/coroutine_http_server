#pragma once

#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/epoll.h>
#include <unistd.h>
#include <utility>

#include "aio.hpp"
#include "task.hpp"
#include "utility.hpp"

namespace coro {

using EpollEventMask = std::uint32_t;

struct EpollScheduler;

struct EpollFileAwaiter;

struct EpollFilePromise : Promise<EpollEventMask> {
  auto get_return_object() {
    return std::coroutine_handle<EpollFilePromise>::from_promise(*this);
  }

  EpollFilePromise &operator=(EpollFilePromise &&) = delete;

  inline ~EpollFilePromise();

  inline void set_resume_events(EpollEventMask ev);

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
    struct epoll_event ebuf[1024];
    int res = epoll_wait(epoll_, ebuf, std::size(ebuf), timeout);
    if (res == -1 && errno == EINTR) {
      res = 0;
    }
    if (res == -1) {
      THROW_SYSCALL("epoll_wait");
    }
    for (int i = 0; i < res; i++) {
      auto &event = ebuf[i];

      // The pointer comes from add_listener.
      auto &promise = *(EpollFilePromise *)event.data.ptr;

      // Let the promise know which events occur.
      promise.set_resume_events(event.events);

      // DEBUG() << std::format("epoll loop resumes: promise: {:p} ev: 0x{:x}",
      //                        (void *)(event.data.ptr),
      //                        (unsigned)event.events)
      //         << std::endl;

      // When epoll gives us an event, we get a coroutine handle from data.ptr
      // and resume it.
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
      THROW_SYSCALL("epoll_ctl");
    }
    return suspend;
  }

  auto await_resume() noexcept {
    return std::exchange<EpollEventMask>(resume_events_, 0);
  }

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

void EpollFilePromise::set_resume_events(EpollEventMask mask) {
  awaiter_->resume_events_ = mask;
}

bool EpollScheduler::add_listener(EpollFilePromise &promise, int epoll_op) {
  struct epoll_event event;
  event.events = promise.awaiter_->events_;
  event.data.ptr = &promise;

  // DEBUG() << std::format("add_lister: epoll_ctl: fd={} ev=0x{:x}\n",
  //                        promise.awaiter_->fd_, promise.awaiter_->events_);

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
  // DEBUG() << std::format("remove_lister: epoll_ctl: fd={}\n",
  //                        promise.awaiter_->fd_);
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

inline Task<IOResult<std::size_t>>
read_file_best_effort(EpollScheduler &sched, AsyncFile &file,
                      std::span<char> buffer) {
  auto ev =
      co_await wait_file_event(sched, file, EPOLLIN | EPOLLRDHUP | EPOLLHUP);
  bool hup = ev & (EPOLLRDHUP | EPOLLHUP);
  if (hup) {
    co_return {0, hup};
  }
  auto len = read_file_sync(file, buffer);
  co_return {len, hup};
}

inline Task<IOResult<std::size_t>>
write_file_best_effort(EpollScheduler &sched, AsyncFile &file,
                       std::span<char const> buffer) {
  auto ev = co_await wait_file_event(sched, file, EPOLLOUT | EPOLLHUP);
  bool hup = ev & EPOLLHUP;
  if (hup) {
    co_return {0, hup};
  }
  auto len = write_file_sync(file, buffer);
  co_return {len, hup};
}

inline Task<IOResult<std::string>>
read_string_best_effort(EpollScheduler &sched, AsyncFile &file) {
  co_await wait_file_event(sched, file, EPOLLIN);
  std::string s;
  size_t chunk = 64;
  bool hup = false;
  while (true) {
    char c;
    std::size_t exist = s.size();
    s.resize(exist + chunk);
    std::span<char> buffer(s.data() + exist, chunk);
    auto res = co_await read_file_best_effort(sched, file, buffer);
    auto len = res.result;
    auto rdhup = res.hup;
    if (len != chunk || rdhup) {
      s.resize(exist + len);
      hup = rdhup;
      break;
    }
    if (chunk < 65536)
      chunk *= 4;
  }
  co_return {std::move(s), hup};
}

// May return a partially read string if the a hup event is received.
inline Task<IOResult<std::string>> getline(EpollScheduler &sched,
                                           AsyncFileStream &f,
                                           std::string_view delim = "\n") {
  std::string s;
  while (!s.ends_with(delim)) {
    errno = 0;
    int ch = getc(f);
    if (ch == EOF && errno == EAGAIN) {
      auto ev = co_await wait_file_event(
          sched, f, EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLET);
      if (!(ev & EPOLLIN)) {
        co_return {.result = std::move(s), .hup = true};
      }
      continue;
    } else if (ch == EOF && errno == 0) {
      // Someone set the EOF flag.
      co_return {.result = std::move(s), .hup = true};
    } else if (ch == EOF) {
      THROW_SYSCALL("read (getc)");
    }
    s.push_back(ch);
  }
  if (s.ends_with(delim)) {
    s.erase(s.size() - delim.size());
  }
  co_return {.result = std::move(s)};
}

// Returns how many bytes are read.
inline Task<IOResult<std::size_t>>
read_buffer(EpollScheduler &sched, AsyncFileStream &f, std::span<char> buf) {
  std::size_t i = 0;
  while (i < buf.size()) {
    errno = 0;
    int ch = getc(f);
    if (ch == EOF && errno == EAGAIN) {
      auto ev = co_await wait_file_event(
          sched, f, EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLET);
      if (!(ev & EPOLLIN)) {
        co_return {.result = i, .hup = true};
      }
      continue;
    } else if (ch == EOF) {
      THROW_SYSCALL("read (getc)");
    }
    buf[i++] = ch;
  }
  co_return {.result = i};
}

// May partially write a string if the EPOLLHUP is received.
// Returns the length of the written string.
inline Task<IOResult<std::size_t>>
print(EpollScheduler &sched, AsyncFileStream &f, std::string_view sv) {
  std::size_t len = 0;
  for (char ch : sv) {
    errno = 0;
    auto res = putc(ch, f);
    if (res == EOF && errno == EAGAIN) {
      auto ev =
          co_await wait_file_event(sched, f, EPOLLOUT | EPOLLHUP | EPOLLET);
      if (!(ev & EPOLLOUT)) {
        co_return {.result = len, .hup = true};
      }
      continue;
    } else if (res == EOF) {
      THROW_SYSCALL("write (putc)");
    }
    ++len;
  }
  co_return {.result = len};
}

inline void flush(AsyncFileStream &f) {
  CHECK_SYSCALL(fflush(static_cast<FILE *>(f)));
}

} // namespace coro