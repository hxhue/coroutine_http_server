#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <sys/epoll.h>
#include <termios.h>
#include <unistd.h>

#include "aio.hpp"
#include "epoll.hpp"
#include "socket.hpp"
#include "task.hpp"

using namespace coro;

struct AsyncLoop {
  void run() {
    while (true) {
      auto timeout = timed_sched_.run();
      if (epoll_sched_.have_registered_events()) {
        epoll_sched_.run(timeout);
      } else if (timeout) {
        std::this_thread::sleep_for(*timeout);
      } else {
        break;
      }
    }
  }

  operator TimedScheduler &() { return timed_sched_; }

  operator EpollScheduler &() { return epoll_sched_; }

private:
  TimedScheduler timed_sched_;
  EpollScheduler epoll_sched_;
};

AsyncLoop loop;

Task<> amain() {
  // using namespace std::chrono_literals;
  // InputStream ain(loop, dup_stdin(false));
  // while (true) {
  //     auto s = co_await ain.getline(": ");
  //     debug(), s;
  //     s = co_await ain.getline('\n');
  //     debug(), s;
  // }
  co_return;
}

int main() {
  using namespace std::chrono_literals;

  // struct termios tc;
  // tcgetattr(STDIN_FILENO, &tc);
  // tc.c_lflag &= ~ICANON;
  // tc.c_lflag &= ~ECHO;
  // tcsetattr(STDIN_FILENO, TCSANOW, &tc);

  auto task = amain();
  run_task(loop, task);
}