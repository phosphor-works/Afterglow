// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Renders a terminal byte stream to a PNG through the real renderer: feeds the
// bytes to a terminal, snapshots it and draws it offscreen on the GPU.
//
//   afterglow_renderpng [--cols N] [--rows N] [--dpr F] [--size PT] input|- output.png

#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/GridRenderer.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Terminal.h>

#include <QCommandLineParser>
#include <QFile>
#include <QGuiApplication>
#include <QImage>

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

    AfterglowRender::PaneSnapshot snapshot;
    snapshot.capture(terminal);
    const QSize size(cols * metrics.cellWidth, rows * metrics.cellHeight);
    AfterglowRender::WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(QPoint(0, 0), size);
    pane.params.contentOrigin = QPoint(0, 0);
    pane.params.focused = true;
    const QImage image = renderer.renderToImage(size, {pane}, {}, snapshot.defaultBg | 0xFF000000u);
    if (image.isNull() || !image.save(args[1])) {
        std::fprintf(stderr, "rendering failed\n");
        return 1;
    }
    return 0;
}
