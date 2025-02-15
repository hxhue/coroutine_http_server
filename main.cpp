#include <chrono>
#include <coroutine>
#include <deque>
#include <iostream>
#include <queue>
#include <thread>

#include "task.hpp"

using Clock = std::chrono::steady_clock;

struct Scheduler {
  struct Timer {
    // For min-heap.
    bool operator<(Timer const &timer) const { return expire > timer.expire; }

    Clock::time_point expire;
    std::coroutine_handle<> h;
  };

  void add_task(std::coroutine_handle<> h) {
    // A new task gets scheduled early.
    ready_queue_.push_front(h);
  }

  void add_timer(Clock::time_point expire, std::coroutine_handle<> h) {
    auto timer = Timer{expire, h};
    timer_queue_.push(timer);
  }

  void main_loop() {
    while (!timer_queue_.empty() || !ready_queue_.empty()) {
      while (!ready_queue_.empty()) {
        auto task = ready_queue_.front();
        ready_queue_.pop_front();
        // Run the task until it's finished, unless it co_awaits on a sleep
        // function which put the task in the queue again.
        // std::cout << "before resuming " << task.address() << std::endl;
        task.resume();
        // std::cout << "after resuming " << task.address() << std::endl;
      }
      if (!timer_queue_.empty()) {
        auto top = timer_queue_.top();
        if (top.expire <= Clock::now()) {
          timer_queue_.pop();
          ready_queue_.push_back(top.h);
        } else {
          std::this_thread::sleep_until(top.expire);
        }
      }
    }
  }

  // If the function returns by reference, the result may be copied
  // accidentially by auto instead of auto&. Returning by pointer is safer.
  static Scheduler *get() {
    static Scheduler instance;
    return &instance;
  }

  Scheduler() = default;
  Scheduler(Scheduler &&) = delete;

  std::deque<std::coroutine_handle<>> ready_queue_;
  std::deque<std::coroutine_handle<>> waiting_queue_; // TODO: not used
  std::priority_queue<Timer> timer_queue_;
};

struct SleepAwaiter {
  bool await_ready() const { return expire_time_ <= Clock::now(); }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) {
    Scheduler::get()->add_timer(expire_time_, h);
    return std::noop_coroutine();
    //
    // UB: A segmentation fault can be caused by erasing the frame first, then
    // resuming the erased frame!
    //
    // return h;
  }

  auto await_resume() const noexcept {}

  explicit SleepAwaiter(Clock::time_point expire) : expire_time_(expire) {}

  Clock::time_point expire_time_;
};

inline Task<void> sleep_until(Clock::time_point expire) {
  co_return co_await SleepAwaiter(expire);
}

inline Task<void> sleep_for(Clock::duration duration) {
  co_return co_await SleepAwaiter(Clock::now() + duration);
}

int main() {
  using namespace std::chrono_literals;
  auto *scheduler = Scheduler::get();
  auto task1 = []() -> Task<int> {
    std::cout << "task1 goes to sleep\n";
    co_await sleep_for(1s);
    std::cout << "task1 wakes up\n";
    co_return 1;
  }();
  auto task2 = []() -> Task<int> {
    std::cout << "task2 goes to sleep\n";
    co_await sleep_for(2s);
    std::cout << "task2 wakes up\n";
    co_return 2;
  }();
  scheduler->add_task(task1);
  scheduler->add_task(task2);
  scheduler->main_loop();

  std::cout << "task1 result: " << task1.coro_.promise().result() << std::endl;
  std::cout << "task2 result: " << task2.coro_.promise().result() << std::endl;
}