#pragma once

#include <nlohmann/json.hpp>

#include <string_view>

namespace alarmmap {

// The site sends target snapshots as text "~3:<base64url>": a checksummed,
// XOR-scrambled binary encoding of the compact v2 packet. Everything needed
// to read it is inside the text; there is no key.
bool isPackedPacket(const nlohmann::json& value);

// Returns the compact v2 packet {v, e, q, a, d, p?}. Throws
// std::runtime_error on text that is not a well-formed packed packet.
nlohmann::json unpackPacket(std::string_view text);

}  // namespace alarmmap
