// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/ShaderProgram.h>

#include <rhi/qshaderbaker.h>

#include <QRegularExpression>

namespace AfterglowRender {

QByteArray ShaderProgram::prefix()
{
    return QByteArrayLiteral(R"GLSL(#version 440

layout(std140, binding = 0) uniform Globals {
    vec3  iResolution;
    float iTime;
    float iTimeDelta;
    float iFrameRate;
    int   iFrame;
    float iChannelTime[4];
    vec3  iChannelResolution[4];
    vec4  iMouse;
    vec4  iDate;
    float iSampleRate;
    vec4  iCurrentCursor;
    vec4  iPreviousCursor;
    vec4  iCurrentCursorColor;
    vec4  iPreviousCursorColor;
    int   iCurrentCursorStyle;
    int   iPreviousCursorStyle;
    int   iCursorVisible;
    float iTimeCursorChange;
    float iTimeFocus;
    int   iFocus;
    vec3  iPalette[256];
    vec3  iBackgroundColor;
    vec3  iForegroundColor;
    vec3  iCursorColor;
    vec3  iCursorText;
    vec3  iSelectionForegroundColor;
    vec3  iSelectionBackgroundColor;
    vec4  iRegion;
    vec2  iFramebufferSize;
    vec2  iCellSize;
    vec4  customParams[8];
    vec4  customColors[16];
    float iBackgroundOpacity;
    vec4  iSignal[8];
    vec4  iAudioLevels;
    int   iAudioSpectrumSize;
    vec4  iScrollback;
};

#define CURSORSTYLE_BLOCK        0
#define CURSORSTYLE_BLOCK_HOLLOW 1
#define CURSORSTYLE_BAR          2
#define CURSORSTYLE_UNDERLINE    3
#define CURSORSTYLE_LOCK         4

layout(binding = 1) uniform sampler2D iChannel0;
// Previous frame's processed text layer, in the same region coordinates.
// Transparent on first use and after layout, theme or shader changes.
layout(binding = 2) uniform sampler2D iPreviousText;
layout(binding = 3) uniform sampler2D uAudioSpectrum;
layout(location = 0) out vec4 _afterglowFragColor;

// Spectrum coordinates are independent of pane and framebuffer coordinates.
float audioBar(int index)
{
    if (index < 0 || index >= iAudioSpectrumSize) return 0.0;
    return texelFetch(uAudioSpectrum, ivec2(index, 0), 0).r;
}
float audioBarSmooth(float position)
{
    float index = clamp(position, 0.0, 1.0) * float(max(iAudioSpectrumSize - 1, 0));
    int lo = int(floor(index));
    return mix(audioBar(lo), audioBar(min(lo + 1, iAudioSpectrumSize - 1)), fract(index));
}
float getBass() { return iAudioLevels.x; }
float getMids() { return iAudioLevels.y; }
float getTreble() { return iAudioLevels.z; }
float getOverall() { return iAudioLevels.w; }

#define texture2D texture

void mainImage(out vec4 fragColor, in vec2 fragCoord);

// Shaders see their region (a pane or the window) as a y-up image starting at
// (0, 0), as on Shadertoy and in Ghostty. The input texture is the whole
// framebuffer, top-left origin, so map region coordinates onto it. Defined
// before the macro, so the inner call is the built-in.
vec4 _afterglowTexture(sampler2D s, vec2 uv)
{
    vec2 px = vec2(iRegion.x + uv.x * iRegion.z, iRegion.y + (1.0 - uv.y) * iRegion.w);
    return texture(s, px / iFramebufferSize);
}
#define texture _afterglowTexture
)GLSL");
}

namespace {
const QByteArray Suffix = QByteArrayLiteral(R"GLSL(

void main()
{
    mainImage(_afterglowFragColor, vec2(gl_FragCoord.x - iRegion.x, iRegion.y + iRegion.w - gl_FragCoord.y));
}
)GLSL");
}

std::shared_ptr<ShaderProgram> ShaderProgram::compile(const ShaderProgramSource& source, QString* error)
{
    QByteArray full = prefix();
    full += source.parameterPreamble;
    full += "\n#line 1\n";
    full += source.code;
    full += Suffix;

    QShaderBaker baker;
    // Vulkan only: SPIR-V, nothing else to translate (and fail) to.
    baker.setGeneratedShaders({{QShader::SpirvShader, QShaderVersion(100)}});
    baker.setGeneratedShaderVariants({QShader::StandardShader});
    baker.setSourceString(full, QShader::FragmentStage, source.id);
    QShader shader = baker.bake();
    if (!shader.isValid()) {
        if (error) {
            *error = baker.errorMessage();
        }
        return nullptr;
    }
    auto program = std::make_shared<ShaderProgram>();
    program->m_shader = std::move(shader);
    program->m_id = source.id;
    static const QRegularExpression usesTime(QStringLiteral("\\biTime\\b"));
    static const QRegularExpression usesHistory(QStringLiteral("\\biPreviousText\\b"));
    static const QRegularExpression usesHdr(QStringLiteral("^[ \\t]*#[ \\t]*define[ \\t]+AFTERGLOW_HDR[ \\t]+1[ \\t]*(?://[^\\r\\n]*)?\\r?$"),
                                            QRegularExpression::MultilineOption);
    static const QRegularExpression usesAudio(QStringLiteral("^[ \t]*#[ \t]*define[ \t]+AFTERGLOW_AUDIO[ \t]+1[ \t]*(?://[^\r\n]*)?\r?$"),
                                              QRegularExpression::MultilineOption);
    static const QRegularExpression usesScrollback(QStringLiteral("\\biScrollback\\b"));
    const QString code = QString::fromUtf8(source.code);
    program->m_readsPreviousText = code.contains(usesHistory);
    program->m_requiresHdr = code.contains(usesHdr);
    program->m_requiresAudio = code.contains(usesAudio);
    program->m_readsScrollback = code.contains(usesScrollback);
    program->m_animated = code.contains(usesTime) || program->m_readsPreviousText;
    program->m_params = source.params;
    program->m_colors = source.colors;
    return program;
}

std::shared_ptr<ShaderProgram> ShaderProgram::passthrough()
{
    static std::shared_ptr<ShaderProgram> instance = [] {
        ShaderProgramSource src;
        src.id = QStringLiteral("passthrough");
        src.code = "void mainImage(out vec4 fragColor, in vec2 fragCoord)\n"
                   "{\n    fragColor = texture(iChannel0, fragCoord / iResolution.xy);\n}\n";
        QString error;
        auto p = compile(src, &error);
        Q_ASSERT_X(p, "ShaderProgram::passthrough", qPrintable(error));
        return p;
    }();
    return instance;
}

} // namespace AfterglowRender
