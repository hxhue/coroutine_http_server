#pragma once

#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>

#include "aio.hpp"
#include "epoll.hpp"
#include "task.hpp"
#include "utility.hpp"

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
    std::string s;
    s += method;
    s += " "sv;
    s += uri;
    s += " HTTP/1.1\r\n"sv;
    for (auto const &[k, v] : headers) {
      s += k;
      s += ": "sv;
      s += v;
      s += "\r\n"sv;
    }
    if (body.empty()) {
      s += "\r\n"sv;
    } else {
      s += "content-length: "sv;
      s += std::to_string(body.size());
      s += "\r\n"sv;
      s += body;
    }
    auto res = co_await fputs(sched, f, s);
    if (res.hup) {
      THROW_SYSCALL("write-end hung up");
    }
    fflush(f);
    co_return;
  }

  auto to_tuple() const { return std::make_tuple(method, uri, headers, body); }

  std::string method;
  std::string uri;
  std::map<std::string, std::string, CaseInsensitiveCompare> headers;
  std::string body;
};
} // namespace coro