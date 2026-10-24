#include "srpc/error.hpp"

#include <utility>

namespace srpc {
namespace {

std::string make_command_error(int status_code, const std::string& message) {
    std::string text = "XBDM command failed with status " + std::to_string(status_code);
    if (!message.empty()) {
        text += ": ";
        text += message;
    }
    return text;
}

} // namespace

CommandError::CommandError(int status_code, std::string message)
    : Error(make_command_error(status_code, message)),
      status_code_(status_code) {}

int CommandError::status_code() const noexcept {
    return status_code_;
}

} // namespace srpc
