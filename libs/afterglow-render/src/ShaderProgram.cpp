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
};

#define CURSORSTYLE_BLOCK        0
#define CURSORSTYLE_BLOCK_HOLLOW 1
#define CURSORSTYLE_BAR          2
#define CURSORSTYLE_UNDERLINE    3
#define CURSORSTYLE_LOCK         4

layout(binding = 1) uniform sampler2D iChannel0;
layout(location = 0) out vec4 _afterglowFragColor;

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
    program->m_animated = QString::fromUtf8(source.code).contains(usesTime);
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
