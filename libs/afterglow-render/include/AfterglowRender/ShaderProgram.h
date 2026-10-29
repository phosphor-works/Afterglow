// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/afterglowrender_export.h>

#include <QByteArray>
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
};
static_assert(offsetof(TerminalShaderUniforms, iMouse) == 160);
static_assert(offsetof(TerminalShaderUniforms, iCurrentCursor) == 208);
static_assert(offsetof(TerminalShaderUniforms, iCurrentCursorStyle) == 272);
static_assert(offsetof(TerminalShaderUniforms, iPalette) == 304);
static_assert(offsetof(TerminalShaderUniforms, iBackgroundColor) == 4400);
static_assert(offsetof(TerminalShaderUniforms, iRegion) == 4496);
static_assert(offsetof(TerminalShaderUniforms, customParams) == 4528);
static_assert(offsetof(TerminalShaderUniforms, iBackgroundOpacity) == 4912);
static_assert(sizeof(TerminalShaderUniforms) == 4928);

/// What a shader is built from: user code defining
/// `void mainImage(out vec4 fragColor, in vec2 fragCoord)` with includes
/// already expanded, plus parameter accessors and values.
struct ShaderProgramSource
{
    QString id; ///< for messages
    QByteArray code;
    QByteArray parameterPreamble; ///< `#define p_<id> customParams[...]` lines
    std::array<float, 32> params{};
    std::array<float, 64> colors{};
};

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

    const QShader& shader() const noexcept { return m_shader; }
    const QString& id() const noexcept { return m_id; }
    /// The shader reads iTime, so the window must keep rendering frames.
    bool animated() const noexcept { return m_animated; }
    const std::array<float, 32>& params() const noexcept { return m_params; }
    const std::array<float, 64>& colors() const noexcept { return m_colors; }

private:
    QShader m_shader;
    QString m_id;
    bool m_animated = false;
    std::array<float, 32> m_params{};
    std::array<float, 64> m_colors{};
};

using ShaderProgramPtr = std::shared_ptr<const ShaderProgram>;

} // namespace AfterglowRender
