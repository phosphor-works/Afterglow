// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/afterglowrender_export.h>

#include <PhosphorShaders/CustomParamsKey.h>
#include <PhosphorShaders/ShaderParamPreamble.h>
#include <QImage>
#include <vector>
#include <QByteArray>
#include <QHash>
#include <QString>
#include <rhi/qshader.h>

#include <array>
#include <cstdint>
#include <memory>

namespace AfterglowRender {

/// Uniform block of terminal post-processing shaders (std140, binding 0).
/// The leading part is Ghostty's custom shader block field for field, so its
/// shaders run unmodified; afterglow's additions follow.
struct alignas(16) TerminalShaderUniforms
{
    float iResolution[3];
    float iTime;
    float iTimeDelta;
    float iFrameRate;
    std::int32_t iFrame;
    float pad0;
    float iChannelTime[4][4]; // float[4], std140 stride 16
    float iChannelResolution[4][4]; // vec3[4]
    float iMouse[4];
    float iDate[4];
    float iSampleRate;
    float pad1[3];
    float iCurrentCursor[4];
    float iPreviousCursor[4];
    float iCurrentCursorColor[4];
    float iPreviousCursorColor[4];
    std::int32_t iCurrentCursorStyle;
    std::int32_t iPreviousCursorStyle;
    std::int32_t iCursorVisible;
    float iTimeCursorChange;
    float iTimeFocus;
    std::int32_t iFocus;
    float pad2[2];
    float iPalette[256][4]; // vec3[256]
    float iBackgroundColor[4]; // vec3
    float iForegroundColor[4];
    float iCursorColor[4];
    float iCursorText[4];
    float iSelectionForegroundColor[4];
    float iSelectionBackgroundColor[4];
    // afterglow additions
    float iRegion[4]; ///< the shaded region in framebuffer pixels, top-left origin
    float iFramebufferSize[2];
    float iCellSize[2];
    float customParams[8][4]; ///< pack parameters (phosphor slot layout)
    float customColors[16][4];
    float iBackgroundOpacity; ///< alpha of the default background (window translucency)
    float _pad0[3];
    /// Values scripts set on the pane: (value, iTime when set, 0, 0); unset
    /// channels are (0, -1e6).
    float iSignal[8][4];
    float iAudioLevels[4]; ///< bass, mids, treble and overall RMS, 0..1
    std::int32_t iAudioSpectrumSize; ///< 64 while receiving, otherwise 0
    float audioPad[3];
    /// Pane rows, combined current-tab rows, eased current-tab rows, reserved (0).
    float iScrollback[4];
    float iTextureRegion[4]; ///< normalized source rectangle, unchanged by buffer scaling
};
static_assert(offsetof(TerminalShaderUniforms, iMouse) == 160);
static_assert(offsetof(TerminalShaderUniforms, iCurrentCursor) == 208);
static_assert(offsetof(TerminalShaderUniforms, iCurrentCursorStyle) == 272);
static_assert(offsetof(TerminalShaderUniforms, iPalette) == 304);
static_assert(offsetof(TerminalShaderUniforms, iBackgroundColor) == 4400);
static_assert(offsetof(TerminalShaderUniforms, iRegion) == 4496);
static_assert(offsetof(TerminalShaderUniforms, customParams) == 4528);
static_assert(offsetof(TerminalShaderUniforms, iBackgroundOpacity) == 4912);
static_assert(offsetof(TerminalShaderUniforms, iSignal) == 4928);
static_assert(offsetof(TerminalShaderUniforms, iAudioLevels) == 5056);
static_assert(offsetof(TerminalShaderUniforms, iScrollback) == 5088);
static_assert(sizeof(TerminalShaderUniforms) == 5120);

/// What a shader is built from: user code defining
/// `void mainImage(out vec4 fragColor, in vec2 fragCoord)` with includes
/// already expanded, plus parameter accessors and values.
inline constexpr int MaxShaderBuffers = PhosphorShaders::kMaxBufferPasses;
inline constexpr int MaxShaderImages = PhosphorShaders::kMaxImageSlots;
enum class ShaderWrap { Clamp, Repeat, Mirror };
struct ShaderBufferSource {
    QByteArray code;
    bool feedback = false;
    bool halfFloat = true;
    float scale = 1.0f;
    bool nearest = false;
    ShaderWrap wrap = ShaderWrap::Clamp;
    bool depthBuffer = false;
};
struct ShaderProgramSource
{
    QString id; ///< for messages
    QByteArray code;
    QByteArray vertexCode; ///< void mainVertex(inout vec4 position, in vec2 uv)
    bool useDepthBuffer = false; ///< auxiliary RGBA depth data on buffer passes
    bool depthOutput = false; ///< internal compiled MRT pass
    std::vector<ShaderBufferSource> buffers;
    QByteArray bufferCode; ///< compatibility shorthand for the first buffer
    bool bufferFeedback = false;
    bool halfFloatBuffer = true;
    float bufferScale = 1.0f;
    bool bufferNearest = false;
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
    void apply(TerminalShaderUniforms& u) const
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

    /// The GLSL put in front of user code: uniform block, iChannel0, and the
    /// y-up texture wrapper of Ghostty's convention.
    static QByteArray prefix();

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
    int vertexCount() const noexcept { return m_vertexShader.isValid() ? 6 : 3; }
    const QShader& shader() const noexcept { return m_shader; }
    const QString& id() const noexcept { return m_id; }
    const QString& instanceId() const noexcept { return m_instanceId; }
    std::shared_ptr<const ShaderProgram> instance(const QString& id) const
    {
        auto copy = std::make_shared<ShaderProgram>(*this);
        copy->m_instanceId = id;
        return copy;
    }
    /// The shader reads iTime, so the window must keep rendering frames.
    bool animated() const noexcept { return m_animated; }
    bool readsScrollback() const noexcept { return m_readsScrollback; }
    bool readsPreviousText() const noexcept { return m_readsPreviousText; }
    /// Opt-in `#define AFTERGLOW_HDR 1`: retain highlights above 1 between passes.
    bool requiresHdr() const noexcept { return m_requiresHdr; }
    /// Opt-in `#define AFTERGLOW_AUDIO 1`: capture system playback while visible.
    bool requiresAudio() const noexcept { return m_requiresAudio; }
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
    QString m_id;
    QString m_instanceId;
    bool m_animated = false;
    bool m_readsPreviousText = false;
    bool m_readsScrollback = false;
    bool m_requiresHdr = false;
    bool m_requiresAudio = false;
    std::array<float, 32> m_params{};
    std::array<float, 64> m_colors{};
};

using ShaderProgramPtr = std::shared_ptr<const ShaderProgram>;

} // namespace AfterglowRender
