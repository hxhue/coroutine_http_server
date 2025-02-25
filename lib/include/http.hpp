#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <ranges>
#include <span>
#include <sstream>
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
struct CaseInsensitiveLess {
  using is_transparent = void;

  bool operator()(std::string_view a, std::string_view b) const {
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

struct CaseInsensitiveHash {
  using is_transparent = void;

  std::size_t operator()(const std::string &s) const {
    std::size_t hash = 0;
    for (char ch : s) {
      hash = hash * 31 + std::tolower(static_cast<unsigned char>(ch));
    }
    return hash;
  }

  std::size_t operator()(std::string_view s) const {
    std::size_t hash = 0;
    for (char ch : s) {
      hash = hash * 31 + std::tolower(static_cast<unsigned char>(ch));
    }
    return hash;
  }
};
struct CaseInsensitiveEqual {
  using is_transparent = void;

  bool operator()(std::string_view a, std::string_view b) const {
    auto i = a.begin();
    auto j = b.begin();
    while (i != a.end() && j != b.end()) {
      auto c1 = std::tolower((unsigned char)(*i));
      auto c2 = std::tolower((unsigned char)(*j));
      if (c1 != c2) {
        return false;
      }
      ++i, ++j;
    }
    return i == a.end() && j == b.end();
  }
};

struct CaseSensitiveLess {
  using is_transparent = void;

  bool operator()(std::string_view a, std::string_view b) const {
    return a < b;
  }
};

struct CaseSensitiveEqual {
  using is_transparent = void;

  bool operator()(std::string_view a, std::string_view b) const {
    return a == b;
  }
};

struct CaseSensitiveHash {
  using is_transparent = void;

  std::size_t operator()(std::string_view a) const {
    return std::hash<std::string_view>{}(a);
  }
};
} // namespace detail

enum class HTTPMethod : std::uint32_t {
  INVALID = 0,
  GET = 0x1,
  POST = 0x2,
  PUT = 0x4,
  DELETE = 0x8,
  PATCH = 0x10,
  HEAD = 0x20,
  OPTIONS = 0x40,
  ANY = 0x80,
  VALID = 0x7f, // (m & VALID) == m && (m & (m-1)) == 0
};

inline HTTPMethod http_method(std::string const &method) {
  static const std::unordered_map<std::string, HTTPMethod,
                                  detail::CaseInsensitiveHash,
                                  detail::CaseInsensitiveEqual>
      method_map = {
          {"GET", HTTPMethod::GET},         {"POST", HTTPMethod::POST},
          {"PUT", HTTPMethod::PUT},         {"DELETE", HTTPMethod::DELETE},
          {"PATCH", HTTPMethod::PATCH},     {"HEAD", HTTPMethod::HEAD},
          {"OPTIONS", HTTPMethod::OPTIONS}, {"*", HTTPMethod::ANY}};

  auto it = method_map.find(method);
  if (it != method_map.end()) {
    return it->second;
  }
  return HTTPMethod::INVALID;
}

inline auto http_method_to_string(HTTPMethod method) {
  switch (method) {
  case HTTPMethod::GET:
    return "GET";
  case HTTPMethod::POST:
    return "POST";
  case HTTPMethod::PUT:
    return "PUT";
  case HTTPMethod::DELETE:
    return "DELETE";
  case HTTPMethod::PATCH:
    return "PATCH";
  case HTTPMethod::HEAD:
    return "HEAD";
  case HTTPMethod::OPTIONS:
    return "OPTIONS";
  case HTTPMethod::ANY:
    return "*";
  default:
    return "INVALID";
  }
}

inline HTTPMethod http_method(std::string_view method) {
  std::string s{method.data(), method.size()};
  return http_method(s);
}

inline bool valid_http_method(HTTPMethod method, bool allow_wildcard = false) {
  auto m = static_cast<std::uint32_t>(method);
  auto valid = static_cast<std::uint32_t>(HTTPMethod::VALID);
  return (m & ~valid) == 0 && m != 0 && (m & (m - 1)) == 0 ||
         (allow_wildcard && method == HTTPMethod::ANY);
}

using HTTPHeaders =
    std::map<std::string, std::string, detail::CaseInsensitiveLess>;
struct HTTPRequest {
  Task<> read_from(EpollScheduler &sched, AsyncFileStream &f) {
    using namespace std::literals;

    clear();

    auto line = co_await getline(sched, f, "\r\n"sv);
    while (!line.result.empty() && std::isspace(line.result.back())) {
      line.result.pop_back();
    }
    if (line.hup || !line.result.ends_with("HTTP/1.1"sv)) {
      throw std::runtime_error("invalid request: cannot find \"HTTP/1.1\"\n" +
                               SOURCE_LOCATION());
    }
    {
      std::stringstream ss(line.result);
      ss >> method >> uri;
    }
    if (http_method(method) == HTTPMethod::INVALID) {
      throw std::runtime_error("invalid http method: " + method + "\n" +
                               SOURCE_LOCATION());
    }

    // Headers.
    while (true) {
      auto line = co_await getline(sched, f, "\r\n"sv);
      if (line.hup) {
        throw std::runtime_error("invalid request: premature EOF\n" +
                                 SOURCE_LOCATION());
      }
      if (!headers.empty() && line.result.empty()) {
        break;
      }
      // The space after the colon is optional!
      // See https://datatracker.ietf.org/doc/html/rfc7230#section-3.2
      auto i = line.result.find(":"sv);
      if (i == std::string::npos) {
        throw std::runtime_error("invalid request: cannot find \":\"\n" +
                                 SOURCE_LOCATION());
      }
      auto field_name = line.result.substr(0, i);
      for (char ch : field_name) {
        // https://developers.cloudflare.com/rules/transform/request-header-modification/reference/header-format/
        if (!std::isalnum(ch) && !"_-"sv.contains(ch)) {
          throw std::runtime_error(
              "invalid request: get field name " + escape(field_name) +
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
        throw std::runtime_error("invalid request: empty field value\n" +
                                 SOURCE_LOCATION());
      }
      headers.insert_or_assign(field_name, field_value);
    }
    if (auto p = headers.find("content-length"); p != headers.end()) {
      auto len = std::stoi(p->second);
      body.resize(len);
      auto buf = std::span<char>(body.data(), body.size());
      auto res = co_await read(sched, f, buf);
      if (res.hup) {
        throw std::runtime_error("invalid request: premature EOF\n" +
                                 SOURCE_LOCATION());
      }
      assert(res.result == len);
    }
  }

  Task<> write_to(EpollScheduler &sched, AsyncFileStream &f) {
    using namespace std::literals;
    std::string s;
    s += method.empty() ? "<empty>"sv : method;
    s += " "sv;
    s += uri.empty() ? "<empty>"sv : uri;
    s += " HTTP/1.1\r\n"sv;
    for (auto const &[k, v] : headers) {
      if (detail::CaseInsensitiveEqual{}(k, "content-length")) {
        continue; // ignore the content length here
      }
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
    }
    // Send headers.
    auto res = co_await fputs(sched, f, s);
    if (res.hup) {
      THROW_SYSCALL("write-end hung up");
    }
    // Send body.
    res = co_await fputs(sched, f, body);
    if (res.hup) {
      THROW_SYSCALL("write-end hung up");
    }
    co_return;
  }

  auto to_tuple() const { return std::make_tuple(method, uri, headers, body); }

  struct ParsedURI {
    enum class TargetType {
      ORIGIN,    // e.g. /where?q=now
      ABSOLUTE,  // No params. e.g.
                 // http://www.example.org/pub/WWW/TheProject.html
      AUTHORITY, // Only for CONNECT. e.g. www.example.com:80
      ASTERISK,  // Only for server-side OPTIONS. e.g. *
      INVALID,
    };
    TargetType type;
    std::string path;
    std::unordered_map<std::string, std::string> params;

    static ParsedURI from(std::string_view s) {
      ParsedURI res;

      if (s.empty()) {
        res.type = TargetType::INVALID;
        return res;
      }

      // Check for ASTERISK
      if (s == "*") {
        res.type = TargetType::ASTERISK;
        return res;
      }

      // Check for AUTHORITY (used in CONNECT method)
      if (s.find("://") == std::string::npos &&
          s.find('/') == std::string::npos) {
        res.type = TargetType::AUTHORITY;
        res.path = s;
        return res;
      }

      // Check for ABSOLUTE (starts with scheme://)
      if (s.find("://") != std::string::npos) {
        res.type = TargetType::ABSOLUTE;
        res.path = s;
        return res;
      }

      // Otherwise, it's ORIGIN (path with optional query params)
      res.type = TargetType::ORIGIN;

      // Extract path and query params
      size_t query_start = s.find('?');
      if (query_start == std::string::npos) {
        res.path = s;
        return res;
      }

      res.path = s.substr(0, query_start);
      auto query_str = s;
      query_str.remove_prefix(query_start + 1);

      // Parse query params
      std::stringstream param_stream;
      param_stream << query_str;
      std::string pair;
      while (std::getline(param_stream, pair, '&')) {
        size_t equal_pos = pair.find('=');
        if (equal_pos != std::string::npos) {
          std::string key = pair.substr(0, equal_pos);
          std::string value = pair.substr(equal_pos + 1);
          res.params[key] = value;
        }
      }

      // There's a '?' so the params cannot be empty.
      if (res.params.empty()) {
        res.type = TargetType::INVALID;
        res.path.clear();
      }

      return res;
    }
  };

  ParsedURI parse_uri() const { return ParsedURI::from(uri); }

  void clear() {
    method.clear();
    uri.clear();
    headers.clear();
    body.clear();
  }

  std::string method;
  // Request Target: https://datatracker.ietf.org/doc/html/rfc7230#section-5.3
  std::string uri;
  HTTPHeaders headers;
  std::string body;
};

struct HTTPResponse {

  Task<> read_from(EpollScheduler &sched, AsyncFileStream &f) {
    using namespace std::literals;

    clear();

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

  Task<> write_to(EpollScheduler &sched, AsyncFileStream &f) {
    using namespace std::literals;
    std::string s;

    // Write the status line
    s += "HTTP/1.1 "sv;
    s += std::to_string(status);
    s += " "sv;
    s += status_message(status);
    s += "\r\n"sv;

    // Write the headers, excluding "content-length" if it exists
    for (auto const &[k, v] : headers) {
      if (detail::CaseInsensitiveEqual{}(k, "content-length")) {
        continue;
      }
      s += k;
      s += ": "sv;
      s += v;
      s += "\r\n"sv;
    }

    // Write the content-length header if there is a body
    if (!body.empty()) {
      s += "content-length: "sv;
      s += std::to_string(body.size());
      s += "\r\n"sv;
    }

    // End of headers
    s += "\r\n"sv;

    // Send headers
    auto res = co_await fputs(sched, f, s);
    if (res.hup) {
      THROW_SYSCALL("write-end hung up");
    }

    // Send body if it exists
    if (!body.empty()) {
      res = co_await fputs(sched, f, body);
      if (res.hup) {
        THROW_SYSCALL("write-end hung up");
      }
    }

    co_return;
  }

  auto to_tuple() const { return std::make_tuple(status, headers, body); }

  static std::string_view status_message(int status) {
    using namespace std::literals;
    switch (status) {
    case 200:
      return "OK"sv;
    case 404:
      return "Not Found"sv;
    case 500:
      return "Internal Server Error"sv;
    // ...
    default:
      return "Unknown"sv;
    }
  }

  void clear() {
    status = 0;
    headers.clear();
    body.clear();
  }

  int status;
  HTTPHeaders headers;
  std::string body;
};

using HTTPHandler = std::function<Task<HTTPResponse>(HTTPRequest)>;

struct HTTPRouter {
  struct Node {
    std::unordered_map<std::string, std::unique_ptr<Node>,
                       detail::CaseSensitiveHash, detail::CaseSensitiveEqual>
        children;
    std::unordered_map<HTTPMethod, HTTPHandler> handlers; // May be empty.
  };

  // Forward to the next route().
  void route(std::string_view method, std::string_view uri,
             HTTPHandler handler /* to be moved */) {
    using namespace std::literals;
    auto m = http_method(method);
    if (m == HTTPMethod::INVALID) {
      throw std::runtime_error("invalid HTTP method: "s + method.data() + "\n" +
                               SOURCE_LOCATION());
    }
    route(method, uri, std::move(handler));
  }

  void route(HTTPMethod method, std::string_view uri,
             HTTPHandler handler /* to be moved */) {
    using namespace std::literals;
    using TargetType = HTTPRequest::ParsedURI::TargetType;
    // Check method.
    if (!valid_http_method(method, true)) {
      std::runtime_error("method is not valid: "s +
                         http_method_to_string(method) + "\n" +
                         SOURCE_LOCATION());
    }
    // Check uri.
    if (!uri.starts_with('/')) {
      throw std::runtime_error("path should start with '/': "s + uri.data() +
                               "\n" + SOURCE_LOCATION());
    }
    // No such need: std::views::split handles this for us.
    // /a/b/ -> /a/b
    // if (uri.ends_with('/')) {
    //   uri.remove_suffix(1);
    // }
    auto parsed_uri = HTTPRequest::ParsedURI::from(uri);
    if (parsed_uri.type != TargetType::ORIGIN &&
        parsed_uri.type != TargetType::ASTERISK) {
      throw std::runtime_error("invalid path: "s + uri.data() + "\n" +
                               SOURCE_LOCATION());
    }
    if (!parsed_uri.params.empty()) {
      throw std::runtime_error("route entry cannot contain params: "s +
                               uri.data() + "\n" + SOURCE_LOCATION());
    }
    // Check handler.
    if (handler == nullptr) {
      throw std::runtime_error("handler cannot be empty!\n" +
                               SOURCE_LOCATION());
    }
    // Insert the record.
    // DEBUG() << "inserting";
    std::reference_wrapper<std::unique_ptr<Node>> cur = root;
    for (auto com : std::views::split(uri, "/"sv)) {
      auto sv = std::string_view{com};
      if (sv.empty()) {
        continue;
      }
      // DEBUG() << " " << sv;
      auto it = cur.get()->children.find(sv);
      if (it == cur.get()->children.end()) {
        auto [it2, did_insert] = cur.get()->children.insert_or_assign(
            std::string{sv.data(), sv.size()}, std::make_unique<Node>());
        it = it2;
      }
      cur = it->second;
    }
    // DEBUG() << "\n";
    cur.get()->handlers[method] = std::move(handler);
  }

  HTTPHandler find_route(std::string_view method, std::string_view uri) const {
    return find_route(http_method(method), uri);
  }

  HTTPHandler find_route(HTTPMethod method, std::string_view uri) const {
    using namespace std::literals;
    // Check method.
    if (!valid_http_method(method)) {
      std::runtime_error("method is not valid: "s +
                         http_method_to_string(method) + "\n" +
                         SOURCE_LOCATION());
    }
    // Check uri.
    if (!uri.starts_with('/')) {
      throw std::runtime_error("path should start with '/': "s + uri.data() +
                               "\n" + SOURCE_LOCATION());
    }
    // Remove ?param=value
    if (auto pos = uri.find('?')) {
      uri = uri.substr(0, pos);
    }
    std::reference_wrapper<const std::unique_ptr<Node>> cur = root;
    HTTPHandler h = nullptr;
    // Try root.
    auto jt = cur.get()->handlers.find(method);
    if (jt == cur.get()->handlers.end()) {
      jt = cur.get()->handlers.find(HTTPMethod::ANY);
    }
    if (jt != cur.get()->handlers.end()) {
      h = jt->second; // Longest possible match.
    }
    // Try longer components.
    for (auto com : std::views::split(uri, "/"sv)) {
      auto sv = std::string_view{com};
      if (sv.empty()) {
        continue;
      }
      auto it = cur.get()->children.find(sv);
      if (it == cur.get()->children.end()) {
        break;
      }
      cur = it->second;
      auto jt = cur.get()->handlers.find(method);
      if (jt == cur.get()->handlers.end()) {
        jt = cur.get()->handlers.find(HTTPMethod::ANY);
      }
      if (jt != cur.get()->handlers.end()) {
        h = jt->second; // Longest possible match.
      }
    }
    return h;
  }

  std::unique_ptr<Node> root = std::make_unique<Node>();
};

} // namespace coro