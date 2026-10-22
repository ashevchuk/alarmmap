#include "packed_packet.hpp"

#include "base64url.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <optional>
#include <vector>

namespace alarmmap {

using json = nlohmann::json;

namespace {

constexpr std::string_view kPrefixV3 = "~3:";
constexpr std::string_view kPrefixV4 = "~4:";
constexpr size_t kPrefixSize = 3;
constexpr size_t kMaxBodyBytesV3 = 8388616;
constexpr size_t kMaxBodyBytesV4 = 8388624;
constexpr size_t kHeaderBytesV3 = 8;
constexpr size_t kHeaderBytesV4 = 16;
constexpr int kMaxDepth = 64;
constexpr size_t kMaxValues = 1000000;
constexpr size_t kMaxVarintBytes = 8;
constexpr std::uint64_t kMaxSafeInteger = (std::uint64_t{1} << 53) - 1;
constexpr std::uint32_t kStreamFallbackSeed = 1831565813;
constexpr std::uint32_t kShuffleSeedMask = 2654435769u;
constexpr std::uint32_t kArrayOrderSeedMask = 2246822507u;
constexpr std::uint32_t kNumberSeedMask = 3266489909u;
constexpr std::uint32_t kTagShuffleSeedMask = 668265263u;
constexpr std::uint32_t kNumberKeyMask = 0xFFFF;
constexpr std::uint32_t kFnvOffset = 2166136261u;
constexpr std::uint32_t kFnvPrime = 16777619u;
constexpr size_t kPartCount = 5;
constexpr size_t kCompactFieldCount = 12;
constexpr size_t kGuidSize = 16;
constexpr size_t kWireTagCount = 10;
constexpr size_t kCipherBlockBytes = 64;
constexpr int kCipherDoubleRounds = 10;
// ChaCha20 constants and the fixed key the site ships in its script, then the block counter.
constexpr std::array<std::uint32_t, 13> kCipherState = {
    1634760805, 857760878,  2036477234, 1797285236, 2838917605, 566540499, 2028117766,
    3025996967, 1063430289, 3353677899, 1519178294, 2422993229, 1};
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

// What the "~4:" format adds to the value encoding: separate streams for
// numbers and for array order, and a shuffled wire alphabet for the tags.
struct ValueScramble {
    XorShift32 numbers;
    XorShift32 arrayOrder;
    std::array<int, 256> tagOfWire;
};

class Reader {
public:
    Reader(const std::string& bytes, size_t headerBytes, std::optional<ValueScramble> scramble = std::nullopt)
        : bytes_(bytes), offset_(headerBytes), scramble_(std::move(scramble)) {}

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
        const unsigned char wire = nextByte();
        switch (scramble_ ? scramble_->tagOfWire[wire] : wire) {
            case Null:
                return nullptr;
            case False:
                return false;
            case True:
                return true;
            case Integer: {
                std::uint64_t encoded = readVarint();
                if (scramble_) encoded ^= scramble_->numbers.next() & kNumberKeyMask;
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
                if (scramble_) {
                    bits ^= scramble_->numbers.next();
                    bits ^= std::uint64_t{scramble_->numbers.next()} << 32;
                }
                const double value = std::bit_cast<double>(bits);
                if (!std::isfinite(value)) fail("non-finite number");
                floats_.push_back(value);
                return value;
            }
            case String: {
                std::string text = takeString(readVarint());
                if (scramble_) {
                    const std::uint32_t key = scramble_->numbers.next();
                    for (size_t i = 0; i < text.size(); ++i) text[i] ^= static_cast<char>(key >> (8 * (i % 4)) & 0xFF);
                }
                strings_.push_back(text);
                return text;
            }
            case Array: {
                const std::uint64_t count = readVarint();
                require(count);
                if (count > kMaxValues - values_) fail("too deeply nested or too large");
                std::uint64_t start = 0;
                bool reversed = false;
                if (scramble_ && count > 0) {
                    start = scramble_->arrayOrder.next() % count;
                    reversed = scramble_->arrayOrder.next() & 1;
                }
                json items = json::array();
                items.get_ref<json::array_t&>().resize(count);
                for (std::uint64_t i = 0; i < count; ++i) items[(start + (reversed ? count - i : i)) % count] = readValue(depth + 1);
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
                if (scramble_) fail("unknown value tag");
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
    std::optional<ValueScramble> scramble_;
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
std::uint32_t unscrambleV3(std::string& bytes, std::uint32_t seed) {
    XorShift32 stream(seed);
    std::uint32_t word = 0;
    std::uint32_t hash = kFnvOffset;
    for (size_t i = kHeaderBytesV3; i < bytes.size(); ++i) {
        const size_t position = i - kHeaderBytesV3;
        if (position % 4 == 0) word = stream.next();
        bytes[i] = static_cast<char>(static_cast<unsigned char>(bytes[i]) ^ (word >> (8 * (position % 4)) & 0xFF));
        hash = (hash ^ static_cast<unsigned char>(bytes[i])) * kFnvPrime;
    }
    return hash;
}

void quarterRound(std::array<std::uint32_t, 16>& state, size_t a, size_t b, size_t c, size_t d) {
    state[a] += state[b];
    state[d] = std::rotl(state[d] ^ state[a], 16);
    state[c] += state[d];
    state[b] = std::rotl(state[b] ^ state[c], 12);
    state[a] += state[b];
    state[d] = std::rotl(state[d] ^ state[a], 8);
    state[c] += state[d];
    state[b] = std::rotl(state[b] ^ state[c], 7);
}

// Removes the ChaCha20 stream in place and returns the FNV-1a hash of the
// result. The header words 0, 1 and 2 are the nonce.
std::uint32_t decryptV4(std::string& bytes) {
    std::array<std::uint32_t, 16> initial{};
    std::copy(kCipherState.begin(), kCipherState.end(), initial.begin());
    for (size_t i = 0; i < 3; ++i) initial[13 + i] = readUint32(bytes, 4 * i);

    for (size_t block = kHeaderBytesV4; block < bytes.size(); block += kCipherBlockBytes) {
        std::array<std::uint32_t, 16> state = initial;
        for (int round = 0; round < kCipherDoubleRounds; ++round) {
            quarterRound(state, 0, 4, 8, 12);
            quarterRound(state, 1, 5, 9, 13);
            quarterRound(state, 2, 6, 10, 14);
            quarterRound(state, 3, 7, 11, 15);
            quarterRound(state, 0, 5, 10, 15);
            quarterRound(state, 1, 6, 11, 12);
            quarterRound(state, 2, 7, 8, 13);
            quarterRound(state, 3, 4, 9, 14);
        }
        const size_t length = std::min(kCipherBlockBytes, bytes.size() - block);
        for (size_t i = 0; i < length; ++i) {
            const std::uint32_t word = state[i / 4] + initial[i / 4];
            bytes[block + i] = static_cast<char>(static_cast<unsigned char>(bytes[block + i]) ^ (word >> (8 * (i % 4)) & 0xFF));
        }
        ++initial[12];
    }

    std::uint32_t hash = kFnvOffset;
    for (size_t i = kHeaderBytesV4; i < bytes.size(); ++i) hash = (hash ^ static_cast<unsigned char>(bytes[i])) * kFnvPrime;
    return hash;
}

ValueScramble valueScramble(const std::string& bytes) {
    const std::uint32_t first = readUint32(bytes, 0);
    const std::uint32_t second = readUint32(bytes, 4);
    const std::uint32_t third = readUint32(bytes, 8);

    ValueScramble scramble{XorShift32(kNumberSeedMask ^ second), XorShift32(first ^ third ^ kArrayOrderSeedMask), {}};
    scramble.tagOfWire.fill(-1);
    std::array<int, kWireTagCount> wireOfTag{};
    for (size_t i = 0; i < kWireTagCount; ++i) wireOfTag[i] = static_cast<int>(i);
    XorShift32 shuffle(first ^ second ^ third ^ kTagShuffleSeedMask);
    for (size_t i = kWireTagCount - 1; i > 0; --i) std::swap(wireOfTag[i], wireOfTag[shuffle.next() % (i + 1)]);
    for (size_t tag = 0; tag < kWireTagCount; ++tag) scramble.tagOfWire[wireOfTag[tag]] = static_cast<int>(tag);
    return scramble;
}

json compactPacket(const json& fields) {
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

json unpackV3(std::string bytes) {
    if (bytes.size() < kHeaderBytesV3 + 1 || bytes.size() > kMaxBodyBytesV3) fail("invalid size");

    const std::uint32_t seed = readUint32(bytes, 0);
    if (unscrambleV3(bytes, seed) != readUint32(bytes, 4)) fail("checksum mismatch");

    json parts = Reader(bytes, kHeaderBytesV3).readRoot();
    if (!parts.is_array() || parts.size() != kPartCount) fail("invalid layout");

    // The five parts arrive in an order shuffled by the seed.
    std::array<size_t, kPartCount> order = {0, 1, 2, 3, 4};
    XorShift32 shuffle(kShuffleSeedMask ^ seed);
    for (size_t i = kPartCount - 1; i > 0; --i) std::swap(order[i], order[shuffle.next() % (i + 1)]);
    json fields(kPartCount, nullptr);
    for (size_t i = 0; i < kPartCount; ++i) fields[order[i]] = std::move(parts[i]);
    return compactPacket(fields);
}

json unpackV4(std::string bytes) {
    if (bytes.size() < kHeaderBytesV4 + 1 || bytes.size() > kMaxBodyBytesV4) fail("invalid size");
    if (decryptV4(bytes) != readUint32(bytes, 12)) fail("checksum mismatch");

    json fields = Reader(bytes, kHeaderBytesV4, valueScramble(bytes)).readRoot();
    if (!fields.is_array() || fields.size() != kPartCount) fail("invalid layout");
    return compactPacket(fields);
}

}  // namespace

bool isPackedPacket(const json& value) {
    if (!value.is_string()) return false;
    const std::string& text = value.get_ref<const std::string&>();
    return text.starts_with(kPrefixV3) || text.starts_with(kPrefixV4);
}

json unpackPacket(std::string_view text) {
    const bool v4 = text.starts_with(kPrefixV4);
    if (!v4 && !text.starts_with(kPrefixV3)) fail("missing prefix");
    const std::string_view encoded = text.substr(kPrefixSize);
    if (encoded.size() % 4 == 1 || !isBase64UrlText(encoded)) fail("invalid text");
    std::string bytes = decodeBase64Url(encoded);
    return v4 ? unpackV4(std::move(bytes)) : unpackV3(std::move(bytes));
}

}  // namespace alarmmap
