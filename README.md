# Coroutine HTTP Server

- Stackless coroutines with symmetric transfer
- `sleep_for` and `sleep_until`
    - They play nice with coroutines.
- `when_all` and `when_any`
    - They both assume the tasks passed as arguments are not in the scheduler.
    - When the last task of the `when_all` group finishes, it awakes the previous suspended task (which is waiting for `when_all` coroutine to finish).
    - When the first task finishes, `when_any` destroys the other tasks by returning from the coroutine body and letting the temporary tasks' destructors destroy the coroutine handles and remove them from the scheduler.
    - <mark>Currently `when_all` and `when_any` calls `resume`</mark>, so it's not completely stackless.
- `EpollScheduler`
  - Instead of registering function pointers to `epoll_event`, register a pointer to `EpollFilePromise`. When When epoll gives us an event, we get a coroutine handle to resume. Compared to a normal function pointer, the coroutine can return without finishing, the semantic is being launched rather than having finished.

## `PreviousTask` (`Task`)

Every basic `Promise` is a `PreviousPromise`, and every basic `Task` is a `PreviousTask`. That's why the names omit the prefix. 

1. A Task is also an awaitable, and `await_suspend` (triggered by `co_await`) saves current coroutine handle before returning the task's associated coroutine handle (task transfer). 
2. When the task finishes, `co_await final_suspend(h)` returns the previous coroutine handle and it gets resumed.

## `ReturnPreviousTask`

1. `return_value` (triggered by `co_return`) saves a new coroutine handle.
2. `co_await final_suspend(h)` returns the new handle if it's not nullptr.

## `when_all` vs. multiple consecutive `co_await`s

In the following code, `task3` launches `task1` and `task2` before being suspended.

```cpp
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

  scheduler->add_task(task3);
  scheduler->main_loop();
}
```

However, if we change the `co_await when_all` expression to two separate `co_await` statements, only the first task gets created when the current coroutine is suspended.

```diff
-    auto [result1, result2] = co_await when_all(task1, task2);
+    auto result1 = co_await task1;
+    auto result2 = co_await task2;
```

# Resources

- [C++ Coroutines: Understanding Symmetric Transfer](https://lewissbaker.github.io/2020/05/11/understanding_symmetric_transfer)
- https://github.com/archibate/co_async