#pragma once

#include <cassert>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

inline auto check_syscall(int ret, const char *file, int line, const char *pf,
                          const char *expr) {
  if (ret == -1) {
    int err = errno;
    std::string s = "[" + std::to_string(err) + "] " + strerrorname_np(err);
    s += "\n\nLocation: ";
    s += file;
    s += ":";
    s += std::to_string(line);
    s += "\nFunction: ";
    s += pf;
    s += "\nExpresion: ";
    s += expr;
    s += "\n";
    throw std::runtime_error(s);
  }
  return ret;
}

#define CHECK_SYSCALL(expr)                                                    \
  check_syscall((expr), __FILE__, __LINE__, __PRETTY_FUNCTION__, #expr)

// https://stackoverflow.com/a/44989052/
inline auto escape(std::string_view sv) {
  // s is our escaped output string
  std::string s;
  // loop through all characters
  for (char c : sv) {
    // check if a given character is printable
    // the cast is necessary to avoid undefined behaviour
    if (isprint((unsigned char)c))
      s += c;
    else {
      std::stringstream stream;
      // if the character is not printable
      // we'll convert it to a hex string using a stringstream
      // note that since char is signed we have to cast it to unsigned first
      stream << std::hex << (unsigned int)(unsigned char)(c);
      std::string code = stream.str();
      s += std::string("\\x") + (code.size() < 2 ? "0" : "") + code;
      // alternatively for URL encodings:
      // s += std::string("%")+(code.size()<2?"0":"")+code;
    }
  }
  return s;
}

inline auto escape(char ch) { return escape(std::string_view{&ch, 1}); }

// clang-format off
#ifdef NDEBUG
#define LOG_DEBUG if constexpr (false) std::cerr
#else
#define LOG_DEBUG if constexpr (true) std::cerr
#endif
#define LOG(level) LOG_##level
#define DEBUG() LOG(DEBUG)
// clang-format on

template <typename Func> class Defer {
public:
  Defer(Func func) : callback(std::move(func)) {}

  ~Defer() { callback(); }

private:
  Func callback;
};