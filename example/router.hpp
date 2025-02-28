#pragma once

#include "http.hpp"
#include <string>

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