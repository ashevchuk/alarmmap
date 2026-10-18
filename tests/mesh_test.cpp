// Usage: mesh_test <tests/data directory>

#include "mesh_builder.hpp"

#include <cmath>
#include <iostream>
#include <source_location>
#include <string>
#include <vector>

using namespace alarmmap;

namespace {

int failures = 0;

void check(bool condition, const std::string& what,
           std::source_location location = std::source_location::current()) {
    if (condition) return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": FAILED: " << what << '\n';
}

bool near(double actual, double expected, double tolerance = 1e-6) {
    return std::abs(actual - expected) <= tolerance;
}

std::size_t indexCount(const std::vector<MeshSpan>& spans) {
    std::size_t total = 0;
    for (const MeshSpan& span : spans) total += span.indexCount;
    return total;
}

// Every index points at a vertex of its own chunk.
bool indicesInRange(const MeshBuilder& builder) {
    for (const MeshChunk& chunk : builder.chunks()) {
        for (const std::uint16_t index : chunk.indices) {
            if (index >= chunk.vertices.size()) return false;
        }
    }
    return true;
}

double extrusionLength(const MeshVertex& vertex) { return std::hypot(vertex.extrudeX, vertex.extrudeY); }

void testTriangles() {
    MeshBuilder builder;
    const std::size_t item = builder.beginItem();
    builder.addTriangles({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, {0, 1, 2, 0, 2, 3});
    check(item == 0, "first item has index 0");
    check(builder.chunks().size() == 1, "small mesh fits one chunk");
    check(builder.chunks()[0].vertices.size() == 4, "shared vertices are stored once");
    check(builder.chunks()[0].indices.size() == 6, "two triangles");
    check(indexCount(builder.items()[0]) == 6, "item covers both triangles");
    check(extrusionLength(builder.chunks()[0].vertices[0]) == 0, "triangle vertices do not extrude");
}

void testStraightLine() {
    MeshBuilder builder;
    builder.beginItem();
    builder.addPolyline({{0, 0}, {1, 0}, {2, 0}}, false);
    const MeshChunk& chunk = builder.chunks().at(0);
    check(chunk.vertices.size() == 6, "two vertices per point");
    check(chunk.indices.size() == 12, "two triangles per segment");
    for (const MeshVertex& vertex : chunk.vertices) {
        check(near(std::abs(vertex.extrudeY), 1) && near(vertex.extrudeX, 0),
              "a straight line extrudes one half width sideways");
    }
}

void testCorner() {
    MeshBuilder builder;
    builder.beginItem();
    builder.addPolyline({{0, 0}, {1, 0}, {1, 1}}, false);
    const MeshVertex& corner = builder.chunks().at(0).vertices.at(2);
    check(near(extrusionLength(corner), std::sqrt(2.0)), "a right-angle miter is sqrt(2) half widths long");
    check(near(std::abs(corner.extrudeX), std::abs(corner.extrudeY)), "the miter follows the bisector");
}

void testSharpTurnIsClamped() {
    MeshBuilder builder;
    builder.beginItem();
    builder.addPolyline({{0, 0}, {1, 0}, {0, 0.01}}, false);
    const MeshVertex& turn = builder.chunks().at(0).vertices.at(2);
    check(extrusionLength(turn) <= MeshBuilder::kMaxMiter + 1e-6, "a sharp turn does not spike out");
}

void testClosedRing() {
    MeshBuilder builder;
    builder.beginItem();
    builder.addPolyline({{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0, 0}}, true);
    const MeshChunk& chunk = builder.chunks().at(0);
    check(chunk.vertices.size() == 10, "a ring of four points returns to the first");
    check(chunk.indices.size() == 24, "four segments");
    check(near(extrusionLength(chunk.vertices[0]), std::sqrt(2.0)), "the closing joint is mitered too");
    check(near(chunk.vertices[0].extrudeX, chunk.vertices[8].extrudeX) &&
              near(chunk.vertices[0].extrudeY, chunk.vertices[8].extrudeY),
          "the ring ends with the joint it starts with");
}

void testDegenerateLines() {
    MeshBuilder builder;
    builder.beginItem();
    builder.addPolyline({{0, 0}, {0, 0}}, false);
    builder.addPolyline({{0, 0}, {1, 0}}, true);
    check(builder.chunks().empty(), "lines without length or area add nothing");
}

void testTrianglesSplitAcrossChunks() {
    // A strip of triangles over more vertices than one chunk holds.
    std::vector<MapPoint> vertices;
    std::vector<std::uint32_t> indices;
    const std::uint32_t columns = 40000;
    for (std::uint32_t i = 0; i < columns; ++i) {
        vertices.push_back({static_cast<double>(i), 0});
        vertices.push_back({static_cast<double>(i), 1});
    }
    for (std::uint32_t i = 0; i + 1 < columns; ++i) {
        const std::uint32_t a = 2 * i;
        for (const std::uint32_t index : {a, a + 1, a + 2, a + 1, a + 3, a + 2}) indices.push_back(index);
    }
    MeshBuilder builder;
    builder.beginItem();
    builder.addTriangles(vertices, indices);
    check(builder.chunks().size() == 2, "80000 vertices take two chunks");
    for (const MeshChunk& chunk : builder.chunks()) {
        check(chunk.vertices.size() <= MeshBuilder::kMaxChunkVertices, "chunks stay addressable");
    }
    check(indicesInRange(builder), "indices stay within their chunk");
    check(indexCount(builder.items()[0]) == indices.size(), "no triangle is lost at the split");
    check(builder.items()[0].size() == 2, "the item spans both chunks");
}

void testLongLineSplitsAcrossChunks() {
    std::vector<MapPoint> points;
    for (int i = 0; i < 40000; ++i) points.push_back({static_cast<double>(i), (i % 2) * 0.5});
    MeshBuilder builder;
    builder.beginItem();
    builder.addPolyline(points, false);
    check(builder.chunks().size() == 2, "a long line continues in a second chunk");
    check(indicesInRange(builder), "line indices stay within their chunk");
    check(indexCount(builder.items()[0]) == (points.size() - 1) * 6, "every segment is drawn once");
}

void testItemsAreSeparate() {
    MeshBuilder builder;
    builder.beginItem();
    builder.addTriangles({{0, 0}, {1, 0}, {1, 1}}, {0, 1, 2});
    builder.beginItem();
    builder.addPolyline({{0, 0}, {1, 0}}, false);
    check(builder.items().size() == 2, "two items");
    check(indexCount(builder.items()[0]) == 3 && indexCount(builder.items()[1]) == 6, "each item has its own indices");
    check(builder.items()[1][0].firstIndex == 3, "the second item starts after the first");
}

}  // namespace

int main(int argc, char**) {
    if (argc != 2) {
        std::cerr << "usage: mesh_test <data directory>\n";
        return 2;
    }
    try {
        testTriangles();
        testStraightLine();
        testCorner();
        testSharpTurnIsClamped();
        testClosedRing();
        testDegenerateLines();
        testTrianglesSplitAcrossChunks();
        testLongLineSplitsAcrossChunks();
        testItemsAreSeparate();
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
