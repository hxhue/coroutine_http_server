#include <cassert>
#include <cerrno>
#include <iostream>

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <variant>

#include "epoll.hpp"
#include "task.hpp"
#include "utility.hpp"

inline std::size_t read_file_sync(AsyncFile &file, std::span<char> buffer) {
  return CHECK_SYSCALL_ALLOW(read(file.fd_, buffer.data(), buffer.size()),
                             EAGAIN);
}

inline Task<std::size_t> read_file(EpollScheduler &sched, AsyncFile &file,
                                   std::span<char> buffer) {
  co_await wait_file(sched, file, EPOLLIN | EPOLLRDHUP);
  auto len = read_file_sync(file, buffer);
  co_return len;
}

inline Task<std::string> read_string(AsyncFile &file) {
  auto &sched = EpollScheduler::get();
  co_await wait_file(sched, file, EPOLLIN | EPOLLET);
  std::string s;
  size_t chunk = 64;
  while (true) {
    char c;
    std::size_t exist = s.size();
    s.resize(exist + chunk);
    std::span<char> buffer(s.data() + exist, chunk);
    auto len = co_await read_file(sched, file, buffer);
    if (len != chunk) {
      s.resize(exist + len);
      break;
    }
    if (chunk < 65536)
      chunk *= 4;
  }
  co_return s;
}

int main() {
  using namespace std::chrono_literals;

  int read_fd = STDIN_FILENO;
  AsyncFile read_f{read_fd};
  read_f.set_nonblock();

  // Step 14: read from multiple fds.
  auto task = [](AsyncFile &read_f) -> Task<void> {
    DEBUG() << "task\n";
    // int fd = CHECK_SYSCALL(open("/dev/stdin", O_RDONLY | O_NONBLOCK));
    int fd = CHECK_SYSCALL(open("/dev/pts/14", O_RDONLY | O_NONBLOCK));
    AsyncFile file{fd};

    while (true) {
      std::string s;
      {
        // Wait for one file with a timeout.
        // auto var = co_await when_any(read_string(STDIN_FILENO),
        // sleep_for(1s)); if (var.index() == 0) {
        //   s = std::get<0>(std::move(var));
        // }

        // Wait for one file.
        // s = co_await read_string(read_f);

        // Wait for two files.
        auto var = co_await when_any(read_string(read_f), read_string(file));
        std::visit([&s](auto &&v) { s = std::move(v); }, var);

        // NOTE: var is moved.
      }
      std::cout << "Got " << escape(s) << "\n";
      if (s == "quit\n")
        break;
    }
  }(read_f);

  auto &epoll_sched = EpollScheduler::get();
  auto &sched = Scheduler::get();
  task.coro_.resume();
  while (!task.coro_.done()) {
    using namespace std::chrono;
    auto delay = sched.try_run();
    if (delay.has_value()) {
      std::cout << "-- Next timer in " << delay.value() << "\n";
      auto ms = duration_cast<milliseconds>(delay.value()).count();
      if (ms <= 0)
        ms = 1;
      epoll_sched.try_run(ms);
    } else {
      // std::cout << "No timers\n";
      epoll_sched.try_run();
    }
  }
  // NOTE: check the result to see if there's an error.
  task.coro_.promise().result();
}