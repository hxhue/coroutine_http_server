#include <gtest/gtest.h>

#include "http.hpp"

TEST(HTTPRoutePrefixTest, SimpleRoute) {
  using namespace coro;
  HTTPRouter router;

  router.route_prefix(HTTPMethod::GET, "/hello",
                      [](HTTPRequest) -> Task<HTTPResponse> {
                        HTTPResponse res{
                            .status = 200,
                            .headers =
                                {
                                    {"Content-Type", "text/html"},
                                },
                            .body = R"(<h1>Hello, world!</h1>)",
                        };
                        co_return res;
                      });

  auto handler = router.find_route(HTTPMethod::GET, "/hello");
  ASSERT_NE(handler, nullptr);

  auto h = handler(HTTPRequest{});
  h.coro_.resume();
  auto response = h.result();

  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.headers.at("Content-Type"), "text/html");
  EXPECT_EQ(response.body, R"(<h1>Hello, world!</h1>)");
}

TEST(HTTPRoutePrefixTest, TwoRoutes) {
  using namespace coro;
  HTTPRouter router;

  auto f1 = [](HTTPRequest) -> Task<HTTPResponse> { co_return HTTPResponse{}; };
  auto f2 = [](HTTPRequest) -> Task<HTTPResponse> { co_return HTTPResponse{}; };

  router.route_prefix(HTTPMethod::GET, "/hello", f1);
  router.route_prefix(HTTPMethod::POST, "/hello/world", f2);

  // 测试 GET /hello
  auto handler1 = router.find_route(HTTPMethod::GET, "/hello");
  ASSERT_TRUE(handler1.target<decltype(f1)>());

  // 测试 POST /hello/world
  auto handler2 = router.find_route(HTTPMethod::POST, "/hello/world");
  ASSERT_TRUE(handler2.target<decltype(f2)>());

  // 测试 GET /hello/world (未注册)
  auto handler3 = router.find_route(HTTPMethod::GET, "/hello/world");
  ASSERT_TRUE(handler3.target<decltype(f1)>());

  // 测试 POST /hello (未注册)
  auto handler4 = router.find_route(HTTPMethod::POST, "/hello");
  ASSERT_EQ(handler4, nullptr);
}

TEST(HTTPRoutePrefixTest, RootRoute) {
  using namespace coro;
  HTTPRouter router;

  auto f1 = [](HTTPRequest) -> Task<HTTPResponse> { co_return HTTPResponse{}; };
  auto f2 = [](HTTPRequest) -> Task<HTTPResponse> { co_return HTTPResponse{}; };

  router.route_prefix(HTTPMethod::ANY, "/", f1);
  router.route_prefix(HTTPMethod::GET, "/hello", f2);

  auto handler1 = router.find_route(HTTPMethod::GET, "/hello");
  ASSERT_TRUE(handler1.target<decltype(f2)>());

  auto handler2 = router.find_route(HTTPMethod::POST, "/hello/world");
  ASSERT_TRUE(handler2.target<decltype(f1)>());
}

TEST(HTTPRoutePrefixTest, AnyMethodRoute) {
  using namespace coro;
  HTTPRouter router;

  auto f1 = [](HTTPRequest) -> Task<HTTPResponse> { co_return HTTPResponse{}; };
  auto f2 = [](HTTPRequest) -> Task<HTTPResponse> { co_return HTTPResponse{}; };
  auto f3 = [](HTTPRequest) -> Task<HTTPResponse> { co_return HTTPResponse{}; };

  router.route_prefix(HTTPMethod::ANY, "/hello", f1);
  router.route_prefix(HTTPMethod::GET, "/hello", f2);
  router.route_prefix(HTTPMethod::ANY, "/hello/tom", f3);

  // 测试 GET /hello
  auto handler1 = router.find_route(HTTPMethod::GET, "/hello");
  // An exact match has a higher priority.
  ASSERT_TRUE(handler1.target<decltype(f2)>());

  // 测试 POST /hello
  auto handler2 = router.find_route(HTTPMethod::POST, "/hello");
  ASSERT_TRUE(handler2.target<decltype(f1)>());

  // 测试 DELETE /hello
  auto handler3 = router.find_route(HTTPMethod::DELETE, "/hello");
  ASSERT_TRUE(handler3.target<decltype(f1)>());

  // Path takes precedence over method.
  auto handler4 = router.find_route(HTTPMethod::GET, "/hello/tom");
  ASSERT_TRUE(handler4.target<decltype(f3)>());

  // Falls back to "GET /hello".
  auto handler5 = router.find_route(HTTPMethod::GET, "/hello/alice");
  ASSERT_TRUE(handler5.target<decltype(f2)>());
}

TEST(HTTPRoutePrefixTest, QueryParametersRoute) {
  using namespace coro;
  HTTPRouter router;

  auto f1 = [](HTTPRequest) -> Task<HTTPResponse> { co_return HTTPResponse{}; };

  router.route_prefix(HTTPMethod::GET, "/hello/tom", f1);

  // 测试 GET /hello/tom?from=alice
  auto handler1 = router.find_route(HTTPMethod::GET, "/hello/tom?from=alice");
  ASSERT_TRUE(handler1.target<decltype(f1)>());

  // 测试 GET /hello/tom (没有查询参数)
  auto handler2 = router.find_route(HTTPMethod::GET, "/hello/tom");
  ASSERT_TRUE(handler2.target<decltype(f1)>());

  // 测试 GET /hello/tom/from/alice (未注册)
  auto handler3 = router.find_route(HTTPMethod::GET, "/hello/tom/from/alice");
  ASSERT_TRUE(handler2.target<decltype(f1)>());
}

TEST(HTTPRoutePrefixTest, PrefixMatchingRoute) {
  using namespace coro;
  HTTPRouter router;

  auto f1 = [](HTTPRequest) -> Task<HTTPResponse> { co_return HTTPResponse{}; };
  auto f2 = [](HTTPRequest) -> Task<HTTPResponse> { co_return HTTPResponse{}; };

  router.route_prefix(HTTPMethod::GET, "/hello", f1);
  router.route_prefix(HTTPMethod::GET, "/hello/world", f2);

  // 测试 GET /hello/world (具体匹配)
  auto handler1 = router.find_route(HTTPMethod::GET, "/hello/world");
  ASSERT_NE(handler1, nullptr);

  // 测试 GET /hello/tom (前缀匹配)
  auto handler2 = router.find_route(HTTPMethod::GET, "/hello/tom");
  ASSERT_NE(handler2, nullptr);

  // 测试 GET /hello/world/tom (前缀匹配)
  auto handler3 = router.find_route(HTTPMethod::GET, "/hello/world/tom");
  ASSERT_NE(handler3, nullptr);

  // 测试 GET /hi (未注册)
  auto handler4 = router.find_route(HTTPMethod::GET, "/hi");
  ASSERT_EQ(handler4, nullptr);
}

TEST(HTTPRoutePrefixTest, NoRouteFound) {
  using namespace coro;
  HTTPRouter router;

  auto f1 = [](HTTPRequest) -> Task<HTTPResponse> { co_return HTTPResponse{}; };

  router.route_prefix(HTTPMethod::GET, "/hello", f1);

  // 测试 GET /hi (未注册)
  auto handler1 = router.find_route(HTTPMethod::GET, "/hi");
  ASSERT_EQ(handler1, nullptr);

  // 测试 POST /hello (未注册)
  auto handler2 = router.find_route(HTTPMethod::POST, "/hello");
  ASSERT_EQ(handler2, nullptr);

  // 测试 POST /hello/world (未注册)
  auto handler3 = router.find_route(HTTPMethod::POST, "/hello/world");
  ASSERT_EQ(handler3, nullptr);
}

TEST(HTTPRouteTest, ExactMatch) {
  using namespace coro;
  HTTPRouter router;

  auto f1 = [](HTTPRequest) -> Task<HTTPResponse> { co_return HTTPResponse{}; };

  router.route(HTTPMethod::GET, "/hello", f1);

  // 测试 GET /hello/world (未注册)
  auto handler3 = router.find_route(HTTPMethod::GET, "/hello/world");
  ASSERT_EQ(handler3, nullptr);

  router.route_prefix(HTTPMethod::ANY, "/hello", f1);

  // Now "/hello/world" is registered.
  auto handler4 = router.find_route(HTTPMethod::GET, "/hello/world");
  ASSERT_TRUE(handler4.target<decltype(f1)>());
}

TEST(HTTPRouteTest, ExactMatchHappensFirst) {
  using namespace coro;
  using namespace std::literals;

  HTTPRouter router;
  auto f1 = [](HTTPRequest req) -> Task<HTTPResponse> {
    HTTPResponse res;
    res.status = 302;
    res.headers["Location"] = "/home"sv;
    co_return res;
  };
  auto f2 = [](HTTPRequest req) -> Task<HTTPResponse> {
    HTTPResponse res;
    res.status = 200;
    res.headers["Content-Type"] = "text/html"sv;
    res.body = "<h1>Hello, World!</h1>"sv;
    co_return res;
  };
  auto f3 = [](HTTPRequest req) -> Task<HTTPResponse> {
    HTTPResponse res;
    res.status = 404;
    res.body = "<h1>The page you requested is not found!</h1>"sv;
    co_return res;
  };

  router.route(HTTPMethod::GET, "/", f1);
  router.route(HTTPMethod::GET, "/home/"sv, f2);
  router.route_prefix(HTTPMethod::GET, "/", f3);

  auto h1 = router.find_route("GET", "/");
  auto h2 = router.find_route("GET", "/home");
  auto h3 = router.find_route("GET", "/not-found");

  ASSERT_TRUE(h1.target<decltype(f1)>());
  ASSERT_TRUE(h2.target<decltype(f2)>());
  ASSERT_TRUE(h3.target<decltype(f3)>());
}