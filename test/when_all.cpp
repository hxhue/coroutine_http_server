#include <cassert>
#include <cerrno>
#include <iostream>

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "task.hpp"

int when_all() {
  using namespace std::chrono_literals;
  auto &scheduler = Scheduler::get();

  auto task3 = []() -> Task<int> {
    auto task1 = []() -> Task<int> {
      std::cout << "task1 goes to sleep\n";

      // throw std::runtime_error{"wow"};

      co_await sleep_for(500ms);
      std::cout << "task1 wakes up\n";
      co_return 1;
    }();
    auto task2 = []() -> Task<int> {
      std::cout << "task2 goes to sleep\n";
      co_await sleep_for(700ms);
      std::cout << "task2 wakes up\n";
      co_return 2;
    }();

    auto [result1, result2] = co_await when_all(task1, task2);
    std::cout << "task1 result: " << result1 << std::endl;
    std::cout << "task2 result: " << result2 << std::endl;
    co_return result1 + result2;
  }();

  auto result = scheduler.run(task3);
  std::cout << "result: " << result << "\n";
  return result;
}

TEST(WhenAllTest, WhenAll) { EXPECT_EQ(when_all(), 3); }

int when_all_throws() {
  using namespace std::chrono_literals;
  auto &scheduler = Scheduler::get();

  auto task3 = []() -> Task<int> {
    auto task1 = []() -> Task<int> {
      std::cout << "task1 goes to sleep\n";

      throw std::runtime_error{"wow"};

      co_await sleep_for(500ms);
      std::cout << "task1 wakes up\n";
      co_return 1;
    }();
    auto task2 = []() -> Task<int> {
      std::cout << "task2 goes to sleep\n";
      co_await sleep_for(700ms);
      std::cout << "task2 wakes up\n";
      co_return 2;
    }();

    auto [result1, result2] = co_await when_all(task1, task2);
    std::cout << "task1 result: " << result1 << std::endl;
    std::cout << "task2 result: " << result2 << std::endl;
    co_return result1 + result2;
  }();

  auto result = scheduler.run(task3);
  std::cout << "result: " << result << "\n";
  return result;
}

TEST(WhenAllTest, WhenAllThrows) {
  EXPECT_THROW({ when_all_throws(); }, std::runtime_error);
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}