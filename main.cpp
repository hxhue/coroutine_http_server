#include <cassert>
#include <cerrno>
#include <iostream>

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "epoll.hpp"
#include "task.hpp"

// May have partial read.
inline Task<std::string> reader() {
  using namespace std::chrono_literals;

  // sleep_for belongs to Scheduler and it can never finish if there's only
  // EpollScheduler. We need to manage 2 schedulers together.
  // The waiter must be created outside.
  //
  // TODO: waiter should remove the file descriptor from epoll in its
  // destructor.
  auto waiter = wait_file(EpollScheduler::get(), 0, EPOLLIN);
  auto result = co_await when_any(waiter, sleep_for(1s));
  if (result.index() == 1) {
    std::cout << "Returning because of timeout.\n";
    co_return "";
  }
  std::string s;
  while (true) {
    char c;
    ssize_t len = read(0, &c, 1);
    if (len == -1) {
      if (errno != EWOULDBLOCK) {
        throw std::system_error(errno, std::system_category());
      }
      break;
    }
    s.push_back(c);
  }
  co_return s;
}

int main() {
  int read_fd = STDIN_FILENO;
  check_syscall(fcntl(read_fd, F_SETFL, O_NONBLOCK));

  auto task = []() -> Task<void> {
    while (true) {
      auto s = co_await reader();
      std::cout << "Got \"" << escape(s) << "\"\n";
      if (s == "quit\n")
        break;
    }
  }();

  auto &epoll_sched = EpollScheduler::get();
  auto &sched = Scheduler::get();
  task.coro_.resume();
  while (!task.coro_.done()) {
    using namespace std::chrono;
    auto delay = sched.try_run();
    if (delay.has_value()) {
      std::cout << "Next timer in " << delay.value() << "\n";
      auto ms = duration_cast<milliseconds>(delay.value()).count();
      if (ms <= 0)
        ms = 1;
      epoll_sched.try_run(ms);
    } else {
      std::cout << "No timers\n";
      epoll_sched.try_run();
    }
  }
}