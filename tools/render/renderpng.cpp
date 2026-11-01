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
// --signal CH=VALUE@AGE sets uSignal[CH] as if set AGE seconds earlier.
// --wake-from COL,ROW renders a frame with the cursor there --wake-age
// seconds earlier, so cursor effects show a jump. Further options set the
// other renderer inputs (pointer, selection, search, glide, scroll shift,
// hovered link, blink phase, bidi, contrast, hinting, cell scale); --help
// lists them all.

#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/GridRenderer.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Search.h>
#include <AfterglowVt/Selection.h>
#include <AfterglowVt/Terminal.h>
#include <shaders/ShaderLibrary.h>

#include <QCommandLineParser>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QBuffer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>

namespace {

// The same limits as the application's decoder (Pane::decodeImage).
class Host : public AfterglowVt::ITerminalHost
{
public:
    void writeToPty(std::string_view) override { }
    bool decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height,
                   std::vector<std::uint8_t>& rgba) override
    {
        return decode(png, "PNG", width, height, rgba);
    }
    // iTerm2 inline images: any format Qt reads, as in the application.
    bool decodeImage(std::string_view data, std::uint32_t& width, std::uint32_t& height,
                     std::vector<std::uint8_t>& rgba) override
    {
        return decode(data, "", width, height, rgba);
    }

private:
    static bool decode(std::string_view data, const char* format, std::uint32_t& width, std::uint32_t& height,
                       std::vector<std::uint8_t>& rgba)
    {
        if (data.size() > AfterglowVt::GraphicsStore::DefaultQuota) return false;
        QByteArray bytes = QByteArray::fromRawData(data.data(), qsizetype(data.size()));
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, format);
        const auto size = reader.size();
        if (size.isEmpty() || size.width() > 10000 || size.height() > 10000
            || quint64(size.width()) * size.height() * 4 > AfterglowVt::GraphicsStore::DefaultQuota) return false;
        QImage img = reader.read();
        if (img.isNull() || img.size() != size) return false;
        img.convertTo(QImage::Format_RGBA8888);
        if (img.isNull()) return false;
        width = std::uint32_t(img.width());
        height = std::uint32_t(img.height());
        rgba.resize(std::size_t(width) * height * 4);
        for (int y = 0; y < img.height(); ++y) {
            std::memcpy(rgba.data() + std::size_t(y) * width * 4, img.constScanLine(y), std::size_t(width) * 4);
        }
        return true;
    }
};

/// Parses "A,B" (or "AxB" with @p separator) into two numbers within range.
bool parsePair(const QString& text, QChar separator, double low, double high, double& a, double& b)
{
    const QStringList parts = text.split(separator);
    bool aOk = false, bOk = false;
    if (parts.size() != 2) return false;
    a = parts[0].toDouble(&aOk);
    b = parts[1].toDouble(&bOk);
    return aOk && bOk && std::isfinite(a) && std::isfinite(b) && a >= low && a <= high && b >= low && b <= high;
}

int run(QGuiApplication& app)
{
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("cols"), QStringLiteral("Columns."), QStringLiteral("n"), QStringLiteral("80")});
    parser.addOption({QStringLiteral("rows"), QStringLiteral("Rows."), QStringLiteral("n"), QStringLiteral("24")});
    parser.addOption({QStringLiteral("dpr"), QStringLiteral("Device pixel ratio."), QStringLiteral("f"), QStringLiteral("1")});
    parser.addOption({QStringLiteral("size"), QStringLiteral("Font size in points."), QStringLiteral("pt"),
                      QStringLiteral("11")});
    parser.addOption({QStringLiteral("antialiasing"), QStringLiteral("Font coverage: grayscale, none, rgb or bgr."),
                      QStringLiteral("mode"), QStringLiteral("grayscale")});
    parser.addOption({QStringLiteral("text-gamma"), QStringLiteral("Glyph coverage gamma (0.5..3)."),
                      QStringLiteral("gamma"), QStringLiteral("1")});
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
    parser.addOption({QStringLiteral("frames"), QStringLiteral("Write numbered PNG frames for an animation."),
                      QStringLiteral("n"), QStringLiteral("1")});
    parser.addOption({QStringLiteral("fps"), QStringLiteral("Animation frames per second."),
                      QStringLiteral("n"), QStringLiteral("30")});
    parser.addOption({QStringLiteral("audio-bands"), QStringLiteral("Synthetic bass,mids,treble levels (0..1) for a deterministic preview. Nothing is captured."),
                      QStringLiteral("b,m,t")});
    parser.addOption({QStringLiteral("scrollback-lines"), QStringLiteral("Synthetic combined history rows for a deterministic preview."),
                      QStringLiteral("n")});
    parser.addOption({QStringLiteral("pixels"), QStringLiteral("Exact pane framebuffer size WIDTHxHEIGHT."), QStringLiteral("size")});
    parser.addOption({QStringLiteral("last-only"), QStringLiteral("Save only the final frame (benchmarking).")});
    parser.addOption({QStringLiteral("timings"), QStringLiteral("Report render/readback and PNG times, excluding the first frame (needs --frames 2 or more).")});
    parser.addOption({QStringLiteral("wake-from"), QStringLiteral("Previous cursor cell."), QStringLiteral("col,row")});
    parser.addOption({QStringLiteral("split"), QStringLiteral("Show the terminal twice, side by side. The left pane has focus.")});
    parser.addOption({QStringLiteral("grid"), QStringLiteral("Show the terminal in a grid of COLSxROWS panes. The first pane has focus."), QStringLiteral("size")});
    parser.addOption({QStringLiteral("package"), QStringLiteral("A script package folder whose shaders load as <package>.<pack> (repeatable)."), QStringLiteral("dir")});
    parser.addOption({QStringLiteral("overview"), QStringLiteral("Pane overview shader, shown open."), QStringLiteral("ref")});
    parser.addOption({QStringLiteral("overview-amount"), QStringLiteral("How far the overview is open, 0 to 1 (default 1)."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("highlight"), QStringLiteral("The highlighted pane in the overview, counting every tab (default 0)."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("tab-transition"), QStringLiteral("Tab transition shader, from a copy of the panes on a blue background."), QStringLiteral("ref")});
    parser.addOption({QStringLiteral("transition-progress"), QStringLiteral("Tab transition progress, 0 to 1 (default 0.5)."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("transition-direction"), QStringLiteral("1 toward a tab on the right (default), -1 toward one on the left."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("pane-effects"), QStringLiteral("Pane effects shader for --open-progress and --close-progress."), QStringLiteral("ref")});
    parser.addOption({QStringLiteral("open-progress"), QStringLiteral("The last pane is opening, 0 to 1, from --open-edge."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("open-edge"), QStringLiteral("The edge it opens from: 1 left (default), 2 top, 3 right, 4 bottom."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("context"), QStringLiteral("The last pane's context: 0 here, 1 remote, 2 elevated, 3 container."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("context-progress"), QStringLiteral("How far its context change has run, 0 to 1 (default 1)."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("pulse"), QStringLiteral("A pulse on the last pane: EFFECT:PROGRESS[:COLOR], effect 1 portal, 2 flash, 3 shake."), QStringLiteral("spec")});
    parser.addOption({QStringLiteral("close-progress"), QStringLiteral("The last pane has closed, 0 to 1, leaving its frame on a blue background. Needs --split or --grid."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("overview-position"), QStringLiteral("The highlight's position, a fractional card index (default the highlighted card)."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("tabs"), QStringLiteral("With --overview: N tabs with the same panes, the first current (default 1)."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("current-tab"), QStringLiteral("With --tabs: the current tab (default 0)."), QStringLiteral("n")});
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
    parser.addOption({QStringLiteral("overview-age"), QStringLiteral("Seconds since the overview opened (uOverview.z)."), QStringLiteral("s")});
    parser.addOption({QStringLiteral("highlight-age"), QStringLiteral("Seconds since the overview highlight moved (uOverview.w)."), QStringLiteral("s")});
    parser.addOption({QStringLiteral("transition-age"), QStringLiteral("Seconds since the tab switch started (uTabTransition.z)."), QStringLiteral("s")});
    parser.addOption({QStringLiteral("mouse"), QStringLiteral("The pointer in framebuffer pixels (iMouse)."), QStringLiteral("x,y")});
    parser.addOption({QStringLiteral("bidi"), QStringLiteral("Bidirectional text: off (default), auto, ltr or rtl."), QStringLiteral("mode")});
    parser.addOption({QStringLiteral("minimum-contrast"), QStringLiteral("Lowest text contrast ratio, 1 to 21 (default 1)."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("bold-is-bright"), QStringLiteral("Bold text in colours 0-7 uses colours 8-15.")});
    parser.addOption({QStringLiteral("hinting"), QStringLiteral("Font hinting: none, light (default) or full."), QStringLiteral("mode")});
    parser.addOption({QStringLiteral("cell-scale"), QStringLiteral("Cell width and height against the font's own, 0.5 to 3 each."), QStringLiteral("w,h")});
    parser.addOption({QStringLiteral("cursor-position"), QStringLiteral("Where the cursor is drawn while it glides, in pixels from cell 0,0."), QStringLiteral("x,y")});
    parser.addOption({QStringLiteral("scroll-shift"), QStringLiteral("Content shifted up by this many pixels, as in smooth scrolling."), QStringLiteral("px")});
    parser.addOption({QStringLiteral("select"), QStringLiteral("A selection from cell C0,R0 to cell C1,R1 of the visible screen."), QStringLiteral("c0,r0-c1,r1")});
    parser.addOption({QStringLiteral("search"), QStringLiteral("Highlight the matches of this text."), QStringLiteral("text")});
    parser.addOption({QStringLiteral("search-current"), QStringLiteral("With --search: the current match (default none)."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("hover-link"), QStringLiteral("Underline columns C0 to C1 of row ROW as a hovered link."), QStringLiteral("row,c0-c1")});
    parser.addOption({QStringLiteral("blink-off"), QStringLiteral("Draw the off phase of a blinking cursor and of blinking text.")});
    parser.addPositionalArgument(QStringLiteral("input"), QStringLiteral("Byte stream, or - for stdin."));
    parser.addPositionalArgument(QStringLiteral("output"), QStringLiteral("PNG to write."));
    parser.process(app);
    const QStringList args = parser.positionalArguments();
    if (args.size() != 2) {
        parser.showHelp(1);
    }
    bool framesOk = false, fpsOk = false;
    const int frames = parser.value(QStringLiteral("frames")).toInt(&framesOk);
    const double fps = parser.value(QStringLiteral("fps")).toDouble(&fpsOk);
    if (!framesOk || frames < 1 || frames > 10000 || !fpsOk || !std::isfinite(fps) || fps < 1 || fps > 240) {
        std::fprintf(stderr, "--frames must be 1..10000 and --fps must be 1..240\n");
        return 1;
    }
    auto validNumber = [](const QString& text, double low, double high, bool integer = false) {
        bool ok = false;
        const double value = text.toDouble(&ok);
        return ok && std::isfinite(value) && value >= low && value <= high
            && (!integer || value == std::floor(value));
    };
    struct Range { const char* key; double low, high; bool integer; };
    for (const Range r : {Range{"cols", 1, 1000, true}, {"rows", 1, 500, true},
            {"dpr", .25, 8, false}, {"size", 4, 96, false}, {"time", 0, 10000000, false},
            {"wake-age", 0, 10000000, false}, {"opacity", 0, 1, false}, {"weight", 100, 900, false},
            {"overview-amount", 0, 1, false}, {"overview-position", -1000, 1000, false},
            {"highlight", 0, AfterglowRender::MaxShaderPanes - 1, true}, {"tabs", 1, AfterglowRender::MaxShaderPanes, true},
            {"current-tab", 0, AfterglowRender::MaxShaderPanes - 1, true}, {"transition-progress", 0, 1, false},
            {"transition-direction", -1, 1, true}, {"open-progress", 0, 1, false}, {"open-edge", 1, 4, true},
            {"context", 0, 3, true}, {"context-progress", 0, 1, false}, {"close-progress", 0, 1, false},
            {"overview-age", 0, 10000000, false}, {"highlight-age", 0, 10000000, false},
            {"transition-age", 0, 10000000, false}, {"minimum-contrast", 1, 21, false},
            {"scroll-shift", 0, 1000, false}, {"search-current", -1, 100000, true}}) {
        const auto key = QLatin1String(r.key);
        if (parser.value(key).isEmpty() && !parser.isSet(key)) continue;
        if (!validNumber(parser.value(key), r.low, r.high, r.integer)) {
            std::fprintf(stderr, "--%s requires a finite %s in %g..%g\n", r.key,
                         r.integer ? "integer" : "number", r.low, r.high);
            return 1;
        }
    }
    // Integer options use the same strict conversion as their eventual consumers.
    for (const auto& key : {QStringLiteral("cols"), QStringLiteral("rows")}) {
        bool ok = false;
        parser.value(key).toInt(&ok);
        if (!ok) { std::fprintf(stderr, "--%s requires an integer\n", qPrintable(key)); return 1; }
    }
    if (parser.isSet(QStringLiteral("timings")) && frames < 2) {
        std::fprintf(stderr, "--timings needs --frames 2 or more\n");
        return 1;
    }
    const bool split = parser.isSet(QStringLiteral("split"));
    if (split && parser.isSet(QStringLiteral("grid"))) {
        std::fprintf(stderr, "--split and --grid cannot be combined\n");
        return 1;
    }
    if (parser.isSet(QStringLiteral("close-progress")) && parser.isSet(QStringLiteral("overview"))) {
        std::fprintf(stderr, "--close-progress cannot be combined with --overview\n");
        return 1;
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
    const QString aa = parser.value(QStringLiteral("antialiasing"));
    if (aa == QLatin1String("none")) {
        spec.antialiasing = AfterglowRender::Antialiasing::None;
    } else if (aa == QLatin1String("rgb")) {
        spec.antialiasing = AfterglowRender::Antialiasing::Rgb;
    } else if (aa == QLatin1String("bgr")) {
        spec.antialiasing = AfterglowRender::Antialiasing::Bgr;
    } else if (aa != QLatin1String("grayscale")) {
        std::fprintf(stderr, "--antialiasing must be grayscale, none, rgb or bgr\n");
        return 1;
    }
    bool gammaOk = false;
    spec.gamma = parser.value(QStringLiteral("text-gamma")).toFloat(&gammaOk);
    if (!gammaOk || !std::isfinite(spec.gamma) || spec.gamma < 0.5f || spec.gamma > 3.0f) {
        std::fprintf(stderr, "--text-gamma must be 0.5..3\n");
        return 1;
    }
    if (parser.isSet(QStringLiteral("family"))) {
        spec.regular.family = parser.value(QStringLiteral("family")).toStdString();
        spec.bold.family = spec.italic.family = spec.boldItalic.family = spec.regular.family;
    }
    if (parser.isSet(QStringLiteral("weight"))) {
        spec.regular.weight = parser.value(QStringLiteral("weight")).toDouble();
    }
    if (parser.isSet(QStringLiteral("hinting"))) {
        const QString hinting = parser.value(QStringLiteral("hinting"));
        if (hinting == QLatin1String("none")) spec.hinting = AfterglowRender::Hinting::None;
        else if (hinting == QLatin1String("light")) spec.hinting = AfterglowRender::Hinting::Light;
        else if (hinting == QLatin1String("full")) spec.hinting = AfterglowRender::Hinting::Full;
        else {
            std::fprintf(stderr, "--hinting must be none, light or full\n");
            return 1;
        }
    }
    if (parser.isSet(QStringLiteral("cell-scale"))) {
        double w = 1, h = 1;
        if (!parsePair(parser.value(QStringLiteral("cell-scale")), QLatin1Char(','), 0.5, 3.0, w, h)) {
            std::fprintf(stderr, "--cell-scale requires w,h in 0.5..3\n");
            return 1;
        }
        spec.cellWidthScale = float(w);
        spec.cellHeightScale = float(h);
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
    if (parser.isSet(QStringLiteral("audio-bands"))) {
        const auto bands = parser.value(QStringLiteral("audio-bands")).split(QLatin1Char(','));
        std::array<float, AfterglowRender::GpuDevice::AudioBars> spectrum{};
        if (bands.size() != 3) {
            std::fprintf(stderr, "--audio-bands requires bass,mids,treble in 0..1\n");
            return 1;
        }
        for (int band = 0; band < 3; ++band) {
            bool valid = false;
            const float value = bands[band].toFloat(&valid);
            if (!valid || !std::isfinite(value) || value < 0 || value > 1) {
                std::fprintf(stderr, "--audio-bands requires bass,mids,treble in 0..1\n");
                return 1;
            }
            constexpr int ends[] = {0, 20, 48, 64};
            std::fill(spectrum.begin() + ends[band], spectrum.begin() + ends[band + 1], value);
        }
        device.setAudioSpectrum(spectrum);
    }
    AfterglowRender::WindowRenderer renderer(device, nullptr);
    renderer.setDevicePixelRatio(parser.value(QStringLiteral("dpr")).toDouble());
    if (parser.isSet(QStringLiteral("scrollback-lines"))) {
        bool valid = false;
        const auto lines = parser.value(QStringLiteral("scrollback-lines")).toULongLong(&valid);
        if (!valid) {
            std::fprintf(stderr, "--scrollback-lines requires a nonnegative integer\n");
            return 1;
        }
        renderer.setScrollbackLinesOverride(lines);
    }
    const auto& metrics = renderer.fonts().fonts->metrics();

    const int cols = parser.value(QStringLiteral("cols")).toInt();
    const int rows = parser.value(QStringLiteral("rows")).toInt();
    AfterglowVt::TerminalOptions options;
    if (parser.isSet(QStringLiteral("bidi"))) {
        const QString bidi = parser.value(QStringLiteral("bidi"));
        if (bidi == QLatin1String("off")) options.bidi = AfterglowVt::BidiMode::Off;
        else if (bidi == QLatin1String("auto")) options.bidi = AfterglowVt::BidiMode::Auto;
        else if (bidi == QLatin1String("ltr")) options.bidi = AfterglowVt::BidiMode::LeftToRight;
        else if (bidi == QLatin1String("rtl")) options.bidi = AfterglowVt::BidiMode::RightToLeft;
        else {
            std::fprintf(stderr, "--bidi must be off, auto, ltr or rtl\n");
            return 1;
        }
    }
    if (parser.isSet(QStringLiteral("minimum-contrast")))
        options.minimumContrast = parser.value(QStringLiteral("minimum-contrast")).toDouble();
    options.boldIsBright = parser.isSet(QStringLiteral("bold-is-bright"));
    Host host;
    AfterglowVt::Terminal terminal(cols, rows, host, options);
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
        parser.values(QStringLiteral("shader")), parser.value(QStringLiteral("overview")),
        parser.value(QStringLiteral("tab-transition")), parser.value(QStringLiteral("pane-effects"))};
    const auto wanted = refs.background.size() + refs.text.size() + refs.cursor.size() + refs.pane.size()
        + refs.window.size() + (refs.overview.isEmpty() ? 0 : 1) + (refs.tabTransition.isEmpty() ? 0 : 1) + (refs.paneEffects.isEmpty() ? 0 : 1);
    if (wanted > 0) {
        Afterglow::ShaderLibrary library;
        QMap<QString, QString> packages;
        for (const QString& dir : parser.values(QStringLiteral("package"))) {
            packages.insert(QDir(dir).dirName(), QDir(dir).absoluteFilePath(QStringLiteral("shaders")));
        }
        library.setPackageShaders(packages);
        AfterglowRender::ShaderChains chains = library.build(refs, params);
        const auto got = chains.background.size() + chains.text.size() + chains.cursor.size() + chains.pane.size()
            + chains.window.size() + chains.overview.size() + chains.tabTransition.size() + chains.paneEffects.size();
        if (got != std::size_t(wanted)) {
            std::fprintf(stderr, "a shader failed to load\n");
            return 1;
        }
        renderer.setShaderChains(std::move(chains));
    }

    // Selection and search over the visible screen, as a pane captures them.
    const AfterglowVt::Screen& screen = terminal.screen();
    const std::uint64_t firstRow = screen.rowsDropped() + screen.viewportTop();
    AfterglowVt::Selection selection;
    if (parser.isSet(QStringLiteral("select"))) {
        const QStringList ends = parser.value(QStringLiteral("select")).split(QLatin1Char('-'));
        double c0 = 0, r0 = 0, c1 = 0, r1 = 0;
        if (ends.size() != 2 || !parsePair(ends[0], QLatin1Char(','), 0, 100000, c0, r0)
            || !parsePair(ends[1], QLatin1Char(','), 0, 100000, c1, r1) || c0 >= cols || c1 >= cols || r0 >= rows
            || r1 >= rows) {
            std::fprintf(stderr, "--select requires c0,r0-c1,r1 within the grid\n");
            return 1;
        }
        selection.anchor = {firstRow + std::uint64_t(r0), int(c0)};
        selection.head = {firstRow + std::uint64_t(r1), int(c1)};
        selection.layoutEpoch = screen.layoutEpoch();
        selection.active = true;
    }
    std::vector<AfterglowVt::SearchMatch> matches;
    AfterglowRender::SearchHighlight search;
    if (parser.isSet(QStringLiteral("search"))) {
        const auto needle = parser.value(QStringLiteral("search")).toStdU32String();
        if (needle.empty()) {
            std::fprintf(stderr, "--search requires text\n");
            return 1;
        }
        matches = AfterglowVt::findAll(screen, needle);
        // The application's default highlight colours.
        search = {&matches, parser.isSet(QStringLiteral("search-current")) ? parser.value(QStringLiteral("search-current")).toInt() : -1,
                  AfterglowRender::packRgba(0xe5, 0xc0, 0x7b), AfterglowRender::packRgba(0xff, 0x8c, 0x42),
                  AfterglowRender::packRgba(0x10, 0x10, 0x10)};
    }
    const float scrollShift = parser.isSet(QStringLiteral("scroll-shift")) ? parser.value(QStringLiteral("scroll-shift")).toFloat() : 0.0f;
    AfterglowRender::PaneSnapshot snapshot;
    // A row below the viewport exists only while scrolled back.
    snapshot.capture(terminal, selection.active ? &selection : nullptr, search, scrollShift > 0 ? 1 : 0);
    QSize size(cols * metrics.cellWidth, rows * metrics.cellHeight);
    if (parser.isSet(QStringLiteral("pixels"))) {
        double w = 0, h = 0;
        if (!parsePair(parser.value(QStringLiteral("pixels")), QLatin1Char('x'), 1, 8192, w, h) || w != std::floor(w)
            || h != std::floor(h)) {
            std::fprintf(stderr, "--pixels must be WxH, whole numbers within 1..8192\n");
            return 1;
        }
        size = QSize(int(w), int(h));
    }
    int gridCols = 1, gridRows = 1;
    if (parser.isSet(QStringLiteral("grid"))) {
        double c = 0, r = 0;
        if (!parsePair(parser.value(QStringLiteral("grid")), QLatin1Char('x'), 1, AfterglowRender::MaxShaderPanes, c, r)
            || c != std::floor(c) || r != std::floor(r) || c * r > AfterglowRender::MaxShaderPanes) {
            std::fprintf(stderr, "--grid must be COLSxROWS with at most %d panes\n", AfterglowRender::MaxShaderPanes);
            return 1;
        }
        gridCols = int(c);
        gridRows = int(r);
    }
    const qint64 fullWidth = split ? qint64(size.width()) * 2 + 2 : qint64(size.width() + 2) * gridCols - 2;
    const qint64 fullHeight = qint64(size.height() + 2) * gridRows - 2;
    if (size.width() < 1 || size.height() < 1 || fullWidth > 8192 || fullHeight > 8192
        || fullWidth * fullHeight > 16 * 1024 * 1024) {
        std::fprintf(stderr, "framebuffer must fit within 8192 pixels per side and 16 megapixels\n");
        return 1;
    }
    const auto alpha = std::uint8_t(std::lround(std::clamp(parser.value(QStringLiteral("opacity")).toDouble(), 0.0, 1.0) * 255));
    AfterglowRender::WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(QPoint(0, 0), size);
    pane.params.contentOrigin = QPoint(0, 0);
    pane.params.focused = !parser.isSet(QStringLiteral("unfocused"));
    pane.params.backgroundAlpha = alpha;
    pane.params.scrollShift = scrollShift;
    if (parser.isSet(QStringLiteral("blink-off"))) {
        pane.params.cursorBlinkOn = false;
        pane.params.textBlinkOn = false;
    }
    if (parser.isSet(QStringLiteral("cursor-position"))) {
        double x = 0, y = 0;
        if (!parsePair(parser.value(QStringLiteral("cursor-position")), QLatin1Char(','), 0, 100000, x, y)) {
            std::fprintf(stderr, "--cursor-position requires x,y in pixels\n");
            return 1;
        }
        pane.params.cursorPosition = QPointF(x, y);
    }
    if (parser.isSet(QStringLiteral("hover-link"))) {
        const QStringList parts = parser.value(QStringLiteral("hover-link")).split(QLatin1Char(','));
        double c0 = 0, c1 = 0;
        bool rowOk = false;
        const int row = parts.value(0).toInt(&rowOk);
        if (parts.size() != 2 || !rowOk || row < 0 || row >= rows
            || !parsePair(parts[1], QLatin1Char('-'), 0, cols - 1, c0, c1) || c0 > c1) {
            std::fprintf(stderr, "--hover-link requires row,c0-c1 within the grid\n");
            return 1;
        }
        pane.params.hoverRow = row;
        pane.params.hoverColStart = int(c0);
        pane.params.hoverColEnd = int(c1);
    }
    if (parser.isSet(QStringLiteral("mouse"))) {
        double x = 0, y = 0;
        if (!parsePair(parser.value(QStringLiteral("mouse")), QLatin1Char(','), -100000, 100000, x, y)) {
            std::fprintf(stderr, "--mouse requires x,y in pixels\n");
            return 1;
        }
        renderer.setMousePosition(QPointF(x, y));
    }
    std::array<std::optional<float>, 8> signalAges;
    for (const QString& spec : parser.values(QStringLiteral("signal"))) {
        const QStringList kv = spec.split(QLatin1Char('='));
        const QStringList va = kv.value(1).split(QLatin1Char('@'));
        bool channelOk = false;
        const int channel = kv.value(0).toInt(&channelOk);
        if (kv.size() != 2 || !channelOk || channel < 0 || channel >= int(pane.params.signals.size())
            || va.size() > 2 || !validNumber(va.value(0), -1e30, 1e30)
            || !validNumber(va.value(1, QStringLiteral("0")), 0, 10000000)) {
            std::fprintf(stderr, "bad --signal %s\n", qPrintable(spec));
            return 1;
        }
        const auto age = std::chrono::duration<float>(va.value(1, QStringLiteral("0")).toFloat());
        signalAges[std::size_t(channel)] = age.count();
        pane.params.signals[std::size_t(channel)] = {
            va.value(0).toFloat(),
            std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(age)};
    }
    const std::uint32_t clear = (snapshot.defaultBg & 0x00FFFFFFu) | (std::uint32_t(alpha) << 24);
    const double time = parser.value(QStringLiteral("time")).toDouble();
    std::vector<AfterglowRender::WindowRenderer::Pane> panes{pane};
    std::vector<std::pair<QRect, std::uint32_t>> overlays;
    QSize frameSize = size;
    AfterglowRender::PaneSnapshot twin = snapshot;
    if (split) {
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
    std::vector<AfterglowRender::PaneSnapshot> copies(std::size_t(gridCols * gridRows), snapshot);
    if (gridCols * gridRows > 1) {
        const int divider = 2;
        frameSize = QSize(int(fullWidth), int(fullHeight));
        panes.clear();
        for (int row = 0; row < gridRows; ++row) {
            for (int col = 0; col < gridCols; ++col) {
                AfterglowRender::WindowRenderer::Pane cell = pane;
                // The first cell is the original snapshot, which --wake-from moves.
                cell.snapshot = panes.empty() ? &snapshot : &copies[std::size_t(row * gridCols + col)];
                cell.params.rect = QRect(QPoint(col * (size.width() + divider), row * (size.height() + divider)), size);
                cell.params.contentOrigin = cell.params.rect.topLeft();
                cell.params.focused = panes.empty() && pane.params.focused;
                panes.push_back(cell);
            }
        }
    }
    if (parser.isSet(QStringLiteral("wake-from"))) {
        const QStringList at = parser.value(QStringLiteral("wake-from")).split(QLatin1Char(','));
        bool xOk = false, yOk = false;
        const int previousX = at.value(0).toInt(&xOk), previousY = at.value(1).toInt(&yOk);
        if (at.size() != 2 || !xOk || !yOk || previousX < 0 || previousX >= cols || previousY < 0 || previousY >= rows) {
            std::fprintf(stderr, "--wake-from requires a valid col,row within the grid\n");
            return 1;
        }
        // One frame with the cursor at its old cell and the same panes, so
        // the renderer's cursor history sees the jump.
        const int x = snapshot.cursorX;
        const int y = snapshot.cursorY;
        snapshot.cursorX = previousX;
        snapshot.cursorY = previousY;
        renderer.setClockOverride(time - parser.value(QStringLiteral("wake-age")).toDouble());
        renderer.renderToImage(frameSize, panes, overlays, clear);
        snapshot.cursorX = x;
        snapshot.cursorY = y;
        ++snapshot.version;
    }
    // A closed pane: the last one, removed before anything else is set on
    // the last pane, so open, context and pulse land on a live one.
    AfterglowRender::PaneSnapshot closed = snapshot;
    std::optional<AfterglowRender::WindowRenderer::Pane> ghost;
    float closeProgress = 1.0f;
    if (parser.isSet(QStringLiteral("close-progress"))) {
        if (panes.size() < 2) {
            std::fprintf(stderr, "--close-progress needs --split or --grid\n");
            return 1;
        }
        closed.defaultBg = 0xFF5A2A10u; // blue
        ghost = panes.back();
        ghost->snapshot = &closed;
        panes.pop_back();
        closeProgress = parser.value(QStringLiteral("close-progress")).toFloat();
    }
    // Other tabs: copies of the same panes, with their own snapshots.
    const int tabCount = parser.isSet(QStringLiteral("tabs")) ? parser.value(QStringLiteral("tabs")).toInt() : 1;
    const int currentTab = parser.value(QStringLiteral("current-tab")).toInt();
    if (tabCount * int(panes.size()) > AfterglowRender::MaxShaderPanes || currentTab >= tabCount) {
        std::fprintf(stderr, "--tabs times the panes must be at most %d, and --current-tab below --tabs\n",
                     AfterglowRender::MaxShaderPanes);
        return 1;
    }
    std::vector<AfterglowRender::PaneSnapshot> tabSnapshots(panes.size() * std::size_t(tabCount), snapshot);
    std::optional<AfterglowRender::WindowRenderer::Overview> overview;
    if (parser.isSet(QStringLiteral("overview"))) {
        const int perTab = int(panes.size());
        const int highlight = parser.value(QStringLiteral("highlight")).toInt();
        if (highlight >= perTab * tabCount) {
            std::fprintf(stderr, "--highlight must be below the number of cards (%d)\n", perTab * tabCount);
            return 1;
        }
        overview.emplace();
        overview->open = true;
        overview->amount = parser.isSet(QStringLiteral("overview-amount")) ? parser.value(QStringLiteral("overview-amount")).toFloat() : 1.0f;
        overview->position = parser.isSet(QStringLiteral("overview-position")) ? parser.value(QStringLiteral("overview-position")).toFloat() : float(highlight);
        overview->currentTab = currentTab;
        for (int tab = 0; tab < tabCount; ++tab) {
            if (tab == currentTab) continue;
            AfterglowRender::WindowRenderer::OverviewTab other;
            other.index = tab;
            for (int i = 0; i < perTab; ++i) {
                auto copy = panes[std::size_t(i)];
                copy.snapshot = &tabSnapshots[std::size_t(tab * perTab + i)];
                copy.params.focused = false;
                copy.params.highlighted = tab * perTab + i == highlight;
                other.panes.push_back(copy);
            }
            other.overlays = overlays;
            overview->otherTabs.push_back(std::move(other));
        }
        if (highlight / perTab == currentTab) panes[std::size_t(highlight % perTab)].params.highlighted = true;
    }
    bool effects = ghost.has_value();
    if (parser.isSet(QStringLiteral("open-progress"))) {
        panes.back().params.openProgress = parser.value(QStringLiteral("open-progress")).toFloat();
        panes.back().params.openEdge = parser.isSet(QStringLiteral("open-edge")) ? parser.value(QStringLiteral("open-edge")).toInt() : 1;
        effects = true;
    }
    if (parser.isSet(QStringLiteral("context"))) {
        panes.back().params.context = parser.value(QStringLiteral("context")).toInt();
        panes.back().params.contextProgress = parser.isSet(QStringLiteral("context-progress"))
            ? parser.value(QStringLiteral("context-progress")).toFloat() : 1.0f;
        effects = true;
    }
    if (parser.isSet(QStringLiteral("pulse"))) {
        const auto spec = parser.value(QStringLiteral("pulse")).split(QLatin1Char(':'));
        const QString progress = spec.value(1, QStringLiteral("0.5")), color = spec.value(2, QStringLiteral("4"));
        if (spec.size() < 1 || spec.size() > 3 || !validNumber(spec.value(0), 1, 3, true) || !validNumber(progress, 0, 1)
            || !validNumber(color, 0, 255, true)) {
            std::fprintf(stderr, "--pulse requires EFFECT:PROGRESS[:COLOR], effect 1..3, progress 0..1, colour 0..255\n");
            return 1;
        }
        panes.back().params.pulseEffect = spec.value(0).toInt();
        panes.back().params.pulseProgress = progress.toFloat();
        panes.back().params.pulseColor = color.toInt();
        effects = true;
    }
    if (effects) renderer.setPaneEffects({true, true, ghost, closeProgress});
    // The tab being left: its own blue copies, apart from the overview's.
    std::vector<AfterglowRender::PaneSnapshot> leftSnapshots(panes.size(), snapshot);
    std::optional<AfterglowRender::WindowRenderer::TabTransition> transition;
    if (parser.isSet(QStringLiteral("tab-transition"))) {
        for (auto& copy : leftSnapshots) copy.defaultBg = 0xFF5A2A10u;
        transition.emplace();
        transition->active = true;
        transition->progress = parser.isSet(QStringLiteral("transition-progress")) ? parser.value(QStringLiteral("transition-progress")).toFloat() : 0.5f;
        transition->direction = parser.value(QStringLiteral("transition-direction")).toInt() < 0 ? -1.0f : 1.0f;
        for (std::size_t i = 0; i < panes.size(); ++i) {
            auto copy = panes[i];
            copy.snapshot = &leftSnapshots[i];
            copy.params.focused = false;
            transition->from.panes.push_back(copy);
        }
        transition->from.overlays = overlays;
    }
    auto seconds = [](double s) {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(s));
    };
    auto optionalAge = [&](const char* key) -> std::optional<double> {
        const auto name = QLatin1String(key);
        if (!parser.isSet(name)) return std::nullopt;
        return parser.value(name).toDouble();
    };
    const auto overviewAge = optionalAge("overview-age");
    const auto highlightAge = optionalAge("highlight-age");
    const auto transitionAge = optionalAge("transition-age");
    const QFileInfo output(args[1]);
    double renderMs = 0.0, pngMs = 0.0;
    std::vector<double> samples;
    for (int frame = 0; frame < frames; ++frame) {
        const double elapsed = frame / fps;
        renderer.setClockOverride(time + elapsed);
        // Ages follow the requested playback rate, not render speed: each is
        // stamped this frame as its starting age plus the frames' time.
        const auto wall = std::chrono::steady_clock::now();
        auto stampSignals = [&](AfterglowRender::WindowRenderer::Pane& region) {
            for (std::size_t ch = 0; ch < signalAges.size(); ++ch) {
                if (signalAges[ch]) region.params.signals[ch].at = wall - seconds(*signalAges[ch] + elapsed);
            }
        };
        for (auto& region : panes) stampSignals(region);
        if (overview) {
            // Unset ages start at frame 0, as when the overview opens.
            overview->changedAt = wall - seconds(overviewAge.value_or(0) + elapsed);
            overview->movedAt = wall - seconds(highlightAge.value_or(0) + elapsed);
            for (auto& tab : overview->otherTabs)
                for (auto& region : tab.panes) stampSignals(region);
            renderer.setOverview(*overview);
        }
        if (transition) {
            transition->startedAt = wall - seconds(transitionAge.value_or(0) + elapsed);
            for (auto& region : transition->from.panes) stampSignals(region);
            renderer.setTabTransition(*transition);
        }
        if (ghost) {
            stampSignals(*ghost);
            renderer.setPaneEffects({true, true, ghost, closeProgress});
        }
        const auto renderStart = std::chrono::steady_clock::now();
        const QImage image = renderer.renderToImage(frameSize, panes, overlays, clear);
        const auto renderEnd = std::chrono::steady_clock::now();
        const QString path = frames == 1 ? args[1]
            : output.absolutePath() + QLatin1Char('/') + output.completeBaseName()
                + QStringLiteral("-%1.png").arg(frame, 4, 10, QLatin1Char('0'));
        if (image.isNull() || ((!parser.isSet(QStringLiteral("last-only")) || frame == frames - 1) && !image.save(path))) {
            std::fprintf(stderr, "rendering failed: %s\n", qPrintable(path));
            return 1;
        }
        if (frame > 0) {
            samples.push_back(std::chrono::duration<double, std::milli>(renderEnd - renderStart).count());
            renderMs += samples.back();
            pngMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - renderEnd).count();
        }
    }
    if (parser.isSet(QStringLiteral("timings"))) {
        std::sort(samples.begin(), samples.end());
        std::fprintf(stderr, "framebuffer %dx%d; p50 %.2f ms; p95 %.2f ms\n", frameSize.width(), frameSize.height(),
                     samples[samples.size()/2], samples[std::min(samples.size()-1, std::size_t(samples.size()*.95))]);
        std::fprintf(stderr, "%d frames after warmup: render + readback %.2f ms/frame, PNG %.2f ms/frame (not GPU-only timings)\n",
                     frames - 1, renderMs / (frames - 1), pngMs / (frames - 1));
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    // No Vulkan device, or an allocation that fails: a message, not an abort.
    try {
        return run(app);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
