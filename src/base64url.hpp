#pragma once

#include <string>
#include <string_view>

namespace alarmmap {

// Decodes base64 or base64url text; trailing padding is optional.
// Throws std::runtime_error on characters outside both alphabets.
std::string decodeBase64Url(std::string_view text);

}  // namespace alarmmap
