#pragma once

#include <cerrno>
#include <sstream>
#include <system_error>

inline auto check_syscall(int ret) {
  if (ret == -1) {
    throw std::system_error(errno, std::system_category());
  }
  return ret;
}

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