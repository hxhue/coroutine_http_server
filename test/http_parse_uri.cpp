#include <gtest/gtest.h>

#include "http.hpp"

TEST(HTTPTest, ParseURI) {
  using URI = coro::HTTPRequest::ParsedURI;
  using enum URI::TargetType;

  std::string target1 = "/where?q=now&lang=en";
  std::string target2 = "http://www.example.org/pub/WWW/TheProject.html";
  std::string target3 = "www.example.com:80";
  std::string target4 = "*";
  std::string target5 = "";        // invalid
  std::string target6 = "/where?"; // Also invalid

  URI parsed1 = URI::from(target1);
  URI parsed2 = URI::from(target2);
  URI parsed3 = URI::from(target3);
  URI parsed4 = URI::from(target4);
  URI parsed5 = URI::from(target5);
  URI parsed6 = URI::from(target6);

  // Test Target 1 (ORIGIN)
  EXPECT_EQ(parsed1.type, ORIGIN);
  EXPECT_EQ(parsed1.path, "/where");
  EXPECT_EQ(parsed1.params.size(), 2);
  EXPECT_EQ(parsed1.params["q"], "now");
  EXPECT_EQ(parsed1.params["lang"], "en");

  // Test Target 2 (ABSOLUTE)
  EXPECT_EQ(parsed2.type, ABSOLUTE);
  EXPECT_EQ(parsed2.path, "http://www.example.org/pub/WWW/TheProject.html");
  EXPECT_EQ(parsed2.params, decltype(parsed2.params){});

  // Test Target 3 (AUTHORITY)
  EXPECT_EQ(parsed3.type, AUTHORITY);
  EXPECT_EQ(parsed3.path, "www.example.com:80");
  EXPECT_TRUE(parsed3.params.empty());

  // Test Target 4 (ASTERISK)
  EXPECT_EQ(parsed4.type, ASTERISK);
  EXPECT_EQ(parsed4.path, "");
  EXPECT_TRUE(parsed4.params.empty());

  // Test invalid target
  EXPECT_EQ(parsed5.type, INVALID);
  EXPECT_TRUE(parsed5.path.empty());
  EXPECT_TRUE(parsed5.params.empty());

  // Test Target 6
  EXPECT_EQ(parsed6.type, INVALID);
  EXPECT_TRUE(parsed6.path.empty()) << "parsed6.path is " << parsed6.path;
  EXPECT_TRUE(parsed6.params.empty());
}
