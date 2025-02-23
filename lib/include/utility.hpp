#pragma once

#include <cassert>
#include <cerrno>
#include <cstring>
#include <iostream> // for LOG_DEBUG
#include <stdexcept>
#include <string>

// clang-format off
#ifdef NDEBUG
#define LOG_DEBUG if constexpr (false) std::cerr
#else
#define LOG_DEBUG if constexpr (true) std::cerr
#endif
#define LOG(level) LOG_##level
#define DEBUG() LOG(DEBUG)
// clang-format on

namespace coro {
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

#define CHECK_SYSCALL2(expr)                                                   \
  check_syscall((expr), __FILE__, __LINE__, "", #expr)

#define THROW_SYSCALL(str)                                                     \
  check_syscall(-1, __FILE__, __LINE__, __PRETTY_FUNCTION__, str)

// https://stackoverflow.com/a/2417875/
template <class OutIter>
OutIter write_escaped(std::string_view s, OutIter out) {
  *out++ = '"';
  for (auto i = s.begin(), end = s.end(); i != end; ++i) {
    unsigned char c = *i;
    if (' ' <= c and c <= '~' and c != '\\' and c != '"') {
      *out++ = c;
    } else {
      *out++ = '\\';
      switch (c) {
      case '"':
        *out++ = '"';
        break;
      case '\\':
        *out++ = '\\';
        break;
      case '\t':
        *out++ = 't';
        break;
      case '\r':
        *out++ = 'r';
        break;
      case '\n':
        *out++ = 'n';
        break;
      default:
        char const *const hexdig = "0123456789ABCDEF";
        *out++ = 'x';
        *out++ = hexdig[c >> 4];
        *out++ = hexdig[c & 0xF];
      }
    }
  }
  *out++ = '"';
  return out;
}

inline std::string escape(std::string_view sv) {
  std::string s;
  write_escaped(sv, std::back_inserter(s));
  return s;
}

inline auto escape(char ch) { return escape(std::string_view{&ch, 1}); }

template <typename Func> class Defer {
public:
  Defer(Func func) : callback(std::move(func)) {}

  ~Defer() { callback(); }

private:
  Func callback;
};
} // namespace coro