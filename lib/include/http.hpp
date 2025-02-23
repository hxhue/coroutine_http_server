#pragma once

#include <map>
#include <string>
#include <string_view>
#include <tuple>

#include "task.hpp"

namespace coro {
struct HTTPRequest {
  std::string method;
  std::string uri;
  std::map<std::string, std::string> headers;
  std::string body;

  template <typename T> Task<> write_into(T &sock) {
    using namespace std::string_view_literals;
    co_await sock.puts(method);
    co_await sock.putchar(' ');
    co_await sock.puts(uri);
    co_await sock.puts(" HTTP/1.1\r\n"sv);
    for (auto const &[k, v] : headers) {
      co_await sock.puts(k);
      co_await sock.puts(": "sv);
      co_await sock.puts(v);
      co_await sock.puts("\r\n"sv);
    }
    if (body.empty()) {
      co_await sock.puts("\r\n"sv);
    } else {
      co_await sock.puts("content-length: "sv);
      co_await sock.puts(std::to_string(body.size()));
      co_await sock.puts("\r\n"sv);
      co_await sock.puts(body);
    }
  }

  auto to_tuple() const { return std::make_tuple(method, uri, headers, body); }
};
} // namespace coro