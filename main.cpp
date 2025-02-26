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

  TimedScheduler &get_timed_scheduler() { return timed_sched_; }

  EpollScheduler &get_epoll_scheduler() { return epoll_sched_; }

  operator TimedScheduler &() { return get_timed_scheduler(); }

  operator EpollScheduler &() { return get_epoll_scheduler(); }

private:
  TimedScheduler timed_sched_;
  EpollScheduler epoll_sched_;
};

AsyncLoop loop;
AsyncFileStream ain(dup_stdin(), "r");
AsyncFileStream aout(dup_stdout(), "w");
AsyncFileStream aerr(dup_stdin(), "w");

constexpr int MAX_PENDING_CONNECTIONS = 128;

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

  co_await print(loop, aout, std::format("Status: {}\n\n", response.status));
  for (auto const &[k, v] : response.headers) {
    co_await print(loop, aout, std::format("{}: {}\n", k, v));
  }
  co_await print(loop, aout, std::format("\n{}", response.body));
  co_return;
}

int main() {
  using namespace std::literals;

  /////////////////// Set up routes ///////////////////
  HTTPRouter router;
  router.route(HTTPMethod::GET, "/", [](HTTPRequest req) -> Task<HTTPResponse> {
    HTTPResponse res;
    res.status = 302;
    res.headers["Location"] = "/home/"sv;
    co_return res;
  });
  router.route(HTTPMethod::GET, "/home/"sv,
               [](HTTPRequest req) -> Task<HTTPResponse> {
                 HTTPResponse res;
                 res.status = 200;
                 res.headers["Content-Type"] = "text/html"sv;
                 res.body = "<h1>Hello, World!</h1>"sv;
                 co_return res;
               });

  /////////////////// Create a TCP server ///////////////////
  int server_socket;
  struct sockaddr_in server_addr;

  const std::uint16_t min_port = 9000;
  const std::uint16_t max_port = min_port + 200;
  std::uint16_t port = min_port;
  while (true) {
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
      std::cerr << "Failed to create socket\n";
      return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port); // !!!

    if (bind(server_socket, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
      CHECK_SYSCALL(close(server_socket));
      if (++port > max_port) {
        std::cerr << std::format("Failed to bind socket in port range {}-{}",
                                 min_port, max_port)
                  << std::endl;
      }
    } else {
      break;
    }
  }

  if (listen(server_socket, MAX_PENDING_CONNECTIONS) < 0) {
    CHECK_SYSCALL(close(server_socket));
    THROW_SYSCALL("Failed to listen on socket");
  }

  AsyncFile server_sock{server_socket};

  std::cout << "Server is listening on port " << port << "...\n";

  /////////////////// Create the entrypoint task ///////////////////
  auto task = [](AsyncFile &server_sock, HTTPRouter &router) -> Task<> {
    while (true) {
      struct sockaddr_in client_addr;
      socklen_t client_addr_len = sizeof(client_addr);

      auto client_sock_temp = co_await socket_accept(
          loop, server_sock, (struct sockaddr *)&client_addr, &client_addr_len);
      AsyncFileStream client_stream{std::move(client_sock_temp), "r+"};

      // TODO: we need a co_spawn and put all those logic elsewhere.
      char client_ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
      co_await print(loop, aout,
                     std::format("Accepted connection from {}:{}\n", client_ip,
                                 ntohs(client_addr.sin_port)));

      HTTPRequest req;
      co_await req.read_from(loop, client_stream);
      co_await req.write_to(loop, aout, "> "sv);

      auto r = router.find_route(req.method, req.uri);
      if (r == nullptr) {
        co_await print(loop, aout,
                       std::format("!!! Cannot find route to [{} {}]\n",
                                   req.method, req.uri));
        HTTPResponse res;
        res.headers["Content-Type"] = "application/json";
        res.status = 404;
        res.body = R"({ "message": "Cannot find a route." })"sv;
        co_await res.write_to(loop, client_stream);
      } else {
        auto res = co_await r(req);
        co_await res.write_to(loop, client_stream);
        co_await res.write_to(loop, aout, "< "sv);
        co_await print(loop, aout, "\n"sv);
      }
    }
  }(server_sock, router);

  run_task(loop, task);
  task.result(); // Check exception.

  return 0;
}