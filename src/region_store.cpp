#include "region_store.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace alarmmap {

namespace {

static_assert(std::endian::native == std::endian::little, "the file is written little-endian as is");

constexpr std::string_view kMagic = "ALARMGEO";
constexpr std::uint32_t kVersion = 2;
// Coordinates are whole steps of a shape's extent split this many times.
constexpr double kQuantizationSteps = 65535;

// Where a shape's coordinates start and how far one quantization step goes.
struct Frame {
    MapPoint origin;
    double step = 1;
};

Frame frameOf(const RegionShape& shape) {
    MapPoint low{INFINITY, INFINITY};
    MapPoint high{-INFINITY, -INFINITY};
    const auto include = [&](const std::vector<MapPoint>& points) {
        for (const MapPoint& point : points) {
            low = {std::min(low.x, point.x), std::min(low.y, point.y)};
            high = {std::max(high.x, point.x), std::max(high.y, point.y)};
        }
    };
    include(shape.vertices);
    for (const auto& polygon : shape.polygons) {
        for (const auto& ring : polygon) include(ring);
    }
    if (low.x > high.x) return {};
    const double extent = std::max(high.x - low.x, high.y - low.y);
    return {low, extent > 0 ? extent / kQuantizationSteps : 1};
}

class Writer {
public:
    void number(std::uint64_t value) {
        while (value >= 0x80) {
            out_ += static_cast<char>((value & 0x7f) | 0x80);
            value >>= 7;
        }
        out_ += static_cast<char>(value);
    }
    void signedNumber(std::int64_t value) {
        number((static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63));
    }
    void real(double value) {
        char raw[sizeof value];
        std::memcpy(raw, &value, sizeof value);
        out_.append(raw, sizeof value);
    }
    void text(const std::string& string) {
        number(string.size());
        out_ += string;
    }
    // Points as steps of the frame, each the difference from the one before.
    void points(const std::vector<MapPoint>& points, const Frame& frame) {
        number(points.size());
        std::int64_t previousX = 0;
        std::int64_t previousY = 0;
        for (const MapPoint& point : points) {
            const auto x = static_cast<std::int64_t>(std::llround((point.x - frame.origin.x) / frame.step));
            const auto y = static_cast<std::int64_t>(std::llround((point.y - frame.origin.y) / frame.step));
            signedNumber(x - previousX);
            signedNumber(y - previousY);
            previousX = x;
            previousY = y;
        }
    }
    void raw(std::string_view bytes) { out_ += bytes; }
    std::string take() { return std::move(out_); }

private:
    std::string out_;
};

class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    std::uint64_t number() {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            need(1);
            const auto byte = static_cast<unsigned char>(bytes_[position_++]);
            value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            if (!(byte & 0x80)) return value;
        }
        throw std::runtime_error("prepared regions: bad number");
    }
    std::int64_t signedNumber() {
        const std::uint64_t value = number();
        return static_cast<std::int64_t>(value >> 1) ^ -static_cast<std::int64_t>(value & 1);
    }
    // A count of items stored ahead, each at least `itemBytes` long; a count
    // beyond the data left is corrupt and must not size an allocation.
    std::size_t count(std::size_t itemBytes) {
        const std::uint64_t size = number();
        if (size > (bytes_.size() - position_) / itemBytes) throw std::runtime_error("prepared regions: bad count");
        return static_cast<std::size_t>(size);
    }
    double real() {
        need(sizeof(double));
        double value;
        std::memcpy(&value, bytes_.data() + position_, sizeof value);
        position_ += sizeof value;
        if (!std::isfinite(value)) throw std::runtime_error("prepared regions: bad number");
        return value;
    }
    std::string text() {
        const std::size_t size = count(1);
        std::string string(bytes_.substr(position_, size));
        position_ += size;
        return string;
    }
    std::vector<MapPoint> points(const Frame& frame) {
        // Each point is two numbers of at least one byte each.
        std::vector<MapPoint> result(count(2));
        std::int64_t x = 0;
        std::int64_t y = 0;
        for (MapPoint& point : result) {
            x += signedNumber();
            y += signedNumber();
            point = {frame.origin.x + static_cast<double>(x) * frame.step,
                     frame.origin.y + static_cast<double>(y) * frame.step};
        }
        return result;
    }
    std::string_view raw(std::size_t size) {
        need(size);
        const std::string_view part = bytes_.substr(position_, size);
        position_ += size;
        return part;
    }
    bool atEnd() const { return position_ == bytes_.size(); }

private:
    void need(std::size_t size) const {
        if (bytes_.size() - position_ < size) throw std::runtime_error("prepared regions: truncated");
    }

    std::string_view bytes_;
    std::size_t position_ = 0;
};

// buildRegionShapes triangulates the outline points in ring order, so the
// vertices are the outlines laid end to end and the outlines are stored as
// ring lengths; the bounds follow from the outlines.
void writeShapes(Writer& out, const std::vector<RegionShape>& shapes, bool withPolygons) {
    out.number(shapes.size());
    for (const RegionShape& shape : shapes) {
        std::size_t next = 0;
        for (const auto& ring : shape.outlines) {
            for (const MapPoint& point : ring) {
                if (next >= shape.vertices.size() || !(shape.vertices[next] == point)) {
                    throw std::runtime_error("prepared regions: outlines of " + shape.id + " are not its vertices");
                }
                ++next;
            }
        }
        if (next != shape.vertices.size()) {
            throw std::runtime_error("prepared regions: outlines of " + shape.id + " are not its vertices");
        }
        const std::vector<std::vector<std::vector<MapPoint>>> noPolygons;
        const auto& polygons = withPolygons ? shape.polygons : noPolygons;
        RegionShape framed;
        framed.vertices = shape.vertices;
        framed.polygons = polygons;
        const Frame frame = frameOf(framed);

        out.text(shape.id);
        out.text(shape.name);
        out.signedNumber(shape.rank);
        out.real(frame.origin.x);
        out.real(frame.origin.y);
        out.real(frame.step);
        out.points(shape.vertices, frame);
        out.number(shape.indices.size());
        std::int64_t previous = 0;
        for (const std::uint32_t index : shape.indices) {
            out.signedNumber(static_cast<std::int64_t>(index) - previous);
            previous = index;
        }
        out.number(shape.outlines.size());
        for (const auto& ring : shape.outlines) out.number(ring.size());
        out.number(polygons.size());
        for (const auto& polygon : polygons) {
            out.number(polygon.size());
            for (const auto& ring : polygon) out.points(ring, frame);
        }
    }
}

std::vector<RegionShape> readShapes(Reader& in) {
    std::vector<RegionShape> shapes(in.count(1));
    for (RegionShape& shape : shapes) {
        shape.id = in.text();
        shape.name = in.text();
        shape.rank = static_cast<int>(in.signedNumber());
        Frame frame;
        frame.origin.x = in.real();
        frame.origin.y = in.real();
        frame.step = in.real();
        shape.vertices = in.points(frame);
        shape.indices.resize(in.count(1));
        std::int64_t index = 0;
        for (std::uint32_t& stored : shape.indices) {
            index += in.signedNumber();
            if (index < 0 || static_cast<std::uint64_t>(index) >= shape.vertices.size()) {
                throw std::runtime_error("prepared regions: bad index");
            }
            stored = static_cast<std::uint32_t>(index);
        }
        shape.outlines.resize(in.count(1));
        std::size_t next = 0;
        for (auto& ring : shape.outlines) {
            // A ring length counts vertices already read, not bytes ahead.
            const std::uint64_t size = in.number();
            if (size > shape.vertices.size() - next) throw std::runtime_error("prepared regions: bad outline");
            ring.assign(shape.vertices.begin() + static_cast<std::ptrdiff_t>(next),
                        shape.vertices.begin() + static_cast<std::ptrdiff_t>(next + size));
            next += size;
        }
        bool first = true;
        for (const auto& ring : shape.outlines) {
            for (const MapPoint& point : ring) {
                shape.boundsMin = first ? point : MapPoint{std::min(shape.boundsMin.x, point.x),
                                                           std::min(shape.boundsMin.y, point.y)};
                shape.boundsMax = first ? point : MapPoint{std::max(shape.boundsMax.x, point.x),
                                                           std::max(shape.boundsMax.y, point.y)};
                first = false;
            }
        }
        shape.polygons.resize(in.count(1));
        for (auto& polygon : shape.polygons) {
            polygon.resize(in.count(1));
            for (auto& ring : polygon) ring = in.points(frame);
        }
    }
    return shapes;
}

}  // namespace

std::string writePreparedRegions(const PreparedRegions& regions) {
    Writer out;
    out.raw(kMagic);
    out.number(kVersion);
    writeShapes(out, regions.coarse, true);
    writeShapes(out, regions.detailed, false);
    return out.take();
}

PreparedRegions readPreparedRegions(std::string_view bytes) {
    Reader in(bytes);
    if (in.raw(kMagic.size()) != kMagic) throw std::runtime_error("prepared regions: not a region file");
    if (in.number() != kVersion) throw std::runtime_error("prepared regions: unsupported version");
    PreparedRegions regions;
    regions.coarse = readShapes(in);
    regions.detailed = readShapes(in);
    if (!in.atEnd()) throw std::runtime_error("prepared regions: trailing data");
    return regions;
}

}  // namespace alarmmap
