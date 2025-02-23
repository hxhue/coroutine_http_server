#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <sys/epoll.h>
#include <termios.h>
#include <unistd.h>

#include "epoll.hpp"
#include "socket.hpp"
#include "task.hpp"

struct AsyncLoop {
  void run() {
    while (true) {
      auto timeout = timed_sched_.run();
      if (epoll_sched_.have_registered_events()) {
        epoll_sched_.run(timeout);
      } else if (timeout) {
        std::this_thread::sleep_for(*timeout);
      } else {
        break;
      }
    }
  }

  operator TimedScheduler &() { return timed_sched_; }

  operator EpollScheduler &() { return epoll_sched_; }

private:
  TimedScheduler timed_sched_;
  EpollScheduler epoll_sched_;
};

AsyncLoop loop;

Task<> amain() {
  using namespace std::chrono_literals;

  auto sock_addr = socket_address(ip_address("baidu.com"), 80);
  auto sock = co_await create_tcp_client(loop, sock_addr);

  co_await write_file(loop, sock,
                      "GET / HTTP/1.1\r\n"
                      "Host: baidu.com\r\n"
                      "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                      "AppleWebKit/537.36 (KHTML, like Gecko) "
                      "Chrome/91.0.4472.124 Safari/537.36\r\n"
                      "Accept: "
                      "text/html,application/xhtml+xml,application/"
                      "xml;q=0.9,image/avif,image/webp,image/apng,*/"
                      "*;q=0.8,application/signed-exchange;v=b3;q=0.9\r\n"
                      "Accept-Language: en-US,en;q=0.9\r\n"
                      "Connection: close\r\n"
                      "\r\n");

  char buf[4096];
  int spins = 0;
  while (true) {
    auto res =
        co_await when_any(sleep_for(loop, 3s), read_file(loop, sock, buf));
    if (auto *p = std::get_if<IOResult<std::size_t>>(&res)) {
      auto len = p->result;
      std::string_view sv{buf, len};
      std::cout << sv;
      if (p->hup) {
        std::cout << "HUP\n";
        break;
      }
      if (++spins > 100) {
        std::cout << "Too many spins!\n";
        break;
      }
    } else {
      std::cout << "Timed out\n";
    }
  }

  std::cout << "\n====================\nDone!\n";
}

int main() {
  using namespace std::chrono_literals;

  // struct termios tc;
  // tcgetattr(STDIN_FILENO, &tc);
  // tc.c_lflag &= ~ICANON;
  // tc.c_lflag &= ~ECHO;
  // tcsetattr(STDIN_FILENO, TCSANOW, &tc);

  auto task = amain();
  run_task(loop, task);
}