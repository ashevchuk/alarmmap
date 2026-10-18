#include "region_store.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace alarmmap {

namespace {

static_assert(std::endian::native == std::endian::little, "the file is written little-endian as is");

constexpr std::string_view kMagic = "ALARMGEO";
constexpr std::uint32_t kVersion = 1;

class Writer {
public:
    template <typename T>
    void value(T number) {
        char raw[sizeof(T)];
        std::memcpy(raw, &number, sizeof(T));
        out_.append(raw, sizeof(T));
    }
    void count(std::size_t size) { value(static_cast<std::uint32_t>(size)); }
    void text(const std::string& string) {
        count(string.size());
        out_ += string;
    }
    void point(const MapPoint& point) {
        value(static_cast<float>(point.x));
        value(static_cast<float>(point.y));
    }
    void points(const std::vector<MapPoint>& points) {
        count(points.size());
        for (const MapPoint& p : points) point(p);
    }
    void raw(std::string_view bytes) { out_ += bytes; }
    std::string take() { return std::move(out_); }

private:
    std::string out_;
};

class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    template <typename T>
    T value() {
        need(sizeof(T));
        T number;
        std::memcpy(&number, bytes_.data() + position_, sizeof(T));
        position_ += sizeof(T);
        return number;
    }
    std::size_t count() {
        const auto size = value<std::uint32_t>();
        // Each counted item takes at least one byte, so a count beyond the
        // remaining data is corrupt and must not size an allocation.
        if (size > bytes_.size() - position_) throw std::runtime_error("prepared regions: bad count");
        return size;
    }
    std::string text() {
        const std::size_t size = count();
        need(size);
        std::string string(bytes_.substr(position_, size));
        position_ += size;
        return string;
    }
    MapPoint point() {
        const auto x = value<float>();
        const auto y = value<float>();
        return {x, y};
    }
    std::vector<MapPoint> points() {
        std::vector<MapPoint> result(count());
        for (MapPoint& p : result) p = point();
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
// ring lengths. Shapes stay far below 65536 vertices, so indices fit 16 bits.
void writeShapes(Writer& out, const std::vector<RegionShape>& shapes, bool withPolygons) {
    out.count(shapes.size());
    for (const RegionShape& shape : shapes) {
        if (shape.vertices.size() > 65536) throw std::runtime_error("prepared regions: shape " + shape.id + " too large");
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
        out.text(shape.id);
        out.text(shape.name);
        out.value(static_cast<std::int32_t>(shape.rank));
        out.points(shape.vertices);
        out.count(shape.indices.size());
        for (const std::uint32_t index : shape.indices) out.value(static_cast<std::uint16_t>(index));
        out.count(shape.outlines.size());
        for (const auto& ring : shape.outlines) out.count(ring.size());
        out.point(shape.boundsMin);
        out.point(shape.boundsMax);
        out.count(withPolygons ? shape.polygons.size() : 0);
        if (!withPolygons) continue;
        for (const auto& polygon : shape.polygons) {
            out.count(polygon.size());
            for (const auto& ring : polygon) out.points(ring);
        }
    }
}

std::vector<RegionShape> readShapes(Reader& in) {
    std::vector<RegionShape> shapes(in.count());
    for (RegionShape& shape : shapes) {
        shape.id = in.text();
        shape.name = in.text();
        shape.rank = in.value<std::int32_t>();
        shape.vertices = in.points();
        shape.indices.resize(in.count());
        for (std::uint32_t& index : shape.indices) {
            index = in.value<std::uint16_t>();
            if (index >= shape.vertices.size()) throw std::runtime_error("prepared regions: bad index");
        }
        shape.outlines.resize(in.count());
        std::size_t next = 0;
        for (auto& ring : shape.outlines) {
            // A ring length counts vertices already read, not bytes ahead.
            const std::size_t size = in.value<std::uint32_t>();
            if (size > shape.vertices.size() - next) throw std::runtime_error("prepared regions: bad outline");
            ring.assign(shape.vertices.begin() + static_cast<std::ptrdiff_t>(next),
                        shape.vertices.begin() + static_cast<std::ptrdiff_t>(next + size));
            next += size;
        }
        shape.boundsMin = in.point();
        shape.boundsMax = in.point();
        shape.polygons.resize(in.count());
        for (auto& polygon : shape.polygons) {
            polygon.resize(in.count());
            for (auto& ring : polygon) ring = in.points();
        }
    }
    return shapes;
}

}  // namespace

std::string writePreparedRegions(const PreparedRegions& regions) {
    Writer out;
    out.raw(kMagic);
    out.value(kVersion);
    writeShapes(out, regions.coarse, true);
    writeShapes(out, regions.detailed, false);
    return out.take();
}

PreparedRegions readPreparedRegions(std::string_view bytes) {
    Reader in(bytes);
    if (in.raw(kMagic.size()) != kMagic) throw std::runtime_error("prepared regions: not a region file");
    if (in.value<std::uint32_t>() != kVersion) throw std::runtime_error("prepared regions: unsupported version");
    PreparedRegions regions;
    regions.coarse = readShapes(in);
    regions.detailed = readShapes(in);
    if (!in.atEnd()) throw std::runtime_error("prepared regions: trailing data");
    return regions;
}

}  // namespace alarmmap
