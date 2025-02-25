#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
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
  auto client = AsyncFileStream(create_tcp_socket(saddr), "r+");
  co_await socket_connect(loop, client, saddr);

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
  // auto task = amain();
  // run_task(loop, task);
  // task.result();

  HTTPRouter r;
  r.route(HTTPMethod::GET, "/home/sa/", [](HTTPRequest req) -> Task<HTTPResponse> {
    HTTPResponse res;
    res.status = 200;
    res.headers["Content-Type"] = "text/plain";
    res.body = "Hello, World!";
    co_return res;
  });
  // TODO: an http server that can route
}