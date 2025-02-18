#include <cassert>
#include <cerrno>
#include <iostream>

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "epoll.hpp"
#include "task.hpp"

// May have partial read.
inline Task<std::string> reader() {
  co_await wait_file(*EpollScheduler::get(), 0, EPOLLIN);
  std::string s;
  while (true) {
    char c;
    ssize_t len = read(0, &c, 1);
    if (len == -1) {
      if (errno != EWOULDBLOCK) {
        throw std::system_error(errno, std::system_category());
      }
      break;
    }
    s.push_back(c);
  }
  co_return s;
}

int main() {
  int read_fd = STDIN_FILENO;
  check_syscall(fcntl(read_fd, F_SETFL, O_NONBLOCK));

  auto task = []() -> Task<void> {
    while (true) {
      auto s = co_await reader();
      std::cout << "Got " << escape(s) << "\n";
      if (s == "quit\n")
        break;
    }
  }();
  EpollScheduler::get()->run(task);

  return 0;

  // int epfd = check_syscall(epoll_create1(0));
  // struct epoll_event event;
  // event.events = EPOLLIN;
  // event.data.fd = read_fd;
  // check_syscall(epoll_ctl(epfd, EPOLL_CTL_ADD, read_fd, &event));

  // while (true) {
  //   struct epoll_event ebuf[64];
  //   // Returns when there's signal/event/timeout.
  //   errno = 0;
  //   int ret =
  //       check_syscall(epoll_wait(epfd, ebuf, std::size(ebuf), 1000 /*ms*/));
  //   if (ret == 0) {
  //     std::cout << "epoll timeout\n";
  //     continue;
  //   }
  //   if (errno == EINTR) {
  //     std::cout << "epoll interrupted\n";
  //     continue;
  //   }
  //   std::cout << "ret: " << ret << "\n";
  //   for (int i = 0; i < ret; ++i) {
  //     auto &ev = ebuf[i];
  //     int fd = ev.data.fd;
  //     char ch;
  //     while (true) {
  //       errno = 0;
  //       ssize_t len = read(fd, &ch, 1);
  //       if (errno == EAGAIN) {
  //         std::cout << "read would block, try again\n";
  //         break;
  //       } else if (len == 0) {
  //         std::cout << "end of file (which is unlikely to happen because
  //         epoll "
  //                      "reports an event!)\n";
  //         break;
  //       } else {
  //         check_syscall(len);
  //         std::cout << "read: " << escape(ch) << "\n";
  //       }
  //     }
  //   }
  // }
}