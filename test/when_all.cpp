#include <cassert>
#include <cerrno>
#include <iostream>

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "task.hpp"

using namespace coro;

template <bool Throws = false> int when_all() {
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

    auto [result1, result2] = co_await when_all(task1, task2);
    std::cout << "task1 result: " << result1 << std::endl;
    std::cout << "task2 result: " << result2 << std::endl;
    co_return result1 + result2;
  }(scheduler);

  auto result = scheduler.run(task3);
  std::cout << "result: " << result << "\n";
  return result;
}

TEST(WhenAll, WhenAll) { EXPECT_EQ(when_all(), 1); }

TEST(WhenAll, WhenAllThrows) {
  EXPECT_THROW({ when_all<true>(); }, std::runtime_error);
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}