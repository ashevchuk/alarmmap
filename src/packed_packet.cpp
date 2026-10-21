#include "packed_packet.hpp"

#include "base64url.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace alarmmap {

using json = nlohmann::json;

namespace {

constexpr std::string_view kPrefix = "~3:";
constexpr size_t kMaxBodyBytes = 8388616;
constexpr size_t kHeaderBytes = 8;
constexpr size_t kMinBytes = 9;
constexpr int kMaxDepth = 64;
constexpr size_t kMaxValues = 1000000;
constexpr size_t kMaxVarintBytes = 8;
constexpr std::uint64_t kMaxSafeInteger = (std::uint64_t{1} << 53) - 1;
constexpr std::uint32_t kStreamFallbackSeed = 1831565813;
constexpr std::uint32_t kShuffleSeedMask = 2654435769u;
constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;
constexpr size_t kPartCount = 5;
constexpr size_t kCompactFieldCount = 12;
constexpr size_t kGuidSize = 16;
constexpr char kBase64UrlAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

enum Tag : std::uint8_t {
    Null = 0,
    False = 1,
    True = 2,
    Integer = 3,
    Float = 4,
    String = 5,
    Array = 6,
    StringReference = 7,
    FloatReference = 8,
    Guid = 9,
};

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("packed packet: " + message); }

class XorShift32 {
public:
    explicit XorShift32(std::uint32_t seed) : state_(seed) {}

    std::uint32_t next() {
        std::uint32_t value = state_ ? state_ : kStreamFallbackSeed;
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        state_ = value;
        return value;
    }

private:
    std::uint32_t state_;
};

std::uint32_t readUint32(const std::string& bytes, size_t offset) {
    std::uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    }
    return value;
}

class Reader {
public:
    explicit Reader(const std::string& bytes) : bytes_(bytes), offset_(kHeaderBytes) {}

    json readRoot() {
        json root = readValue(0);
        if (offset_ != bytes_.size()) fail("trailing bytes");
        return root;
    }

private:
    void require(size_t count) const {
        if (count > bytes_.size() - offset_) fail("truncated");
    }

    unsigned char nextByte() {
        require(1);
        return static_cast<unsigned char>(bytes_[offset_++]);
    }

    std::uint64_t readVarint() {
        std::uint64_t result = 0;
        std::uint64_t scale = 1;
        for (size_t i = 0; i < kMaxVarintBytes; ++i) {
            const unsigned char byte = nextByte();
            result += (byte & 0x7F) * scale;
            if (result > kMaxSafeInteger) fail("integer out of range");
            if (!(byte & 0x80)) return result;
            scale *= 128;
        }
        fail("integer too long");
    }

    std::string takeString(std::uint64_t length) {
        require(length);
        std::string text = bytes_.substr(offset_, length);
        offset_ += length;
        return text;
    }

    json readValue(int depth) {
        if (depth > kMaxDepth || ++values_ > kMaxValues) fail("too deeply nested or too large");
        switch (nextByte()) {
            case Null:
                return nullptr;
            case False:
                return false;
            case True:
                return true;
            case Integer: {
                const std::uint64_t encoded = readVarint();
                const auto half = static_cast<std::int64_t>((encoded + (encoded & 1)) / 2);
                return encoded & 1 ? -half : half;
            }
            case Float: {
                require(sizeof(double));
                std::uint64_t bits = 0;
                for (size_t i = 0; i < sizeof(double); ++i) {
                    bits |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[offset_ + i])) << (8 * i);
                }
                offset_ += sizeof(double);
                const double value = std::bit_cast<double>(bits);
                if (!std::isfinite(value)) fail("non-finite number");
                floats_.push_back(value);
                return value;
            }
            case String: {
                std::string text = takeString(readVarint());
                strings_.push_back(text);
                return text;
            }
            case Array: {
                const std::uint64_t count = readVarint();
                require(count);
                if (count > kMaxValues - values_) fail("too deeply nested or too large");
                json items = json::array();
                for (std::uint64_t i = 0; i < count; ++i) items.push_back(readValue(depth + 1));
                return items;
            }
            case StringReference: {
                const std::uint64_t index = readVarint();
                if (index >= strings_.size()) fail("invalid string reference");
                return strings_[index];
            }
            case FloatReference: {
                const std::uint64_t index = readVarint();
                if (index >= floats_.size()) fail("invalid number reference");
                return floats_[index];
            }
            case Guid: {
                require(kGuidSize);
                std::string text = "~" + guidText();
                offset_ += kGuidSize;
                strings_.push_back(text);
                return text;
            }
            default:
                fail("unknown value tag");
        }
    }

    // The GUID's 16 bytes as unpadded base64url.
    std::string guidText() const {
        std::string text;
        for (size_t i = 0; i < kGuidSize; i += 3) {
            const size_t left = kGuidSize - i;
            const unsigned group = static_cast<unsigned char>(bytes_[offset_ + i]) << 16 |
                                   (left > 1 ? static_cast<unsigned char>(bytes_[offset_ + i + 1]) : 0) << 8 |
                                   (left > 2 ? static_cast<unsigned char>(bytes_[offset_ + i + 2]) : 0);
            text.push_back(kBase64UrlAlphabet[group >> 18 & 63]);
            text.push_back(kBase64UrlAlphabet[group >> 12 & 63]);
            if (left > 1) text.push_back(kBase64UrlAlphabet[group >> 6 & 63]);
            if (left > 2) text.push_back(kBase64UrlAlphabet[group & 63]);
        }
        return text;
    }

    const std::string& bytes_;
    size_t offset_;
    size_t values_ = 0;
    std::vector<std::string> strings_;
    std::vector<double> floats_;
};

bool isBase64UrlText(std::string_view text) {
    for (const char symbol : text) {
        const bool valid = (symbol >= 'A' && symbol <= 'Z') || (symbol >= 'a' && symbol <= 'z') ||
                           (symbol >= '0' && symbol <= '9') || symbol == '-' || symbol == '_';
        if (!valid) return false;
    }
    return true;
}

// Removes the XOR stream in place and returns the FNV-1a hash of the result.
std::uint32_t unscramble(std::string& bytes, std::uint32_t seed) {
    XorShift32 stream(seed);
    std::uint32_t word = 0;
    std::uint32_t hash = kFnvOffset;
    for (size_t i = kHeaderBytes; i < bytes.size(); ++i) {
        const size_t position = i - kHeaderBytes;
        if (position % 4 == 0) word = stream.next();
        bytes[i] = static_cast<char>(static_cast<unsigned char>(bytes[i]) ^ (word >> (8 * (position % 4)) & 0xFF));
        hash = (hash ^ static_cast<unsigned char>(bytes[i])) * kFnvPrime;
    }
    return hash;
}

}  // namespace

bool isPackedPacket(const json& value) {
    return value.is_string() && value.get_ref<const std::string&>().starts_with(kPrefix);
}

json unpackPacket(std::string_view text) {
    if (!text.starts_with(kPrefix)) fail("missing prefix");
    const std::string_view encoded = text.substr(kPrefix.size());
    if (encoded.size() % 4 == 1 || !isBase64UrlText(encoded)) fail("invalid text");
    std::string bytes = decodeBase64Url(encoded);
    if (bytes.size() < kMinBytes || bytes.size() > kMaxBodyBytes) fail("invalid size");

    const std::uint32_t seed = readUint32(bytes, 0);
    if (unscramble(bytes, seed) != readUint32(bytes, 4)) fail("checksum mismatch");

    json parts = Reader(bytes).readRoot();
    if (!parts.is_array() || parts.size() != kPartCount) fail("invalid layout");

    // The five parts arrive in an order shuffled by the seed.
    std::array<size_t, kPartCount> order = {0, 1, 2, 3, 4};
    XorShift32 shuffle(kShuffleSeedMask ^ seed);
    for (size_t i = kPartCount - 1; i > 0; --i) std::swap(order[i], order[shuffle.next() % (i + 1)]);
    json fields(kPartCount, nullptr);
    for (size_t i = 0; i < kPartCount; ++i) fields[order[i]] = std::move(parts[i]);

    const json& coverage = fields[3];
    if (!fields[0].is_string() || !fields[1].is_string() || !fields[2].is_boolean() ||
        !coverage.is_number() || coverage.get<double>() < 0 || coverage.get<double>() > 100 ||
        !fields[4].is_array() || fields[4].size() != kCompactFieldCount) {
        fail("invalid metadata");
    }
    json packet = {{"v", 2}, {"e", fields[0]}, {"q", fields[1]}, {"a", coverage}, {"d", fields[4]}};
    if (!fields[2].get<bool>()) packet["p"] = 0;
    return packet;
}

}  // namespace alarmmap
