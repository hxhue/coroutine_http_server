#pragma once

#include <map>
#include <string>
#include <string_view>
#include <tuple>

#include "aio.hpp"
#include "epoll.hpp"
#include "task.hpp"

namespace coro {
struct HTTPRequest {
  struct CaseInsensitiveCompare {
    bool operator()(std::string const &a, std::string const &b) const {
      auto i = a.begin();
      auto j = b.begin();
      while (i != a.end() && j != b.end()) {
        auto c1 = std::tolower((unsigned char)(*i));
        auto c2 = std::tolower((unsigned char)(*j));
        if (c1 != c2) {
          return c1 < c2;
        }
        ++i, ++j;
      }
      return i == a.end() && j != b.end();
    }
  };

  Task<> write(EpollScheduler &sched, AsyncFileStream &f) {
    using namespace std::string_view_literals;
    co_await fputs(sched, f, method);
    co_await fputs(sched, f, method);
    co_await fputs(sched, f, " "sv);
    co_await fputs(sched, f, uri);
    co_await fputs(sched, f, " HTTP/1.1\r\n"sv);
    for (auto const &[k, v] : headers) {
      co_await fputs(sched, f, k);
      co_await fputs(sched, f, ": "sv);
      co_await fputs(sched, f, v);
      co_await fputs(sched, f, "\r\n"sv);
    }
    if (body.empty()) {
      co_await fputs(sched, f, "\r\n"sv);
    } else {
      co_await fputs(sched, f, "content-length: "sv);
      co_await fputs(sched, f, std::to_string(body.size()));
      co_await fputs(sched, f, "\r\n"sv);
      co_await fputs(sched, f, body);
    }
    co_return;
  }

  auto to_tuple() const { return std::make_tuple(method, uri, headers, body); }

  std::string method;
  std::string uri;
  std::map<std::string, std::string, CaseInsensitiveCompare> headers;
  std::string body;
};
} // namespace coro