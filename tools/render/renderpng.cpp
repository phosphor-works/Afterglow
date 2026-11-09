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
// seconds earlier, so cursor effects show a jump. --dock EDGE:CELLS:TEXT adds
// a script dock at a window edge, drawn after every chain as in the window;
// --pane-dock EDGE:CELLS:TEXT adds one at that edge of every pane, drawn with
// the pane under its chains. --overlay TEXT draws a terminal that shows TEXT
// over the first pane, faded in by --overlay-progress through the
// --overlay-transition shader, or as it fades out with --overlay-ended;
// --overlay-width, --overlay-height or --overlay-border true make it a
// popup, placed as the window places one, at --overlay-position. Further
// options set the other renderer inputs (pointer, selection, search, glide,
// scroll shift, hovered link, blink phase, bidi, contrast, hinting, cell
// scale); --help lists them all.

#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/GridRenderer.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Search.h>
#include <AfterglowVt/Selection.h>
#include <AfterglowVt/Terminal.h>
#include <ImageDecode.h>
#include <config/Settings.h>
#include <config/Theme.h>
#include <scripting/OverlaySpec.h>
#include <shaders/ShaderLibrary.h>

#include <QCommandLineParser>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMargins>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <initializer_list>
#include <memory>
#include <numeric>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace {

// Images decode as in a pane, with its decoder and its limits.
class Host : public AfterglowVt::ITerminalHost
{
public:
    void writeToPty(std::string_view) override { }
    bool decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height,
                   std::vector<std::uint8_t>& rgba) override
    {
        return Afterglow::decodeBoundedImage(png, "PNG", width, height, rgba);
    }
    // iTerm2 inline images: the common formats by their content.
    bool decodeImage(std::string_view data, std::uint32_t& width, std::uint32_t& height,
                     std::vector<std::uint8_t>& rgba) override
    {
        return Afterglow::decodeBoundedImage(data, "", width, height, rgba);
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

/// A script dock from --dock or --pane-dock.
struct DockSpec
{
    QString edge;
    int cells = 0;
    QByteArray text;
    bool side() const { return edge == QLatin1String("left") || edge == QLatin1String("right"); }
    int extent(int cellWidth, int cellHeight) const { return cells * (side() ? cellWidth : cellHeight); }
};

/// Parses EDGE:CELLS:TEXT within the sizes scripts may use, or prints the
/// limits for --@p option.
std::optional<DockSpec> parseDock(const char* option, const QString& value)
{
    const qsizetype first = value.indexOf(QLatin1Char(':'));
    const qsizetype second = first < 0 ? -1 : value.indexOf(QLatin1Char(':'), first + 1);
    DockSpec spec{value.left(first), 0, {}};
    bool cellsOk = false;
    if (second >= 0) spec.cells = value.mid(first + 1, second - first - 1).toInt(&cellsOk);
    if (!cellsOk || spec.cells < 1 || spec.cells > (spec.side() ? 80 : 8)
        || !(spec.side() || spec.edge == QLatin1String("top") || spec.edge == QLatin1String("bottom"))) {
        std::fprintf(stderr, "--%s requires EDGE:CELLS:TEXT, edge top, bottom, left or right, cells 1..8 for top and bottom, 1..80 for left and right\n",
                     option);
        return std::nullopt;
    }
    spec.text = value.mid(second + 1).toUtf8();
    return spec;
}

/// What @p specs take at each edge, in pixels.
QMargins dockInsets(const std::vector<DockSpec>& specs, int cellWidth, int cellHeight)
{
    QMargins insets;
    for (const DockSpec& spec : specs) {
        const int extent = spec.extent(cellWidth, cellHeight);
        if (spec.edge == QLatin1String("top")) insets.setTop(insets.top() + extent);
        else if (spec.edge == QLatin1String("bottom")) insets.setBottom(insets.bottom() + extent);
        else if (spec.edge == QLatin1String("left")) insets.setLeft(insets.left() + extent);
        else insets.setRight(insets.right() + extent);
    }
    return insets;
}

/// Each spec's rectangle in @p area, laid out as the window does: from the
/// edge inwards, top and bottom across the whole width first, then left and
/// right between them, each group in the order given.
std::vector<QRect> stackDocks(const std::vector<DockSpec>& specs, QRect area, int cellWidth, int cellHeight)
{
    std::vector<std::size_t> order(specs.size());
    std::iota(order.begin(), order.end(), std::size_t(0));
    std::stable_partition(order.begin(), order.end(), [&](std::size_t i) { return !specs[i].side(); });
    std::vector<QRect> rects(specs.size());
    for (const std::size_t i : order) {
        const DockSpec& spec = specs[i];
        const int extent = spec.extent(cellWidth, cellHeight);
        if (spec.edge == QLatin1String("top")) {
            rects[i] = QRect(area.left(), area.top(), area.width(), extent);
            area.setTop(area.top() + extent);
        } else if (spec.edge == QLatin1String("bottom")) {
            rects[i] = QRect(area.left(), area.bottom() + 1 - extent, area.width(), extent);
            area.setBottom(area.bottom() - extent);
        } else if (spec.edge == QLatin1String("left")) {
            rects[i] = QRect(area.left(), area.top(), extent, area.height());
            area.setLeft(area.left() + extent);
        } else {
            rects[i] = QRect(area.right() + 1 - extent, area.top(), extent, area.height());
            area.setRight(area.right() - extent);
        }
    }
    return rects;
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
    parser.addOption({QStringLiteral("pixels"), QStringLiteral("Exact pane framebuffer size WIDTHxHEIGHT, without its --pane-dock docks."), QStringLiteral("size")});
    parser.addOption({QStringLiteral("last-only"), QStringLiteral("Save only the final frame (benchmarking).")});
    parser.addOption({QStringLiteral("timings"), QStringLiteral("Report render/readback and PNG times, excluding the first frame (needs --frames 2 or more).")});
    parser.addOption({QStringLiteral("wake-from"), QStringLiteral("Previous cursor cell."), QStringLiteral("col,row")});
    parser.addOption({QStringLiteral("split"), QStringLiteral("Show the terminal twice, side by side. The left pane has focus.")});
    parser.addOption({QStringLiteral("grid"), QStringLiteral("Show the terminal in a grid of COLSxROWS panes. The first pane has focus."), QStringLiteral("size")});
    parser.addOption({QStringLiteral("package"), QStringLiteral("A script package folder whose shaders load as <package>.<pack> (repeatable)."), QStringLiteral("dir")});
    parser.addOption({QStringLiteral("overview"), QStringLiteral("Pane overview shader, shown open (or closing, with --overview-closing)."), QStringLiteral("ref")});
    parser.addOption({QStringLiteral("overview-amount"), QStringLiteral("How far the overview is open, 0 to 1 (default 1)."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("highlight"), QStringLiteral("The highlighted pane in the overview, counting every tab (default 0)."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("overview-closing"), QStringLiteral("The overview is closing onto the --highlight card, at --overview-amount.")});
    parser.addOption({QStringLiteral("tab-transition"), QStringLiteral("Tab transition shader, from a copy of the panes on a blue background."), QStringLiteral("ref")});
    parser.addOption({QStringLiteral("transition-progress"), QStringLiteral("Tab transition progress, 0 to 1 (default 0.5)."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("transition-direction"), QStringLiteral("1 toward a tab on the right (default), -1 toward one on the left."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("pane-effects"), QStringLiteral("Pane effects shader for --open-progress and --close-progress."), QStringLiteral("ref")});
    parser.addOption({QStringLiteral("open-progress"), QStringLiteral("The last pane is opening, 0 to 1, from --open-edge."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("open-edge"), QStringLiteral("The edge it opens from: 1 left (default), 2 top, 3 right, 4 bottom."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("context"), QStringLiteral("The last pane's context: 0 here, 1 remote, 2 elevated, 3 container."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("context-progress"), QStringLiteral("How far its context change has run, 0 to 1 (default 1)."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("previous-context"), QStringLiteral("Its context before that change, 0 to 3 (default 0)."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("pulse"), QStringLiteral("A pulse on the last pane: EFFECT:PROGRESS[:COLOR], effect 1 portal, 2 flash, 3 shake, or 4 to 255 for a package's pulse."), QStringLiteral("spec")});
    parser.addOption({QStringLiteral("close-progress"), QStringLiteral("The last pane has closed, 0 to 1, leaving its frame on a blue background. Needs --split or --grid."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("pointer"), QStringLiteral("Print where a click at X,Y (pixels of the image) lands through the packs' pointer maps (repeatable)."),
                      QStringLiteral("x,y")});
    parser.addOption({QStringLiteral("overview-position"), QStringLiteral("The highlight's position, a fractional card index (default the highlighted card)."), QStringLiteral("f")});
    parser.addOption({QStringLiteral("tabs"), QStringLiteral("With --overview: N tabs with the same panes, the first current (default 1)."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("current-tab"), QStringLiteral("With --tabs: the current tab (default 0)."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("family"), QStringLiteral("Font family."), QStringLiteral("name")});
    parser.addOption({QStringLiteral("weight"), QStringLiteral("Weight of regular and italic text, 100 to 900 (Font.weight)."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("bold-weight"), QStringLiteral("Weight of bold text, 100 to 900 (Font.boldWeight)."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("width"), QStringLiteral("Width axis of variable fonts, 50 to 200 (Font.width)."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("italic-slant"), QStringLiteral("Slant of italic text in degrees, -90 to 90 (Font.italicSlant)."), QStringLiteral("deg")});
    parser.addOption({QStringLiteral("feature"), QStringLiteral("OpenType feature in HarfBuzz syntax, replacing the defaults (repeatable; \"none\" for none)."),
                      QStringLiteral("tag")});
    parser.addOption({QStringLiteral("signal"), QStringLiteral("Shader signal set AGE seconds ago (repeatable)."),
                      QStringLiteral("ch=value[@age]")});
    parser.addOption({QStringLiteral("wake-age"), QStringLiteral("Seconds since the cursor moved."),
                      QStringLiteral("s"), QStringLiteral("0.1")});
    parser.addOption({QStringLiteral("opacity"), QStringLiteral("Background opacity."), QStringLiteral("f"),
                      QStringLiteral("1")});
    parser.addOption({QStringLiteral("unfocused"), QStringLiteral("Render the pane unfocused.")});
    // The window's defaults (afterglow/src/config/Settings.h).
    const Afterglow::Settings defaults;
    parser.addOption({QStringLiteral("unfocused-dim"), QStringLiteral("With --split or --grid: the opacity of the black layer over the unfocused panes (Window.unfocusedDim, default %1).")
                          .arg(defaults.window.unfocusedDim),
                      QStringLiteral("f"), QString::number(defaults.window.unfocusedDim)});
    parser.addOption({QStringLiteral("date"), QStringLiteral("The date and time of iDate, as YYYY-MM-DDTHH:MM:SS (default 2026-01-01T00:00:00, so that previews do not change from run to run)."),
                      QStringLiteral("time"), QStringLiteral("2026-01-01T00:00:00")});
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
    parser.addOption({QStringLiteral("hover-link-id"), QStringLiteral("Underline the cells of OSC 8 link N as hovered, 1 for the first link on the screen."), QStringLiteral("n")});
    parser.addOption({QStringLiteral("blink-off"), QStringLiteral("Draw the off phase of a blinking cursor and of blinking text.")});
    parser.addOption({QStringLiteral("pane-dock"), QStringLiteral("A script dock at an edge of every pane (repeatable), as --dock: drawn in the pane, under its chains, overview and pane effects. The pane grows by the dock and its grid keeps its size."),
                      QStringLiteral("EDGE:CELLS:TEXT")});
    parser.addOption({QStringLiteral("overlay"), QStringLiteral("An overlay over the first pane: a terminal that shows TEXT, the bytes it gets. It takes the pane's grid, or a part of it as a popup."),
                      QStringLiteral("text")});
    parser.addOption({QStringLiteral("overlay-progress"), QStringLiteral("With --overlay: how far it has faded in, 0 the pane to 1 the overlay (default 1)."),
                      QStringLiteral("f")});
    const QString overlayTransition = defaults.shaders.overlayTransition;
    parser.addOption({QStringLiteral("overlay-transition"),
                      QStringLiteral("With --overlay: the overlay transition shader (default %1, \"\" for none, which switches at once).")
                          .arg(overlayTransition),
                      QStringLiteral("ref"), overlayTransition});
    parser.addOption({QStringLiteral("overlay-ended"), QStringLiteral("With --overlay: its program has ended, as while it fades out: no cursor, and the pane has the keyboard again.")});
    parser.addOption({QStringLiteral("overlay-width"), QStringLiteral("With --overlay: a popup this wide, in cells or as N% of the pane's grid, border included."),
                      QStringLiteral("size")});
    parser.addOption({QStringLiteral("overlay-height"), QStringLiteral("With --overlay: a popup this tall, in cells or as N% of the pane's grid, border included."),
                      QStringLiteral("size")});
    parser.addOption({QStringLiteral("overlay-position"), QStringLiteral("With a popup: where it sits (center, top, bottom, left, right, top-left, top-right, bottom-left, bottom-right)."),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("overlay-border"), QStringLiteral("With --overlay: true or false for a border (default: one for a popup). True makes a popup."),
                      QStringLiteral("bool")});
    parser.addOption({QStringLiteral("overlay-title"), QStringLiteral("With a popup: the title in its border, in visual order with --bidi."), QStringLiteral("text")});
    parser.addOption({QStringLiteral("popup-corners"), QStringLiteral("With a popup: rounded (default) or square corners of its border (Window.popupCorners)."),
                      QStringLiteral("style")});
    parser.addOption({QStringLiteral("popup-border"), QStringLiteral("With a popup: the color of its border (Colors.popupBorder). By default halfway from the configured background to the configured text, as the window derives it."),
                      QStringLiteral("color")});
    const QString popupDim = QString::number(defaults.window.popupDim);
    parser.addOption({QStringLiteral("popup-dim"), QStringLiteral("With a popup: the opacity of the black layer over the pane around it (Colors.popupDim, default %1).").arg(popupDim),
                      QStringLiteral("f"), popupDim});
    parser.addOption({QStringLiteral("dock"), QStringLiteral("A script dock (repeatable): EDGE top, bottom, left or right, CELLS 1..8 rows (top, bottom) or 1..80 columns (left, right) as scripts may use, TEXT the bytes it shows. The frame grows by the dock and the panes keep their size."),
                      QStringLiteral("edge:cells:text")});
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
    // An integer goes through toInt(), as the code that reads it: "2.0" is no integer.
    auto validNumber = [](const QString& text, double low, double high, bool integer = false) {
        bool ok = false;
        const double value = integer ? double(text.toInt(&ok)) : text.toDouble(&ok);
        return ok && std::isfinite(value) && value >= low && value <= high;
    };
    struct Range { const char* key; double low, high; bool integer; };
    for (const Range r : {Range{"cols", 1, 1000, true}, {"rows", 1, 500, true},
            {"dpr", .25, 8, false}, {"size", AfterglowRender::FontSpec::MinPoints, AfterglowRender::FontSpec::MaxPoints, false}, {"time", 0, 10000000, false},
            {"wake-age", 0, 10000000, false}, {"opacity", 0, 1, false}, {"weight", 100, 900, true},
            {"bold-weight", 100, 900, true}, {"width", 50, 200, false}, {"italic-slant", -90, 90, false},
            {"unfocused-dim", 0, 1, false}, {"hover-link-id", 1, 1000000, true},
            {"overview-amount", 0, 1, false}, {"overview-position", -1000, 1000, false},
            {"highlight", 0, AfterglowRender::MaxShaderPanes - 1, true}, {"tabs", 1, AfterglowRender::MaxShaderPanes, true},
            {"current-tab", 0, AfterglowRender::MaxShaderPanes - 1, true}, {"transition-progress", 0, 1, false},
            {"transition-direction", -1, 1, true}, {"open-progress", 0, 1, false}, {"open-edge", 1, 4, true},
            {"context", 0, 3, true}, {"context-progress", 0, 1, false}, {"previous-context", 0, 3, true}, {"close-progress", 0, 1, false},
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
    if (parser.isSet(QStringLiteral("transition-direction")) && parser.value(QStringLiteral("transition-direction")).toInt() == 0) {
        std::fprintf(stderr, "--transition-direction must be 1 or -1\n");
        return 1;
    }
    // Options that set an input only another option draws.
    struct Needs { const char* option; std::initializer_list<const char*> any; };
    for (const Needs& rule : {Needs{"open-edge", {"open-progress"}}, Needs{"context-progress", {"context"}},
            Needs{"previous-context", {"context"}}, Needs{"overview-amount", {"overview"}}, Needs{"highlight", {"overview"}},
            Needs{"overview-closing", {"overview"}}, Needs{"overview-position", {"overview"}}, Needs{"tabs", {"overview"}},
            Needs{"current-tab", {"overview"}}, Needs{"overview-age", {"overview"}}, Needs{"highlight-age", {"overview"}},
            Needs{"transition-progress", {"tab-transition"}}, Needs{"transition-direction", {"tab-transition"}},
            Needs{"transition-age", {"tab-transition"}}, Needs{"search-current", {"search"}}, Needs{"wake-age", {"wake-from"}},
            Needs{"unfocused-dim", {"split", "grid"}},
            Needs{"pane-effects", {"open-progress", "context", "pulse", "close-progress"}}}) {
        if (!parser.isSet(QLatin1String(rule.option))) continue;
        if (std::any_of(rule.any.begin(), rule.any.end(), [&](const char* other) { return parser.isSet(QLatin1String(other)); }))
            continue;
        QString needed;
        for (const char* other : rule.any)
            needed += (needed.isEmpty() ? QString() : QStringLiteral(" or ")) + QStringLiteral("--") + QLatin1String(other);
        std::fprintf(stderr, "--%s needs %s\n", rule.option, qPrintable(needed));
        return 1;
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
    // As the application fills the four styles from Font.weight,
    // Font.boldWeight, Font.width and Font.italicSlant.
    if (parser.isSet(QStringLiteral("weight"))) {
        spec.regular.weight = spec.italic.weight = float(parser.value(QStringLiteral("weight")).toInt());
    }
    if (parser.isSet(QStringLiteral("bold-weight"))) {
        spec.bold.weight = spec.boldItalic.weight = float(parser.value(QStringLiteral("bold-weight")).toInt());
    }
    if (parser.isSet(QStringLiteral("width"))) {
        spec.regular.width = spec.bold.width = spec.italic.width = spec.boldItalic.width =
            float(parser.value(QStringLiteral("width")).toDouble());
    }
    if (parser.isSet(QStringLiteral("italic-slant"))) {
        spec.italic.slant = spec.boldItalic.slant = float(parser.value(QStringLiteral("italic-slant")).toDouble());
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
            if (f == QLatin1String("none")) continue;
            if (!AfterglowRender::FontSet::isFeature(f.toStdString())) {
                std::fprintf(stderr, "--feature: \"%s\" is not an OpenType feature in HarfBuzz syntax\n", qPrintable(f));
                return 1;
            }
            spec.features.push_back(f.toStdString());
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
        .background = parser.values(QStringLiteral("background-shader")),
        .text = parser.values(QStringLiteral("text-shader")),
        .cursor = parser.values(QStringLiteral("cursor-shader")),
        .pane = parser.values(QStringLiteral("pane-shader")),
        .window = parser.values(QStringLiteral("shader")),
        .overview = parser.value(QStringLiteral("overview")),
        .tabTransition = parser.value(QStringLiteral("tab-transition")),
        .paneEffects = parser.value(QStringLiteral("pane-effects")),
        .overlayTransition = parser.isSet(QStringLiteral("overlay")) ? parser.value(QStringLiteral("overlay-transition")) : QString()};
    const qsizetype wanted = refs.size();
    if (wanted > 0) {
        Afterglow::ShaderLibrary library;
        QMap<QString, QString> packages;
        for (const QString& dir : parser.values(QStringLiteral("package"))) {
            // The application loads a package only when its id names its
            // directory, and registers the packs under that id.
            // By the absolute path: "." and "pkg/" name their directory too.
            const QDir package(QDir(dir).absolutePath());
            QFile manifest(package.absoluteFilePath(QStringLiteral("package.json")));
            const QString id = manifest.open(QIODevice::ReadOnly)
                ? QJsonDocument::fromJson(manifest.readAll()).object().value(QLatin1String("id")).toString() : QString();
            if (id != package.dirName()) {
                std::fprintf(stderr, "--package %s: package.json must give the id \"%s\", the name of its directory\n",
                             qPrintable(dir), qPrintable(package.dirName()));
                return 1;
            }
            packages.insert(id, package.absoluteFilePath(QStringLiteral("shaders")));
        }
        library.setPackageShaders(packages);
        AfterglowRender::ShaderChains chains = library.build(refs, params);
        const std::size_t got = chains.size();
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
            || r1 >= rows || c0 != std::floor(c0) || r0 != std::floor(r0) || c1 != std::floor(c1) || r1 != std::floor(r1)) {
            std::fprintf(stderr, "--select requires whole cells c0,r0-c1,r1 within the grid\n");
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
        const Afterglow::HighlightColors& colors = defaults.colors;
        search = {&matches, parser.isSet(QStringLiteral("search-current")) ? parser.value(QStringLiteral("search-current")).toInt() : -1,
                  AfterglowRender::packRgba(colors.searchMatch), AfterglowRender::packRgba(colors.searchCurrentMatch),
                  AfterglowRender::packRgba(colors.searchMatchText)};
    }
    const float scrollShift = parser.isSet(QStringLiteral("scroll-shift")) ? parser.value(QStringLiteral("scroll-shift")).toFloat() : 0.0f;
    AfterglowRender::PaneSnapshot snapshot;
    // A row below the viewport exists only while scrolled back. The window
    // captures it past half a pixel of shift, as here.
    snapshot.capture(terminal, selection.active ? &selection : nullptr, search, scrollShift > 0.5f ? 1 : 0);
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
    // Script docks at the window's edges grow the frame, and docks at each
    // pane's edges grow the pane: either way the grid keeps its size.
    std::vector<DockSpec> dockSpecs, paneDockSpecs;
    for (const QString& value : parser.values(QStringLiteral("dock"))) {
        auto spec = parseDock("dock", value);
        if (!spec) return 1;
        dockSpecs.push_back(std::move(*spec));
    }
    for (const QString& value : parser.values(QStringLiteral("pane-dock"))) {
        auto spec = parseDock("pane-dock", value);
        if (!spec) return 1;
        paneDockSpecs.push_back(std::move(*spec));
    }
    const QMargins dockInset = dockInsets(dockSpecs, metrics.cellWidth, metrics.cellHeight);
    const QMargins paneInset = dockInsets(paneDockSpecs, metrics.cellWidth, metrics.cellHeight);
    // The window shows a pane's docks only while they leave it this much.
    if (!paneDockSpecs.empty() && (size.width() < 4 * metrics.cellWidth || size.height() < 2 * metrics.cellHeight)) {
        std::fprintf(stderr, "--pane-dock needs a pane grid of at least 4 columns and 2 rows\n");
        return 1;
    }
    const QSize paneSize = size.grownBy(paneInset);
    // The window's divider: a device pixel ratio of pixels, at least one.
    const int divider = std::max(1, int(std::lround(parser.value(QStringLiteral("dpr")).toDouble())));
    const qint64 fullWidth = split ? qint64(paneSize.width()) * 2 + divider : qint64(paneSize.width() + divider) * gridCols - divider;
    const qint64 fullHeight = qint64(paneSize.height() + divider) * gridRows - divider;
    const qint64 windowWidth = fullWidth + dockInset.left() + dockInset.right();
    const qint64 windowHeight = fullHeight + dockInset.top() + dockInset.bottom();
    if (size.width() < 1 || size.height() < 1 || windowWidth > 8192 || windowHeight > 8192
        || windowWidth * windowHeight > 16 * 1024 * 1024) {
        std::fprintf(stderr, "framebuffer must fit within 8192 pixels per side and 16 megapixels\n");
        return 1;
    }
    // Cell (0, 0) of a pane, from its top left: inside its docks.
    const QPoint gridOffset(paneInset.left(), paneInset.top());
    const auto alpha = std::uint8_t(std::lround(std::clamp(parser.value(QStringLiteral("opacity")).toDouble(), 0.0, 1.0) * 255));
    AfterglowRender::WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(QPoint(0, 0), paneSize);
    pane.params.contentOrigin = gridOffset;
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
    if (parser.isSet(QStringLiteral("hover-link-id"))) {
        const int link = parser.value(QStringLiteral("hover-link-id")).toInt();
        if (std::size_t(link) > snapshot.links.size()) {
            std::fprintf(stderr, "--hover-link-id must name one of the %zu OSC 8 links on the screen\n", snapshot.links.size());
            return 1;
        }
        pane.params.hoveredLink = std::uint32_t(link);
    }
    if (parser.isSet(QStringLiteral("mouse"))) {
        double x = 0, y = 0;
        if (!parsePair(parser.value(QStringLiteral("mouse")), QLatin1Char(','), -100000, 100000, x, y)) {
            std::fprintf(stderr, "--mouse requires x,y in pixels\n");
            return 1;
        }
        renderer.setMousePosition(QPointF(x, y));
    }
    std::array<std::optional<float>, std::tuple_size_v<decltype(pane.params.signals)>> signalAges;
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
    // iDate: the date given, moved by the same seconds as iTime from --time.
    const QDateTime date = QDateTime::fromString(parser.value(QStringLiteral("date")), Qt::ISODate);
    if (!date.isValid()) {
        std::fprintf(stderr, "--date must be YYYY-MM-DDTHH:MM:SS\n");
        return 1;
    }
    auto setDate = [&](double elapsed) {
        const QDateTime at = date.addMSecs(qint64(std::llround(elapsed * 1000)));
        renderer.setDateOverride(std::array<float, 4>{float(at.date().year()), float(at.date().month()), float(at.date().day()),
                                                      float(at.time().msecsSinceStartOfDay()) / 1000.0f});
    };
    std::vector<AfterglowRender::WindowRenderer::Pane> panes{pane};
    std::vector<std::pair<QRect, std::uint32_t>> overlays;
    QSize frameSize = paneSize;
    AfterglowRender::PaneSnapshot twin = snapshot;
    if (split) {
        // Two panes with a divider between, as a split window shows them.
        frameSize = QSize(paneSize.width() * 2 + divider, paneSize.height());
        AfterglowRender::WindowRenderer::Pane right = pane;
        right.snapshot = &twin;
        right.params.rect = QRect(QPoint(paneSize.width() + divider, 0), paneSize);
        right.params.contentOrigin = right.params.rect.topLeft() + gridOffset;
        right.params.focused = false;
        panes.push_back(right);
    }
    std::vector<AfterglowRender::PaneSnapshot> copies(std::size_t(gridCols * gridRows), snapshot);
    if (gridCols * gridRows > 1) {
        frameSize = QSize(int(fullWidth), int(fullHeight));
        panes.clear();
        for (int row = 0; row < gridRows; ++row) {
            for (int col = 0; col < gridCols; ++col) {
                AfterglowRender::WindowRenderer::Pane cell = pane;
                // The first cell is the original snapshot, which --wake-from moves.
                cell.snapshot = panes.empty() ? &snapshot : &copies[std::size_t(row * gridCols + col)];
                cell.params.rect = QRect(QPoint(col * (paneSize.width() + divider), row * (paneSize.height() + divider)), paneSize);
                cell.params.contentOrigin = cell.params.rect.topLeft() + gridOffset;
                cell.params.focused = panes.empty() && pane.params.focused;
                panes.push_back(cell);
            }
        }
    }
    // Names for the pointer maps' lookups (--pointer).
    for (std::size_t i = 0; i < panes.size(); ++i) panes[i].params.id = std::uint32_t(i + 1);
    // The shading the window puts over several panes (addPaneShading): a
    // black layer over each unfocused pane, which fades as the overview
    // opens, and a divider in the theme's colour right of and below each
    // pane that does not reach the window's edge. The pane that
    // --close-progress closes (the last) has none: the window shades the
    // panes that stay, and dims them only while there are several.
    const std::size_t staying = panes.size() - (parser.isSet(QStringLiteral("close-progress")) && panes.size() > 1 ? 1 : 0);
    if (panes.size() > 1) {
        const double overviewAmount = parser.isSet(QStringLiteral("overview"))
            ? (parser.isSet(QStringLiteral("overview-amount")) ? parser.value(QStringLiteral("overview-amount")).toDouble() : 1.0) : 0.0;
        const auto dim = staying > 1
            ? std::uint8_t(std::lround(parser.value(QStringLiteral("unfocused-dim")).toDouble() * (1.0 - overviewAmount) * 255))
            : std::uint8_t(0);
        const std::uint32_t dividerColor = defaults.window.dividerColor;
        for (std::size_t i = 0; i < staying; ++i) {
            const QRect shown = panes[i].params.rect;
            if (!panes[i].params.focused && dim > 0) overlays.emplace_back(shown, AfterglowRender::packRgba(0, 0, 0, dim));
            if (shown.right() < frameSize.width() - 1)
                overlays.emplace_back(QRect(shown.right() + 1, shown.top(), divider, shown.height()), dividerColor);
            if (shown.bottom() < frameSize.height() - 1)
                overlays.emplace_back(QRect(shown.left(), shown.bottom() + 1, shown.width(), divider), dividerColor);
        }
    }
    // Each dock is a terminal of its own that runs no program, without wrap,
    // cursor, scrollback or bidi, as the window makes it, sized to its
    // rectangle. The terminals stay while their snapshots are drawn.
    AfterglowVt::TerminalOptions dockOptions = options;
    dockOptions.maxScrollback = 0;
    dockOptions.bidi = AfterglowVt::BidiMode::Off;
    std::vector<std::unique_ptr<AfterglowVt::Terminal>> dockTerminals;
    auto captureDock = [&](const DockSpec& spec, const QRect& rect, AfterglowRender::PaneSnapshot& out) {
        // Text past the edge goes into columns that do not show, which the
        // dock's rectangle clips: one for each byte of the text, at most
        // 1000. Without wrap, a terminal of the dock's width would write it
        // all into its last cell. Two differences from a script's dock
        // remain: a wide character across the edge shows its left half,
        // where a dock writes a space, and text past the 1000 hidden
        // columns lands in the last of them, which does not show either.
        const int dockCols = std::max(2, rect.width() / metrics.cellWidth) + int(std::min<qsizetype>(spec.text.size(), 1000));
        const int dockRows = std::max(1, rect.height() / metrics.cellHeight);
        auto& dockTerminal = dockTerminals.emplace_back(std::make_unique<AfterglowVt::Terminal>(dockCols, dockRows, host, dockOptions));
        dockTerminal->resize(dockCols, dockRows, metrics.cellWidth, metrics.cellHeight);
        dockTerminal->feed("\x1b[?7l\x1b[?25l");
        dockTerminal->feed(std::string_view(spec.text.constData(), std::size_t(spec.text.size())));
        out.capture(*dockTerminal);
    };
    std::vector<AfterglowRender::PaneSnapshot> dockSnapshots(dockSpecs.size());
    std::vector<AfterglowRender::WindowRenderer::Pane> docks;
    if (!dockSpecs.empty()) {
        const QPoint offset(dockInset.left(), dockInset.top());
        for (auto& region : panes) {
            region.params.rect.translate(offset);
            region.params.contentOrigin += offset;
        }
        for (auto& overlay : overlays) overlay.first.translate(offset);
        frameSize = frameSize.grownBy(dockInset);
        const std::vector<QRect> rects = stackDocks(dockSpecs, QRect(QPoint(0, 0), frameSize), metrics.cellWidth, metrics.cellHeight);
        for (std::size_t i = 0; i < dockSpecs.size(); ++i) {
            captureDock(dockSpecs[i], rects[i], dockSnapshots[i]);
            AfterglowRender::WindowRenderer::Pane dock;
            dock.snapshot = &dockSnapshots[i];
            dock.params.rect = rects[i];
            dock.params.contentOrigin = rects[i].topLeft();
            dock.params.backgroundAlpha = alpha;
            docks.push_back(dock);
        }
    }
    // Pane docks: the same areas at the same edges of every pane, inside it,
    // drawn with the pane and under its chains. Every pane has the same
    // size, so one terminal for each dock serves them all; each pane draws
    // its own copy, as each pane's dock is its own in the window.
    std::vector<AfterglowRender::PaneSnapshot> paneDockSnapshots(paneDockSpecs.size());
    const std::vector<QRect> paneDockRects = stackDocks(paneDockSpecs, QRect(QPoint(0, 0), paneSize), metrics.cellWidth, metrics.cellHeight);
    for (std::size_t i = 0; i < paneDockSpecs.size(); ++i) captureDock(paneDockSpecs[i], paneDockRects[i], paneDockSnapshots[i]);
    // Stable addresses: copies are added after panes point at earlier ones.
    std::deque<AfterglowRender::PaneSnapshot> paneDockCopies;
    auto addPaneDocks = [&](AfterglowRender::WindowRenderer::Pane& region) {
        region.docks.clear();
        for (std::size_t i = 0; i < paneDockSpecs.size(); ++i) {
            const AfterglowRender::PaneSnapshot& copy = paneDockCopies.emplace_back(paneDockSnapshots[i]);
            region.docks.push_back({&copy, paneDockRects[i].translated(region.params.rect.topLeft())});
        }
    };
    for (auto& region : panes) addPaneDocks(region);
    // An overlay: a terminal of its own over the first pane, drawn in its
    // place with the pane's rectangle and docks, or as a popup over a part of
    // its grid, as the window does it.
    AfterglowVt::Terminal overlayTerminal(cols, rows, host, options);
    AfterglowRender::PaneSnapshot overlaySnapshot;
    if (parser.isSet(QStringLiteral("overlay"))) {
        bool ok = true;
        const float progress = parser.isSet(QStringLiteral("overlay-progress"))
            ? parser.value(QStringLiteral("overlay-progress")).toFloat(&ok) : 1.0f;
        if (!ok || !(progress >= 0 && progress <= 1)) {
            std::fprintf(stderr, "--overlay-progress must be from 0 to 1\n");
            return 1;
        }
        // The options of afterglow.overlay, read by the same rules.
        QVariantMap shape;
        for (const char* key : {"width", "height", "position"}) {
            const QString option = QStringLiteral("overlay-") + QLatin1String(key);
            if (!parser.isSet(option)) continue;
            // A number as a script gives it, else a string ("80%", a position).
            const QString value = parser.value(option);
            bool number = false;
            const double cells = value.toDouble(&number);
            shape.insert(QLatin1String(key), number ? QVariant(cells) : QVariant(value));
        }
        if (parser.isSet(QStringLiteral("overlay-title"))) shape.insert(QStringLiteral("title"), parser.value(QStringLiteral("overlay-title")));
        if (parser.isSet(QStringLiteral("overlay-border"))) {
            const QString value = parser.value(QStringLiteral("overlay-border"));
            shape.insert(QStringLiteral("border"), value == QLatin1String("true") ? QVariant(true)
                                                   : value == QLatin1String("false") ? QVariant(false) : QVariant(value));
        }
        QString problem;
        const auto spec = Afterglow::OverlaySpec::read({QStringLiteral("overlay")}, shape, &problem);
        if (!spec) {
            std::fprintf(stderr, "--overlay: %s\n", qPrintable(problem));
            return 1;
        }
        if (!spec->popup()) {
            for (const char* option : {"overlay-position", "overlay-title", "popup-border", "popup-dim", "popup-corners"}) {
                if (!parser.isSet(QLatin1String(option))) continue;
                std::fprintf(stderr, "--%s needs a popup: --overlay-width, --overlay-height or --overlay-border true\n", option);
                return 1;
            }
        }
        const float dim = parser.value(QStringLiteral("popup-dim")).toFloat(&ok);
        if (!ok || !(dim >= 0 && dim <= 1)) {
            std::fprintf(stderr, "--popup-dim must be from 0 to 1\n");
            return 1;
        }
        const Afterglow::OverlaySpec::Placement placement = spec->place(cols, rows);
        const QRect grid = spec->popup() ? placement.grid : QRect(0, 0, cols, rows);
        overlayTerminal.resize(grid.width(), grid.height(), metrics.cellWidth, metrics.cellHeight);
        const QByteArray text = parser.value(QStringLiteral("overlay")).toUtf8();
        overlayTerminal.feed(std::string_view(text.constData(), std::size_t(text.size())));
        overlaySnapshot.capture(overlayTerminal);
        // As the window keeps an ended overlay's last frame (overlayEnded).
        const bool ended = parser.isSet(QStringLiteral("overlay-ended"));
        if (ended) {
            overlaySnapshot.cursorVisible = false;
            overlaySnapshot.extraCursors.clear();
        }
        // Its own grid state is at rest: no glide, scroll shift or hovered
        // link. The rest (and what later options set) is the pane's.
        AfterglowRender::WindowRenderer::Pane::Overlay overlay;
        overlay.snapshot = &overlaySnapshot;
        overlay.progress = progress;
        overlay.running = !ended;
        if (spec->popup()) {
            // Its own cells of the pane's grid, as the window places it.
            AfterglowRender::WindowRenderer::Pane::Overlay::Popup popup;
            popup.rect = QRect(panes.front().params.contentOrigin + QPoint(grid.x() * metrics.cellWidth, grid.y() * metrics.cellHeight),
                               QSize(grid.width() * metrics.cellWidth, grid.height() * metrics.cellHeight));
            popup.border = placement.border;
            popup.title = spec->title.toStdString();
            // The window's look (popupLook): a border halfway from the
            // configured background to the configured text, and that text
            // colour for the title. OSC 10 and 11 in a terminal's text
            // change its own colours, not these.
            popup.borderColor =
                AfterglowRender::packRgba(Afterglow::Themes::derivedPopupBorder(options.defaultBackground, options.defaultForeground));
            if (parser.isSet(QStringLiteral("popup-border"))) {
                const QColor border = QColor::fromString(parser.value(QStringLiteral("popup-border")));
                if (!border.isValid()) {
                    std::fprintf(stderr, "--popup-border must be a color\n");
                    return 1;
                }
                popup.borderColor = AfterglowRender::packRgba(std::uint8_t(border.red()), std::uint8_t(border.green()), std::uint8_t(border.blue()));
            }
            popup.titleColor = AfterglowRender::packRgba(options.defaultForeground);
            popup.bidi = options.bidi;
            if (parser.isSet(QStringLiteral("popup-corners"))) {
                const QString corners = parser.value(QStringLiteral("popup-corners"));
                if (corners != QLatin1String("rounded") && corners != QLatin1String("square")) {
                    std::fprintf(stderr, "--popup-corners must be rounded or square\n");
                    return 1;
                }
                popup.roundedCorners = corners == QLatin1String("rounded");
            }
            popup.dim = dim;
            overlay.popup = std::move(popup);
        }
        panes.front().overlay = overlay;
    } else {
        for (const char* option : {"overlay-progress", "overlay-transition", "overlay-ended", "overlay-width", "overlay-height", "overlay-position", "overlay-border",
                                   "overlay-title", "popup-border", "popup-dim", "popup-corners"}) {
            if (!parser.isSet(QLatin1String(option))) continue;
            std::fprintf(stderr, "--%s needs --overlay\n", option);
            return 1;
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
        // A popup's cursor moves in its own, smaller grid.
        if (panes.front().overlay && (previousX >= overlaySnapshot.cols || previousY >= overlaySnapshot.rows)) {
            std::fprintf(stderr, "--wake-from requires a col,row within the overlay's grid too\n");
            return 1;
        }
        // One frame with the cursor at its old cell and the same panes, so
        // the renderer's cursor history sees the jump: the first pane's
        // cursor, and its overlay's, which shows in its place.
        std::vector<AfterglowRender::PaneSnapshot*> moved{&snapshot};
        if (panes.front().overlay) moved.push_back(&overlaySnapshot);
        std::vector<std::pair<int, int>> cells;
        for (AfterglowRender::PaneSnapshot* jumped : moved) {
            cells.emplace_back(jumped->cursorX, jumped->cursorY);
            jumped->cursorX = previousX;
            jumped->cursorY = previousY;
        }
        renderer.setClockOverride(time - parser.value(QStringLiteral("wake-age")).toDouble());
        setDate(-parser.value(QStringLiteral("wake-age")).toDouble());
        renderer.renderToImage(frameSize, panes, overlays, clear);
        for (std::size_t i = 0; i < moved.size(); ++i) {
            std::tie(moved[i]->cursorX, moved[i]->cursorY) = cells[i];
            ++moved[i]->version;
        }
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
        // Closing: open is 0, and the highlight stays on the card it closes onto.
        overview->open = !parser.isSet(QStringLiteral("overview-closing"));
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
                copy.overlay.reset(); // the overlay is the current tab's
                copy.params.focused = false;
                copy.params.highlighted = tab * perTab + i == highlight;
                addPaneDocks(copy);
                other.panes.push_back(copy);
            }
            other.overlays = overlays;
            overview->otherTabs.push_back(std::move(other));
        }
        if (highlight / perTab == currentTab) panes[std::size_t(highlight % perTab)].params.highlighted = true;
    }
    // The tab being left: its own blue copies, apart from the overview's.
    // Copied before the pane effects below set the last pane's opening,
    // context and pulse, which are the current tab's.
    std::vector<AfterglowRender::PaneSnapshot> leftSnapshots(panes.size(), snapshot);
    std::optional<AfterglowRender::WindowRenderer::TabTransition> transition;
    if (parser.isSet(QStringLiteral("tab-transition"))) {
        for (auto& copy : leftSnapshots) copy.defaultBg = 0xFF5A2A10u;
        transition.emplace();
        transition->active = true;
        transition->progress = parser.isSet(QStringLiteral("transition-progress")) ? parser.value(QStringLiteral("transition-progress")).toFloat() : 0.5f;
        transition->direction = parser.value(QStringLiteral("transition-direction")).toInt() < 0 ? -1.0f : 1.0f;
        // Another tab than the current one, tab 0.
        transition->from.index = 1;
        for (std::size_t i = 0; i < panes.size(); ++i) {
            auto copy = panes[i];
            copy.snapshot = &leftSnapshots[i];
            copy.overlay.reset(); // the overlay is the current tab's
            copy.params.focused = false;
            addPaneDocks(copy);
            transition->from.panes.push_back(copy);
        }
        transition->from.overlays = overlays;
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
        panes.back().params.previousContext = parser.value(QStringLiteral("previous-context")).toInt();
        effects = true;
    }
    if (parser.isSet(QStringLiteral("pulse"))) {
        const auto spec = parser.value(QStringLiteral("pulse")).split(QLatin1Char(':'));
        const QString progress = spec.value(1, QStringLiteral("0.5")), color = spec.value(2, QStringLiteral("4"));
        if (spec.size() < 1 || spec.size() > 3 || !validNumber(spec.value(0), 1, 255, true) || !validNumber(progress, 0, 1)
            || !validNumber(color, 0, 255, true)) {
            std::fprintf(stderr, "--pulse requires EFFECT:PROGRESS[:COLOR], effect 1..255, progress 0..1, colour 0..255\n");
            return 1;
        }
        panes.back().params.pulseEffect = spec.value(0).toInt();
        panes.back().params.pulseProgress = progress.toFloat();
        panes.back().params.pulseColor = color.toInt();
        effects = true;
    }
    if (effects) renderer.setPaneEffects({true, true, ghost, closeProgress});
    // The window turns translucent for an opacity below 1 or content with
    // translucent cells (a pane, its overlay, a dock), and then draws no
    // subpixel text anywhere.
    const auto translucentDock = [](const AfterglowRender::WindowRenderer::Pane& dock) { return dock.snapshot->hasTransparentBackgrounds; };
    const auto translucentPane = [&](const AfterglowRender::WindowRenderer::Pane& region) {
        return region.params.backgroundAlpha < 255 || region.snapshot->hasTransparentBackgrounds
            || (region.overlay && region.overlay->snapshot->hasTransparentBackgrounds)
            || std::any_of(region.docks.begin(), region.docks.end(), [](const auto& d) { return d.snapshot->hasTransparentBackgrounds; });
    };
    renderer.setTranslucent(std::any_of(panes.begin(), panes.end(), translucentPane)
                            || std::any_of(docks.begin(), docks.end(), translucentDock));
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
        // iDate: the date given, advanced with the frames.
        setDate(elapsed);
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
        // The renderer takes docks for one frame, as the window sets them before each.
        renderer.setDocks(docks);
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
    // A click goes through the window chain's maps to the pane drawn there,
    // then through that pane's chain to its cell, as the window finds it.
    const auto& cellMetrics = renderer.fonts().fonts->metrics();
    for (const QString& value : parser.values(QStringLiteral("pointer"))) {
        const QStringList xy = value.split(QLatin1Char(','));
        bool okX = false, okY = false;
        const QPointF at(xy.value(0).toDouble(&okX), xy.value(1).toDouble(&okY));
        if (xy.size() != 2 || !okX || !okY) {
            std::fprintf(stderr, "--pointer takes x,y: %s\n", qPrintable(value));
            return 1;
        }
        const QPointF window = renderer.windowContentPoint(at);
        const auto pane = std::find_if(panes.begin(), panes.end(), [&](const auto& p) {
            return QRectF(p.params.rect).contains(window);
        });
        if (pane == panes.end()) {
            std::printf("pointer %g,%g: window %.2f,%.2f, in no pane\n", at.x(), at.y(), window.x(), window.y());
            continue;
        }
        const QPointF content = renderer.paneContentPoint(window, pane->params.id);
        const QPointF grid = content - QPointF(pane->params.contentOrigin);
        // Outside the cells, the window takes the nearest cell at the edge.
        const int col = int(std::floor(grid.x() / cellMetrics.cellWidth)), row = int(std::floor(grid.y() / cellMetrics.cellHeight));
        const int nearestCol = std::clamp(col, 0, std::max(0, pane->snapshot->cols - 1));
        const int nearestRow = std::clamp(row, 0, std::max(0, pane->snapshot->rows - 1));
        std::printf("pointer %g,%g: window %.2f,%.2f, pane %u at %.2f,%.2f, cell %d,%d%s\n", at.x(), at.y(), window.x(),
                    window.y(), pane->params.id, content.x(), content.y(), nearestCol, nearestRow,
                    nearestCol != col || nearestRow != row ? " (outside the cells: the nearest one)" : "");
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
    // On a machine without Vulkan as the device asks for it, every error is
    // put down to that, and the message starts with "no Vulkan: ", so that a
    // test skips there. On a machine with Vulkan an error fails the test.
    try {
        return run(app);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s%s\n", AfterglowRender::GpuDevice::vulkanAvailable() ? "" : "no Vulkan: ", e.what());
        return 1;
    }
}
