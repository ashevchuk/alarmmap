#pragma once

#include "projection.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace alarmmap {

// A point on the map plane and the direction a line vertex is pushed out by
// half the line width; zero for triangle vertices. Lines keep their width
// out of the mesh, so one mesh serves every zoom.
struct MeshVertex {
    float x = 0;
    float y = 0;
    float extrudeX = 0;
    float extrudeY = 0;
};

// Part of a mesh that 16-bit indices can address.
struct MeshChunk {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint16_t> indices;
};

// A run of indices in one chunk.
struct MeshSpan {
    std::size_t chunk = 0;
    std::size_t firstIndex = 0;
    std::size_t indexCount = 0;
};

// Collects triangles and polylines into chunks of at most kMaxChunkVertices
// vertices, grouped into items that can be drawn on their own.
class MeshBuilder {
public:
    static constexpr std::size_t kMaxChunkVertices = 65536;
    // A miter is at most this many half widths long; sharper turns are cut
    // short instead of spiking out.
    static constexpr float kMaxMiter = 4;

    // Starts an item; what is added next belongs to it. Returns its index.
    std::size_t beginItem();
    // Adds a triangle list over `vertices`.
    void addTriangles(const std::vector<MapPoint>& vertices, const std::vector<std::uint32_t>& indices);
    // Adds a polyline as a strip of quads with miter joins; `closed` also
    // joins the last point to the first.
    void addPolyline(const std::vector<MapPoint>& points, bool closed);

    const std::vector<MeshChunk>& chunks() const { return chunks_; }
    const std::vector<std::vector<MeshSpan>>& items() const { return items_; }

private:
    // The chunk to append to, with room for `vertexCount` more vertices.
    MeshChunk& chunkWithRoom(std::size_t vertexCount);
    void appendIndex(std::size_t chunk, std::uint16_t index);

    std::vector<MeshChunk> chunks_;
    std::vector<std::vector<MeshSpan>> items_;
};

}  // namespace alarmmap
