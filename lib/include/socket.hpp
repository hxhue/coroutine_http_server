#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <variant>

#include "epoll.hpp"
#include "utility.hpp"

namespace coro {

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
    THROW_SYSCALL("didn't get a result from getaddrinfo");
  }
  if (res->ai_family == AF_INET) {
    auto *ipv4 = (struct sockaddr_in *)res->ai_addr;
    return ipv4->sin_addr;
  } else {
    auto *ipv6 = (struct sockaddr_in6 *)res->ai_addr;
    return ipv6->sin6_addr;
  }

  throw std::invalid_argument("invalid domain name or ip address\n" +
                              SOURCE_LOCATION());
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

inline AsyncFile create_udp_socket(sa_family_t const &family) {
  AsyncFile sock(CHECK_SYSCALL(socket(family, SOCK_DGRAM, 0)));
  return sock;
}

// It's an unconnected socket and can be either a server or a client later.
// inline AsyncFile create_tcp_socket(SocketAddress const &addr) {
//   AsyncFile sock(CHECK_SYSCALL(socket(addr.addr_.ss_family, SOCK_STREAM,
//   0))); return sock;
// }

inline AsyncFile create_tcp_socket(sa_family_t const &family) {
  AsyncFile sock(CHECK_SYSCALL(socket(family, SOCK_STREAM, 0)));
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
  // sock.set_nonblock();
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
    THROW_SYSCALL("getsockopt SO_ERROR");
  }
}

inline Task<AsyncFile> create_tcp_client(EpollScheduler &sched,
                                         SocketAddress const &addr) {
  auto sock = create_tcp_socket(addr.addr_.ss_family);
  co_await socket_connect(sched, sock, addr);
  co_return sock;
}
} // namespace coro