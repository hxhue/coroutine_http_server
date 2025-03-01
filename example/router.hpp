#pragma once

#include "http.hpp"
#include "task.hpp"
#include <chrono>

coro::Task<> sleep_until(coro::Clock::time_point then);

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
  // Simulate a time-consuming task.
  // e.g. /sleep?ms=1.5
  router.route(
      HTTPMethod::GET, "/sleep"sv, [](HTTPRequest req) -> Task<HTTPResponse> {
        HTTPResponse res;
        res.status = 200;
        res.headers["Content-Type"] = "text/html"sv;
        auto uri = req.parse_uri();
        auto ms = std::stod(uri.params.at("ms"));
        if (ms < 0) {
          throw std::runtime_error("Negative sleep duration is not allowed.\n" +
                                   SOURCE_LOCATION());
        }
        if (ms > 0) {
          auto now = Clock::now();
          auto duration = std::chrono::duration_cast<Clock::duration>(
              std::chrono::duration<double, std::ratio<1, 1000>>(ms));
          auto then = now + duration;
          co_await sleep_until(then);
        }
        res.body = "<h1>Hello, World!</h1>"sv;
        co_return res;
      });
  // Simulate a output-heavy task.
  // e.g. /repeat?count=10000
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