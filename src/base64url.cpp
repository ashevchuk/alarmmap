#include "base64url.hpp"

#include <stdexcept>

namespace alarmmap {

namespace {

int sextetValue(char symbol) {
    if (symbol >= 'A' && symbol <= 'Z') return symbol - 'A';
    if (symbol >= 'a' && symbol <= 'z') return symbol - 'a' + 26;
    if (symbol >= '0' && symbol <= '9') return symbol - '0' + 52;
    if (symbol == '-' || symbol == '+') return 62;
    if (symbol == '_' || symbol == '/') return 63;
    throw std::runtime_error("invalid base64url character");
}

}  // namespace

std::string decodeBase64Url(std::string_view text) {
    std::string decoded;
    unsigned buffer = 0;
    int bits = 0;
    for (const char symbol : text) {
        if (symbol == '=') break;
        buffer = (buffer << 6) | static_cast<unsigned>(sextetValue(symbol));
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return decoded;
}

}  // namespace alarmmap
