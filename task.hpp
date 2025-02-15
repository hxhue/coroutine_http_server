#pragma once

#include <coroutine>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>

struct NoValue : std::runtime_error {
  NoValue() : std::runtime_error("The value is either consumed or not set") {}
};

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

struct PromiseBase {
  std::suspend_always initial_suspend() noexcept { return {}; }
  auto final_suspend() noexcept {
    // 有可能是从 TaskAwaiter::await_suspend 跳转来的，因此要检查是否需要恢复上一个协程
    return PreviousAwaiter(prev_);
  }
  void unhandled_exception() { exception_ = std::current_exception(); }

  std::exception_ptr exception_{};
  std::coroutine_handle<> prev_{};
};

template <typename T> struct Promise : PromiseBase {
  auto get_return_object() {
    return std::coroutine_handle<Promise>::from_promise(*this);
  }

  void return_value(T x) { value_ = std::move(x); }

  std::suspend_always yield_value(T x) {
    value_ = std::move(x);
    return {};
  }

  // Helper function
  T result() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
    if (value_.has_value()) {
      auto ret = std::move(value_.value());
      value_ = std::nullopt;
      return ret;
    }
    throw NoValue();
  }

  std::optional<T> value_{};
};

template <> struct Promise<void> : PromiseBase {
  auto get_return_object() {
    return std::coroutine_handle<Promise>::from_promise(*this);
  }

  void return_void() {}

  // Helper function
  void result() {
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }
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