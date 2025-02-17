#pragma once

#include <concepts>
#include <coroutine>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <variant>

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
struct Empty {};

} // namespace detail

template <class A> struct AwaitableTraits;

template <Awaiter A> struct AwaitableTraits<A> {
  using RetType = decltype(std::declval<A>().await_resume());
  using NonVoidRetType =
      std::conditional_t<std::is_same_v<void, RetType>, detail::Empty, RetType>;
};

template <class A>
  requires(!Awaiter<A> && Awaitable<A>)
struct AwaitableTraits<A>
    : AwaitableTraits<decltype(std::declval<A>().operator co_await())> {};

template <typename T>
concept PreviousPromise = requires(T p) {
  { p.prev_ } -> std::convertible_to<std::coroutine_handle<>>;
};

// 用保存上个协程句柄的方式实现了有栈协程的恢复功能。
// https://en.cppreference.com/w/cpp/coroutine/noop_coroutine
struct PreviousAwaiter {
  bool await_ready() const noexcept { return false; }

  template <PreviousPromise P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const noexcept {
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
      throw std::runtime_error("The value is not set");
    }
  }

  std::coroutine_handle<> prev_{};
  detail::promise::PromiseVariant<T> result_;
};

template <typename T = void, typename P = Promise<T>> struct Task {
  using promise_type = P;

  Task(std::coroutine_handle<promise_type> coro) : coro_(std::move(coro)) {}

  // Task is the owner of the coroutine handle and should not be copied.
  Task(Task const &) = delete;
  Task(Task &&) = default;

  ~Task() {
    if (coro_) {
      coro_.destroy();
      coro_ = nullptr;
    }
  }

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
};

struct ReturnPreviousAwaiter {
  bool await_ready() const noexcept { return false; }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) const noexcept {
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