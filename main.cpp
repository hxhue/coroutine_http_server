#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <exception>
#include <iostream>
#include <queue>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include "task.hpp"

using Clock = std::chrono::steady_clock;

struct TimedPromise : Promise<void> {
  TimedPromise() = default;

  TimedPromise(Clock::time_point expire) : expire_(expire) {}

  TimedPromise(Clock::time_point expire,
               std::set<std::coroutine_handle<TimedPromise>> *tree)
      : expire_(expire), tree_(tree) {}

  TimedPromise(TimedPromise &&other) = delete;

  ~TimedPromise() {
    // TimedPromise does not own the coroutine handle, so it does not destroy
    // it.
    // std::cout << "~TimedPromise():\n";
    // std::cout << "  tree: " << tree_ << "\n";
    if (tree_) {
      auto h = std::coroutine_handle<TimedPromise>::from_promise(*this);
      assert(tree_->contains(h));
      tree_->erase(h);
      tree_ = nullptr;
      // std::cout << "  erased: " << h.address() << std::endl;
    } else {
      std::cerr << "~TimedPromise(): tree_ is null\n";
    }
  }

  auto get_return_object() {
    return std::coroutine_handle<TimedPromise>::from_promise(*this);
  }

  bool operator<(const TimedPromise &other) const {
    return expire_ < other.expire_;
  }

  Clock::time_point expire_{Clock::now()};
  std::set<std::coroutine_handle<TimedPromise>> *tree_{nullptr};
};

bool operator<(const std::coroutine_handle<TimedPromise> &lhs,
               const std::coroutine_handle<TimedPromise> &rhs) {
  auto t1 = lhs.promise().expire_;
  auto t2 = rhs.promise().expire_;
  if (t1 != t2) {
    return t1 < t2;
  }
  return lhs.address() < rhs.address();
}

struct Scheduler {
  void add_task(std::coroutine_handle<TimedPromise> h) {
    timed_coros_.insert(h);
  }

  template <typename P> auto run(std::coroutine_handle<P> entry_point) {
    while (!entry_point.done()) {
      entry_point.resume();
      // Either finished or left some timers.
      while (!timed_coros_.empty()) {
        auto it = timed_coros_.begin();
        if (it->promise().expire_ <= Clock::now()) {
          auto coro = *it;
          coro.resume();
          // When coro is done, it will destroy the promise and remove itself.
          // Don't erase the coroutine handle here.
          //
          // coroutines_.erase(it);
        } else {
          std::this_thread::sleep_until(it->promise().expire_);
        }
      }
    }
    return entry_point.promise().result();
  }

  template <typename... Ts> auto run(Task<Ts...> const &task) {
    return run(task.coro_);
  }

  // If the function returns by reference, the result may be copied
  // accidentally by auto instead of auto&. Returning by pointer is safer.
  static Scheduler *get() {
    static Scheduler instance;
    return &instance;
  }

  Scheduler() = default;
  Scheduler(Scheduler &&) = delete;

  // std::deque<std::coroutine_handle<>> ready_coros_;
  std::set<std::coroutine_handle<TimedPromise>> timed_coros_;
};

struct SleepAwaiter {
  bool await_ready() const noexcept { return expire_ <= Clock::now(); }

  void await_suspend(std::coroutine_handle<TimedPromise> h) noexcept {
    if (scheduler_) {
      auto &promise = h.promise();
      promise.expire_ = expire_;
      promise.tree_ = &scheduler_->timed_coros_;
      scheduler_->add_task(h);
    }
    // return std::noop_coroutine();

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

  auto await_resume() const {}

  Clock::time_point expire_{Clock::now()};
  Scheduler *scheduler_{nullptr};
};

inline Task<void, TimedPromise> sleep_until(Clock::time_point expire) {
  co_await SleepAwaiter{expire, Scheduler::get()};
  co_return;
}

inline auto sleep_for(Clock::duration duration) {
  return sleep_until(Clock::now() + duration);
}

namespace detail::when_all {

struct WhenAllTaskGroup {
  std::size_t count_{};
  std::exception_ptr exception_{};
  std::coroutine_handle<> prev_{};
};

struct WhenAllAwaiter {
  bool await_ready() const noexcept { return tasks_.empty(); }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
    group_.prev_ = h;
    auto s = Scheduler::get();
    for (auto &task : tasks_.subspan(1)) {
      // s->add_task(task);
      // TODO: resume is not stackless!
      task.coro_.resume();
    }
    return tasks_[0].coro_;
  }

  auto await_resume() const {
    if (group_.exception_) {
      std::rethrow_exception(group_.exception_);
    }
  }

  std::span<const ReturnPreviousTask> tasks_;
  WhenAllTaskGroup &group_;
};

template <typename R>
ReturnPreviousTask when_all_task(WhenAllTaskGroup &group,
                                 Awaitable auto &awaitable, R &result) {
  try {
    using RealRetType = typename AwaitableTraits<
        std::remove_reference_t<decltype(awaitable)>>::RetType;
    if constexpr (std::is_same_v<void, RealRetType>) {
      co_await awaitable;
    } else {
      result = co_await awaitable;
    }
  } catch (...) {
    group.exception_ = std::current_exception();
    co_return group.prev_;
  }
  assert(group.count_ > 0);
  if (--group.count_ == 0) {
    co_return group.prev_;
  }
  co_return nullptr;
}

} // namespace detail::when_all

// when_all 假定了要等待的任务都是新任务，还不在调度器中
template <Awaitable... As, typename Tuple = std::tuple<
                               typename AwaitableTraits<As>::NonVoidRetType...>>
  requires(sizeof...(As) != 0)
Task<Tuple> when_all(As &&...as) {
  using namespace detail::when_all;

  Tuple result;
  WhenAllTaskGroup group{.count_ = sizeof...(As)};

  // Create tasks from awaitables.
  auto idx = std::make_index_sequence<sizeof...(As)>{};
  auto tasks = [&]<std::size_t... I>(std::index_sequence<I...>) {
    return std::array{(when_all_task(group, as, std::get<I>(result)))...};
  }(idx);

  // Start the first task and put all other tasks in the ready queue. When the
  // last task of the group finishes, it awakes the current coroutine.
  co_await WhenAllAwaiter{.tasks_ = tasks, .group_ = group};

  co_return result;
}

namespace detail::when_any {

struct WhenAnyTaskGroup {
  static constexpr auto invalid_index = static_cast<std::size_t>(-1);

  std::size_t index{invalid_index};
  std::exception_ptr exception_{};
  std::coroutine_handle<> prev_{};
};

struct WhenAnyAwaiter {
  bool await_ready() const noexcept { return tasks_.empty(); }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
    group_.prev_ = h;
    auto s = Scheduler::get();
    for (auto &task : tasks_.subspan(1)) {
      // TODO: resume is not stackless!
      task.coro_.resume();
    }
    return tasks_[0].coro_;
  }

  auto await_resume() const {
    if (group_.exception_) {
      std::rethrow_exception(group_.exception_);
    }
  }

  std::span<const ReturnPreviousTask> tasks_;
  WhenAnyTaskGroup &group_;
};

template <size_t I, typename Variant>
ReturnPreviousTask when_any_task(WhenAnyTaskGroup &group,
                                 Awaitable auto &awaitable, Variant &result) {
  // One of the tasks is already finished.
  if (group.index != WhenAnyTaskGroup::invalid_index || group.exception_) {
    co_return nullptr;
  }
  try {
    using RealRetType = typename AwaitableTraits<
        std::remove_reference_t<decltype(awaitable)>>::RetType;
    if constexpr (std::is_same_v<void, RealRetType>) {
      co_await awaitable;
      result.template emplace<I>();
    } else {
      result.template emplace<I>(co_await awaitable);
    }
    group.index = I;
  } catch (...) {
    group.exception_ = std::current_exception();
  }
  co_return group.prev_;
}

} // namespace detail::when_any

template <Awaitable... As, typename Variant = std::variant<
                               typename AwaitableTraits<As>::NonVoidRetType...>>
  requires(sizeof...(As) != 0)
Task<Variant> when_any(As &&...as) {
  using namespace detail::when_any;

  Variant result;
  WhenAnyTaskGroup group{};

  // Create tasks from awaitables.
  auto idx = std::make_index_sequence<sizeof...(As)>{};
  auto tasks = [&]<std::size_t... I>(std::index_sequence<I...>) {
    return std::array{(when_any_task<I>(group, as, result))...};
  }(idx);

  co_await WhenAnyAwaiter{.tasks_ = tasks, .group_ = group};
  co_return result;
}

int main() {
  using namespace std::chrono_literals;
  auto *scheduler = Scheduler::get();

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

    // auto result = co_await when_any(task1, task2);
    // if (result.index() == 0) {
    //   auto r = std::get<0>(result);
    //   std::cout << "task1 finished first: " << r << std::endl;
    //   co_return r;
    // } else {
    //   auto r = std::get<1>(result);
    //   std::cout << "task2 finished first: " << r << std::endl;
    //   co_return r;
    // }
  }();

  auto result = scheduler->run(task3);
  std::cout << "result: " << result << "\n";
}