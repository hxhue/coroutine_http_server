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
#include "http.hpp"
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
  // FIXME: when mode is incorrect, there is no error message.
  auto f = AsyncFileStream(dup_stdout(), "w");
  HTTPRequest request{
      .method = "GET",
      .uri = "/api/tts?text=一二三四五",
      .headers =
          {
              {"host", "142857.red:8080"},
              {"user-agent", "co_async"},
              {"user-aGEnt", "co_async"},
              {"connection", "keep-alive"},
          },
  };
  co_await request.write(loop, f);
  co_return;
}

int main() {
  auto task = amain();
  run_task(loop, task);
}