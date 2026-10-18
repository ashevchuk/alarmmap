#include "mesh_builder.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace alarmmap {

namespace {

struct Direction {
    double x = 0;
    double y = 0;
};

Direction unitDirection(const MapPoint& from, const MapPoint& to) {
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double length = std::hypot(dx, dy);
    return {dx / length, dy / length};
}

Direction normalOf(const Direction& direction) { return {-direction.y, direction.x}; }

// Extrusion at a joint between segments with the given normals: along the
// bisector, long enough that both edges stay half a width away.
Direction miter(const Direction& before, const Direction& after) {
    const Direction sum = {before.x + after.x, before.y + after.y};
    const double length = std::hypot(sum.x, sum.y);
    if (length < 1e-9) return after;  // the line turns straight back
    const Direction bisector = {sum.x / length, sum.y / length};
    const double cosine = bisector.x * after.x + bisector.y * after.y;
    const double scale = 1 / std::max(cosine, 1.0 / MeshBuilder::kMaxMiter);
    return {bisector.x * scale, bisector.y * scale};
}

}  // namespace

std::size_t MeshBuilder::beginItem() {
    items_.emplace_back();
    return items_.size() - 1;
}

MeshChunk& MeshBuilder::chunkWithRoom(std::size_t vertexCount) {
    if (vertexCount > kMaxChunkVertices) throw std::logic_error("mesh piece larger than a chunk");
    if (chunks_.empty() || chunks_.back().vertices.size() + vertexCount > kMaxChunkVertices) {
        chunks_.emplace_back();
    }
    return chunks_.back();
}

void MeshBuilder::appendIndex(std::size_t chunk, std::uint16_t index) {
    if (items_.empty()) beginItem();
    std::vector<MeshSpan>& spans = items_.back();
    std::vector<std::uint16_t>& indices = chunks_[chunk].indices;
    if (spans.empty() || spans.back().chunk != chunk ||
        spans.back().firstIndex + spans.back().indexCount != indices.size()) {
        spans.push_back({chunk, indices.size(), 0});
    }
    indices.push_back(index);
    ++spans.back().indexCount;
}

void MeshBuilder::addTriangles(const std::vector<MapPoint>& vertices,
                               const std::vector<std::uint32_t>& indices) {
    // Position of each source vertex in the current chunk, or -1.
    std::vector<std::int64_t> placed(vertices.size(), -1);
    std::size_t placedChunk = 0;
    bool placedValid = false;
    for (std::size_t first = 0; first + 2 < indices.size(); first += 3) {
        chunkWithRoom(3);
        const std::size_t chunk = chunks_.size() - 1;
        if (!placedValid || placedChunk != chunk) {
            std::fill(placed.begin(), placed.end(), -1);
            placedChunk = chunk;
            placedValid = true;
        }
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const std::uint32_t source = indices[first + corner];
            if (placed[source] < 0) {
                placed[source] = static_cast<std::int64_t>(chunks_[chunk].vertices.size());
                const MapPoint& point = vertices[source];
                chunks_[chunk].vertices.push_back({static_cast<float>(point.x), static_cast<float>(point.y), 0, 0});
            }
            appendIndex(chunk, static_cast<std::uint16_t>(placed[source]));
        }
    }
}

void MeshBuilder::addPolyline(const std::vector<MapPoint>& points, bool closed) {
    std::vector<MapPoint> line;
    for (const MapPoint& point : points) {
        if (line.empty() || point.x != line.back().x || point.y != line.back().y) line.push_back(point);
    }
    if (closed && line.size() > 1 && line.front().x == line.back().x && line.front().y == line.back().y) {
        line.pop_back();
    }
    if (line.size() < (closed ? 3u : 2u)) return;

    const std::size_t count = line.size();
    std::vector<Direction> extrusions(count);
    for (std::size_t i = 0; i < count; ++i) {
        const bool hasBefore = closed || i > 0;
        const bool hasAfter = closed || i + 1 < count;
        const MapPoint& previous = line[(i + count - 1) % count];
        const MapPoint& next = line[(i + 1) % count];
        if (!hasBefore) {
            extrusions[i] = normalOf(unitDirection(line[i], next));
        } else if (!hasAfter) {
            extrusions[i] = normalOf(unitDirection(previous, line[i]));
        } else {
            extrusions[i] = miter(normalOf(unitDirection(previous, line[i])),
                                  normalOf(unitDirection(line[i], next)));
        }
    }
    // A closed line ends where it starts, with the same joint.
    std::vector<std::size_t> strip(count);
    for (std::size_t i = 0; i < count; ++i) strip[i] = i;
    if (closed) strip.push_back(0);

    // Pieces that fit a chunk share their end points, so the strip stays
    // continuous across chunks.
    const std::size_t maxPoints = kMaxChunkVertices / 2;
    for (std::size_t start = 0; start + 1 < strip.size();) {
        const std::size_t pieceSize = std::min(strip.size() - start, maxPoints);
        MeshChunk& chunk = chunkWithRoom(pieceSize * 2);
        const std::size_t chunkIndex = chunks_.size() - 1;
        const auto base = static_cast<std::uint16_t>(chunk.vertices.size());
        for (std::size_t i = 0; i < pieceSize; ++i) {
            const MapPoint& point = line[strip[start + i]];
            const Direction& out = extrusions[strip[start + i]];
            const auto x = static_cast<float>(point.x);
            const auto y = static_cast<float>(point.y);
            chunk.vertices.push_back({x, y, static_cast<float>(out.x), static_cast<float>(out.y)});
            chunk.vertices.push_back({x, y, static_cast<float>(-out.x), static_cast<float>(-out.y)});
        }
        for (std::size_t i = 0; i + 1 < pieceSize; ++i) {
            const auto left = static_cast<std::uint16_t>(base + 2 * i);
            const auto right = static_cast<std::uint16_t>(left + 1);
            const auto nextLeft = static_cast<std::uint16_t>(left + 2);
            const auto nextRight = static_cast<std::uint16_t>(left + 3);
            for (const std::uint16_t index : {left, right, nextLeft, right, nextRight, nextLeft}) {
                appendIndex(chunkIndex, index);
            }
        }
        start += pieceSize - 1;
    }
}

}  // namespace alarmmap
