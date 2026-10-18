#pragma once

#include "mesh_builder.hpp"

#include <raylib.h>

#include <cstddef>
#include <vector>

// A MeshBuilder's chunks uploaded to GPU buffers once, so drawing them costs
// no CPU work per vertex. Requires an OpenGL context.
class GpuGeometry {
public:
    GpuGeometry() = default;
    explicit GpuGeometry(const alarmmap::MeshBuilder& mesh);
    ~GpuGeometry();
    GpuGeometry(GpuGeometry&& other) noexcept;
    GpuGeometry& operator=(GpuGeometry&& other) noexcept;
    GpuGeometry(const GpuGeometry&) = delete;
    GpuGeometry& operator=(const GpuGeometry&) = delete;

private:
    friend class GeometryRenderer;

    struct Buffers {
        unsigned int vertexArray = 0;
        unsigned int vertexBuffer = 0;
        unsigned int indexBuffer = 0;
    };

    void release();

    std::vector<Buffers> chunks_;
    std::vector<std::vector<alarmmap::MeshSpan>> items_;
};

// Draws GpuGeometry in map units under BeginMode2D, one colour per pass:
// begin(), any number of draw() calls, end(). Requires an OpenGL context.
class GeometryRenderer {
public:
    GeometryRenderer();
    ~GeometryRenderer();
    GeometryRenderer(const GeometryRenderer&) = delete;
    GeometryRenderer& operator=(const GeometryRenderer&) = delete;

    // `lineWidth` is the full width of lines in map units; triangles have no
    // extrusion and ignore it.
    void begin(Color color, float lineWidth = 0) const;
    void draw(const GpuGeometry& geometry, std::size_t item) const;
    void drawAll(const GpuGeometry& geometry) const;
    void end() const;

private:
    void drawSpans(const GpuGeometry& geometry, const std::vector<alarmmap::MeshSpan>& spans) const;

    Shader shader_{};
    int mvpLocation_ = -1;
    int colorLocation_ = -1;
    int halfWidthLocation_ = -1;
};
