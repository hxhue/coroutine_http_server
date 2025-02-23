#include <cassert>
#include <cerrno>
#include <iostream>

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "task.hpp"

using namespace coro;

template <bool Throws = false> int when_any() {
  using namespace std::chrono_literals;
  TimedScheduler scheduler;

  auto task3 = [](auto &sched) -> Task<int> {
    auto task1 = [](auto &sched) -> Task<int> {
      std::cout << "task1 goes to sleep\n";

      if constexpr (Throws) {
        throw std::runtime_error{"wow"};
      }

      co_await sleep_for(sched, 500ms);
      std::cout << "task1 wakes up\n";
      co_return 1;
    }(sched);
    auto task2 = [](auto &sched) -> Task<int> {
      std::cout << "task2 goes to sleep\n";
      co_await sleep_for(sched, 700ms);
      std::cout << "task2 wakes up\n";
      co_return 2;
    }(sched);

    auto result = co_await when_any(task1, task2);
    if (result.index() == 0) {
      auto r = std::get<0>(result);
      std::cout << "task1 finished first: " << r << std::endl;
      co_return r;
    } else {
      auto r = std::get<1>(result);
      std::cout << "task2 finished first: " << r << std::endl;
      co_return r;
    }
  }(scheduler);

  auto result = scheduler.run(task3);
  std::cout << "result: " << result << "\n";
  return result;
}

TEST(WhenAny, Basic) { EXPECT_EQ(when_any(), 1); }

TEST(WhenAny, Throws) {
  EXPECT_THROW({ when_any<true>(); }, std::runtime_error);
}
