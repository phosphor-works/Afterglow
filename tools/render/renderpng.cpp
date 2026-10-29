// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Renders a terminal byte stream to a PNG through the real renderer: feeds the
// bytes to a terminal, snapshots it and draws it offscreen on the GPU.
//
//   afterglow_renderpng [--cols N] [--rows N] [--dpr F] [--size PT] input|- output.png
//
// Shaders: --shader REF (window chain), --pane-shader REF and the layer
// chains --background-shader, --text-shader and --cursor-shader, repeatable;
// --param REF:ID=VALUE overrides a pack parameter. --time sets iTime, and
// --signal CH=VALUE@AGE sets iSignal[CH] as if set AGE seconds earlier.
// --wake-from COL,ROW renders a frame with the cursor there --wake-age
// seconds earlier, so cursor effects show a jump.

#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/GridRenderer.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Terminal.h>
#include <shaders/ShaderLibrary.h>

#include <QCommandLineParser>
#include <QFile>
#include <QGuiApplication>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

class Host : public AfterglowVt::ITerminalHost
{
public:
    void writeToPty(std::string_view) override { }
    bool decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height,
                   std::vector<std::uint8_t>& rgba) override
    {
        QImage img = QImage::fromData(reinterpret_cast<const uchar*>(png.data()), int(png.size()), "PNG");
        if (img.isNull()) {
            return false;
        }
        img.convertTo(QImage::Format_RGBA8888);
        width = std::uint32_t(img.width());
        height = std::uint32_t(img.height());
        rgba.resize(std::size_t(width) * height * 4);
        for (int y = 0; y < img.height(); ++y) {
            std::memcpy(rgba.data() + std::size_t(y) * width * 4, img.constScanLine(y), std::size_t(width) * 4);
        }
        return true;
    }
};

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("cols"), QStringLiteral("Columns."), QStringLiteral("n"), QStringLiteral("80")});
    parser.addOption({QStringLiteral("rows"), QStringLiteral("Rows."), QStringLiteral("n"), QStringLiteral("24")});
    parser.addOption({QStringLiteral("dpr"), QStringLiteral("Device pixel ratio."), QStringLiteral("f"), QStringLiteral("1")});
    parser.addOption({QStringLiteral("size"), QStringLiteral("Font size in points."), QStringLiteral("pt"),
                      QStringLiteral("11")});
    parser.addOption({QStringLiteral("shader"), QStringLiteral("Window shader (repeatable)."), QStringLiteral("ref")});
    parser.addOption({QStringLiteral("pane-shader"), QStringLiteral("Pane shader (repeatable)."), QStringLiteral("ref")});
    parser.addOption({QStringLiteral("background-shader"), QStringLiteral("Background layer shader (repeatable)."),
                      QStringLiteral("ref")});
    parser.addOption({QStringLiteral("text-shader"), QStringLiteral("Text layer shader (repeatable)."), QStringLiteral("ref")});
    parser.addOption({QStringLiteral("cursor-shader"), QStringLiteral("Cursor layer shader (repeatable)."),
                      QStringLiteral("ref")});
    parser.addOption({QStringLiteral("param"), QStringLiteral("Shader parameter."), QStringLiteral("ref:id=value")});
    parser.addOption({QStringLiteral("time"), QStringLiteral("Shader time in seconds."), QStringLiteral("s"),
                      QStringLiteral("10")});
    parser.addOption({QStringLiteral("wake-from"), QStringLiteral("Previous cursor cell."), QStringLiteral("col,row")});
    parser.addOption({QStringLiteral("split"), QStringLiteral("Show the terminal twice, side by side; the left pane focused.")});
    parser.addOption({QStringLiteral("family"), QStringLiteral("Font family."), QStringLiteral("name")});
    parser.addOption({QStringLiteral("weight"), QStringLiteral("Font weight (variable fonts)."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("feature"), QStringLiteral("OpenType feature, replacing the defaults (repeatable; \"none\" for none)."),
                      QStringLiteral("tag")});
    parser.addOption({QStringLiteral("signal"), QStringLiteral("Shader signal set AGE seconds ago (repeatable)."),
                      QStringLiteral("ch=value[@age]")});
    parser.addOption({QStringLiteral("wake-age"), QStringLiteral("Seconds since the cursor moved."),
                      QStringLiteral("s"), QStringLiteral("0.1")});
    parser.addOption({QStringLiteral("opacity"), QStringLiteral("Background opacity."), QStringLiteral("f"),
                      QStringLiteral("1")});
    parser.addOption({QStringLiteral("unfocused"), QStringLiteral("Render the pane unfocused.")});
    parser.addPositionalArgument(QStringLiteral("input"), QStringLiteral("Byte stream, or - for stdin."));
    parser.addPositionalArgument(QStringLiteral("output"), QStringLiteral("PNG to write."));
    parser.process(app);
    const QStringList args = parser.positionalArguments();
    if (args.size() != 2) {
        parser.showHelp(1);
    }

    QFile in;
    const bool opened = args[0] == QLatin1String("-") ? in.open(stdin, QIODevice::ReadOnly)
                                                      : (in.setFileName(args[0]), in.open(QIODevice::ReadOnly));
    if (!opened) {
        std::fprintf(stderr, "cannot read %s\n", qPrintable(args[0]));
        return 1;
    }
    const QByteArray bytes = in.readAll();

    AfterglowRender::FontSpec spec;
    spec.sizePoints = parser.value(QStringLiteral("size")).toFloat();
    if (parser.isSet(QStringLiteral("family"))) {
        spec.regular.family = parser.value(QStringLiteral("family")).toStdString();
        spec.bold.family = spec.italic.family = spec.boldItalic.family = spec.regular.family;
    }
    if (parser.isSet(QStringLiteral("weight"))) {
        spec.regular.weight = parser.value(QStringLiteral("weight")).toDouble();
    }
    if (parser.isSet(QStringLiteral("feature"))) {
        spec.features.clear();
        for (const QString& f : parser.values(QStringLiteral("feature"))) {
            if (f != QLatin1String("none")) {
                spec.features.push_back(f.toStdString());
            }
        }
    }
    AfterglowRender::GpuDevice device(spec);
    device.rhi(nullptr);
    AfterglowRender::WindowRenderer renderer(device, nullptr);
    renderer.setDevicePixelRatio(parser.value(QStringLiteral("dpr")).toDouble());
    const auto& metrics = renderer.fonts().fonts->metrics();

    const int cols = parser.value(QStringLiteral("cols")).toInt();
    const int rows = parser.value(QStringLiteral("rows")).toInt();
    Host host;
    AfterglowVt::Terminal terminal(cols, rows, host);
    terminal.resize(cols, rows, metrics.cellWidth, metrics.cellHeight);
    terminal.feed(std::string_view(bytes.constData(), std::size_t(bytes.size())));

    QVariantMap params;
    for (const QString& p : parser.values(QStringLiteral("param"))) {
        const qsizetype colon = p.indexOf(QLatin1Char(':'));
        const qsizetype eq = p.indexOf(QLatin1Char('='), colon);
        if (colon < 0 || eq < 0) {
            std::fprintf(stderr, "bad --param %s\n", qPrintable(p));
            return 1;
        }
        const QString ref = p.left(colon);
        QVariantMap overrides = params.value(ref).toMap();
        const QString value = p.mid(eq + 1);
        bool numeric = false;
        const double number = value.toDouble(&numeric);
        overrides.insert(p.mid(colon + 1, eq - colon - 1),
                         numeric                                  ? QVariant(number)
                             : value == QLatin1String("true")     ? QVariant(true)
                             : value == QLatin1String("false")    ? QVariant(false)
                                                                  : QVariant(value));
        params.insert(ref, overrides);
    }
    const Afterglow::ShaderLibrary::Refs refs{
        parser.values(QStringLiteral("background-shader")), parser.values(QStringLiteral("text-shader")),
        parser.values(QStringLiteral("cursor-shader")), parser.values(QStringLiteral("pane-shader")),
        parser.values(QStringLiteral("shader"))};
    const auto wanted = refs.background.size() + refs.text.size() + refs.cursor.size() + refs.pane.size()
        + refs.window.size();
    if (wanted > 0) {
        Afterglow::ShaderLibrary library;
        AfterglowRender::ShaderChains chains = library.build(refs, params);
        const auto got = chains.background.size() + chains.text.size() + chains.cursor.size() + chains.pane.size()
            + chains.window.size();
        if (got != std::size_t(wanted)) {
            std::fprintf(stderr, "a shader failed to load\n");
            return 1;
        }
        renderer.setShaderChains(std::move(chains));
    }

    AfterglowRender::PaneSnapshot snapshot;
    snapshot.capture(terminal);
    const QSize size(cols * metrics.cellWidth, rows * metrics.cellHeight);
    const auto alpha = std::uint8_t(std::lround(std::clamp(parser.value(QStringLiteral("opacity")).toDouble(), 0.0, 1.0) * 255));
    AfterglowRender::WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(QPoint(0, 0), size);
    pane.params.contentOrigin = QPoint(0, 0);
    pane.params.focused = !parser.isSet(QStringLiteral("unfocused"));
    pane.params.backgroundAlpha = alpha;
    for (const QString& spec : parser.values(QStringLiteral("signal"))) {
        const QStringList kv = spec.split(QLatin1Char('='));
        const QStringList va = kv.value(1).split(QLatin1Char('@'));
        const int channel = kv.value(0).toInt();
        if (kv.size() != 2 || channel < 0 || channel >= int(pane.params.signals.size())) {
            std::fprintf(stderr, "bad --signal %s\n", qPrintable(spec));
            return 1;
        }
        const auto age = std::chrono::duration<float>(va.value(1, QStringLiteral("0")).toFloat());
        pane.params.signals[std::size_t(channel)] = {
            va.value(0).toFloat(),
            std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(age)};
    }
    const std::uint32_t clear = (snapshot.defaultBg & 0x00FFFFFFu) | (std::uint32_t(alpha) << 24);
    const float time = parser.value(QStringLiteral("time")).toFloat();
    if (parser.isSet(QStringLiteral("wake-from"))) {
        const QStringList at = parser.value(QStringLiteral("wake-from")).split(QLatin1Char(','));
        const int x = snapshot.cursorX;
        const int y = snapshot.cursorY;
        snapshot.cursorX = at.value(0).toInt();
        snapshot.cursorY = at.value(1).toInt();
        renderer.setClockOverride(time - parser.value(QStringLiteral("wake-age")).toFloat());
        renderer.renderToImage(size, {pane}, {}, clear);
        snapshot.cursorX = x;
        snapshot.cursorY = y;
        ++snapshot.version;
    }
    renderer.setClockOverride(time);
    std::vector<AfterglowRender::WindowRenderer::Pane> panes{pane};
    std::vector<std::pair<QRect, std::uint32_t>> overlays;
    QSize frameSize = size;
    AfterglowRender::PaneSnapshot twin = snapshot;
    if (parser.isSet(QStringLiteral("split"))) {
        // Two panes with a divider between, as a split window shows them.
        const int divider = 2;
        frameSize = QSize(size.width() * 2 + divider, size.height());
        AfterglowRender::WindowRenderer::Pane right = pane;
        right.snapshot = &twin;
        right.params.rect = QRect(QPoint(size.width() + divider, 0), size);
        right.params.contentOrigin = right.params.rect.topLeft();
        right.params.focused = false;
        panes.push_back(right);
        overlays.push_back({QRect(size.width(), 0, divider, size.height()), 0xFF0E0706u});
    }
    const QImage image = renderer.renderToImage(frameSize, panes, overlays, clear);
    if (image.isNull() || !image.save(args[1])) {
        std::fprintf(stderr, "rendering failed\n");
        return 1;
    }
    return 0;
}
