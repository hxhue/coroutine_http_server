#include <chrono>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <exception>
#include <iostream>
#include <queue>
#include <thread>
#include <type_traits>

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
        task.resume();
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

    // UB: A segmentation fault can be caused by erasing the frame first, then
    // resuming the erased frame! The details may differ but it's UB.
    //
    // https://stackoverflow.com/a/78405278/
    // <quote>When await_suspend returns a handle, resume() is called on that
    // handle. This violates the precondition of resume(), so the behavior is
    // undefined.</quote>
    // The "precondition of resume()" means the coroutine must be suspended.
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

struct WhenAllTaskGroup {
  std::size_t count_;
  std::exception_ptr exception_;
  std::coroutine_handle<> prev_;
};

template <typename R>
Task<void> when_all_task(WhenAllTaskGroup &group, Awaitable auto &awaitable,
                         R &result) {
  try {
    assign(result, co_await awaitable);
  } catch (...) {
    group.exception_ = std::current_exception();
    Scheduler::get()->add_task(group.prev_);
  }
  if (--group.count_ == 0) {
    Scheduler::get()->add_task(group.prev_);
  }
}

struct WhenAllAwaiter {
  bool await_ready() const { return tasks_.empty(); }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) {
    group_.prev_ = h;
    auto s = Scheduler::get();
    for (auto &task : tasks_.subspan(1)) {
      s->add_task(task);
    }
    return tasks_[0];
  }

  auto await_resume() const {
    if (group_.exception_) {
      std::rethrow_exception(group_.exception_);
    }
  }

  std::span<const std::coroutine_handle<>> tasks_;
  WhenAllTaskGroup &group_;
};

// when_all 假定了要等待的任务都是新任务，还不在调度器中
template <Awaitable... As, typename R = std::tuple<
                               typename AwaitableTraits<As>::NonVoidRetType...>>
  requires(sizeof...(As) != 0)
Task<R> when_all(As &&...as) {
  R result;
  WhenAllTaskGroup group{.count_ = sizeof...(As)};

  // Create tasks from awaitables.
  auto idx = std::make_index_sequence<sizeof...(As)>{};
  std::array tasks = [&]<std::size_t... I>(std::index_sequence<I...>) {
    return std::array{static_cast<std::coroutine_handle<>>(
        when_all_task(group, as, std::get<I>(result)).coro_)...};
  }(idx);

  // Start the first task and put all other tasks in the ready queue. When the
  // last task of the group finishes, it awakes the current coroutine.
  co_await WhenAllAwaiter{.tasks_ = tasks, .group_ = group};

  co_return result;
}

int main() {
  using namespace std::chrono_literals;
  auto *scheduler = Scheduler::get();

  auto task3 = []() -> Task<void> {
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
    auto [result1, result2] = co_await when_all(task1, task2);

    std::cout << "task1 result: " << result1 << std::endl;
    std::cout << "task2 result: " << result2 << std::endl;
  }();
  // scheduler->add_task(task1);
  // scheduler->add_task(task2);
  scheduler->add_task(task3);
  scheduler->main_loop();

  // std::cout << "task1 result: " << task1.coro_.promise().result() <<
  // std::endl; std::cout << "task2 result: " << task2.coro_.promise().result()
  // << std::endl;
}