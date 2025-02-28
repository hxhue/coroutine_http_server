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

using namespace coro;

EpollScheduler epoll_sched;
TimedScheduler sched;

int main() {
  using namespace std::chrono_literals;

  int read_fd = STDIN_FILENO;
  AsyncFile read_f{read_fd};
  // read_f.set_nonblock();

  // Step 14: read from multiple fds.
  auto task = [](AsyncFile &read_f) -> Task<void> {
    DEBUG() << "task\n";
    // int fd = CHECK_SYSCALL(open("/dev/stdin", O_RDONLY | O_NONBLOCK));
    int fd = CHECK_SYSCALL(open("/dev/pts/14", O_RDONLY | O_NONBLOCK));
    AsyncFile file{fd};

    while (true) {
      std::string s;
      {
        // Wait for one file with a timeout.
        // auto var = co_await when_any(read_string(STDIN_FILENO),
        // sleep_for(1s)); if (var.index() == 0) {
        //   s = std::get<0>(std::move(var));
        // }

        // Wait for one file.
        // s = co_await read_string(read_f);

        // Wait for two files.
        auto var = co_await when_any(read_string(epoll_sched, read_f),
                                     read_string(epoll_sched, file));
        std::visit([&s](auto &&v) { s = std::move(v.result); }, var);

        // NOTE: var is moved.
      }
      std::cout << "Got " << escape(s) << "\n";
      if (s == "quit\n")
        break;
    }
  }(read_f);

  task.coro_.resume();
  while (!task.coro_.done()) {
    using namespace std::chrono;
    auto delay = sched.run();
    if (delay.has_value()) {
      std::cout << "-- Next timer in " << delay.value() << "\n";
      epoll_sched.run(delay);
    } else {
      // std::cout << "No timers\n";
      epoll_sched.run();
    }
  }
  // NOTE: check the result to see if there's an error.
  task.coro_.promise().result();

  return 0;
}
