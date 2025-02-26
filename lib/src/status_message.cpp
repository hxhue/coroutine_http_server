#include "HttpStatusCodes_C++11.h"
#include "http.hpp"

namespace coro {
std::string HTTPResponse::status_message(int status) {
  return HttpStatus::reasonPhrase(status);
}
} // namespace coro