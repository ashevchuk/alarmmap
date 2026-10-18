#include "gpu_geometry.hpp"

#include "render_util.hpp"

#include <raymath.h>
#include <rlgl.h>

#include <stdexcept>
#include <utility>

namespace {

// raylib binds these attribute names to locations 0 and 1 in every shader.
constexpr unsigned int kPositionLocation = 0;
constexpr unsigned int kExtrudeLocation = 1;

constexpr const char* kVertexShader = R"(#version 330
in vec2 vertexPosition;
in vec2 vertexTexCoord;
uniform mat4 mvp;
uniform float halfWidth;

void main() {
    gl_Position = mvp * vec4(vertexPosition + vertexTexCoord * halfWidth, 0.0, 1.0);
}
)";

constexpr const char* kFragmentShader = R"(#version 330
uniform vec4 color;
out vec4 finalColor;

void main() {
    finalColor = color;
}
)";

}  // namespace

GpuGeometry::GpuGeometry(const alarmmap::MeshBuilder& mesh) : items_(mesh.items()) {
    for (const alarmmap::MeshChunk& chunk : mesh.chunks()) {
        Buffers buffers;
        buffers.vertexArray = rlLoadVertexArray();
        if (buffers.vertexArray == 0) {
            release();
            throw std::runtime_error("vertex arrays are not supported");
        }
        rlEnableVertexArray(buffers.vertexArray);
        buffers.vertexBuffer =
            rlLoadVertexBuffer(chunk.vertices.data(),
                               static_cast<int>(chunk.vertices.size() * sizeof(alarmmap::MeshVertex)), false);
        constexpr int kStride = sizeof(alarmmap::MeshVertex);
        rlSetVertexAttribute(kPositionLocation, 2, RL_FLOAT, false, kStride, 0);
        rlEnableVertexAttribute(kPositionLocation);
        rlSetVertexAttribute(kExtrudeLocation, 2, RL_FLOAT, false, kStride, 2 * sizeof(float));
        rlEnableVertexAttribute(kExtrudeLocation);
        buffers.indexBuffer = rlLoadVertexBufferElement(
            chunk.indices.data(), static_cast<int>(chunk.indices.size() * sizeof(std::uint16_t)), false);
        rlDisableVertexArray();
        chunks_.push_back(buffers);
    }
}

GpuGeometry::~GpuGeometry() { release(); }

GpuGeometry::GpuGeometry(GpuGeometry&& other) noexcept
    : chunks_(std::exchange(other.chunks_, {})), items_(std::move(other.items_)) {}

GpuGeometry& GpuGeometry::operator=(GpuGeometry&& other) noexcept {
    if (this != &other) {
        release();
        chunks_ = std::exchange(other.chunks_, {});
        items_ = std::move(other.items_);
    }
    return *this;
}

void GpuGeometry::release() {
    for (const Buffers& buffers : chunks_) {
        rlUnloadVertexBuffer(buffers.indexBuffer);
        rlUnloadVertexBuffer(buffers.vertexBuffer);
        rlUnloadVertexArray(buffers.vertexArray);
    }
    chunks_.clear();
}

GeometryRenderer::GeometryRenderer()
    : shader_(loadShader(kVertexShader, kFragmentShader, "geometry")),
      mvpLocation_(GetShaderLocation(shader_, "mvp")),
      colorLocation_(GetShaderLocation(shader_, "color")),
      halfWidthLocation_(GetShaderLocation(shader_, "halfWidth")) {}

GeometryRenderer::~GeometryRenderer() { UnloadShader(shader_); }

void GeometryRenderer::begin(Color color, float lineWidth) const {
    // Whatever raylib has batched so far lies beneath this geometry.
    rlDrawRenderBatchActive();
    rlEnableShader(shader_.id);
    rlSetUniformMatrix(mvpLocation_, MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection()));
    const float rgba[] = {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
    rlSetUniform(colorLocation_, rgba, RL_SHADER_UNIFORM_VEC4, 1);
    const float halfWidth = lineWidth / 2;
    rlSetUniform(halfWidthLocation_, &halfWidth, RL_SHADER_UNIFORM_FLOAT, 1);
}

void GeometryRenderer::draw(const GpuGeometry& geometry, std::size_t item) const {
    drawSpans(geometry, geometry.items_.at(item));
}

void GeometryRenderer::drawAll(const GpuGeometry& geometry) const {
    for (const auto& spans : geometry.items_) drawSpans(geometry, spans);
}

void GeometryRenderer::end() const {
    rlDisableVertexArray();
    rlDisableShader();
}

void GeometryRenderer::drawSpans(const GpuGeometry& geometry,
                                 const std::vector<alarmmap::MeshSpan>& spans) const {
    for (const alarmmap::MeshSpan& span : spans) {
        rlEnableVertexArray(geometry.chunks_[span.chunk].vertexArray);
        rlDrawVertexArrayElements(static_cast<int>(span.firstIndex), static_cast<int>(span.indexCount), nullptr);
    }
}
