#include <cassert>
#include <cerrno>
#include <cstdio>
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
  using namespace std::chrono_literals;
  using namespace std::string_view_literals;
  auto f = AsyncFileStream(dup_stdin(false, false), "r"); // disable cannon mode
  while (true) {
    // std::string s;
    // while (!s.ends_with(": ")) {
    //   int ch = getc(f);
    //   if (ch == EOF) {
    //     auto ev = co_await wait_file_event(loop, f, EPOLLIN | EPOLLRDHUP);
    //     if (!(ev & EPOLLIN)) {
    //       DEBUG() << "\nHung up" << std::endl;
    //       co_return;
    //     }
    //     continue;
    //   }
    //   s.push_back(ch);
    // }
    // DEBUG() << s;
    // s.clear();
    // while (!s.ends_with("\n")) {
    //   int ch = getc(f);
    //   if (ch == EOF) {
    //     auto ev = co_await wait_file_event(loop, f, EPOLLIN | EPOLLRDHUP);
    //     if (!(ev & EPOLLIN)) {
    //       DEBUG() << "Hung up" << std::endl;
    //       co_return;
    //     }
    //     continue;
    //   }
    //   s.push_back(ch);
    // }
    // DEBUG() << s;
    auto res1 = co_await getline(loop, f, ":"sv);
    DEBUG() << res1.result;
    if (res1.partial) {
      break;
    }
    auto res2 = co_await getline(loop, f);
    DEBUG() << res2.result;
    if (res2.partial) {
      break;
    }
  }
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