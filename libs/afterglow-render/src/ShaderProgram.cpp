// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/ShaderProgram.h>

#include <PhosphorShaders/ShaderEntryPoint.h>
#include <PhosphorShaders/ShaderIncludeResolver.h>
#include <PhosphorShaders/ShaderParamPreamble.h>

#include <rhi/qshaderbaker.h>

#include <QMatrix4x4>
#include <QRegularExpression>
#include <QVector3D>
#include <QVector2D>
#include <algorithm>
#include <limits>
#include <span>
#include <cmath>
#include <stdexcept>

namespace AfterglowRender {

namespace {

// Each stage's prologue and generated main(), Phosphor's entry scaffold for
// the terminal family. A stage that defines main() itself gets neither.
QString fragmentPrologue(bool depthOutput)
{
    return QStringLiteral("#version 450\n#include <terminal_lib.glsl>\nlayout(location = 0) out vec4 fragColor;\n")
        + (depthOutput ? QStringLiteral("layout(location = 1) out vec4 depthBuffer;\n") : QStringLiteral("vec4 depthBuffer;\n"));
}

const QList<PhosphorShaders::EntryCandidate>& fragmentEntry()
{
    static const QList<PhosphorShaders::EntryCandidate> candidates{{QStringLiteral("pTerminal"), QStringLiteral(
        "void main() {\n"
        "    depthBuffer = vec4(0.0);\n"
        "    fragColor = pTerminal((gl_FragCoord.xy - uCanvas.xy) / uCanvas.zw);\n"
        "}\n")}};
    return candidates;
}

QString vertexPrologue()
{
    return QStringLiteral("#version 450\n#define AFTERGLOW_TERMINAL_VERTEX_STAGE 1\n#include <terminal_lib.glsl>\n");
}

const QList<PhosphorShaders::EntryCandidate>& vertexEntry()
{
    // The canvas quad, deformed by the pack, then placed over the canvas in
    // its render target. Scissoring still keeps it to the canvas.
    static const QList<PhosphorShaders::EntryCandidate> candidates{{QStringLiteral("pVertex"), QStringLiteral(
        "void main() {\n"
        "    const vec2 corners[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));\n"
        "    vec4 position = pVertex(corners[gl_VertexIndex]);\n"
        "    vec2 center = uCanvas.xy + uCanvas.zw * 0.5;\n"
        "    position.xy = position.xy * uCanvas.zw / uTargetSize + (center * 2.0 / uTargetSize - 1.0) * position.w;\n"
        "    gl_Position = position;\n"
        "}\n")}};
    return candidates;
}

QString meshPrologue(bool imported)
{
    return vertexPrologue() + (imported ? QStringLiteral(
        "layout(location = 0) in vec3 meshPosition;\n"
        "layout(location = 1) in vec3 meshNormal;\n"
        "layout(location = 2) in vec2 meshUV;\n") : QString());
}

const QList<PhosphorShaders::EntryCandidate>& meshEntry()
{
    static const QList<PhosphorShaders::EntryCandidate> candidates{
        {QStringLiteral("pMesh"), QStringLiteral("void main() {\n    gl_Position = pMesh();\n}\n")}};
    return candidates;
}

/// Completes, expands and bakes one stage. @p expanded receives the GLSL that
/// was baked, for the caller's checks.
QShader bakeStage(const QByteArray& code, const QString& prologue, const QList<PhosphorShaders::EntryCandidate>& entry,
                  QShader::Stage stage, const QString& directory, const QStringList& includeDirs,
                  const QByteArray& preamble, const QString& name, QString& expanded, QString* error)
{
    const QString assembled = PhosphorShaders::assembleEntryPoint(QString::fromUtf8(code), prologue, entry);
    QString includeError;
    expanded = PhosphorShaders::ShaderIncludeResolver::expandIncludes(
        assembled, directory, QStringList{ShaderProgram::contractDirectory()} + includeDirs, &includeError);
    if (!includeError.isEmpty()) {
        if (error) *error = includeError;
        return {};
    }
    if (!preamble.isEmpty()) expanded = PhosphorShaders::spliceAfterVersion(expanded, QString::fromUtf8(preamble));
    QShaderBaker baker;
    // Vulkan only: SPIR-V, nothing else to translate (and fail) to.
    baker.setGeneratedShaders({{QShader::SpirvShader, QShaderVersion(100)}});
    baker.setGeneratedShaderVariants({QShader::StandardShader});
    baker.setSourceString(expanded.toUtf8(), stage, name);
    QShader shader = baker.bake();
    if (!shader.isValid() && error) *error = baker.errorMessage();
    return shader;
}

/// @p expanded without the terminal contract's own text, so that what the
/// contract declares is not mistaken for what the pack uses. Every contract
/// file is one include guard, opened and closed by name. Comments go too: a
/// note that mentions iTime does not make a pack render every frame.
QString authorText(const QString& expanded)
{
    static const QRegularExpression comment(QStringLiteral(R"(//[^\n]*|/\*.*?\*/)"),
                                            QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression open(QStringLiteral(R"(^\s*#ifndef\s+AFTERGLOW_TERMINAL_\w+_GLSL\s*$)"));
    static const QRegularExpression close(QStringLiteral(R"(^\s*#endif\s*//\s*AFTERGLOW_TERMINAL_\w+_GLSL\s*$)"));
    QString result;
    int depth = 0;
    for (const QString& line : expanded.split(QLatin1Char('\n'))) {
        if (open.match(line).hasMatch()) {
            ++depth;
        } else if (depth > 0 && close.match(line).hasMatch()) {
            --depth;
        } else if (depth == 0) {
            result += line + QLatin1Char('\n');
        }
    }
    return result.remove(comment);
}

/// Whether one of the opt-in contract modules was included.
bool includesModule(const QString& expanded, QLatin1String guard)
{
    return expanded.contains(QLatin1String("#define ") + guard);
}

} // namespace

QString ShaderProgram::contractDirectory()
{
    return QStringLiteral(":/afterglow/shaders");
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
            // v x y z, with an optional w or an RGB vertex colour (ignored);
            // vn x y z; vt u, with optional v and w (w ignored).
            const qsizetype count = fields.size() - 1;
            if (type == "v" && count != 3 && count != 4 && count != 6)
                return fail("OBJ positions need x y z, optionally w or r g b");
            if (type == "vn" && count != 3) return fail("OBJ normals need x y z");
            if (type == "vt" && (count < 1 || count > 3)) return fail("OBJ texture coordinates need u, optionally v and w");
            const int components = type == "vt" ? int(std::min<qsizetype>(count, 2)) : 3;
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
            // A face of zero area has no direction: give it +Z rather than a
            // zero normal, which normalize() turns into NaN.
            auto normal = QVector3D::crossProduct(point(1) - point(0), point(2) - point(0)).normalized();
            if (normal.isNull()) normal = QVector3D(0, 0, 1);
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

void ShaderProgram::sceneMatrices(const std::vector<ShaderSceneObject>& objects, const std::array<float, 32>& params,
                                  double time, TerminalSceneUniforms& out)
{
    std::array<QMatrix4x4, MaxSceneObjects> world;
    const auto count = std::min<std::size_t>(objects.size(), MaxSceneObjects);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& object = objects[i];
        auto component = [&](const std::array<ShaderSceneValue, 3>& values, int axis) { return values[axis].resolve(params); };
        // Spin is bounded to one turn so the angle keeps its precision.
        auto angle = [&](int axis) {
            return float(std::fmod(double(component(object.rotate, axis)) + double(component(object.spin, axis)) * time, 360.0));
        };
        QMatrix4x4 local;
        local.translate(component(object.translate, 0), component(object.translate, 1), component(object.translate, 2));
        local.rotate(angle(2), 0, 0, 1);
        local.rotate(angle(1), 0, 1, 0);
        local.rotate(angle(0), 1, 0, 0);
        local.scale(component(object.scale, 0), component(object.scale, 1), component(object.scale, 2));
        world[i] = object.parent >= 0 && std::size_t(object.parent) < i ? world[std::size_t(object.parent)] * local : local;
        std::copy_n(world[i].constData(), 16, out.uObjectMatrix[i]);
        const QMatrix4x4 normal = world[i].inverted().transposed();
        std::copy_n(normal.constData(), 16, out.uObjectNormalMatrix[i]);
    }
}

std::shared_ptr<ShaderProgram> ShaderProgram::compile(const ShaderProgramSource& source, QString* error)
{
    return compileStage(source, error, false);
}

std::shared_ptr<ShaderProgram> ShaderProgram::compileStage(const ShaderProgramSource& source, QString* error, bool bufferPass)
{
    // PostChain draws a final pass on targets with one colour attachment and
    // no depth: a mesh or a second output there fails pipeline creation.
    if (!bufferPass && (!source.geometry.vertexCode.isEmpty() || source.depthOutput)) {
        if (error) *error = QStringLiteral("Meshes and depth outputs need a buffer pass");
        return nullptr;
    }
    const auto& buffers = source.buffers;
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
    } else if (geometry.vertexCount || !geometry.vertices.empty() || geometry.instances != 1 || geometry.samples != 1
               || !geometry.objects.empty()) {
        if (error) *error = QStringLiteral("Mesh requires a vertex shader");
        return nullptr;
    }
    if (!geometry.objects.empty()) {
        if (geometry.objects.size() > std::size_t(MaxSceneObjects) || geometry.instances != 1 || geometry.vertices.empty()) {
            if (error) *error = QStringLiteral("Objects need imported meshes, one instance and at most %1 objects").arg(MaxSceneObjects);
            return nullptr;
        }
        for (std::size_t i = 0; i < geometry.objects.size(); ++i) {
            const auto& object = geometry.objects[i];
            bool valid = object.parent < int(i) && object.firstVertex >= 0 && object.vertexCount >= 0
                && object.vertexCount % 3 == 0 && qint64(object.firstVertex) + object.vertexCount <= geometry.vertexCount;
            for (const auto* values : {&object.translate, &object.rotate, &object.scale, &object.spin})
                for (const auto& value : *values) valid = valid && value.slot < 32 && std::isfinite(value.value);
            if (!valid) {
                if (error) *error = QStringLiteral("Invalid scene object %1").arg(i);
                return nullptr;
            }
        }
    }

    auto program = std::make_shared<ShaderProgram>();
    QString fragment, vertex;
    program->m_shader = bakeStage(source.code, fragmentPrologue(source.depthOutput), fragmentEntry(),
        QShader::FragmentStage, source.directory, source.includeDirs, source.parameterPreamble, source.id, fragment, error);
    if (!program->m_shader.isValid()) return nullptr;
    if (!geometry.vertexCode.isEmpty()) {
        program->m_vertexShader = bakeStage(geometry.vertexCode, meshPrologue(!geometry.vertices.empty()), meshEntry(),
            QShader::VertexStage, geometry.directory, source.includeDirs, source.parameterPreamble,
            source.id + QStringLiteral(" mesh vertex"), vertex, error);
        if (!program->m_vertexShader.isValid()) return nullptr;
    } else if (!source.vertexCode.isEmpty()) {
        program->m_vertexShader = bakeStage(source.vertexCode, vertexPrologue(), vertexEntry(), QShader::VertexStage,
            source.directory, source.includeDirs, source.parameterPreamble, source.id + QStringLiteral(" vertex"),
            vertex, error);
        if (!program->m_vertexShader.isValid()) return nullptr;
    }
    program->m_depthOutput = source.depthOutput;
    program->m_geometry = geometry;
    program->m_id = source.id;
    static const QRegularExpression usesTime(QStringLiteral(
        "\\b(?:iTime|iTimeDelta|iTimeHi|iFrame|iDate|timeAngle|timeCycle|timeSeconds)\\b"));
    static const QRegularExpression usesCursorAge(QStringLiteral("\\b(?:cursorAge|uEventAges)\\b"));
    static const QRegularExpression usesFocusAge(QStringLiteral("\\b(?:focusAge|uEventAges)\\b"));
    static const QRegularExpression usesSignalAge(QStringLiteral("\\b(?:signalAge|uSignal)\\b"));
    static const QRegularExpression usesScrollback(QStringLiteral("\\buScrollback\\b"));
    static const QRegularExpression usesMouse(QStringLiteral("\\b(?:iMouse|uWindowMouse)\\b"));
    const QString expanded = fragment + QLatin1Char('\n') + vertex;
    const QString author = authorText(expanded);
    program->m_readsPreviousText = includesModule(expanded, QLatin1String("AFTERGLOW_TERMINAL_HISTORY_GLSL"));
    program->m_requiresAudio = includesModule(expanded, QLatin1String("AFTERGLOW_TERMINAL_AUDIO_GLSL"));
    program->m_requiresHdr = source.highDynamicRange;
    program->m_windowBackground = source.windowBackground;
    program->m_readsScrollback = author.contains(usesScrollback);
    program->m_readsMouse = author.contains(usesMouse);
    program->m_animated = author.contains(usesTime) || program->m_readsPreviousText;
    program->m_eventAges = (author.contains(usesCursorAge) ? CursorAge : 0u)
        | (author.contains(usesFocusAge) ? FocusAge : 0u) | (author.contains(usesSignalAge) ? SignalAge : 0u);
    program->m_settleSeconds = source.settleSeconds;
    // Spinning objects move without the shader reading time.
    for (const auto& object : geometry.objects)
        for (const auto& value : object.spin) program->m_animated |= value.slot >= 0 || value.value != 0.0f;
    program->m_params = source.params;
    program->m_colors = source.colors;
    program->m_images = source.images;
    program->m_imagePaths = source.imagePaths;
    for (const auto& settings : buffers) {
        ShaderProgramSource bufferSource;
        bufferSource.id = source.id;
        bufferSource.directory = settings.directory;
        bufferSource.includeDirs = source.includeDirs;
        bufferSource.parameterPreamble = source.parameterPreamble;
        bufferSource.params = source.params;
        bufferSource.colors = source.colors;
        bufferSource.images = source.images;
        bufferSource.imagePaths = source.imagePaths;
        bufferSource.code = settings.code;
        bufferSource.geometry = settings.geometry;
        bufferSource.depthOutput = source.useDepthBuffer || settings.depthBuffer;
        auto buffer = compileStage(bufferSource, error, true);
        if (!buffer) return nullptr;
        program->m_animated |= settings.feedback || buffer->animated();
        program->m_eventAges |= buffer->eventAges();
        program->m_readsPreviousText |= buffer->readsPreviousText();
        program->m_readsScrollback |= buffer->readsScrollback();
        program->m_readsMouse |= buffer->readsMouse();
        program->m_requiresAudio |= buffer->requiresAudio();
        program->m_buffers.push_back(std::move(buffer));
        // The compiled pass holds the code and geometry; instance() copies
        // the settings for every occurrence, so keep them small.
        auto stored = settings;
        stored.geometry = {};
        stored.code.clear();
        program->m_bufferSettings.push_back(std::move(stored));
    }
    return program;
}

std::shared_ptr<ShaderProgram> ShaderProgram::passthrough()
{
    static std::shared_ptr<ShaderProgram> instance = [] {
        ShaderProgramSource src;
        src.id = QStringLiteral("passthrough");
        src.code = "vec4 pTerminal(vec2 uv)\n{\n    return terminalTexel(uv);\n}\n";
        QString error;
        auto p = compile(src, &error);
        if (!p) throw std::runtime_error("passthrough shader failed: " + error.toStdString());
        return p;
    }();
    return instance;
}

} // namespace AfterglowRender
