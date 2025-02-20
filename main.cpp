#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <iostream>

#include <sys/epoll.h>
#include <termios.h>
#include <unistd.h>

#include "epoll.hpp"
#include "task.hpp"
#include "utility.hpp"

// socket.hpp
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

struct IpAddress {
  IpAddress(in_addr addr) noexcept : addr_(addr) {}

  IpAddress(in6_addr addr6) noexcept : addr_(addr6) {}

  IpAddress() = default;

  std::variant<in_addr, in6_addr> addr_;
};

inline IpAddress ip_address(char const *ip_or_domain) {
  in_addr addr{};
  in6_addr addr6{};
  if (CHECK_SYSCALL(inet_pton(AF_INET, ip_or_domain, &addr))) {
    return addr;
  }
  if (CHECK_SYSCALL(inet_pton(AF_INET6, ip_or_domain, &addr6))) {
    return addr6;
  }
  // It's a domain.
  struct addrinfo hints{};
  struct addrinfo *res{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (0 != getaddrinfo(ip_or_domain, nullptr, &hints, &res)) {
    THROW_SYSCALL("getaddrinfo");
  }
  if (!res) {
    THROW_SYSCALL("didn't get a result");
  }
  if (res->ai_family == AF_INET) {
    auto *ipv4 = (struct sockaddr_in *)res->ai_addr;
    return ipv4->sin_addr;
  } else {
    auto *ipv6 = (struct sockaddr_in6 *)res->ai_addr;
    return ipv6->sin6_addr;
  }

  throw std::invalid_argument("invalid domain name or ip address");
}

struct SocketAddress {
  SocketAddress() = default;

  SocketAddress(char const *path) {
    sockaddr_un saddr = {};
    saddr.sun_family = AF_UNIX;
    std::strncpy(saddr.sun_path, path, sizeof(saddr.sun_path) - 1);
    std::memcpy(&addr_, &saddr, sizeof(saddr));
    len_ = sizeof(saddr);
  }

  SocketAddress(in_addr host, int port) {
    sockaddr_in saddr = {};
    saddr.sin_family = AF_INET;
    std::memcpy(&saddr.sin_addr, &host, sizeof(saddr.sin_addr));
    // hton: host to network, s: short
    saddr.sin_port = htons(port);
    std::memcpy(&addr_, &saddr, sizeof(saddr));
    len_ = sizeof(saddr);
  }

  SocketAddress(in6_addr host, int port) {
    sockaddr_in6 saddr = {};
    saddr.sin6_family = AF_INET6;
    std::memcpy(&saddr.sin6_addr, &host, sizeof(saddr.sin6_addr));
    saddr.sin6_port = htons(port);
    std::memcpy(&addr_, &saddr, sizeof(saddr));
    len_ = sizeof(saddr);
  }

  sockaddr_storage addr_;
  socklen_t len_;
};

inline SocketAddress socket_address(IpAddress ip, int port) {
  return std::visit([&](auto const &addr) { return SocketAddress(addr, port); },
                    ip.addr_);
}

inline AsyncFile create_udp_socket(SocketAddress const &addr) {
  AsyncFile sock(CHECK_SYSCALL(socket(addr.addr_.ss_family, SOCK_DGRAM, 0)));
  return sock;
}

inline AsyncFile create_tcp_socket(SocketAddress const &addr) {
  AsyncFile sock(CHECK_SYSCALL(socket(addr.addr_.ss_family, SOCK_STREAM, 0)));
  return sock;
}

template <typename T = int>
inline int socket_getopt(AsyncFile &sock, int level, int optname) {
  T optval;
  socklen_t optlen = sizeof(optval);
  CHECK_SYSCALL(getsockopt(sock.fd_, level, optname, &optval, &optlen));
  return optval;
}

inline Task<void> socket_connect(EpollScheduler &sched, AsyncFile &sock,
                                 SocketAddress const &addr) {
  sock.set_nonblock();
  int res = connect(sock.fd_, (sockaddr const *)&addr.addr_, addr.len_);
  if (res == -1 && errno == EINPROGRESS) {
    res = 0;
  }
  if (res == -1) {
    THROW_SYSCALL("connect");
  }
  // As stated by the linux manual, it is possible to select(2) or poll(2) for
  // completion by selecting the socket for writing.
  co_await wait_file_event(sched, sock, EPOLLOUT);
  int pending_error =
      socket_getopt(sock,       //
                    SOL_SOCKET, // socket-level, protocol-independent
                    SO_ERROR);  // get and clear the pending error
  if (pending_error != 0) {
    errno = pending_error;
    THROW_SYSCALL("SO_ERROR");
  }
}

inline Task<AsyncFile> create_tcp_client(EpollScheduler &sched,
                                         SocketAddress const &addr) {
  auto sock = create_tcp_socket(addr);
  co_await socket_connect(sched, sock, addr);
  co_return sock;
}

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

  // auto sock_addr = socket_address(ip_address("baidu.com"), 80);

  // https://tcpbin.com/
  auto sock_addr = socket_address(ip_address("tcpbin.com"), 4242);

  auto sock = co_await create_tcp_client(loop, sock_addr);

  // co_await write_file(loop, sock, "GET / HTTP/1.1\r\n\r\n");

  for (int i = 0; i < 5; ++i) {
    // Don't have to sleep. The server sends messages slow because of long
    // distance.
    //
    // if (i > 0) {
    //   co_await sleep_for(loop, 300ms);
    // }

    // The echo program expected an newline.
    co_await write_file(loop, sock, std::to_string(i) + "\n");
    // DEBUG() << "after write_file()\n";

    char buf[4096];
    auto len = co_await read_file(loop, sock, buf);
    // DEBUG() << "after read_file()\n";

    std::string_view res(buf, len);
    std::cout << escape(res) << std::endl;
  }
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