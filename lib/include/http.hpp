#pragma once

#include <cstddef>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

#include "aio.hpp"
#include "epoll.hpp"
#include "task.hpp"
#include "utility.hpp"

namespace coro {

namespace detail {
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
} // namespace detail

using HTTPHeaders =
    std::map<std::string, std::string, detail::CaseInsensitiveCompare>;
struct HTTPRequest {

  Task<> write_to(EpollScheduler &sched, AsyncFileStream &f) {
    using namespace std::string_view_literals;
    std::string s;
    s += method;
    s += " "sv;
    s += path;
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
    co_return;
  }

  auto to_tuple() const { return std::make_tuple(method, path, headers, body); }

  std::string method;
  std::string path;
  HTTPHeaders headers;
  std::string body;
};

struct HTTPResponse {
  Task<> read_from(EpollScheduler &sched, AsyncFileStream &f) {
    using namespace std::string_view_literals;

    status = 0;
    headers.clear();
    body.clear();

    auto line = co_await getline(sched, f, "\r\n"sv);
    if (line.hup || !line.result.starts_with("HTTP/1.1 "sv)) {
      throw std::runtime_error("invalid response: cannot find \"HTTP/1.1\"\n" +
                               SOURCE_LOCATION());
    }
    status = std::stoi(line.result.substr("HTTP/1.1 "sv.size()));
    while (true) {
      auto line = co_await getline(sched, f, "\r\n"sv);
      if (line.hup) {
        throw std::runtime_error("invalid response: premature EOF\n" +
                                 SOURCE_LOCATION());
      }
      if (!headers.empty() && line.result.empty()) {
        break;
      }
      // The space after the colon is optional!
      // See https://datatracker.ietf.org/doc/html/rfc7230#section-3.2
      auto i = line.result.find(":"sv);
      if (i == std::string::npos) {
        throw std::runtime_error("invalid response: cannot find \":\"\n" +
                                 SOURCE_LOCATION());
      }
      auto field_name = line.result.substr(0, i);
      for (char ch : field_name) {
        // https://developers.cloudflare.com/rules/transform/request-header-modification/reference/header-format/
        if (!std::isalnum(ch) && !"_-"sv.contains(ch)) {
          throw std::runtime_error(
              "invalid response: get field name " + escape(field_name) +
              " and it contains illegal characters!\n" + SOURCE_LOCATION());
        }
      }
      std::size_t j = i + 1;
      while (j < line.result.size() && std::isspace(line.result[j])) {
        ++j;
      }
      // Ok as long as j <= line.result.size().
      auto field_value = line.result.substr(j);
      while (!field_value.empty() && std::isspace(field_value.back())) {
        field_value.pop_back();
      }
      if (field_value.empty()) {
        throw std::runtime_error("invalid response: empty field value\n" +
                                 SOURCE_LOCATION());
      }
      for (char ch : field_value) {
        // https://developers.cloudflare.com/rules/transform/request-header-modification/reference/header-format/
        //
        // 2025/2/24: I don't know why it's wrong. Maybe do not check the
        // characters for now.
        //
        // if (!std::isalnum(ch) &&
        //     !R"(_ :;.,\/"'?!(){}[]@<>=-+*#$&`|~^%)"sv.contains(ch)) {
        //   throw std::runtime_error(
        //       "invalid response: get field value " + escape(field_value) +
        //       " for field " + escape(field_name) +
        //       ", and it contains illegal characters!\n" + SOURCE_LOCATION());
        // }
      }
      // - emplace does not overwrite an existing record.
      // - operator[] requires the value to be default-constructible.
      // - insert_or_assign overwrites an existing record.
      headers.insert_or_assign(field_name, field_value);
    }
    if (auto p = headers.find("content-length"); p != headers.end()) {
      auto len = std::stoi(p->second);
      body.resize(len);
      auto buf = std::span<char>(body.data(), body.size());
      auto res = co_await read(sched, f, buf);
      if (res.hup) {
        throw std::runtime_error("invalid response: premature EOF\n" +
                                 SOURCE_LOCATION());
      }
      assert(res.result == len);
    }
  }

  auto to_tuple() const { return std::make_tuple(status, headers, body); }

  int status;
  HTTPHeaders headers;
  std::string body;
};

} // namespace coro