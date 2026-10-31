// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/ShaderProgram.h>

#include <rhi/qshaderbaker.h>

#include <QRegularExpression>
#include <QVector3D>
#include <QVector2D>
#include <limits>
#include <span>
#include <cmath>
#include <stdexcept>

namespace AfterglowRender {

QByteArray ShaderProgram::prefix()
{
    QByteArray result = QByteArrayLiteral(R"GLSL(#version 440

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
    vec4  iTextureRegion;
    vec4  iWindowRegion;
    vec2  iWindowSize;
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
// Previous buffer in the buffer pass; current buffer in the image pass.
// iChannel0 remains the terminal/layer input in both passes.
// AFTERGLOW_EXTRA_SAMPLERS

// Explicit conversions of straight RGB. Terminal input/output is encoded sRGB,
// premultiplied alpha. Buffer data has no implicit transfer function.
vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow(max((c + 0.055) / 1.055, vec3(0.0)), vec3(2.4)), step(vec3(0.04045), c));
}
vec3 linearToSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0/2.4)) - 0.055, step(vec3(0.0031308), c));
}
vec4 toLinearPremultiplied(vec4 c) {
    return c.a > 0.0 ? vec4(srgbToLinear(c.rgb / c.a) * c.a, c.a) : vec4(0.0);
}
vec4 toSrgbPremultiplied(vec4 c) {
    return c.a > 0.0 ? vec4(linearToSrgb(c.rgb / c.a) * c.a, c.a) : vec4(0.0);
}
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

// Every input is local to this region. Hardware sampler modes therefore work
// for buffers and image parameters even when passed through a user helper.
vec4 _afterglowTexture(sampler2D s, vec2 uv)
{
    return texture(s, vec2(uv.x, 1.0-uv.y));
}
#define texture _afterglowTexture
)GLSL");
    QByteArray samplers;
    for (int i = 0; i < MaxShaderBuffers; ++i)
        samplers += "layout(binding = " + QByteArray::number(4 + i) + ") uniform sampler2D iBuffer" + QByteArray::number(i) + ";\n";
    samplers += "#define iBuffer iBuffer0\n";
    for (int i = 0; i < MaxShaderImages; ++i)
        samplers += "layout(binding = " + QByteArray::number(4 + MaxShaderBuffers + i) + ") uniform sampler2D uTexture" + QByteArray::number(i) + ";\n";
    samplers += "layout(binding = 16) uniform sampler2D iDepth;\n#define uDepthBuffer iDepth\n";
    result.replace("// AFTERGLOW_EXTRA_SAMPLERS", samplers);
    return result;
}

namespace {
const QByteArray Suffix = QByteArrayLiteral(R"GLSL(

void main()
{
    mainImage(_afterglowFragColor, vec2(gl_FragCoord.x - iRegion.x, iRegion.y + iRegion.w - gl_FragCoord.y));
}
)GLSL");
}

bool ShaderProgram::loadObj(const QByteArray& data, ShaderGeometry& geometry, QString* error)
{
    constexpr int MaxVertices = 3 * 1024 * 1024;
    auto fail = [&](const char* message) { if (error) *error = QString::fromLatin1(message); return false; };
    if (data.size() > 32 * 1024 * 1024) return fail("OBJ exceeds 32 MiB");
    std::vector<QVector3D> positions, normals;
    std::vector<QVector2D> uvs;
    std::vector<ShaderMeshVertex> vertices;
    auto index = [](const QByteArray& text, std::size_t count) -> int {
        bool ok = false;
        const qint64 value = text.toLongLong(&ok);
        if (!ok || value == 0 || value > qint64(count) || value < -qint64(count)) return -1;
        return int(value > 0 ? value - 1 : qint64(count) + value);
    };
    for (qsizetype start = 0; start < data.size();) {
        const auto newline = data.indexOf('\n', start);
        const auto end = newline < 0 ? data.size() : newline;
        if (end - start > 4096) return fail("OBJ line exceeds 4096 bytes");
        const auto raw = data.mid(start, end - start);
        start = end + 1;
        const auto line = raw.left(raw.indexOf('#') < 0 ? raw.size() : raw.indexOf('#')).simplified();
        if (line.isEmpty()) continue;
        const auto fields = line.split(' ');
        const auto type = fields.front();
        if (type == "v" || type == "vn" || type == "vt") {
            const int components = type == "vt" ? 2 : 3;
            if (fields.size() != components + 1) return fail("OBJ needs XYZ positions/normals and UV coordinates");
            float values[3]{};
            for (int i = 0; i < components; ++i) {
                bool ok = false; values[i] = fields[i + 1].toFloat(&ok);
                if (!ok || !std::isfinite(values[i]) || std::abs(values[i]) > 1e6f) return fail("Invalid OBJ coordinate");
            }
            if (type == "v") positions.emplace_back(values[0], values[1], values[2]);
            else if (type == "vn") normals.emplace_back(values[0], values[1], values[2]);
            else uvs.emplace_back(values[0], values[1]);
        } else if (type == "f") {
            // Require explicit triangles: fan triangulation corrupts concave polygons.
            if (fields.size() != 4) return fail("OBJ faces must be triangulated before export");
            if (vertices.size() + 3 > MaxVertices) return fail("OBJ exceeds triangle budget");
            ShaderMeshVertex triangle[3]{};
            bool hasNormal[3]{};
            for (int i = 0; i < 3; ++i) {
                const auto refs = fields[i + 1].split('/');
                if (refs.size() > 3) return fail("Invalid OBJ face reference");
                const int p = index(refs[0], positions.size());
                if (p < 0) return fail("Invalid OBJ position index");
                const auto v = positions[p];
                triangle[i].position[0] = v.x(); triangle[i].position[1] = v.y(); triangle[i].position[2] = v.z();
                if (refs.size() > 1 && !refs[1].isEmpty()) {
                    const int t = index(refs[1], uvs.size());
                    if (t < 0) return fail("Invalid OBJ UV index");
                    triangle[i].uv[0] = uvs[t].x(); triangle[i].uv[1] = uvs[t].y();
                }
                if (refs.size() > 2 && !refs[2].isEmpty()) {
                    const int n = index(refs[2], normals.size());
                    if (n < 0 || normals[n].isNull()) return fail("Invalid OBJ normal");
                    const auto normal = normals[n].normalized();
                    triangle[i].normal[0] = normal.x(); triangle[i].normal[1] = normal.y(); triangle[i].normal[2] = normal.z();
                    hasNormal[i] = true;
                }
            }
            const auto point = [&](int i) { const auto* p = triangle[i].position; return QVector3D(p[0], p[1], p[2]); };
            const auto normal = QVector3D::crossProduct(point(1) - point(0), point(2) - point(0)).normalized();
            for (int i = 0; i < 3; ++i) {
                if (!hasNormal[i]) { triangle[i].normal[0] = normal.x(); triangle[i].normal[1] = normal.y(); triangle[i].normal[2] = normal.z(); }
                vertices.push_back(triangle[i]);
            }
        } else if (type != "o" && type != "g" && type != "s" && type != "mtllib" && type != "usemtl") {
            return fail("Unsupported OBJ record");
        }
    }
    if (vertices.empty()) return fail("OBJ has no triangles");
    geometry.vertices = std::move(vertices);
    geometry.vertexCount = int(geometry.vertices.size());
    return true;
}

std::shared_ptr<ShaderProgram> ShaderProgram::compile(const ShaderProgramSource& source, QString* error)
{
    auto buffers = source.buffers;
    if (!source.bufferCode.isEmpty()) {
        if (!buffers.empty()) { if (error) *error = QStringLiteral("Use buffers or bufferCode, not both"); return nullptr; }
        buffers.push_back({source.bufferCode, source.bufferFeedback, source.halfFloatBuffer, source.bufferScale, source.bufferNearest, ShaderWrap::Clamp, false, {}});
    }
    if (buffers.size() > MaxShaderBuffers) { if (error) *error = QStringLiteral("Too many buffer passes"); return nullptr; }
    for (const auto& buffer : buffers) {
        if (buffer.code.isEmpty() || !std::isfinite(buffer.scale) || buffer.scale < .125f || buffer.scale > 1.f) {
            if (error) *error = QStringLiteral("Buffer needs code and scale 0.125..1");
            return nullptr;
        }
    }
    const auto& geometry = source.geometry;
    if (!geometry.vertexCode.isEmpty()) {
        if (!buffers.empty() || geometry.vertexCount < 3 || geometry.vertexCount > 3 * 1024 * 1024
            || (geometry.samples != 1 && geometry.samples != 2 && geometry.samples != 4 && geometry.samples != 8)
            || geometry.vertexCount % 3 || geometry.instances < 1 || geometry.instances > 4096
            || qint64(geometry.vertexCount) * geometry.instances > 12 * 1024 * 1024
            || (!geometry.vertices.empty() && geometry.vertices.size() != std::size_t(geometry.vertexCount))) {
            if (error) *error = QStringLiteral("Invalid mesh size, instance count or nested passes");
            return nullptr;
        }
        for (const auto& v : geometry.vertices) {
            for (const auto& values : {std::span(v.position), std::span(v.normal)})
                for (float value : values) if (!std::isfinite(value)) {
                    if (error) *error = QStringLiteral("Non-finite mesh vertex");
                    return nullptr;
                }
            for (float value : v.uv) if (!std::isfinite(value)) {
                if (error) *error = QStringLiteral("Non-finite mesh UV");
                return nullptr;
            }
        }
    } else if (geometry.vertexCount || !geometry.vertices.empty() || geometry.instances != 1 || geometry.samples != 1) {
        if (error) *error = QStringLiteral("Mesh requires a vertex shader");
        return nullptr;
    }
    QByteArray full = prefix();
    if (source.depthOutput) full += "\nlayout(location=1) out vec4 depthBuffer;\n";
    else full += "\nvec4 depthBuffer;\n";
    full += source.parameterPreamble;
    full += "\n#line 1\n";
    full += source.code;
    auto suffix = Suffix;
    suffix.replace("    mainImage(", "    depthBuffer = vec4(0.0);\n    mainImage(");
    full += suffix;

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
    program->m_depthOutput = source.depthOutput;
    program->m_geometry = geometry;
    if (!geometry.vertexCode.isEmpty()) {
        auto vertex = prefix();
        vertex.replace("layout(location = 0) out vec4 _afterglowFragColor;", "");
        if (!geometry.vertices.empty()) vertex += R"GLSL(
layout(location=0) in vec3 meshPosition;
layout(location=1) in vec3 meshNormal;
layout(location=2) in vec2 meshUV;
)GLSL";
        vertex += source.parameterPreamble + "\n" + geometry.vertexCode;
        vertex += "\nvoid main() { mainMesh(gl_Position); }\n";
        baker.setSourceString(vertex, QShader::VertexStage, source.id + QStringLiteral(" mesh vertex"));
        program->m_vertexShader = baker.bake();
        if (!program->m_vertexShader.isValid()) { if (error) *error = baker.errorMessage(); return nullptr; }
    }
    if (!source.vertexCode.isEmpty() && geometry.vertexCode.isEmpty()) {
        auto vertex = prefix();
        vertex.replace("layout(location = 0) out vec4 _afterglowFragColor;", "");
        vertex += source.parameterPreamble + "\n" + source.vertexCode;
        vertex += R"GLSL(
void main() {
    const vec2 corners[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
    vec2 uv = corners[gl_VertexIndex];
    vec4 position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    mainVertex(position, uv);
    vec2 center = vec2(iRegion.x + iRegion.z * 0.5, iRegion.y + iRegion.w * 0.5);
    position.xy = position.xy * iRegion.zw / iFramebufferSize + (center * 2.0 / iFramebufferSize - 1.0) * position.w;
    gl_Position = position;
}
)GLSL";
        baker.setSourceString(vertex, QShader::VertexStage, source.id + QStringLiteral(" vertex"));
        program->m_vertexShader = baker.bake();
        if (!program->m_vertexShader.isValid()) { if (error) *error = baker.errorMessage(); return nullptr; }
    }
    program->m_id = source.id;
    static const QRegularExpression usesTime(QStringLiteral("\\b(?:iTime|iTimeDelta|iFrame|iFrameRate)\\b"));
    static const QRegularExpression usesHistory(QStringLiteral("\\biPreviousText\\b"));
    static const QRegularExpression usesHdr(QStringLiteral("^[ \\t]*#[ \\t]*define[ \\t]+AFTERGLOW_HDR[ \\t]+1[ \\t]*(?://[^\\r\\n]*)?\\r?$"),
                                            QRegularExpression::MultilineOption);
    static const QRegularExpression usesAudio(QStringLiteral("^[ \t]*#[ \t]*define[ \t]+AFTERGLOW_AUDIO[ \t]+1[ \t]*(?://[^\r\n]*)?\r?$"),
                                              QRegularExpression::MultilineOption);
    static const QRegularExpression usesWindowBackground(
        QStringLiteral(R"(^[ \t]*#[ \t]*define[ \t]+AFTERGLOW_WINDOW_BACKGROUND[ \t]+1[ \t]*(?://[^\r\n]*)?\r?$)"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression usesScrollback(QStringLiteral("\\biScrollback\\b"));
    const QString code = QString::fromUtf8(source.code + "\n" + source.vertexCode + "\n" + geometry.vertexCode);
    program->m_readsPreviousText = code.contains(usesHistory);
    program->m_requiresHdr = code.contains(usesHdr);
    program->m_requiresAudio = code.contains(usesAudio);
    program->m_windowBackground = code.contains(usesWindowBackground);
    program->m_readsScrollback = code.contains(usesScrollback);
    program->m_animated = code.contains(usesTime) || program->m_readsPreviousText;
    program->m_params = source.params;
    program->m_colors = source.colors;
    program->m_images = source.images;
    program->m_imagePaths = source.imagePaths;
    for (const auto& settings : buffers) {
        ShaderProgramSource bufferSource;
        bufferSource.id = source.id;
        bufferSource.parameterPreamble = source.parameterPreamble;
        bufferSource.params = source.params;
        bufferSource.colors = source.colors;
        bufferSource.images = source.images;
        bufferSource.imagePaths = source.imagePaths;
        bufferSource.code = settings.code;
        bufferSource.geometry = settings.geometry;
        bufferSource.depthOutput = source.useDepthBuffer || settings.depthBuffer;
        auto buffer = compile(bufferSource, error);
        if (!buffer) return nullptr;
        program->m_animated |= settings.feedback || buffer->animated();
        program->m_readsPreviousText |= buffer->readsPreviousText();
        program->m_readsScrollback |= buffer->readsScrollback();
        program->m_requiresAudio |= buffer->requiresAudio();
        program->m_requiresHdr |= buffer->requiresHdr();
        program->m_buffers.push_back(std::move(buffer));
        program->m_bufferSettings.push_back(settings);
    }
    if (!buffers.empty()) {
        const auto& first = buffers.front();
        program->m_bufferFeedback = first.feedback;
        program->m_halfFloatBuffer = first.halfFloat;
        program->m_bufferScale = first.scale;
        program->m_bufferNearest = first.nearest;
    }
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
        if (!p) throw std::runtime_error("passthrough shader failed: " + error.toStdString());
        return p;
    }();
    return instance;
}

} // namespace AfterglowRender
