#pragma once

#include <cassert>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <exception>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <variant>

#include "type_name.hpp"
#include "utility.hpp"

namespace coro {
template <class A>
concept Awaiter = requires(A a, std::coroutine_handle<> h) {
  { a.await_ready() };
  { a.await_suspend(h) };
  { a.await_resume() };
};

template <class A>
concept Awaitable = Awaiter<A> || requires(A a) {
  { a.operator co_await() } -> Awaiter;
};

namespace detail {
struct Void {};

} // namespace detail

template <class A> struct AwaitableTraits;

template <Awaiter A> struct AwaitableTraits<A> {
  using RetType = decltype(std::declval<A>().await_resume());
  using NonVoidRetType =
      std::conditional_t<std::is_same_v<void, RetType>, detail::Void, RetType>;
};

template <class A>
  requires(!Awaiter<A> && Awaitable<A>)
struct AwaitableTraits<A>
    : AwaitableTraits<decltype(std::declval<A>().operator co_await())> {};

template <typename T>
concept PreviousPromise = requires(T p) {
  { p.prev_ } -> std::convertible_to<std::coroutine_handle<>>;
};

// https://en.cppreference.com/w/cpp/coroutine/noop_coroutine
struct PreviousAwaiter {
  bool await_ready() const noexcept { return false; }

  template <PreviousPromise P>
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<P> h) const noexcept {
    if (auto prev = h.promise().prev_; prev)
      return prev;
    else
      return std::noop_coroutine();
  }

  void await_resume() const noexcept {}
};

namespace detail::promise {

template <typename T> struct PromiseVariantTrait {
  using type = std::variant<std::monostate, T, std::exception_ptr>;
};

template <> struct PromiseVariantTrait<void> {
  using type = std::variant<std::monostate, std::exception_ptr>;
};

template <typename T>
using PromiseVariant = typename PromiseVariantTrait<T>::type;

template <typename Derived, typename T, bool IsVoid = std::is_same_v<void, T>>
struct PromiseReturnYield {
  void return_value(T x) {
    auto &self = static_cast<Derived &>(*this);
    self.result_ = std::move(x);
  }

  std::suspend_always yield_value(T x) {
    auto &self = static_cast<Derived &>(*this);
    self.result_ = std::move(x);
    return {};
  }
};

template <typename Derived, typename T>
struct PromiseReturnYield<Derived, T, true> {
  void return_void() {}
};
} // namespace detail::promise

template <typename T>
struct Promise : detail::promise::PromiseReturnYield<Promise<T>, T> {

  std::suspend_always initial_suspend() noexcept { return {}; }

  auto final_suspend() noexcept { return PreviousAwaiter(); }

  void unhandled_exception() { result_ = std::current_exception(); }

  auto get_return_object() {
    return std::coroutine_handle<Promise>::from_promise(*this);
  }

  T result() {
    if (auto *pe = std::get_if<std::exception_ptr>(&result_)) {
      std::rethrow_exception(*pe);
    }
    if constexpr (!std::is_same_v<void, T>) {
      if (auto *pv = std::get_if<T>(&result_)) {
        auto ret = std::move(*pv);
        // result_ = std::monostate{}; // maybe do not reset the result?
        return ret;
      }
      auto h = std::coroutine_handle<Promise>::from_promise(*this);
      // DEBUG() << "-- Problematic handle: " << h.address() << std::endl;
      std::string s = "The value is not set: ";
      s += type_name<T>();
      throw std::runtime_error(s + "\n" + SOURCE_LOCATION());
    }
  }

  Promise() {
    auto h = std::coroutine_handle<Promise<T>>::from_promise(*this);
    // DEBUG() << " Promise(): " << h.address() << "\n";
  }

  inline ~Promise();

  std::coroutine_handle<> prev_{};
  detail::promise::PromiseVariant<T> result_;
};

template <typename T = void, typename P = Promise<T>>
struct [[nodiscard("maybe co_await this task?")]] Task {
  using promise_type = P;

  Task(std::coroutine_handle<promise_type> coro) : coro_(std::move(coro)) {}

  // Task is the owner of the coroutine handle and should not be copied.
  Task(Task const &) = delete;

  Task(Task &&other) : coro_(other.coro_) { other.coro_ = nullptr; }

  Task &operator=(Task other) {
    swap(*this, other);
    return *this;
  }

  inline ~Task();

  struct TaskAwaiter {
    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept {
      // 被等待时跳转到本协程执行，但是保留上一协程，将来恢复
      coro_.promise().prev_ = h;

      // Do not call coro_.resume() because std::coroutine_handle<...>::resume
      // allocates a new stack frame and is prune to stack overflow!
      //
      // https://lewissbaker.github.io/2020/05/11/understanding_symmetric_transfer#the-stack-overflow-problem
      // <quote>Every time we resume a coroutine by calling .resume() we create
      // a new stack-frame for the execution of that coroutine.</quote>
      return coro_;
    }

    T await_resume() const { return coro_.promise().result(); }

    TaskAwaiter(std::coroutine_handle<promise_type> coro)
        : coro_(std::move(coro)) {}

    std::coroutine_handle<promise_type> coro_;
  };

  auto operator co_await() const { return TaskAwaiter(coro_); }

  std::coroutine_handle<promise_type> coro_{};

  decltype(auto) result() const { return coro_.promise().result(); }
};

struct ReturnPreviousAwaiter {
  bool await_ready() const noexcept { return false; }

  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> h) const noexcept {
    return prev_ ? prev_ : std::noop_coroutine();
  }

  void await_resume() const noexcept {}

  std::coroutine_handle<> prev_{nullptr};
};

struct ReturnPreviousPromise {
  auto initial_suspend() noexcept { return std::suspend_always(); }

  auto final_suspend() noexcept { return ReturnPreviousAwaiter(prev_); }

  void unhandled_exception() { throw; }

  // Saves the coroutine handle returned by co_return and resumes it when
  // this->final_suspend() is co_awaited.
  void return_value(std::coroutine_handle<> previous) { prev_ = previous; }

  auto get_return_object() {
    return std::coroutine_handle<ReturnPreviousPromise>::from_promise(*this);
  }

  std::coroutine_handle<> prev_{nullptr};

  ReturnPreviousPromise &operator=(ReturnPreviousPromise &&) = delete;
};

struct ReturnPreviousTask {
  using promise_type = ReturnPreviousPromise;

  ReturnPreviousTask(std::coroutine_handle<promise_type> coroutine)
      : coro_(coroutine) {}

  ReturnPreviousTask(ReturnPreviousTask &&) = delete;

  ~ReturnPreviousTask() { coro_.destroy(); }

  std::coroutine_handle<promise_type> coro_;
};

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
    // it, but it can remove itself from the tree to avoid being resumed again.
    //
    // DEBUG() << "~TimedPromise():\n";
    // DEBUG() << "  tree: " << tree_ << "\n";
    if (tree_) {
      auto h = std::coroutine_handle<TimedPromise>::from_promise(*this);
      assert(tree_->contains(h));
      tree_->erase(h);
      tree_ = nullptr;
      // DEBUG() << "  erased: " << h.address() << std::endl;
    } else {
      // DEBUG() << "~TimedPromise(): tree_ is null\n";
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

inline bool operator<(const std::coroutine_handle<TimedPromise> &lhs,
                      const std::coroutine_handle<TimedPromise> &rhs) {
  auto t1 = lhs.promise().expire_;
  auto t2 = rhs.promise().expire_;
  if (t1 != t2) {
    return t1 < t2;
  }
  return lhs.address() < rhs.address();
}

struct TimedScheduler {
  void add_task(std::coroutine_handle<TimedPromise> h) {
    timed_coros_.insert(h);
  }

  template <typename P> auto run(std::coroutine_handle<P> entry_point) {
    int loop_count = 0;
    while (!entry_point.done()) {
      assert(++loop_count <= 1);
      entry_point.resume();
      while (auto delay = run()) {
        std::this_thread::sleep_for(*delay);
      }
    }
    return entry_point.promise().result();
  }

  template <typename... Ts> auto run(Task<Ts...> const &task) {
    return run(task.coro_);
  }

  // Returns the time we still have to wait for the next task to be ready.
  // Returns std::nullopt if there's no task to wait.
  std::optional<Clock::duration> run() {
    // There's no entry point, so the task must be put by other functions.
    while (!ready_coros_.empty() || !timed_coros_.empty()) {
      while (!ready_coros_.empty()) {
        auto it = ready_coros_.begin();
        auto coro = *it;
        ready_coros_.erase(it);
        coro.resume();
      }
      while (!timed_coros_.empty()) {
        auto it = timed_coros_.begin();
        auto &promise = it->promise();
        // Get the result and don't let it change.
        auto now = Clock::now();
        if (promise.expire_ <= now) {
          auto coro = *it;
          coro.resume();
          // TimedPromise is special and is only created by sleep functions.
          // Their coroutines do not contain co_yield, and we can assume that
          // when coro.resume() returns, coro is destroyed. NOTE: when a
          // coroutine handle is destroyed, method done() is unreliable
          // anymore.
          //
          // When that happens, it will also remove itself from the tree, so
          // don't erase the coroutine handle here.
          //
          // coroutines_.erase(it);
        } else {
          return promise.expire_ - now;
        }
      }
    }
    return std::nullopt;
  }

  // static TimedScheduler &get() {
  //   static TimedScheduler instance;
  //   return instance;
  // }

  TimedScheduler() = default;
  TimedScheduler(TimedScheduler &&) = delete;

  std::unordered_set<std::coroutine_handle<>> ready_coros_;
  std::set<std::coroutine_handle<TimedPromise>> timed_coros_;
};

template <typename T> Promise<T>::~Promise() {
  // auto h = std::coroutine_handle<Promise<T>>::from_promise(*this);
  // DEBUG() << "~Promise(): " << h.address() << "\n";
  // auto &sched = Scheduler::get();
  // if (sched.ready_coros_.contains(h)) {
  //   sched.ready_coros_.erase(h);
  // }
}

template <typename T, typename P> Task<T, P>::~Task() {
  if (coro_ && !coro_.done()) {
    // DEBUG() << "Task canceled: " << coro_.address() << "\n";
  }
  if (coro_) {
    // DEBUG() << "destroying " << coro_.address() << "\n";
    coro_.destroy();
    coro_ = nullptr;
  }
  // auto &sched = Scheduler::get();
  // if (sched.ready_coros_.contains(coro_)) {
  //   sched.ready_coros_.erase(coro_);
  // }
}

struct SleepAwaiter {
  bool await_ready() const noexcept { return expire_ <= Clock::now(); }

  void await_suspend(std::coroutine_handle<TimedPromise> h) noexcept {
    auto &promise = h.promise();
    promise.expire_ = expire_;
    promise.tree_ = &scheduler_.timed_coros_;
    scheduler_.add_task(h);
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
  TimedScheduler &scheduler_;
};

inline Task<void, TimedPromise> sleep_until(TimedScheduler &sched,
                                            Clock::time_point expire) {
  co_return co_await SleepAwaiter{expire, sched};
}

inline auto sleep_for(TimedScheduler &sched, Clock::duration duration) {
  return sleep_until(sched, Clock::now() + duration);
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
    for (auto &task : tasks_.subspan(1)) {
      // TODO: resume is not stackless!
      task.coro_.resume();
      // ERROR: If there's a task that's never resumed, its promise
      // never gets created and never gets destructed! So withdrawing
      // the task from the ready queue is impossible.
      //
      // Scheduler::get().ready_coros_.insert(task.coro_);
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

// The `when_all` function assumes that the tasks to be awaited are all new
// tasks and are not yet in the scheduler.
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
    for (auto &task : tasks_.subspan(1)) {
      // TODO: resume is not stackless!
      task.coro_.resume();
      // Scheduler::get().ready_coros_.insert(task.coro_);
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

template <class Loop, class T, class P>
T run_task(Loop &loop, Task<T, P> const &t) {
  auto a = t.operator co_await();
  a.await_suspend(std::noop_coroutine()).resume();
  loop.run();
  return a.await_resume();
}

template <class T, class P> void spawn_task(Task<T, P> const &t) {
  auto a = t.operator co_await();
  a.await_suspend(std::noop_coroutine()).resume();
}
} // namespace coro