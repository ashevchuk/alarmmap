#include "vector_tile.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace alarmmap {

using json = nlohmann::json;

namespace {

enum WireType : std::uint32_t { kVarint = 0, kFixed64 = 1, kLengthDelimited = 2, kFixed32 = 5 };

// Field numbers of the vector tile schema.
constexpr std::uint32_t kTileLayers = 3;
constexpr std::uint32_t kLayerName = 1, kLayerFeatures = 2, kLayerKeys = 3, kLayerValues = 4,
                        kLayerExtent = 5;
constexpr std::uint32_t kFeatureTags = 2, kFeatureType = 3, kFeatureGeometry = 4;
constexpr std::uint32_t kValueString = 1, kValueFloat = 2, kValueDouble = 3, kValueInt = 4,
                        kValueUint = 5, kValueSint = 6, kValueBool = 7;
constexpr std::uint32_t kMoveTo = 1, kLineTo = 2, kClosePath = 7;
constexpr int kMaxVarintBytes = 10;

[[noreturn]] void malformed(const char* what) {
    throw std::runtime_error(std::string("malformed vector tile: ") + what);
}

// Sequential reader over one protobuf message.
class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    bool atEnd() const { return position_ >= data_.size(); }

    std::uint64_t varint() {
        std::uint64_t value = 0;
        for (int i = 0; i < kMaxVarintBytes; ++i) {
            if (atEnd()) malformed("truncated varint");
            const auto byte = static_cast<std::uint8_t>(data_[position_++]);
            value |= static_cast<std::uint64_t>(byte & 0x7F) << (7 * i);
            if (byte < 0x80) return value;
        }
        malformed("varint too long");
    }

    std::string_view bytes(size_t count) {
        if (count > data_.size() - position_) malformed("field past the end");
        const std::string_view result = data_.substr(position_, count);
        position_ += count;
        return result;
    }

    // Reads the next field key; returns the field number and wire type.
    std::pair<std::uint32_t, std::uint32_t> key() {
        const std::uint64_t key = varint();
        return {static_cast<std::uint32_t>(key >> 3), static_cast<std::uint32_t>(key & 7)};
    }

    std::string_view lengthDelimited() { return bytes(static_cast<size_t>(varint())); }

    void skip(std::uint32_t wireType) {
        switch (wireType) {
            case kVarint: varint(); return;
            case kFixed64: bytes(8); return;
            case kLengthDelimited: lengthDelimited(); return;
            case kFixed32: bytes(4); return;
            default: malformed("unsupported wire type");
        }
    }

private:
    std::string_view data_;
    size_t position_ = 0;
};

void expectWireType(std::uint32_t actual, std::uint32_t expected) {
    if (actual != expected) malformed("unexpected wire type");
}

std::vector<std::uint32_t> packedUints(std::string_view data) {
    std::vector<std::uint32_t> values;
    Reader reader(data);
    while (!reader.atEnd()) values.push_back(static_cast<std::uint32_t>(reader.varint()));
    return values;
}

std::int64_t zigzag(std::uint64_t value) {
    return static_cast<std::int64_t>(value >> 1) ^ -static_cast<std::int64_t>(value & 1);
}

json decodeValue(std::string_view data) {
    Reader reader(data);
    json value;
    while (!reader.atEnd()) {
        const auto [field, wireType] = reader.key();
        switch (field) {
            case kValueString:
                expectWireType(wireType, kLengthDelimited);
                value = std::string(reader.lengthDelimited());
                break;
            case kValueFloat: {
                expectWireType(wireType, kFixed32);
                std::uint32_t bits;
                std::memcpy(&bits, reader.bytes(4).data(), 4);
                value = static_cast<double>(std::bit_cast<float>(bits));
                break;
            }
            case kValueDouble: {
                expectWireType(wireType, kFixed64);
                std::uint64_t bits;
                std::memcpy(&bits, reader.bytes(8).data(), 8);
                value = std::bit_cast<double>(bits);
                break;
            }
            case kValueInt:
                expectWireType(wireType, kVarint);
                value = static_cast<std::int64_t>(reader.varint());
                break;
            case kValueUint:
                expectWireType(wireType, kVarint);
                value = reader.varint();
                break;
            case kValueSint:
                expectWireType(wireType, kVarint);
                value = zigzag(reader.varint());
                break;
            case kValueBool:
                expectWireType(wireType, kVarint);
                value = reader.varint() != 0;
                break;
            default:
                reader.skip(wireType);
        }
    }
    return value;
}

std::vector<TileRing> decodeGeometry(const std::vector<std::uint32_t>& commands) {
    std::vector<TileRing> parts;
    std::int64_t x = 0, y = 0;
    size_t i = 0;
    while (i < commands.size()) {
        const std::uint32_t command = commands[i] & 7;
        const std::uint32_t count = commands[i] >> 3;
        ++i;
        if (command == kClosePath) continue;
        if (command != kMoveTo && command != kLineTo) malformed("unknown geometry command");
        if (count > (commands.size() - i) / 2) malformed("geometry past the end");
        for (std::uint32_t n = 0; n < count; ++n) {
            x += zigzag(commands[i]);
            y += zigzag(commands[i + 1]);
            i += 2;
            if (command == kMoveTo) {
                parts.emplace_back();
            } else if (parts.empty()) {
                malformed("LineTo before MoveTo");
            }
            parts.back().push_back({static_cast<double>(x), static_cast<double>(y)});
        }
    }
    return parts;
}

TileLayer decodeLayer(std::string_view data) {
    TileLayer layer;
    std::vector<std::string> keys;
    std::vector<json> values;
    std::vector<std::string_view> features;
    Reader reader(data);
    while (!reader.atEnd()) {
        const auto [field, wireType] = reader.key();
        switch (field) {
            case kLayerName:
                expectWireType(wireType, kLengthDelimited);
                layer.name = reader.lengthDelimited();
                break;
            case kLayerFeatures:
                expectWireType(wireType, kLengthDelimited);
                features.push_back(reader.lengthDelimited());
                break;
            case kLayerKeys:
                expectWireType(wireType, kLengthDelimited);
                keys.emplace_back(reader.lengthDelimited());
                break;
            case kLayerValues:
                expectWireType(wireType, kLengthDelimited);
                values.push_back(decodeValue(reader.lengthDelimited()));
                break;
            case kLayerExtent:
                expectWireType(wireType, kVarint);
                layer.extent = static_cast<double>(reader.varint());
                break;
            default:
                reader.skip(wireType);
        }
    }

    for (const std::string_view data : features) {
        TileFeature feature;
        Reader featureReader(data);
        while (!featureReader.atEnd()) {
            const auto [field, wireType] = featureReader.key();
            if (field == kFeatureTags) {
                expectWireType(wireType, kLengthDelimited);
                const auto tags = packedUints(featureReader.lengthDelimited());
                if (tags.size() % 2 != 0) malformed("odd tag count");
                for (size_t i = 0; i < tags.size(); i += 2) {
                    if (tags[i] >= keys.size() || tags[i + 1] >= values.size()) malformed("tag out of range");
                    feature.properties[keys[tags[i]]] = values[tags[i + 1]];
                }
            } else if (field == kFeatureType) {
                expectWireType(wireType, kVarint);
                const auto type = featureReader.varint();
                if (type > static_cast<std::uint64_t>(TileGeometryType::Polygon)) malformed("geometry type");
                feature.type = static_cast<TileGeometryType>(type);
            } else if (field == kFeatureGeometry) {
                expectWireType(wireType, kLengthDelimited);
                feature.parts = decodeGeometry(packedUints(featureReader.lengthDelimited()));
            } else {
                featureReader.skip(wireType);
            }
        }
        layer.features.push_back(std::move(feature));
    }
    return layer;
}

// Twice the signed area; positive for rings clockwise on a y-down plane.
double signedArea2(const TileRing& ring) {
    double sum = 0;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        sum += ring[j].x * ring[i].y - ring[i].x * ring[j].y;
    }
    return sum;
}

}  // namespace

std::vector<TileLayer> decodeVectorTile(std::string_view data) {
    std::vector<TileLayer> layers;
    Reader reader(data);
    while (!reader.atEnd()) {
        const auto [field, wireType] = reader.key();
        if (field == kTileLayers) {
            expectWireType(wireType, kLengthDelimited);
            layers.push_back(decodeLayer(reader.lengthDelimited()));
        } else {
            reader.skip(wireType);
        }
    }
    return layers;
}

std::vector<std::vector<TileRing>> groupPolygonRings(const std::vector<TileRing>& rings) {
    std::vector<std::vector<TileRing>> polygons;
    for (const TileRing& ring : rings) {
        if (ring.size() < 3) continue;
        const double area = signedArea2(ring);
        if (area > 0) {
            polygons.push_back({ring});
        } else if (area < 0 && !polygons.empty()) {
            polygons.back().push_back(ring);
        }
    }
    return polygons;
}

}  // namespace alarmmap
