#include <cassert>
#include <cerrno>

#include <fcntl.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "epoll.hpp"
#include "task.hpp"

using namespace coro;

TEST(AwaitTaskTest, AwaitThrowingTask) {
  auto f = []() {
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
    auto task = []() -> Task<> {
      auto task2 = []() -> Task<> { throw std::logic_error{"123"}; }();
      co_await task2;
    }();
    run_task(loop, task);
    task.result();
  };

  EXPECT_THROW(f(), std::logic_error);
}