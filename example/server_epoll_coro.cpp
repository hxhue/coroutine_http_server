#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cxxabi.h>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

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

inline coro::HTTPRouter create_router() {
  using namespace coro;
  using namespace std::literals;
  HTTPRouter router;
  router.route(HTTPMethod::GET, "/", [](HTTPRequest req) -> Task<HTTPResponse> {
    HTTPResponse res;
    res.status = 302;
    res.headers["Location"] = "/home"sv;
    co_return res;
  });
  router.route(HTTPMethod::GET, "/home"sv,
               [](HTTPRequest req) -> Task<HTTPResponse> {
                 HTTPResponse res;
                 res.status = 200;
                 res.headers["Content-Type"] = "text/html"sv;
                 res.body = "<h1>Hello, World!</h1>"sv;
                 co_return res;
               });
  router.route(
      HTTPMethod::GET, "/sleep"sv, [](HTTPRequest req) -> Task<HTTPResponse> {
        HTTPResponse res;
        res.status = 200;
        res.headers["Content-Type"] = "text/html"sv;
        auto uri = req.parse_uri();
        auto sec = std::stod(uri.params.at("seconds"));
        if (sec < 0) {
          throw std::runtime_error("Negative sleep duration is not allowed.\n" +
                                   SOURCE_LOCATION());
        }
        auto then = Clock::now() +
                    std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double, std::ratio<1, 1>>(sec));
        co_await sleep_until(loop, then);
        res.body = "<h1>Hello, World!</h1>"sv;
        co_return res;
      });
  router.route(HTTPMethod::GET, "/repeat"sv,
               [](HTTPRequest req) -> Task<HTTPResponse> {
                 HTTPResponse res;
                 res.status = 200;
                 res.headers["Content-Type"] = "text/html"sv;
                 auto uri = req.parse_uri();
                 auto cnt = std::stoll(uri.params.at("count"));
                 res.body.assign(cnt, '@');
                 co_return res;
               });
  return router;
}

Task<void> handle_request(struct sockaddr_in client_addr,
                          socklen_t client_addr_len, AsyncFile client_sock,
                          HTTPRouter &router) {
  using namespace std::literals;

  try {
    AsyncFileBuffer client_buffer{loop, std::move(client_sock)};

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

    HTTPRequest req;
    co_await req.read_from(loop, client_buffer);

    auto r = router.find_route(req.method, req.uri);
    if (r == nullptr) {
      HTTPResponse res;
      res.headers["Content-Type"] = "application/json";
      res.status = 404;
      res.body = R"({ "message": "Cannot find a route." })"sv;
      co_await res.write_to(loop, client_buffer);
    } else {
      auto res = co_await r(req);
      co_await res.write_to(loop, client_buffer);
    }
    co_await client_buffer.flush();
  } catch (EOFException &e) {
    // Ignore EOF.
  } catch (std::exception &e) {
    std::cerr << e.what() << "\n";
  }
}

std::vector<Task<>> spawned_tasks;

template <class T, class P> void spawn_task(Task<T, P> t) {
  for (size_t i = 0; i < spawned_tasks.size();) {
    if (spawned_tasks[i].coro_.done()) {
      std::swap(spawned_tasks[i], spawned_tasks.back());
      spawned_tasks.pop_back();
    } else {
      ++i;
    }
  }

  auto a = t.operator co_await();
  a.await_suspend(std::noop_coroutine()).resume();

  spawned_tasks.push_back(std::move(t));
}

int main() {
  using namespace std::literals;

  /////////////////// Set up routes ///////////////////
  HTTPRouter router = create_router();

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
    server_addr.sin_port = htons(port);

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

  if (listen(server_socket, SOMAXCONN) < 0) {
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

      auto client_sock = co_await socket_accept(
          loop, server_sock, (struct sockaddr *)&client_addr, &client_addr_len);

      // DEBUG() << std::format("client fd: {}, spawned count: {}\n",
      //                        client_sock.fd_, spawned_tasks.size());

      auto handler = handle_request(client_addr, client_addr_len,
                                    std::move(client_sock), router);

      spawn_task(std::move(handler));
    }
  }(server_sock, router);

  run_task(loop, task);
  task.result(); // Check exception.

  return 0;
}