#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <termios.h>
#include <unistd.h>

#include "aio.hpp"
#include "epoll.hpp"
#include "http.hpp"
#include "socket.hpp"
#include "task.hpp"
#include "utility.hpp"

using namespace coro;

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
AsyncFileStream ain(dup_stdin(), "r");
AsyncFileStream aout(dup_stdout(), "w");
AsyncFileStream aerr(dup_stdin(), "w");

Task<> amain() {
  using namespace std::literals;

  const char *host = "baidu.com";
  std::uint16_t port = 80;
  auto saddr = socket_address(ip_address(host), port);
  auto client = AsyncFileStream(co_await create_tcp_client(loop, saddr), "r+");

  HTTPRequest request{
      .method = "GET",
      .uri = "/",
      .headers =
          {
              {"host", host},
              {"user-agent", "Teapot"},
              {"connection", "keep-alive"},
          },
  };
  co_await request.write_to(loop, client);
  fflush(client);

  HTTPResponse response;
  co_await response.read_from(loop, client);

  co_await fputs(loop, aout, std::format("Status: {}\n\n", response.status));
  for (auto const &[k, v] : response.headers) {
    co_await fputs(loop, aout, std::format("{}: {}\n", k, v));
  }
  co_await fputs(loop, aout, std::format("\n{}", response.body));
  co_return;
}

int main() {
  HTTPRouter router;
  router.route(HTTPMethod::GET, "/home",
               [](HTTPRequest req) -> Task<HTTPResponse> {
                 HTTPResponse res;
                 res.status = 200;
                 res.headers["Content-Type"] = "text/html";
                 res.body = "<h1>Hello, World!</h1>";
                 co_return res;
               });
  router.route(HTTPMethod::GET, "/", [](HTTPRequest req) -> Task<HTTPResponse> {
    HTTPResponse res;
    res.status = 302;
    res.headers["Location"] = "/home";
    co_return res;
  });

  // const char *host = "baidu.com";
  // std::uint16_t port = 80;
  // auto saddr = socket_address(ip_address(host), port);

  // auto server_sock = create_tcp_socket(AF_INET);
  // // auto saddr = socket_address(ip_address("0.0.0.0"), 8080);
  // auto saddr = socket_address(IpAddress{in_addr{INADDR_ANY}}, 8080);

  /////////////////////////////////////////////////////////////

  int server_socket, client_socket;
  struct sockaddr_in server_addr, client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  std::uint16_t port = 8080;

  while (true) {
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
      std::cerr << "Failed to create socket\n";
      return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // 监听所有网络接口

    server_addr.sin_port = htons(port); // 监听端口

    if (bind(server_socket, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
      std::cerr << "Failed to bind socket to port " << port << "\n";
      close(server_socket);
      // return 1;
      port++;
    } else {
      break;
    }
  }

  if (listen(server_socket, 5) < 0) {
    std::cerr << "Failed to listen on socket\n";
    close(server_socket);
    return 1;
  }

  std::cout << "Server is listening on port " << port << "...\n";

  while (true) {
    client_socket = accept(server_socket, (struct sockaddr *)&client_addr,
                           &client_addr_len);
    if (client_socket < 0) {
      std::cerr << "Failed to accept client connection\n";
      continue;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    std::cout << "Accepted connection from " << client_ip << ":"
              << ntohs(client_addr.sin_port) << "\n";

    // 处理客户端请求

    auto t = [](auto client_socket, HTTPRouter &router) -> Task<> {
      AsyncFileStream f{AsyncFile{client_socket}, "r+"};
      HTTPRequest req;
      co_await req.read_from(loop, f);

      // co_await req.write_to(loop, aout);

      auto r = router.find_route(req.method, req.uri);
      if (r == nullptr) {
        DEBUG() << "cannot find route to [" << req.method << " " << req.uri
                << "]";
      } else {
        auto res = co_await r(req);
        co_await res.write_to(loop, f);
      }
    }(client_socket, router);

    run_task(loop, t);
    t.result(); // Check if there's an exception.
  }

  close(server_socket);
  return 0;
}