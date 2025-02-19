#include <cassert>
#include <cerrno>
#include <iostream>

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <variant>

#include "epoll.hpp"
#include "task.hpp"
#include "utility.hpp"

// TODO: improve performance by reading more than one byte.
// May have partial read.
inline Task<std::string> reader(int fd) {
  using namespace std::chrono_literals;
  DEBUG() << "before wait_file\n";
  // sleep_for belongs to Scheduler and it can never finish if there's only
  // EpollScheduler.
  co_await wait_file(EpollScheduler::get(), fd, EPOLLIN);
  DEBUG() << "after  wait_file\n";
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
  using namespace std::chrono_literals;
  
  int read_fd = STDIN_FILENO;
  int flags = CHECK_SYSCALL(fcntl(read_fd, F_GETFL, 0));
  flags = flags | O_NONBLOCK;
  CHECK_SYSCALL(fcntl(read_fd, F_SETFL, flags));

  // Step 14: read from multiple fds.
  auto task = []() -> Task<void> {
    DEBUG() << "task\n";
    // Regular files do not support epoll. Open a new console and get its pty.
    //
    // $ readlink /proc/self/fd/0
    // /dev/pts/14
    //
    int fd = CHECK_SYSCALL(open("/dev/pts/14", O_RDONLY | O_NONBLOCK));
    auto defer = Defer([fd] { CHECK_SYSCALL(close(fd)); });
    while (true) {
      std::string s;
      {
        // Wait for one file with a timeout.
        // auto var = co_await when_any(reader(STDIN_FILENO), sleep_for(1s));
        // if (var.index() == 0) {
        //   s = std::get<0>(std::move(var));
        // }

        // Wait for one file.
        // s = co_await reader(STDIN_FILENO);
        
        // Wait for two files.
        auto var = co_await when_any(reader(STDIN_FILENO), reader(fd));
        std::visit([&s](auto &&v) { s = std::move(v); }, var);
        
        // NOTE: var is moved.
      }
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
      std::cout << "-- Next timer in " << delay.value() << "\n";
      auto ms = duration_cast<milliseconds>(delay.value()).count();
      if (ms <= 0)
        ms = 1;
      epoll_sched.try_run(ms);
    } else {
      // std::cout << "No timers\n";
      epoll_sched.try_run();
    }
  }
  // NOTE: check the result to see if there's an error.
  task.coro_.promise().result();
}