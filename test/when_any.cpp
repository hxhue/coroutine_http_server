#include <cassert>
#include <cerrno>
#include <iostream>

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "task.hpp"

int main() {
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
  }();

  auto result = scheduler.run(task3);
  std::cout << "result: " << result << "\n";
}