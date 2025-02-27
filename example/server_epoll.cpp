#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <list>
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

void handle_request(struct sockaddr_in client_addr, socklen_t client_addr_len,
                    FileDescriptor client_sock /* to be moved */,
                    HTTPRouter &router) {
  using namespace std::literals;

  auto sock = FileStream(std::move(client_sock), "r+");

  char client_ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

  // TODO: req from string / FILE*
  HTTPRequest req;
  auto read_req = req.read_from(loop, client_stream);
  co_await read_req;

  assert(read_req.coro_.done());

  // Hope there's no I/O in any route handler.
  auto r = router.find_route(req.method, req.uri);
  HTTPResponse res;
  if (r == nullptr) {
    res.headers["Content-Type"] = "application/json";
    res.status = 404;
    res.body = R"({ "message": "Cannot find a route." })"sv;
  } else {
    auto h = r(req);
    h.coro_.resume();
    auto res = h.result();
    assert(h.coro_.done());
  }

  auto s = res.to_string();
  auto n = fputs(s.c_str(), sock.stream);
  if (n != s.size()) {
    THROW_SYSCALL("partial write");
  }
  fflush(sock.stream);
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

  // Guard the socket fd.
  FileDescriptor server_sock{server_socket};

  if (listen(server_socket, SOMAXCONN) < 0) {
    CHECK_SYSCALL(close(server_socket));
    THROW_SYSCALL("Failed to listen on socket");
  }

  std::cout << "Server is listening on port " << port << "...\n";

  /////////////////// Create the entrypoint task ///////////////////
  while (true) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    auto client_sock = socket_accept_sync(
        server_sock.fd, (struct sockaddr *)&client_addr, &client_addr_len);
    handle_request(client_addr, client_addr_len, std::move(client_sock),
                   router);
  }

  return 0;
}