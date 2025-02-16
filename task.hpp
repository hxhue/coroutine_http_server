#pragma once

#include <coroutine>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <variant>

// 用保存上个协程句柄的方式实现了有栈协程的恢复功能。
// https://en.cppreference.com/w/cpp/coroutine/noop_coroutine
struct PreviousAwaiter {
  std::coroutine_handle<> prev_;

  bool await_ready() const noexcept { return false; }

  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> coroutine) const noexcept {
    if (prev_)
      return prev_;
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
    self.value_ = std::move(x);
  }

  std::suspend_always yield_value(T x) {
    auto &self = static_cast<Derived &>(*this);
    self.value_ = std::move(x);
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

  auto final_suspend() noexcept { return PreviousAwaiter(prev_); }

  void unhandled_exception() { value_ = std::current_exception(); }

  auto get_return_object() {
    return std::coroutine_handle<Promise>::from_promise(*this);
  }

  T result() {
    if (auto *pe = std::get_if<std::exception_ptr>(&value_)) {
      std::rethrow_exception(*pe);
    }
    if constexpr (!std::is_same_v<void, T>) {
      if (auto *pv = std::get_if<T>(&value_)) {
        auto ret = std::move(*pv);
        value_ = std::monostate{};
        return ret;
      }
      throw std::runtime_error("The value is either consumed or not set");
    }
  }

  std::coroutine_handle<> prev_{};
  detail::promise::PromiseVariant<T> value_;
};

template <typename T> struct Task {
  using promise_type = Promise<T>;

  Task(std::coroutine_handle<promise_type> coro) : coro_(std::move(coro)) {}

  // Task(Task &&) = delete;

  ~Task() {
    // if (coro_) {
    //   coro_.destroy();
    // }
    // TODO: when to destroy the handle?
  }

  struct TaskAwaiter {
    bool await_ready() const { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) {
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

  operator std::coroutine_handle<>() const { return coro_; }

  std::coroutine_handle<promise_type> coro_{};
};