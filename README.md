# Coroutine HTTP Server

<!-- - ~~Stackless coroutines with symmetric transfer~~ -->

# Design

How to start a task:

1. Create an entrypoint task.
2. This task may spawn other tasks, leaving them in different schedulers.
3. A task is either running, staying in schedulers to be resumed, or cancelled.
4. Schedulers collaborate and the entrypoint task finally finishes.

Schedulers:

- `TimedScheduler`
  - Stores coroutines waiting for time.
  - e.g. `sleep_for` and `sleep_until`.
- `EpollScheduler` (Blocking)
  - Stores coroutines waiting for files to become ready.
  - e.g. `AsyncFile`, `wait_file_event`, etc.
  - NOTE: Instead of registering function pointers to `epoll_event`, every waiter registers a `EpollFilePromise*`. When epoll signals an event, it provides us with a coroutine handle to resume. Unlike a standard function pointer, when a coroutine returns from the `resume()` call, it does not necessarily reach its conclusion. It can be launched, but not finished.

A blocking scheduler should be put at the end of a loop, making sure that there're no other tasks that are ready to run. They normally come with a timeout so we can check for new tasks.

File operations:

- The operations encapsulates `getc` and `putc` around a `FILE*` that is associated with non-blocking file descriptors.
- `AsyncFile` is to a file descriptor what `std::unique_ptr` is to a raw pointer.
- `AsyncFileStream` serves as a wrapper for `AsyncFile`, analogous to how `FILE*` operates.

---

🚧 *Where to put this?*

- `when_all` and `when_any`
    - They both assume the tasks passed as arguments are not in the scheduler.
    - When the last task of the `when_all` group finishes, it awakes the previous suspended task (which is waiting for `when_all` coroutine to finish).
    - When the first task finishes, `when_any` destroys the other tasks by returning from the coroutine body and letting the temporary tasks' destructors destroy the coroutine handles and remove them from the scheduler.

## Details

`co_await` is where a lot of logic happens.

### `PreviousTask` (`Task`)

Every basic `Promise` is a `PreviousPromise`, and every basic `Task` is a `PreviousTask`. That's why the names omit the prefix. 

1. A Task is also an awaitable, and `await_suspend` (triggered by `co_await`) saves current coroutine handle before returning the task's associated coroutine handle (task transfer). 
2. When the task finishes, `co_await final_suspend(h)` returns the previous coroutine handle and it gets resumed.

### `ReturnPreviousTask`

1. `return_value` (triggered by `co_return`) saves a new coroutine handle.
2. `co_await final_suspend(h)` returns the new handle if it's not null.

### `when_all` vs. multiple consecutive `co_await`s

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

However, if we change the `co_await when_all` expression to two separate `co_await` statements, only the first task gets created when the current coroutine is suspended. You end up waiting for 3 seconds!

```diff
-    auto [result1, result2] = co_await when_all(task1, task2);
+    auto result1 = co_await task1;
+    auto result2 = co_await task2;
```
For a more in-depth explanation, please refer to this link: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1316r0.pdf.

# Build

First, download googletest:

```bash
git submodule add https://github.com/google/googletest.git extern/googletest
git submodule update --init --recursive
```

Then run cmake commands to build the project.

Development setup:

- Compiler: GCC 13.3.0
- System: Ubuntu 24.04.1 LTS (WSL2)

# Resources

- [C++ Coroutines: Understanding Symmetric Transfer](https://lewissbaker.github.io/2020/05/11/understanding_symmetric_transfer)
- https://github.com/archibate/co_async
- [Lord of io_uring](https://unixism.net/loti/index.html)