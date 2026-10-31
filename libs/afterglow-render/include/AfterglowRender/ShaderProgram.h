// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/afterglowrender_export.h>

#include <PhosphorShaders/BaseUniforms.h>
#include <PhosphorShaders/CustomParamsKey.h>
#include <PhosphorShaders/ShaderParamPreamble.h>
#include <QImage>
#include <vector>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <rhi/qshader.h>

#include <array>
#include <cstdint>
#include <memory>

namespace AfterglowRender {

/// The terminal tail of the uniform block (std140), after Phosphor's base
/// region. Mirrors data/shaders/shared/terminal_uniforms.glsl field for field;
/// that file documents each member.
struct alignas(16) TerminalUniformsTail
{
    float uCanvas[4];
    float uTargetSize[2];
    float uCellSize[2];
    float uTerminalRect[4];
    float uWindowSize[2];
    float uWindowMouse[2];
    float uCursorRect[4];
    float uPreviousCursorRect[4];
    float uCursorColor[4];
    float uPreviousCursorColor[4];
    std::int32_t uCursorState[4];
    float uEventAges[4];
    float uBackgroundColor[4];
    float uForegroundColor[4];
    float uThemeCursorColor[4];
    float uThemeCursorTextColor[4];
    float uSelectionBackgroundColor[4];
    float uSelectionForegroundColor[4];
    float uAudioLevels[4];
    float uScrollback[4];
    float uSignal[8][4];
    float uPalette[256][4];
};

/// Uniform block of terminal shaders (std140, binding 0): Phosphor's base
/// region, then the terminal tail.
struct alignas(16) TerminalUniforms
{
    PhosphorShaders::BaseUniforms base;
    TerminalUniformsTail terminal;
};

static_assert(offsetof(TerminalUniforms, terminal) == sizeof(PhosphorShaders::BaseUniforms));
static_assert(offsetof(TerminalUniforms, terminal) + offsetof(TerminalUniformsTail, uTerminalRect) == 704);
static_assert(offsetof(TerminalUniforms, terminal) + offsetof(TerminalUniformsTail, uCursorRect) == 736);
static_assert(offsetof(TerminalUniforms, terminal) + offsetof(TerminalUniformsTail, uCursorState) == 800);
static_assert(offsetof(TerminalUniforms, terminal) + offsetof(TerminalUniformsTail, uBackgroundColor) == 832);
static_assert(offsetof(TerminalUniforms, terminal) + offsetof(TerminalUniformsTail, uAudioLevels) == 928);
static_assert(offsetof(TerminalUniforms, terminal) + offsetof(TerminalUniformsTail, uSignal) == 960);
static_assert(offsetof(TerminalUniforms, terminal) + offsetof(TerminalUniformsTail, uPalette) == 1088);
static_assert(sizeof(TerminalUniforms) == 5184);

inline constexpr int MaxShaderBuffers = PhosphorShaders::kMaxBufferPasses;
inline constexpr int MaxShaderImages = PhosphorShaders::kMaxImageSlots;
enum class ShaderWrap { Clamp, Repeat, Mirror };
/// Triangle geometry. Empty vertices select procedural generation with gl_VertexIndex.
struct ShaderMeshVertex {
    float position[3];
    float normal[3];
    float uv[2];
};
inline constexpr int MaxSceneObjects = 64;
/// One transform component of a scene object: a constant, or the value of a
/// scalar pack parameter, so scripts and transitions can move objects.
struct ShaderSceneValue {
    float value = 0.0f;
    int slot = -1; ///< customParams lane (0..31), or -1 for the constant
    float resolve(const std::array<float, 32>& params) const { return slot >= 0 ? params[std::size_t(slot)] : value; }
};
/// A node of a mesh pass's object hierarchy. Its world transform is its
/// parent's world transform, then translate, rotate (degrees about X, then Y,
/// then Z, plus spin degrees per second) and scale. Objects without vertices
/// are pivots: they carry a transform for their children but draw nothing.
struct ShaderSceneObject {
    int parent = -1; ///< an earlier object, or -1 for a root
    int firstVertex = 0; ///< into the pass's vertices
    int vertexCount = 0;
    std::array<ShaderSceneValue, 3> translate{};
    std::array<ShaderSceneValue, 3> rotate{};
    std::array<ShaderSceneValue, 3> scale{ShaderSceneValue{1.0f}, ShaderSceneValue{1.0f}, ShaderSceneValue{1.0f}};
    std::array<ShaderSceneValue, 3> spin{};
};
/// The scene block (std140, binding 18): mirrors terminal_scene.glsl.
struct alignas(16) TerminalSceneUniforms {
    float uObjectMatrix[MaxSceneObjects][16];
    float uObjectNormalMatrix[MaxSceneObjects][16];
};
static_assert(sizeof(TerminalSceneUniforms) == 8192);
inline constexpr int MaxShaderPanes = 32;
/// The pane block (std140, binding 19): mirrors terminal_panes.glsl.
struct alignas(16) TerminalPaneUniforms {
    std::int32_t uPaneState[4] {0, -1, -1, 0}; ///< panes, focused, highlighted, overview open
    float uOverview[4] {0.0f, 0.0f, 1e6f, 1e6f}; ///< open amount, highlight position, ages since open/close and highlight move
    std::int32_t uTabState[4] {0, 0, 0, 0}; ///< tabs with cards, the current tab
    float uTabTransition[4] {1.0f, 1.0f, 1e6f, 0.0f}; ///< progress, direction, age, running
    float uPaneRect[MaxShaderPanes][4] {};
    float uPaneSource[MaxShaderPanes][4] {}; ///< uv rectangle in the card's source
    std::int32_t uPaneTab[MaxShaderPanes][4] {}; ///< tab, source (0 input, 1 tab, 2 atlas), index in tab
};
static_assert(sizeof(TerminalPaneUniforms) == 1600);
struct ShaderGeometry {
    QByteArray vertexCode; ///< defines `vec4 pMesh()`, the Vulkan clip position, or its own main()
    QString directory; ///< resolves the vertex code's quoted includes
    std::vector<ShaderMeshVertex> vertices;
    int vertexCount = 0;
    int instances = 1;
    int samples = 1; ///< 1, 2, 4 or 8 samples per pixel
    /// With objects, each object with vertices is drawn once and
    /// gl_InstanceIndex is its index; `instances` must be 1.
    std::vector<ShaderSceneObject> objects;
};
struct ShaderBufferSource {
    QByteArray code; ///< defines `vec4 pTerminal(vec2 uv)` or its own main()
    QString directory; ///< resolves the code's quoted includes
    bool feedback = false;
    bool halfFloat = true;
    float scale = 1.0f;
    bool nearest = false;
    ShaderWrap wrap = ShaderWrap::Clamp;
    bool depthBuffer = false;
    ShaderGeometry geometry;
};
/// What a terminal shader is built from: pack sources as written, which the
/// compiler completes with the terminal contract (terminal_lib.glsl and the
/// generated main()), expands and bakes. A stage that defines main() itself is
/// taken as complete and must include what it uses.
struct ShaderProgramSource
{
    QString id; ///< for messages
    QByteArray code; ///< the final pass: defines `vec4 pTerminal(vec2 uv)` or its own main()
    QByteArray vertexCode; ///< optional: defines `vec4 pVertex(vec2 uv)`, the canvas quad's clip position
    QString directory; ///< resolves quoted includes of code and vertexCode
    QStringList includeDirs; ///< searched for <...> includes after the terminal contract
    bool useDepthBuffer = false; ///< auxiliary RGBA depth data on buffer passes
    bool depthOutput = false; ///< internal compiled MRT pass
    ShaderGeometry geometry; ///< internal compiled mesh pass
    std::vector<ShaderBufferSource> buffers;
    bool highDynamicRange = false; ///< metadata `highDynamicRange`: keep values above 1 between passes
    bool windowBackground = false; ///< metadata `windowBackground`: one background for the whole window
    QByteArray parameterPreamble; ///< `#define p_<id> customParams[...]` lines
    std::array<float, 32> params{};
    std::array<float, 64> colors{};
    std::array<QImage, MaxShaderImages> images;
    std::array<QString, MaxShaderImages> imagePaths;
};

/// Sparse per-instance overrides; applied only while packing frame uniforms.
struct ShaderParameterValues
{
    std::array<float, 32> params{};
    std::array<float, 64> colors{};
    std::array<QImage, MaxShaderImages> images;
    std::array<QString, MaxShaderImages> imagePaths;
    std::uint8_t imageMask = 0;
    std::uint32_t paramMask = 0;
    std::uint16_t colorMask = 0;
    void merge(const ShaderParameterValues& other)
    {
        for (int i = 0; i < 32; ++i)
            if (other.paramMask & (std::uint32_t(1) << i)) params[i] = other.params[i];
        for (int i = 0; i < 16; ++i)
            if (other.colorMask & (1u << i))
                for (int c = 0; c < 4; ++c) colors[i * 4 + c] = other.colors[i * 4 + c];
        for (int i = 0; i < MaxShaderImages; ++i)
            if (other.imageMask & (1u << i)) { images[i] = other.images[i]; imagePaths[i] = other.imagePaths[i]; }
        imageMask |= other.imageMask;
        paramMask |= other.paramMask;
        colorMask |= other.colorMask;
    }
    void apply(PhosphorShaders::BaseUniforms& u) const
    {
        for (int i = 0; i < 32; ++i)
            if (paramMask & (std::uint32_t(1) << i)) u.customParams[i / 4][i % 4] = params[i];
        for (int i = 0; i < 16; ++i)
            if (colorMask & (1u << i))
                for (int c = 0; c < 4; ++c) u.customColors[i][c] = colors[i * 4 + c];
    }
};
using ShaderParameterMap = QHash<QString, ShaderParameterValues>;

/// A compiled terminal post-processing shader (SPIR-V for Vulkan).
class AFTERGLOWRENDER_EXPORT ShaderProgram
{
public:
    /// Compiles @p source. Returns nullptr and sets @p error on failure.
    static std::shared_ptr<ShaderProgram> compile(const ShaderProgramSource& source, QString* error);
    /// Copies the input through unchanged; ends a chain that has no window shader.
    static std::shared_ptr<ShaderProgram> passthrough();

    /// Where `#include <terminal_*.glsl>` resolves: the contract built into
    /// this library, searched before any other include directory.
    static QString contractDirectory();
    /// Read triangulated OBJ faces (v/vt/vn, negative indices supported).
    static bool loadObj(const QByteArray& data, ShaderGeometry& geometry, QString* error);
    /// The world and normal matrices of @p objects at @p time seconds, with
    /// @p params as the parameter values. Objects past MaxSceneObjects are ignored.
    static void sceneMatrices(const std::vector<ShaderSceneObject>& objects, const std::array<float, 32>& params,
                              float time, TerminalSceneUniforms& out);

    const ShaderProgram* buffer() const noexcept { return m_buffers.empty() ? nullptr : m_buffers.front().get(); }
    const auto& buffers() const noexcept { return m_buffers; }
    const ShaderBufferSource& bufferSettings(std::size_t i) const { return m_bufferSettings.at(i); }
    const auto& images() const noexcept { return m_images; }
    const auto& imagePaths() const noexcept { return m_imagePaths; }
    bool bufferFeedback() const noexcept { return m_bufferFeedback; }
    bool halfFloatBuffer() const noexcept { return m_halfFloatBuffer; }
    float bufferScale() const noexcept { return m_bufferScale; }
    bool bufferNearest() const noexcept { return m_bufferNearest; }
    const QShader& vertexShader() const noexcept { return m_vertexShader; }
    bool depthOutput() const noexcept { return m_depthOutput; }
    const ShaderGeometry& geometry() const noexcept { return m_geometry; }
    bool meshPass() const noexcept { return !m_geometry.vertexCode.isEmpty(); }
    int vertexCount() const noexcept { return meshPass() ? m_geometry.vertexCount : m_vertexShader.isValid() ? 6 : 3; }
    const QShader& shader() const noexcept { return m_shader; }
    const QString& id() const noexcept { return m_id; }
    const QString& instanceId() const noexcept { return m_instanceId; }
    std::shared_ptr<const ShaderProgram> instance(const QString& id) const
    {
        auto copy = std::make_shared<ShaderProgram>(*this);
        copy->m_instanceId = id;
        return copy;
    }
    /// The shader reads time or event ages, so the window must keep rendering frames.
    bool animated() const noexcept { return m_animated; }
    bool readsScrollback() const noexcept { return m_readsScrollback; }
    /// Includes terminal_history.glsl.
    bool readsPreviousText() const noexcept { return m_readsPreviousText; }
    /// Metadata `highDynamicRange`: retain highlights above 1 between passes.
    bool requiresHdr() const noexcept { return m_requiresHdr; }
    /// Includes terminal_audio.glsl: capture system playback while visible.
    bool requiresAudio() const noexcept { return m_requiresAudio; }
    /// Render a background once for the window, then clip it to the visible panes.
    bool windowBackground() const noexcept { return m_windowBackground; }
    const std::array<float, 32>& params() const noexcept { return m_params; }
    const std::array<float, 64>& colors() const noexcept { return m_colors; }

private:
    std::vector<std::shared_ptr<ShaderProgram>> m_buffers;
    std::vector<ShaderBufferSource> m_bufferSettings;
    std::array<QImage, MaxShaderImages> m_images;
    std::array<QString, MaxShaderImages> m_imagePaths;
    bool m_bufferFeedback = false;
    bool m_halfFloatBuffer = true;
    float m_bufferScale = 1.0f;
    bool m_bufferNearest = false;
    QShader m_shader;
    QShader m_vertexShader;
    bool m_depthOutput = false;
    ShaderGeometry m_geometry;
    QString m_id;
    QString m_instanceId;
    bool m_animated = false;
    bool m_readsPreviousText = false;
    bool m_readsScrollback = false;
    bool m_requiresHdr = false;
    bool m_requiresAudio = false;
    bool m_windowBackground = false;
    std::array<float, 32> m_params{};
    std::array<float, 64> m_colors{};
};

using ShaderProgramPtr = std::shared_ptr<const ShaderProgram>;

} // namespace AfterglowRender
