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
                                  cmp::CaseInsensitiveHash,
                                  cmp::CaseInsensitiveEqual>
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
    std::map<std::string, std::string, cmp::CaseInsensitiveLess>;

struct HTTPHeaderBody {
  static Task<> read_from(EpollScheduler &sched, AsyncFileStream &f,
                          HTTPHeaders &headers, std::string &body) {
    using namespace std::literals;

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
    if (auto p = headers.find("Content-Length"); p != headers.end()) {
      auto len = std::stoi(p->second);
      body.resize(len);
      auto buf = std::span<char>(body.data(), body.size());
      auto res = co_await read_buffer(sched, f, buf);
      if (res.hup) {
        throw std::runtime_error("invalid response: premature EOF\n" +
                                 SOURCE_LOCATION());
      }
      assert(res.result == len);
    }
  }

  static Task<> write_to(EpollScheduler &sched, AsyncFileStream &f,
                         HTTPHeaders const &headers, std::string const &body,
                         std::string_view line_start = "") {
    using namespace std::literals;

    std::string s;

    // Write the headers, excluding "Content-Length" if it exists
    for (auto const &[k, v] : headers) {
      if (cmp::CaseInsensitiveEqual{}(k, "Content-Length")) {
        continue;
      }
      s += line_start;
      s += k;
      s += ": "sv;
      s += v;
      s += "\r\n"sv;
    }

    // Write the Content-Length header if there is a body
    if (!body.empty()) {
      s += line_start;
      s += "Content-Length: "sv;
      s += std::to_string(body.size());
      s += "\r\n"sv;
    }

    // End of headers
    s += line_start;
    s += "\r\n"sv;

    // Send headers
    auto res = co_await print(sched, f, s);
    if (res.hup) {
      THROW_SYSCALL("write-end hung up");
    }

    // Send body if it exists
    if (!body.empty()) {
      if (line_start == ""sv) {
        res = co_await print(sched, f, body);
        if (res.hup) {
          THROW_SYSCALL("write-end hung up");
        }
      } else {
        // TODO: is there a problem?
        for (auto line : std::views::split(body, '\n')) {
          auto sv = std::string_view{line};
          res = co_await print(sched, f, sv);
          if (res.hup) {
            THROW_SYSCALL("write-end hung up");
          }
          res = co_await print(sched, f, "\n"sv);
          if (res.hup) {
            THROW_SYSCALL("write-end hung up");
          }
        }
      }
    }
  }
};
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

    co_await HTTPHeaderBody::read_from(sched, f, headers, body);
  }

  Task<> write_to(EpollScheduler &sched, AsyncFileStream &f,
                  std::string_view line_start = "") const {
    using namespace std::literals;
    std::string s;
    s += line_start;
    s += method.empty() ? "<empty>"sv : method;
    s += " "sv;
    s += uri.empty() ? "<empty>"sv : uri;
    s += " HTTP/1.1\r\n"sv;
    co_await print(sched, f, s);
    co_await HTTPHeaderBody::write_to(sched, f, headers, body, line_start);
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
  std::string uri; // Request Target:
                   // https://datatracker.ietf.org/doc/html/rfc7230#section-5.3

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

    co_await HTTPHeaderBody::read_from(sched, f, headers, body);
  }

  Task<> write_to(EpollScheduler &sched, AsyncFileStream &f,
                  std::string_view line_start = "") const {
    using namespace std::literals;
    co_await print(sched, f,
                   std::format("{}HTTP/1.1 {} {}\r\n", line_start, status,
                               status_message(status)));
    co_await HTTPHeaderBody::write_to(sched, f, headers, body, line_start);
  }

  auto to_tuple() const { return std::make_tuple(status, headers, body); }

  void clear() {
    status = 0;
    headers.clear();
    body.clear();
  }

  static std::string status_message(int status);

  int status;
  HTTPHeaders headers;
  std::string body;
};

using HTTPHandler = std::function<Task<HTTPResponse>(HTTPRequest)>;

struct HTTPRouter {
  struct Node {
    std::unordered_map<std::string, std::unique_ptr<Node>,
                       cmp::CaseSensitiveHash, cmp::CaseSensitiveEqual>
        children;
    std::unordered_map<HTTPMethod, HTTPHandler> handlers; // May be empty.
  };

  void route(std::string_view method, std::string_view uri,
             HTTPHandler const &handler) {
    using namespace std::literals;
    auto m = http_method(method);
    if (m == HTTPMethod::INVALID) {
      throw std::runtime_error("invalid HTTP method: "s + method.data() + "\n" +
                               SOURCE_LOCATION());
    }
    route(method, uri, handler);
  }

  void route(HTTPMethod method, std::string_view uri,
             HTTPHandler const &handler) {
    if (!uri.starts_with('/')) {
      throw std::runtime_error(std::format(
          "uri does not start with /: uri: {}\n{}", uri, SOURCE_LOCATION()));
    }
    // Remove ?param=value.
    if (auto pos = uri.find('?'); pos != std::string_view::npos) {
      uri = uri.substr(0, pos);
    }
    // //a/b// -> /a/b/
    std::string s;
    char last = '\0';
    for (char ch : uri) {
      if (last == '/' && ch == '/') {
        continue;
      }
      s += ch;
      last = ch;
    }
    exact_matches[s][method] = handler;
  }

  void route_prefix(std::string_view method, std::string_view uri,
                    HTTPHandler const &handler) {
    using namespace std::literals;
    auto m = http_method(method);
    if (m == HTTPMethod::INVALID) {
      throw std::runtime_error("invalid HTTP method: "s + method.data() + "\n" +
                               SOURCE_LOCATION());
    }
    route_prefix(method, uri, handler);
  }

  void route_prefix(HTTPMethod method, std::string_view uri,
                    HTTPHandler const &handler) {
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
    auto parsed_uri = HTTPRequest::ParsedURI::from(uri);
    // TODO: asterisk
    if (parsed_uri.type != TargetType::ORIGIN) {
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
    std::reference_wrapper<std::unique_ptr<Node>> cur = trie;
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
    cur.get()->handlers[method] = handler;
  }

  HTTPHandler find_route_exact(HTTPMethod m, std::string_view uri) const {
    // Remove ?param=value.
    if (auto pos = uri.find('?'); pos != std::string_view::npos) {
      uri = uri.substr(0, pos);
    }
    // Build key.
    std::string s;
    char last = '\0';
    for (char ch : uri) {
      if (last == '/' && ch == '/') {
        continue;
      }
      s += ch;
      last = ch;
    }
    // Search.
    auto it = exact_matches.find(uri);
    if (it == exact_matches.end()) {
      return nullptr;
    }
    auto jt = it->second.find(m);
    if (jt == it->second.end()) {
      jt = it->second.find(HTTPMethod::ANY);
    }
    if (jt == it->second.end()) {
      return nullptr;
    }
    return jt->second;
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
    // Try exact match first.
    HTTPHandler h = find_route_exact(method, uri);
    if (h) {
      return h;
    }
    if (!uri.ends_with('/')) {
      std::string uri2;
      uri2 = uri;
      uri2 += '/';
      h = find_route_exact(method, uri2);
    }
    if (h) {
      return h;
    }
    // Try prefix match.
    std::reference_wrapper<const std::unique_ptr<Node>> cur = trie;
    auto jt = cur.get()->handlers.find(method);
    if (jt == cur.get()->handlers.end()) {
      jt = cur.get()->handlers.find(HTTPMethod::ANY);
    }
    if (jt != cur.get()->handlers.end()) {
      h = jt->second;
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
      // The same logic also applies to the root of trie.
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

  std::unique_ptr<Node> trie = std::make_unique<Node>();
  std::unordered_map<std::string, std::unordered_map<HTTPMethod, HTTPHandler>,
                     cmp::CaseSensitiveHash, cmp::CaseSensitiveEqual>
      exact_matches;
};

} // namespace coro