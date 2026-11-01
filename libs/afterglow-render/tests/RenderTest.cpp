// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/RenderFrame.h>

#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/GridRenderer.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Search.h>
#include <AfterglowVt/Terminal.h>

#include <QGuiApplication>
#include <QImage>

#include <QMatrix4x4>
#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <limits>
#include <string>

using namespace AfterglowRender;

namespace {

class NullHost : public AfterglowVt::ITerminalHost
{
public:
    void writeToPty(std::string_view) override { }
};

/// The face FontSet picks for @p cp has a glyph for it (not a fallback .notdef).
bool hasGlyph(FontSet& fonts, char32_t cp)
{
    return fonts.hasGlyph(fonts.faceFor(cp, StyleSlot::Regular, false), cp);
}

std::unique_ptr<GpuDevice> s_device;

class Render : public ::testing::Test
{
protected:
    static constexpr int Cols = 20;
    static constexpr int Rows = 4;

    void SetUp() override
    {
        if (!s_device) {
            GTEST_SKIP() << "no Vulkan device";
        }
        renderer = std::make_unique<WindowRenderer>(*s_device, nullptr);
        const FontMetrics& m = renderer->fonts().fonts->metrics();
        cw = m.cellWidth;
        ch = m.cellHeight;
        ASSERT_GT(cw, 0);
        ASSERT_GT(ch, 0);
        terminal = std::make_unique<AfterglowVt::Terminal>(Cols, Rows, host);
        terminal->resize(Cols, Rows, cw, ch);
    }

    QImage render(const std::string& bytes, bool focused = true, std::uint8_t backgroundAlpha = 255)
    {
        terminal->feed(bytes);
        snapshot.capture(*terminal);
        const QSize size(Cols * cw, Rows * ch);
        WindowRenderer::Pane pane;
        pane.snapshot = &snapshot;
        pane.params.rect = QRect(QPoint(0, 0), size);
        pane.params.focused = focused;
        pane.params.backgroundAlpha = backgroundAlpha;
        const std::uint32_t clear = (snapshot.defaultBg & 0x00FFFFFFu) | (std::uint32_t(backgroundAlpha) << 24);
        return renderer->renderToImage(size, {pane}, {}, clear);
    }

    // Packed 0xAABBGGRR as the snapshot stores colours, alpha ignored.
    static QRgb rgb(std::uint32_t c) { return qRgb(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF); }
    QPoint cellCenter(int x, int y) const { return {x * cw + cw / 2, y * ch + ch / 2}; }

    NullHost host;
    std::unique_ptr<WindowRenderer> renderer;
    std::unique_ptr<AfterglowVt::Terminal> terminal;
    PaneSnapshot snapshot;
    int cw = 0;
    int ch = 0;
};

#define EXPECT_PIXEL_NEAR(image, point, expected, tolerance)                                                                   \
    do {                                                                                                         \
        const QRgb _got = (image).pixel(point);                                                                  \
        const QRgb _want = (expected);                                                                           \
        EXPECT_LE(std::abs(qRed(_got) - qRed(_want)) + std::abs(qGreen(_got) - qGreen(_want))                    \
                      + std::abs(qBlue(_got) - qBlue(_want)),                                                    \
                  tolerance)                                                                                     \
            << "at " << (point).x() << "," << (point).y() << ": got " << std::hex << _got << " want " << _want; \
    } while (0)
#define EXPECT_PIXEL(image, point, expected) EXPECT_PIXEL_NEAR(image, point, expected, 6)

namespace {

struct Captured
{
    NullHost host;
    AfterglowVt::Terminal terminal;
    PaneSnapshot snapshot;

    explicit Captured(const AfterglowVt::TerminalOptions& options)
        : terminal(20, 2, host, options)
    {
    }
    const SnapshotCell& capture(const std::string& bytes, const AfterglowVt::Selection* selection = nullptr)
    {
        terminal.feed(bytes);
        snapshot.capture(terminal, selection);
        return snapshot.at(0, 0);
    }
    AfterglowVt::Selection word(int col)
    {
        const AfterglowVt::Screen& s = terminal.screen();
        AfterglowVt::Selection sel;
        sel.anchor = sel.head = {s.rowsDropped() + s.historySize(), col};
        sel.mode = AfterglowVt::SelectionMode::Word;
        sel.layoutEpoch = s.layoutEpoch();
        sel.active = true;
        return sel;
    }
};

using Clock = std::chrono::steady_clock;

WindowRenderer::Overview overviewAt(bool open, float amount, float position, std::optional<Clock::time_point> changedAt,
                                    std::optional<Clock::time_point> movedAt = {})
{
    WindowRenderer::Overview overview;
    overview.open = open;
    overview.amount = amount;
    overview.position = position;
    overview.changedAt = changedAt;
    overview.movedAt = movedAt;
    return overview;
}

// Named fields, so a struct that gains one does not warn (-Wmissing-field-initializers).
WindowRenderer::OverviewTab tabOf(int index, std::vector<WindowRenderer::Pane> panes)
{
    WindowRenderer::OverviewTab tab;
    tab.index = index;
    tab.panes = std::move(panes);
    return tab;
}

WindowRenderer::TabTransition switchFrom(float progress, float direction, WindowRenderer::OverviewTab from)
{
    WindowRenderer::TabTransition transition;
    transition.active = true;
    transition.progress = progress;
    transition.direction = direction;
    transition.startedAt = std::chrono::steady_clock::now();
    transition.from = std::move(from);
    return transition;
}

double contrastRatio(std::uint32_t a, std::uint32_t b)
{
    auto lum = [](std::uint32_t c) {
        auto ch = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * ch((c & 0xFF) / 255.0) + 0.7152 * ch(((c >> 8) & 0xFF) / 255.0)
            + 0.0722 * ch(((c >> 16) & 0xFF) / 255.0);
    };
    const double la = lum(a);
    const double lb = lum(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

} // namespace

TEST(Snapshot, ScrollbackCountsRetainedPrimaryRows)
{
    AfterglowVt::TerminalOptions options;
    options.maxScrollback = 4;
    Captured pane(options);
    pane.capture("one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix\r\n");
    EXPECT_EQ(pane.snapshot.scrollbackLines, 4u); // capped; dropped rows do not count
    pane.terminal.setViewportOffset(2);
    pane.capture("");
    EXPECT_EQ(pane.snapshot.scrollbackLines, 4u); // independent of viewport position
    pane.capture("\x1b[?1049hfull-screen app\r\n\r\n\r\n");
    ASSERT_EQ(pane.terminal.screen().historySize(), 0u);
    EXPECT_EQ(pane.snapshot.scrollbackLines, 4u);
    options.maxScrollback = 2;
    pane.terminal.setOptions(options);
    pane.capture("");
    EXPECT_EQ(pane.snapshot.scrollbackLines, 2u);
    pane.terminal.clearScreenAndHistory();
    pane.capture("");
    EXPECT_EQ(pane.snapshot.scrollbackLines, 0u);
    pane.capture("\x1b[?1049l");
    EXPECT_EQ(pane.snapshot.scrollbackLines, 0u);
}

TEST(Snapshot, BoldIsBrightUsesTheBrightColours)
{
    AfterglowVt::TerminalOptions o;
    EXPECT_EQ(Captured(o).capture("\x1b[1;31mA").fg, packRgba(o.basePalette[1]));
    o.boldIsBright = true;
    EXPECT_EQ(Captured(o).capture("\x1b[1;31mA").fg, packRgba(o.basePalette[9]));
    EXPECT_EQ(Captured(o).capture("\x1b[31mA").fg, packRgba(o.basePalette[1])); // not bold
    EXPECT_EQ(Captured(o).capture("\x1b[1;91mA").fg, packRgba(o.basePalette[9])); // already bright
}

TEST(Snapshot, MinimumContrastLiftsFaintColours)
{
    AfterglowVt::TerminalOptions o;
    const std::string dim = "\x1b[38;2;40;40;40mA";
    const SnapshotCell plain = Captured(o).capture(dim);
    EXPECT_EQ(plain.fg, packRgba(40, 40, 40));
    o.minimumContrast = 4.5;
    Captured lifted(o);
    const SnapshotCell cell = lifted.capture(dim);
    EXPECT_GE(contrastRatio(cell.fg, lifted.snapshot.defaultBg), 4.5);
    EXPECT_LT(contrastRatio(cell.fg, lifted.snapshot.defaultBg), 5.0); // only as far as needed
    // Colours that already stand out stay as they are.
    EXPECT_EQ(Captured(o).capture("\x1b[38;2;250;250;250mA").fg, packRgba(250, 250, 250));
}

TEST(Snapshot, ConfiguredSelectionAndCursorTextColours)
{
    AfterglowVt::TerminalOptions o;
    Captured plain(o);
    plain.capture("x");
    EXPECT_EQ(plain.snapshot.cursorTextColor, plain.snapshot.defaultBg);
    o.selectionBackground = AfterglowVt::Rgb{0x20, 0x40, 0x60};
    o.cursorTextColor = AfterglowVt::Rgb{1, 2, 3};
    Captured c(o);
    c.capture("word");
    const AfterglowVt::Selection sel = c.word(0);
    EXPECT_EQ(c.capture("", &sel).bg, packRgba(0x20, 0x40, 0x60));
    EXPECT_EQ(c.snapshot.cursorTextColor, packRgba(1, 2, 3));
}

TEST(Snapshot, WordSeparatorsShapeWordSelections)
{
    AfterglowVt::TerminalOptions o;
    Captured standard(o);
    standard.capture("foo:bar");
    AfterglowVt::Selection sel = standard.word(0);
    standard.capture("", &sel);
    EXPECT_NE(standard.snapshot.at(2, 0).bg, 0u);
    EXPECT_EQ(standard.snapshot.at(3, 0).bg, 0u); // ':' ends the word and is not part of it
    EXPECT_EQ(standard.snapshot.at(4, 0).bg, 0u);
    o.wordBoundaries = U" ";
    Captured custom(o);
    custom.capture("foo:bar");
    sel = custom.word(0);
    custom.capture("", &sel);
    EXPECT_NE(custom.snapshot.at(6, 0).bg, 0u);
}

TEST(Fonts, CellScaleGrowsCellsAndCentresText)
{
    FontSpec spec;
    FontSet natural(spec, 1.0);
    spec.cellHeightScale = 1.5f;
    spec.cellWidthScale = 1.5f;
    FontSet scaled(spec, 1.0);
    const FontMetrics& a = natural.metrics();
    const FontMetrics& b = scaled.metrics();
    EXPECT_NEAR(b.cellHeight, a.cellHeight * 1.5, 1.0);
    EXPECT_NEAR(b.cellWidth, a.cellWidth * 1.5, 1.0);
    EXPECT_EQ(a.glyphOffsetX, 0);
    EXPECT_NEAR(b.glyphOffsetX, (b.cellWidth - a.cellWidth) / 2, 1);
    // The extra height splits evenly above and below the text.
    EXPECT_NEAR(b.baseline - a.baseline, (b.cellHeight - a.cellHeight) / 2, 1);
}

TEST(Fonts, SymbolMapPinsRangesToAFamily)
{
    FontSpec spec;
    spec.symbolMap = {{U'A', U'C', "DejaVu Sans"}};
    FontSet fonts(spec, 1.0);
    const int mapped = fonts.faceFor(U'B', StyleSlot::Regular, false);
    if (fonts.face(mapped).path.find("DejaVu") == std::string::npos) {
        GTEST_SKIP() << "DejaVu Sans is not installed";
    }
    EXPECT_NE(mapped, fonts.primary(StyleSlot::Regular));
    EXPECT_EQ(fonts.faceFor(U'D', StyleSlot::Regular, false), fonts.primary(StyleSlot::Regular)); // outside the range
}

TEST(Fonts, HintingChoosesLoadFlags)
{
    FontSpec spec;
    spec.hinting = Hinting::None;
    const auto none = FontSet(spec, 1.0).loadFlags();
    spec.hinting = Hinting::Full;
    const auto full = FontSet(spec, 1.0).loadFlags();
    spec.hinting = Hinting::Light;
    const auto light = FontSet(spec, 1.0).loadFlags();
    EXPECT_NE(none, full);
    EXPECT_NE(full, light);
    EXPECT_NE(none, light);
}

class FontRendering : public Render
{
protected:
    void SetUp() override
    {
        if (s_device) {
            saved = s_device->fontSpec();
        }
        Render::SetUp();
    }
    void TearDown() override
    {
        renderer.reset();
        if (s_device) {
            s_device->setFontSpec(saved);
        }
    }
    void mode(Antialiasing aa, float gamma = 1.0f, Hinting hinting = Hinting::Light)
    {
        FontSpec spec = saved;
        spec.antialiasing = aa;
        spec.gamma = gamma;
        spec.hinting = hinting;
        s_device->setFontSpec(spec);
        const auto& metrics = renderer->fonts().fonts->metrics();
        cw = metrics.cellWidth;
        ch = metrics.cellHeight;
        terminal->resize(Cols, Rows, cw, ch);
    }
    QImage sample(const std::string& text = " MWi Il -> fi", const std::string& colors = "255;255;255;48;2;0;0;0")
    {
        return render("\x1b[?25l\x1b[H\x1b#5\x1b[0;38;2;" + colors + "m\x1b[2J" + text);
    }
    static int coloredEdges(const QImage& image)
    {
        int n = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QRgb c = image.pixel(x, y);
                n += qRed(c) != qGreen(c) || qGreen(c) != qBlue(c);
            }
        }
        return n;
    }
    FontSpec saved;
};

TEST_F(FontRendering, ModesAndHintingProduceTheRequestedCoverage)
{
    for (const auto hint : {Hinting::None, Hinting::Light, Hinting::Full}) {
        for (const auto aa : {Antialiasing::None, Antialiasing::Grayscale, Antialiasing::Rgb, Antialiasing::Bgr}) {
            mode(aa, 1.0f, hint);
            const QImage img = sample();
            ASSERT_FALSE(img.isNull());
            int ink = 0, intermediate = 0;
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    const QRgb c = img.pixel(x, y);
                    EXPECT_EQ(qAlpha(c), 255);
                    ink += qRed(c) > 0;
                    intermediate += qRed(c) > 0 && qRed(c) < 255;
                }
            }
            EXPECT_GT(ink, 0);
            if (aa == Antialiasing::None) {
                EXPECT_EQ(intermediate, 0);
            } else {
                EXPECT_GT(intermediate, 0);
            }
            if (aa == Antialiasing::Rgb || aa == Antialiasing::Bgr) {
                EXPECT_GT(coloredEdges(img), 0);
            } else {
                EXPECT_EQ(coloredEdges(img), 0);
            }
        }
    }
}

TEST_F(FontRendering, BgrReversesRedAndBlueCoverage)
{
    mode(Antialiasing::Rgb);
    const QImage rgb = sample();
    mode(Antialiasing::Bgr);
    const QImage bgr = sample();
    ASSERT_EQ(rgb.size(), bgr.size());
    EXPECT_GT(coloredEdges(rgb), 0);
    for (int y = 0; y < rgb.height(); ++y) {
        for (int x = 0; x < rgb.width(); ++x) {
            const QRgb a = rgb.pixel(x, y), b = bgr.pixel(x, y);
            EXPECT_EQ(qRed(a), qBlue(b));
            EXPECT_EQ(qGreen(a), qGreen(b));
            EXPECT_EQ(qBlue(a), qRed(b));
        }
    }
}

TEST_F(FontRendering, SubpixelCompositesEachChannelOverColoredBackgrounds)
{
    for (const auto aa : {Antialiasing::Rgb, Antialiasing::Bgr}) {
        for (const float gamma : {1.0f, 1.6f}) {
            mode(aa, gamma);
            const QImage coverage = sample();
            const QImage colored = sample(" MWi Il -> fi", "220;90;40;48;2;30;120;200");
            for (int y = 0; y < colored.height(); ++y) {
                for (int x = 0; x < colored.width(); ++x) {
                    const QRgb c = coverage.pixel(x, y), actual = colored.pixel(x, y);
                    EXPECT_NEAR(qRed(actual), 30 + (220 - 30) * qRed(c) / 255.0, 2);
                    EXPECT_NEAR(qGreen(actual), 120 + (90 - 120) * qGreen(c) / 255.0, 2);
                    EXPECT_NEAR(qBlue(actual), 200 + (40 - 200) * qBlue(c) / 255.0, 2);
                    EXPECT_EQ(qAlpha(actual), 255);
                }
            }
        }
    }
}

TEST_F(FontRendering, GammaChangesCoverageButLeavesMonochromeBinary)
{
    for (const auto aa : {Antialiasing::Grayscale, Antialiasing::Rgb, Antialiasing::None}) {
        mode(aa);
        // Isolate one glyph: coverage gamma is applied before overlapping
        // glyphs are composited, so it is not a power of the final pixel.
        const QImage normal = sample(" M ");
        mode(aa, 2.0f);
        const QImage stronger = sample(" M ");
        int changed = 0;
        for (int y = 0; y < normal.height(); ++y) {
            for (int x = 0; x < normal.width(); ++x) {
                const QRgb a = normal.pixel(x, y), b = stronger.pixel(x, y);
                changed += a != b;
                EXPECT_NEAR(qRed(b), 255 * std::sqrt(qRed(a) / 255.0), 2);
                EXPECT_NEAR(qGreen(b), 255 * std::sqrt(qGreen(a) / 255.0), 2);
                EXPECT_NEAR(qBlue(b), 255 * std::sqrt(qBlue(a) / 255.0), 2);
            }
        }
        if (aa == Antialiasing::None) {
            EXPECT_EQ(changed, 0);
        } else {
            EXPECT_GT(changed, 0);
        }
    }
}

TEST_F(FontRendering, SubpixelFallbackTracksShadersAndTranslucencyWithoutSnapshotChanges)
{
    const QImage gray = sample();
    mode(Antialiasing::Rgb);
    const QImage lcd = sample();
    EXPECT_GT(coloredEdges(lcd), 0);
    WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(QPoint(), lcd.size());
    pane.params.focused = true;
    auto redraw = [&] { return renderer->renderToImage(lcd.size(), {pane}, {}, packRgba(0, 0, 0)); };
    for (int layer = 0; layer < 3; ++layer) {
        ShaderChains chains;
        if (layer == 0) chains.text = {ShaderProgram::passthrough()};
        if (layer == 1) chains.pane = {ShaderProgram::passthrough()};
        if (layer == 2) chains.window = {ShaderProgram::passthrough()};
        renderer->setShaderChains(chains);
        EXPECT_EQ(redraw(), gray);
        renderer->setShaderChains({});
        EXPECT_EQ(redraw(), lcd);
    }
    renderer->setTranslucent(true);
    EXPECT_EQ(redraw(), gray);
    renderer->setTranslucent(false);
    EXPECT_EQ(redraw(), lcd);
    pane.params.backgroundAlpha = 150;
    // Explicit cell backgrounds are opaque, but the pane still requests the
    // fallback. This also checks cached instances after an opacity change.
    EXPECT_EQ(redraw(), gray);
    pane.params.backgroundAlpha = 255;
    EXPECT_EQ(redraw(), lcd);
}

TEST_F(FontRendering, IconsAndScaledTextWorkInEveryMode)
{
    for (const auto aa : {Antialiasing::None, Antialiasing::Grayscale, Antialiasing::Rgb, Antialiasing::Bgr}) {
        mode(aa);
        if (!hasGlyph(*renderer->fonts().fonts, U'\uf015')) GTEST_SKIP() << "no font with Nerd Font icons";
        const QImage icon = sample(" \uf015 ");
        int ink = 0;
        for (int y = 0; y < ch; ++y) {
            for (int x = cw; x < cw * 3; ++x) {
                ink += qRed(icon.pixel(x, y)) != 0;
            }
        }
        EXPECT_GT(ink, 0);
        const QImage scaled = sample("\x1b#6 WMi");
        EXPECT_EQ(coloredEdges(scaled) > 0, aa == Antialiasing::Rgb || aa == Antialiasing::Bgr);
        EXPECT_NE(scaled, sample(" WMi"));
    }
}

TEST_F(FontRendering, SubpixelColorRunsRespectFaintTextAndWhiteBackgrounds)
{
    mode(Antialiasing::Rgb);
    const QImage white = sample(" MMMM", "255;255;255;48;2;0;0;0");
    const QImage black = sample(" MMMM", "0;0;0;48;2;255;255;255");
    for (int y = 0; y < white.height(); ++y) {
        for (int x = 0; x < white.width(); ++x) {
            const QRgb a = white.pixel(x, y), b = black.pixel(x, y);
            EXPECT_NEAR(qRed(a) + qRed(b), 255, 1);
            EXPECT_NEAR(qGreen(a) + qGreen(b), 255, 1);
            EXPECT_NEAR(qBlue(a) + qBlue(b), 255, 1);
        }
    }
    const QImage mixed = sample(" \x1b[31mM\x1b[32mM\x1b[34mM\x1b[2mM");
    const QImage plain = sample(" \x1b[34mMMMM");
    int redInk = 0, greenInk = 0, blueInk = 0, faintInk = 0;
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            redInk += qRed(mixed.pixel(cw + x, y));
            greenInk += qGreen(mixed.pixel(2 * cw + x, y));
            blueInk += qBlue(mixed.pixel(3 * cw + x, y));
            faintInk += qBlue(mixed.pixel(4 * cw + x, y));
            EXPECT_EQ(mixed.pixel(3 * cw + x, y), plain.pixel(3 * cw + x, y));
        }
    }
    EXPECT_GT(redInk, 0);
    EXPECT_GT(greenInk, 0);
    EXPECT_GT(blueInk, faintInk);
    EXPECT_GT(faintInk, 0);
}

TEST_F(FontRendering, ColorEmojiBypassAntialiasingAndGamma)
{
    FontSet& fonts = *renderer->fonts().fonts;
    if (!fonts.face(fonts.faceFor(U'🙂', StyleSlot::Regular, true)).color) {
        GTEST_SKIP() << "no color emoji font";
    }
    const QImage original = sample(" 🙂 ");
    for (const auto aa : {Antialiasing::None, Antialiasing::Grayscale, Antialiasing::Rgb, Antialiasing::Bgr}) {
        mode(aa, 2.0f);
        EXPECT_EQ(sample(" 🙂 "), original);
    }
}

TEST_F(FontRendering, SubpixelCoverageWorksAtFractionalAndHighDpi)
{
    for (const double dpr : {1.25, 2.0}) {
        renderer->setDevicePixelRatio(dpr);
        mode(Antialiasing::Rgb);
        const QImage lcd = sample();
        EXPECT_GT(coloredEdges(lcd), 0);
        mode(Antialiasing::Grayscale);
        const QImage gray = sample();
        EXPECT_EQ(coloredEdges(gray), 0);
        EXPECT_EQ(gray.size(), lcd.size());
    }
}

TEST_F(Render, EmptyScreenIsBackground)
{
    const QImage img = render("\x1b[?25l");
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.size(), QSize(Cols * cw, Rows * ch));
    EXPECT_PIXEL(img, QPoint(0, 0), rgb(snapshot.defaultBg));
    EXPECT_PIXEL(img, cellCenter(Cols - 1, Rows - 1), rgb(snapshot.defaultBg));
}

TEST_F(Render, TranslucentBackgroundKeepsItsAlpha)
{
    // The pane background over the translucent clear colour must not
    // compound: 0.9 stays 0.9, not 0.99. Text stays opaque.
    const QImage img = render("\x1b[?25l\u2588", true, 230);
    EXPECT_NEAR(qAlpha(img.pixel(cellCenter(5, 2))), 230, 2);
    EXPECT_EQ(qAlpha(img.pixel(cellCenter(0, 0))), 255);
}

TEST_F(Render, TruecolorBackgroundFillsCells)
{
    const QImage img = render("\x1b[?25l\x1b[48;2;200;30;60m  \x1b[0m");
    EXPECT_PIXEL(img, QPoint(0, 0), qRgb(200, 30, 60));
    EXPECT_PIXEL(img, QPoint(2 * cw - 1, ch - 1), qRgb(200, 30, 60));
    EXPECT_PIXEL(img, cellCenter(2, 0), rgb(snapshot.defaultBg));
}

TEST_F(Render, BlockSpritesCoverTheirCell)
{
    // Full block, upper half, lower half, in a known foreground.
    const QImage img = render("\x1b[?25l\x1b[38;2;10;220;90m█▀▄");
    const QRgb fg = qRgb(10, 220, 90);
    const QRgb bg = rgb(snapshot.defaultBg);
    EXPECT_PIXEL(img, QPoint(0, 0), fg);
    EXPECT_PIXEL(img, QPoint(cw - 1, ch - 1), fg);
    EXPECT_PIXEL(img, QPoint(cw + cw / 2, 1), fg);
    EXPECT_PIXEL(img, QPoint(cw + cw / 2, ch - 2), bg);
    EXPECT_PIXEL(img, QPoint(2 * cw + cw / 2, 1), bg);
    EXPECT_PIXEL(img, QPoint(2 * cw + cw / 2, ch - 2), fg);
}

TEST_F(Render, InverseSwapsColours)
{
    const QImage img = render("\x1b[?25l\x1b[7m \x1b[0m");
    EXPECT_PIXEL(img, cellCenter(0, 0), rgb(snapshot.defaultFg));
}

TEST_F(Render, FocusedBlockCursorFillsItsCell)
{
    const QImage img = render("\x1b[2 q\x1b[2;3H");
    EXPECT_NE(img.pixel(cellCenter(2, 1)), rgb(snapshot.defaultBg));
    EXPECT_PIXEL(img, cellCenter(3, 1), rgb(snapshot.defaultBg));
}

TEST_F(Render, OwnedCursorBlinkOverridesInvalidateCachedFramesAndRestoreTerminalMode)
{
    for (int terminalStyle : {1, 2}) {
        render("\x1b[" + std::to_string(terminalStyle) + " q");
        WindowRenderer::Pane pane{&snapshot, {}};
        const QSize size(Cols * cw, Rows * ch);
        pane.params.rect = QRect(QPoint(), size);
        pane.params.focused = true;
        pane.params.cursorBlinkOn = false;
        auto draw = [&] {
            return renderer->renderToImage(size, {pane}, {}, snapshot.defaultBg).pixel(cellCenter(0, 0));
        };
        pane.params.cursorBlinkOverride = 0;
        EXPECT_NE(draw(), rgb(snapshot.defaultBg));
        pane.params.cursorBlinkOverride = 1;
        EXPECT_EQ(draw(), rgb(snapshot.defaultBg));
        pane.params.cursorBlinkOverride = -1;
        if (terminalStyle == 1) EXPECT_EQ(draw(), rgb(snapshot.defaultBg));
        else EXPECT_NE(draw(), rgb(snapshot.defaultBg));
    }
}

TEST_F(Render, ScaledBlockFillsItsCells)
{
    // OSC 66 s=2 with a full block: 2x2 cells of foreground.
    const QImage img = render("\x1b[?25l\x1b[38;2;250;120;20m\x1b]66;s=2;\u2588\x07");
    const QRgb fg = qRgb(250, 120, 20);
    EXPECT_PIXEL(img, cellCenter(0, 0), fg);
    EXPECT_PIXEL(img, cellCenter(1, 1), fg);
    EXPECT_PIXEL(img, QPoint(2 * cw - 1, 2 * ch - 1), fg);
    EXPECT_PIXEL(img, cellCenter(2, 0), rgb(snapshot.defaultBg));
    EXPECT_PIXEL(img, cellCenter(0, 2), rgb(snapshot.defaultBg));
}

TEST_F(Render, DoubleSizeLines)
{
    const QRgb fg = qRgb(30, 200, 240);
    // Double width: one cell's block covers two cells of one row.
    QImage img = render("\x1b[?25l\x1b[38;2;30;200;240m\x1b#6\u2588");
    EXPECT_PIXEL(img, cellCenter(0, 0), fg);
    EXPECT_PIXEL(img, cellCenter(1, 0), fg);
    EXPECT_PIXEL(img, cellCenter(2, 0), rgb(snapshot.defaultBg));
    EXPECT_PIXEL(img, cellCenter(0, 1), rgb(snapshot.defaultBg));
    // Double height: top and bottom halves make a 2x2 block across two rows.
    img = render("\x1b[2J\x1b[H\x1b#3\u2588\r\n\x1b#4\u2588");
    EXPECT_PIXEL(img, cellCenter(1, 0), fg);
    EXPECT_PIXEL(img, cellCenter(0, 1), fg);
    EXPECT_PIXEL(img, cellCenter(2, 1), rgb(snapshot.defaultBg));
}

TEST_F(Render, KittyImageIsTopDown)
{
    // A 2x2 image: red, green / blue, white, scaled over 2x2 cells. Its first
    // row must land at the top (an earlier bug drew images upside down).
    const std::string rgba("\xff\x00\x00\xff\x00\xff\x00\xff\x00\x00\xff\xff\xff\xff\xff\xff", 16);
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    for (std::size_t i = 0; i < rgba.size(); i += 3) {
        std::uint32_t v = std::uint8_t(rgba[i]) << 16;
        if (i + 1 < rgba.size()) {
            v |= std::uint8_t(rgba[i + 1]) << 8;
        }
        if (i + 2 < rgba.size()) {
            v |= std::uint8_t(rgba[i + 2]);
        }
        enc += b64[(v >> 18) & 63];
        enc += b64[(v >> 12) & 63];
        enc += i + 1 < rgba.size() ? b64[(v >> 6) & 63] : '=';
        enc += i + 2 < rgba.size() ? b64[v & 63] : '=';
    }
    const QImage img = render("\x1b[?25l\x1b[2;2H\x1b_Ga=T,f=32,s=2,v=2,c=2,r=2,q=2;" + enc + "\x1b\\");
    // Each image pixel covers one cell of the 2x2-cell placement at (1, 1).
    // The image is filtered, so cell centres off texel centres blend a little.
    EXPECT_PIXEL_NEAR(img, cellCenter(1, 1), qRgb(255, 0, 0), 40);
    EXPECT_PIXEL_NEAR(img, cellCenter(2, 1), qRgb(0, 255, 0), 40);
    EXPECT_PIXEL_NEAR(img, cellCenter(1, 2), qRgb(0, 0, 255), 40);
    EXPECT_PIXEL_NEAR(img, cellCenter(2, 2), qRgb(255, 255, 255), 40);
    EXPECT_PIXEL(img, cellCenter(0, 0), rgb(snapshot.defaultBg));
}

} // namespace

namespace {
ShaderProgramPtr layerShader(const char* id, const char* body, bool hdr = false)
{
    ShaderProgramSource src;
    src.id = QString::fromLatin1(id);
    src.highDynamicRange = hdr;
    const QByteArray text(body);
    // A body that reads the previous text opts in to it, as a pack does.
    src.code = QByteArray(text.contains("uPreviousText") ? "#include <terminal_history.glsl>\n" : "")
        + "vec4 pTerminal(vec2 uv)\n{\n    vec4 c = terminalTexel(uv);\n    vec4 fragColor = c;\n"
        + text + "\n    return fragColor;\n}\n";
    QString error;
    auto program = ShaderProgram::compile(src, &error);
    EXPECT_TRUE(program) << error.toStdString();
    return program;
}
} // namespace

TEST_F(Render, LiveParameterOverridesReachLayerPaneAndWindowUniforms)
{
    auto source = layerShader("live-parameters", "fragColor = vec4(customParams[0].x, customColors[0].g, 0.0, 1.0);");
    ASSERT_TRUE(source);
    auto program = source->instance(QStringLiteral("test-instance"));
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(0, 0), size);
    ShaderParameterValues values;
    values.paramMask = 1; values.params[0] = 0.25f;
    values.colorMask = 1; values.colors[1] = 0.75f;
    for (int layer = 0; layer < 3; ++layer) {
        ShaderChains chains;
        if (layer == 0) chains.background = {program};
        if (layer == 1) chains.pane = {program};
        if (layer == 2) chains.window = {program};
        renderer->setShaderChains(chains);
        pane.params.shaderParameters.insert(program->instanceId(), values);
        auto image = renderer->renderToImage(size, {pane}, {}, 0xFF000000u);
        EXPECT_PIXEL(image, cellCenter(1, 1), qRgb(64, 191, 0));
        pane.params.shaderParameters.clear();
        image = renderer->renderToImage(size, {pane}, {}, 0xFF000000u);
        EXPECT_PIXEL(image, cellCenter(1, 1), qRgb(0, 0, 0));
    }
}

TEST_F(Render, PersistentBufferAccumulatesAcrossFramesAndAllScopes)
{
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    const QSize size(Cols*cw, Rows*ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(0,0), size);
    for (bool half : {false, true}) for (int scope = 0; scope < 5; ++scope) {
        ShaderProgramSource src;
        src.id = QStringLiteral("feedback");
        ShaderBufferSource buffer; buffer.feedback = true; buffer.halfFloat = half; buffer.scale = .5f;
        buffer.code = "#include <terminal_multipass.glsl>\n"
            "vec4 pTerminal(vec2 uv){vec4 c=texture(iChannel0,uv)+vec4(customParams[0].x,0,0,0);c.a=1.;return c;}";
        src.buffers = {buffer};
        src.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
        QString error; auto compiled = ShaderProgram::compile(src, &error); ASSERT_TRUE(compiled) << error.toStdString();
        auto program = compiled->instance(QStringLiteral("feedback-live"));
        ShaderChains chains;
        auto* chain = scope==0 ? &chains.background : scope==1 ? &chains.text : scope==2 ? &chains.cursor : scope==3 ? &chains.pane : &chains.window;
        chain->push_back(program); renderer->setShaderChains(chains);
        ShaderParameterValues values; values.paramMask=1; values.params[0]=.1f;
        pane.params.shaderParameters.insert(program->instanceId(), values);
        renderer->setClockOverride(1.f);
        auto image=renderer->renderToImage(size,{pane},{},0xFF000000u);
        EXPECT_PIXEL(image,cellCenter(2,2),qRgb(26,0,0));
        renderer->setClockOverride(2.f);
        image=renderer->renderToImage(size,{pane},{},0xFF000000u);
        EXPECT_PIXEL_NEAR(image,cellCenter(2,2),qRgb(51,0,0),3);
        renderer->setClockOverride(.5f); // Clock rewind resets accumulation.
        image=renderer->renderToImage(size,{pane},{},0xFF000000u);
        EXPECT_PIXEL(image,cellCenter(2,2),qRgb(26,0,0));
        renderer->setShaderChains(chains); // Reload is also a reset.
        image=renderer->renderToImage(size,{pane},{},0xFF000000u);
        EXPECT_PIXEL(image,cellCenter(2,2),qRgb(26,0,0));
        image=renderer->renderToImage(size+QSize(cw,ch),{pane},{},0xFF000000u);
        EXPECT_PIXEL(image,cellCenter(2,2),qRgb(26,0,0));
    }
}

TEST_F(Render, PersistentBufferIsolatesPanesAndResetsIdentityAndTheme)
{
    ShaderProgramSource src; src.id=QStringLiteral("isolation");
    ShaderBufferSource buffer; buffer.feedback=true; buffer.scale=.375f;
    buffer.code="#include <terminal_multipass.glsl>\n"
        "vec4 pTerminal(vec2 uv){vec4 c=texture(iChannel0,uv+vec2(10,0))+vec4(customParams[0].x,0,0,0);c.a=1.;return c;}";
    src.buffers={buffer};
    src.code="#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
    QString error; auto p=ShaderProgram::compile(src,&error); ASSERT_TRUE(p) << error.toStdString();
    auto program=p->instance(QStringLiteral("isolation"));
    ShaderChains chains; chains.background={program}; renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal); PaneSnapshot other=snapshot;
    const QSize size(Cols*cw+3,Rows*ch+1), frame(size.width()*2,size.height());
    WindowRenderer::Pane left{&snapshot,{}},right{&other,{}};
    left.params.rect=QRect(QPoint(0,0),size); right.params.rect=QRect(QPoint(size.width(),0),size);
    right.params.contentOrigin=right.params.rect.topLeft();
    ShaderParameterValues values;values.paramMask=1;values.params[0]=.1f;
    left.params.shaderParameters.insert(program->instanceId(),values);
    renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    auto image=renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    EXPECT_PIXEL(image,cellCenter(2,2),qRgb(51,0,0));
    EXPECT_PIXEL(image,cellCenter(2,2)+QPoint(size.width(),0),qRgb(0,0,0));
    left.params.shaderParameters.clear();
    left.params.rect.setWidth(size.width()-cw); // Geometry reset.
    image=renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    EXPECT_PIXEL(image,cellCenter(2,2),qRgb(0,0,0));
    left.params.shaderParameters.insert(program->instanceId(),values);
    renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    left.params.shaderParameters.clear(); PaneSnapshot newTab=snapshot;left.snapshot=&newTab;
    image=renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    EXPECT_PIXEL(image,cellCenter(2,2),qRgb(0,0,0));
    left.params.shaderParameters.insert(program->instanceId(),values);
    renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    left.params.shaderParameters.clear();newTab.defaultBg=0xFF102030u;
    image=renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    EXPECT_PIXEL(image,cellCenter(2,2),qRgb(0,0,0));
}

TEST_F(Render, BufferFormatRetainsHdrAndNonfeedbackClears)
{
    for (bool half : {false,true}) {
        ShaderProgramSource src;src.id=QStringLiteral("buffer-format");
        ShaderBufferSource buffer; buffer.halfFloat=half;
        buffer.code="#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return vec4(texture(iChannel0,uv).r+4.,0,0,1);}";
        src.buffers={buffer};
        src.code="#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return vec4(texture(iChannel0,uv).r/4.,0,0,1);}";
        QString error;auto program=ShaderProgram::compile(src,&error);ASSERT_TRUE(program);
        ShaderChains chains;chains.pane={program};renderer->setShaderChains(chains);
        auto image=render("\x1b[?25l");
        EXPECT_PIXEL(image,cellCenter(2,2),qRgb(half ? 255 : 64,0,0));
        image=render(""); // No feedback: input remains clear on every pass.
        EXPECT_PIXEL(image,cellCenter(2,2),qRgb(half ? 255 : 64,0,0));
    }
}

TEST_F(Render, BufferInstanceLimitUsesTransparentFallback)
{
    ShaderProgramSource src; src.id=QStringLiteral("bounded");
    ShaderBufferSource buffer; buffer.feedback=true;
    buffer.code="vec4 pTerminal(vec2 uv){return vec4(.5,0,0,1);}";
    src.buffers={buffer};
    src.code="#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
    QString error;auto program=ShaderProgram::compile(src,&error);ASSERT_TRUE(program);
    ShaderChains chains;
    for(int i=0;i<34;++i) chains.pane.push_back(program);
    renderer->setShaderChains(chains);
    auto image=render("\x1b[?25l");
    EXPECT_EQ(image.pixelColor(cellCenter(2,2)).alpha(),0);
    image=render("");
    EXPECT_EQ(image.pixelColor(cellCenter(2,2)).alpha(),0);
    src.buffers.front().scale=0; EXPECT_FALSE(ShaderProgram::compile(src,&error));
}

TEST_F(Render, ExplicitColorConversionsPreserveAlphaAndLinearLight)
{
    // Averaging black and white in linear light is approximately sRGB .735.
    ShaderChains chains; chains.window={layerShader("linear-mix", R"(
        vec4 a=toLinearPremultiplied(vec4(0.0,0.0,0.0,0.5));
        vec4 b=toLinearPremultiplied(vec4(0.5,0.5,0.5,0.5));
        fragColor=toSrgbPremultiplied(mix(a,b,.5));
    )")};
    renderer->setShaderChains(chains); auto image=render("\x1b[?25l",true,128);
    auto pixel=image.pixel(cellCenter(2,2));
    EXPECT_NEAR(qAlpha(pixel),128,1);
    // pixelColor() returns straight color components from premultiplied storage.
    EXPECT_NEAR(image.pixelColor(cellCenter(2,2)).redF(),.735, .02);
}

TEST_F(Render, IdentityLayerShadersChangeNothing)
{
    // Coloured cells, an underline, translucency and a block cursor on a letter.
    const std::string content = "\x1b[2 q\x1b[42m bg \x1b[0m \x1b[4mul\x1b[0m █x\x1b[1D";
    const QImage plain = render(content, true, 200);
    ShaderChains chains;
    chains.background = {ShaderProgram::passthrough()};
    chains.text = {ShaderProgram::passthrough()};
    chains.cursor = {ShaderProgram::passthrough(), ShaderProgram::passthrough()};
    renderer->setShaderChains(std::move(chains));
    const QImage layered = render("", true, 200);
    ASSERT_EQ(plain.size(), layered.size());
    int worst = 0;
    for (int y = 0; y < plain.height(); ++y) {
        for (int x = 0; x < plain.width(); ++x) {
            const QRgb a = plain.pixel(x, y);
            const QRgb b = layered.pixel(x, y);
            worst = std::max({worst, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                              std::abs(qBlue(a) - qBlue(b)), std::abs(qAlpha(a) - qAlpha(b))});
        }
    }
    EXPECT_LE(worst, 1);
}

TEST_F(Render, LayerShadersSeeOnlyTheirLayer)
{
    // A full block (text) at 0, a blank cell at 1.
    const std::string content = "\x1b[?25l█";
    const QImage plain = render(content);
    ShaderChains chains;
    chains.text = {layerShader("red-text", "fragColor = vec4(c.a, 0.0, 0.0, c.a);")};
    renderer->setShaderChains(std::move(chains));
    QImage img = render("");
    EXPECT_PIXEL(img, cellCenter(0, 0), qRgb(255, 0, 0));
    EXPECT_PIXEL(img, cellCenter(1, 0), plain.pixel(cellCenter(1, 0)));

    chains = {};
    chains.background = {layerShader("blue-bg", "fragColor = vec4(0.0, 0.0, c.a, c.a);")};
    renderer->setShaderChains(std::move(chains));
    img = render("");
    EXPECT_PIXEL(img, cellCenter(0, 0), rgb(snapshot.defaultFg));
    EXPECT_PIXEL(img, cellCenter(1, 0), qRgb(0, 0, 255));
}

TEST_F(Render, AudioSpectrumIsSharedAcrossPanesAndClearsOnStop)
{
    ShaderProgramSource source;
    source.id = QStringLiteral("audio");
    source.code = R"(
#include <terminal_audio.glsl>
vec4 pTerminal(vec2 uv) {
    if (uv.x > 0.5)
        return vec4(audioBar(0), audioBarSmooth(0.5), audioBar(63), 1.0);
    return vec4(getBass(), getMids(), getTreble(), 1.0);
})";
    QString error;
    auto shader = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(shader) << error.toStdString();
    EXPECT_TRUE(shader->requiresAudio());
    EXPECT_FALSE(shader->animated()); // Spectrum updates, not a busy render loop.
    ShaderChains chains;
    chains.pane = {shader};
    EXPECT_TRUE(chains.audioReactive());
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane left{&snapshot, {}}, right{&snapshot, {}};
    left.params.rect = QRect(QPoint(0, 0), size);
    right.params.rect = QRect(QPoint(size.width(), 0), size);
    right.params.contentOrigin = right.params.rect.topLeft();
    auto draw = [&] { return renderer->renderToImage(QSize(size.width()*2, size.height()), {left, right}, {}, 0xFF000000u); };
    std::array<float, GpuDevice::AudioBars> spectrum;
    std::fill(spectrum.begin(), spectrum.begin()+20, 0.25f);
    std::fill(spectrum.begin()+20, spectrum.begin()+48, 0.5f);
    std::fill(spectrum.begin()+48, spectrum.end(), 0.75f);
    s_device->setAudioSpectrum(spectrum);
    QImage img = draw();
    for (const QPoint offset : {QPoint(), QPoint(size.width(), 0)}) {
        EXPECT_PIXEL(img, cellCenter(1, 0) + offset, qRgb(64, 128, 191));
        EXPECT_PIXEL(img, cellCenter(15, 0) + offset, qRgb(64, 128, 191));
    }
    s_device->setAudioSpectrum({});
    img = draw();
    EXPECT_PIXEL(img, cellCenter(1, 0), qRgb(0, 0, 0));
    EXPECT_PIXEL(img, cellCenter(15, 0) + QPoint(size.width(), 0), qRgb(0, 0, 0));
    // Non-audio shaders see zeros even while another window uses capture.
    s_device->setAudioSpectrum(spectrum);
    source.code = "vec4 pTerminal(vec2 uv) { return vec4(uAudioLevels.xyz + float(iAudioSpectrumSize), 1.0); }";
    auto disabled = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(disabled);
    ASSERT_FALSE(disabled->requiresAudio());
    chains.pane = {disabled};
    renderer->setShaderChains(chains);
    img = draw();
    EXPECT_PIXEL(img, cellCenter(1, 0), qRgb(0, 0, 0));
    EXPECT_PIXEL(img, cellCenter(15, 0), qRgb(0, 0, 0));
    // Same-size reload must replace the fallback sampler with spectrum data.
    chains.pane = {shader};
    renderer->setShaderChains(chains);
    EXPECT_PIXEL(draw(), cellCenter(15, 0), qRgb(64, 128, 191));
    s_device->setAudioSpectrum({});
}

TEST_F(Render, AudioInputSanitizesInvalidAndPartialFrames)
{
    std::array<float, GpuDevice::AudioBars> spectrum;
    spectrum.fill(1.0f);
    spectrum[0] = std::numeric_limits<float>::quiet_NaN();
    spectrum[1] = std::numeric_limits<float>::infinity();
    spectrum[2] = -1.0f;
    spectrum[3] = 9.0f;
    s_device->setAudioSpectrum(spectrum);
    EXPECT_EQ(s_device->audioSpectrumSize(), 64);
    EXPECT_NEAR(s_device->audioLevels()[0], std::sqrt(17.0f/20.0f), 0.0001f);
    EXPECT_FLOAT_EQ(s_device->audioLevels()[1], 1.0f);
    s_device->setAudioSpectrum(std::span<const float>(spectrum.data(), 12));
    EXPECT_EQ(s_device->audioSpectrumSize(), 0);
    for (float value : s_device->audioLevels()) EXPECT_EQ(value, 0);
}

TEST_F(Render, ScrollbackCombinesOnlyTheCurrentTabsPanes)
{
    ShaderChains chains;
    chains.pane = {layerShader("scrollback", "fragColor = vec4(uScrollback.xyz / 20.0, 1.0);")};
    chains.window = {layerShader("scrollback-window", "fragColor = vec4(c.r, uScrollback.yz / 20.0, 1.0);")};
    ASSERT_TRUE(chains.readsScrollback());
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l" + std::string(11, '\n'));
    snapshot.capture(*terminal);
    ASSERT_EQ(snapshot.scrollbackLines, 8u);
    AfterglowVt::Terminal other(Cols, Rows, host);
    other.feed("\x1b[?25l" + std::string(7, '\n'));
    PaneSnapshot rightSnapshot;
    rightSnapshot.capture(other);
    ASSERT_EQ(rightSnapshot.scrollbackLines, 4u);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane left{&snapshot, {}}, right{&rightSnapshot, {}};
    left.params.rect = QRect(QPoint(0, 0), size);
    right.params.rect = QRect(QPoint(size.width(), 0), size);
    right.params.contentOrigin = right.params.rect.topLeft();
    const QSize frame(size.width()*2, size.height());
    auto draw = [&](float time, const std::vector<WindowRenderer::Pane>& panes) {
        renderer->setClockOverride(time);
        return renderer->renderToImage(frame, panes, {}, 0xFF000000u);
    };
    auto img = draw(0, {left, right});
    EXPECT_PIXEL(img, cellCenter(1, 0), qRgb(102, 153, 153));
    EXPECT_PIXEL(img, cellCenter(1, 0) + QPoint(size.width(), 0), qRgb(51, 153, 153));
    EXPECT_FALSE(renderer->animating());

    // Output on a background tab is not among the renderer's supplied panes.
    AfterglowVt::Terminal background(Cols, Rows, host);
    background.feed("\x1b[?25l" + std::string(5, '\n'));
    PaneSnapshot backgroundSnapshot;
    backgroundSnapshot.capture(background);
    ASSERT_EQ(backgroundSnapshot.scrollbackLines, 2u);
    img = draw(0.1f, {left, right});
    EXPECT_PIXEL(img, cellCenter(1, 0), qRgb(102, 153, 153));
    EXPECT_FALSE(renderer->animating());

    // Selecting that tab replaces the raw total immediately and eases the look.
    left.snapshot = &backgroundSnapshot;
    img = draw(0.2f, {left});
    QRgb pixel = img.pixel(cellCenter(1, 0));
    EXPECT_NEAR(qRed(pixel), 26, 1);
    EXPECT_NEAR(qGreen(pixel), 26, 1);
    EXPECT_GT(qBlue(pixel), 26);
    EXPECT_LT(qBlue(pixel), 153);
    EXPECT_TRUE(renderer->animating());
    for (int i = 3; i <= 100; ++i) img = draw(float(i)*0.1f, {left});
    EXPECT_PIXEL(img, cellCenter(1, 0), qRgb(26, 26, 26));
    EXPECT_FALSE(renderer->animating());

    background.clearScreenAndHistory();
    backgroundSnapshot.capture(background);
    img = draw(10.1f, {left});
    pixel = img.pixel(cellCenter(1, 0));
    EXPECT_EQ(qGreen(pixel), 0);
    EXPECT_GT(qBlue(pixel), 0);
    EXPECT_TRUE(renderer->animating());
    for (int i = 102; i <= 150; ++i) img = draw(float(i)*0.1f, {left});
    EXPECT_PIXEL(img, cellCenter(1, 0), qRgb(0, 0, 0));
    EXPECT_FALSE(renderer->animating());
}

TEST_F(Render, ScrollbackHandlesPaneRemovalAndClockReset)
{
    ShaderChains chains;
    chains.background = {layerShader("scrollback", "fragColor = vec4(uScrollback.xyz / 20.0, 1.0);")};
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l" + std::string(7, '\n'));
    snapshot.capture(*terminal);
    PaneSnapshot other = snapshot;
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane left{&snapshot, {}}, right{&other, {}};
    left.params.rect = QRect(QPoint(0, 0), size);
    right.params.rect = QRect(QPoint(size.width(), 0), size);
    right.params.contentOrigin = right.params.rect.topLeft();
    renderer->setClockOverride(10);
    renderer->renderToImage(size*2, {left, right}, {}, 0xFF000000u);
    renderer->setClockOverride(10.1f);
    auto img = renderer->renderToImage(size, {left}, {}, 0xFF000000u);
    EXPECT_NEAR(qGreen(img.pixel(cellCenter(1, 0))), 51, 1);
    EXPECT_GT(qBlue(img.pixel(cellCenter(1, 0))), 51);
    EXPECT_TRUE(renderer->animating());
    renderer->setClockOverride(0);
    img = renderer->renderToImage(size, {left}, {}, 0xFF000000u);
    EXPECT_PIXEL(img, cellCenter(1, 0), qRgb(51, 51, 51));
    EXPECT_FALSE(renderer->animating());
    renderer->setScrollbackLinesOverride(20);
    renderer->setClockOverride(0.1f);
    renderer->renderToImage(size, {left}, {}, 0xFF000000u);
    EXPECT_TRUE(renderer->animating());
    renderer->setShaderChains({});
    EXPECT_FALSE(renderer->animating());
}

TEST_F(Render, HdrHighlightsSurviveLayersAndIntermediatePasses)
{
    const QImage plain = render("\x1b[?25l");
    ShaderChains chains;
    chains.background = {layerShader("hdr-light", "fragColor = vec4(4.0, 2.0, 0.5, 1.0);", true)};
    ASSERT_TRUE(chains.background[0]->requiresHdr());
    chains.pane = {ShaderProgram::passthrough()};
    chains.window = {ShaderProgram::passthrough(), layerShader("expose", "fragColor = vec4(c.rgb / 4.0, c.a);")};
    renderer->setShaderChains(chains);
    EXPECT_PIXEL(render(""), cellCenter(1, 0), qRgb(255, 128, 32));
    // Changing formats with an unchanged framebuffer size rebuilds compatible
    // targets and pipelines. Repeated toggles catch descriptor address reuse.
    for (int i = 0; i < 3; ++i) {
        ShaderChains ldr;
        ldr.background = {ShaderProgram::passthrough()};
        renderer->setShaderChains(ldr);
        EXPECT_PIXEL(render(""), cellCenter(1, 0), plain.pixel(cellCenter(1, 0)));
        renderer->setShaderChains(chains);
        EXPECT_PIXEL(render(""), cellCenter(1, 0), qRgb(255, 128, 32));
    }
    renderer->releaseSwapChain();
    EXPECT_PIXEL(render(""), cellCenter(1, 0), qRgb(255, 128, 32));
}

TEST_F(Render, HdrTextHistoryPreservesRadiance)
{
    ShaderChains chains;
    chains.text = {layerShader("hdr-history", R"(
        fragColor = max(vec4(c.rgb * 4.0, c.a), texture(uPreviousText, uv) * 0.5);
    )", true)};
    chains.window = {layerShader("expose", "fragColor = vec4(c.rgb / 4.0, c.a);")};
    renderer->setShaderChains(chains);
    const std::string content = "\x1b[?25l\x1b]11;#000000\x1b\\\x1b[38;2;255;0;0m█";
    EXPECT_PIXEL(render(content), cellCenter(0, 0), qRgb(255, 0, 0));
    EXPECT_PIXEL(render("\x1b[2J"), cellCenter(0, 0), qRgb(128, 0, 0));
    EXPECT_PIXEL(render(""), cellCenter(0, 0), qRgb(64, 0, 0));
}

TEST_F(Render, TextHistoryDecaysAndResets)
{
    ShaderChains chains;
    chains.text = {layerShader("history", R"(
        vec4 previous = texture(uPreviousText, uv);
        fragColor = max(c, previous * 0.5);
    )")};
    renderer->setShaderChains(chains);
    EXPECT_TRUE(renderer->animating());
    renderer->setClockOverride(1.0f);
    // A red block makes the previous frame unambiguous at the cell centre.
    const QImage first = render("\x1b[?25l\x1b]11;#000000\x1b\\\x1b[38;2;255;0;0m█");
    EXPECT_PIXEL(first, cellCenter(0, 0), qRgb(255, 0, 0));
    renderer->setClockOverride(1.1f);
    const QImage second = render("\x1b[2J\x1b[H");
    EXPECT_PIXEL(second, cellCenter(0, 0), qRgb(128, 0, 0));
    EXPECT_PIXEL(second, cellCenter(1, 0), qRgb(0, 0, 0));
    renderer->setClockOverride(1.2f);
    EXPECT_PIXEL(render(""), cellCenter(0, 0), qRgb(64, 0, 0));
    renderer->setShaderChains(chains);
    EXPECT_PIXEL(render(""), cellCenter(0, 0), qRgb(0, 0, 0));

    render("█");
    // A theme switch must not resurrect the old text.
    EXPECT_PIXEL(render("\x1b[2J\x1b]11;#000020\x1b\\"), cellCenter(0, 0), qRgb(0, 0, 32));
    render("\x1b[H█");
    renderer->setClockOverride(0.5f);
    EXPECT_PIXEL(render("\x1b[2J"), cellCenter(0, 0), qRgb(0, 0, 32));
    renderer->setShaderChains({});
    EXPECT_FALSE(renderer->animating());
    EXPECT_PIXEL(render(""), cellCenter(0, 0), qRgb(0, 0, 32));
}

TEST_F(Render, TextHistoryDoesNotLeakAcrossPanesOrLayouts)
{
    ShaderChains chains;
    chains.text = {layerShader("history", "fragColor = max(c, texture(uPreviousText, uv) * 0.5);")};
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l\x1b]11;#000000\x1b\\\x1b[38;2;255;0;0m█");
    snapshot.capture(*terminal);
    PaneSnapshot other = snapshot;
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane left{&snapshot, {}}, right{&other, {}};
    left.params.rect = QRect(QPoint(0, 0), size);
    right.params.rect = QRect(QPoint(size.width(), 0), size);
    right.params.contentOrigin = right.params.rect.topLeft();
    const QSize frame(size.width() * 2, size.height());
    renderer->renderToImage(frame, {left, right}, {}, 0xFF000000u);
    terminal->feed("\x1b[2J");
    // Each pane captures into its own snapshot, as Pane does: an assignment
    // would make a new snapshot identity, which is a new pane to the renderer.
    snapshot.capture(*terminal);
    other.capture(*terminal);
    QImage img = renderer->renderToImage(frame, {left, right}, {}, 0xFF000000u);
    EXPECT_PIXEL(img, cellCenter(0, 0), qRgb(128, 0, 0));
    EXPECT_PIXEL(img, cellCenter(0, 0) + QPoint(size.width(), 0), qRgb(128, 0, 0));
    // Same framebuffer, changed split geometry: both histories are cleared.
    left.params.rect.setWidth(size.width() - cw);
    right.params.rect.setLeft(size.width() - cw);
    img = renderer->renderToImage(frame, {left, right}, {}, 0xFF000000u);
    EXPECT_PIXEL(img, cellCenter(0, 0), qRgb(0, 0, 0));
    EXPECT_PIXEL(img, cellCenter(0, 0) + QPoint(size.width(), 0), qRgb(0, 0, 0));
    terminal->feed("\x1b[H█");
    snapshot.capture(*terminal);
    renderer->renderToImage(frame, {left, right}, {}, 0xFF000000u);
    PaneSnapshot newTab = other;
    left.snapshot = &newTab;
    img = renderer->renderToImage(frame, {left, right}, {}, 0xFF000000u);
    EXPECT_PIXEL(img, cellCenter(0, 0), qRgb(0, 0, 0));
    // Resize reallocates and clears the history texture.
    img = renderer->renderToImage(frame + QSize(cw, ch), {left, right}, {}, 0xFF000000u);
    EXPECT_PIXEL(img, cellCenter(0, 0), qRgb(0, 0, 0));
}

TEST_F(Render, IconsTakeTwoCellsBeforeABlank)
{
    // A Nerd Font icon followed by a blank is fitted to two cells and
    // centred, as prompts expect; one followed by text keeps to its cell.
    auto inkIn = [&](const QImage& img, int col) {
        int ink = 0;
        for (int y = 0; y < ch; ++y) {
            for (int x = col * cw; x < (col + 1) * cw; ++x) {
                ink += img.pixel(x, y) != rgb(snapshot.defaultBg);
            }
        }
        return ink;
    };
    // Without a Nerd Font the icon is the primary face's .notdef box.
    if (!hasGlyph(*renderer->fonts().fonts, U'\uf015')) {
        GTEST_SKIP() << "no font with Nerd Font icons";
    }
    const QImage spaced = render("\x1b[?25l\uf015 ");
    EXPECT_GT(inkIn(spaced, 1), 0);
    // Centred over both cells: about as much ink in each.
    const double ratio = double(inkIn(spaced, 0)) / std::max(1, inkIn(spaced, 1));
    EXPECT_GT(ratio, 0.6);
    EXPECT_LT(ratio, 1.6);
    terminal->feed("\x1b[2J\x1b[H");
    const QImage tight = render("\uf015\uf015\uf015");
    EXPECT_GT(inkIn(tight, 0), 0);
    EXPECT_GT(inkIn(tight, 1), 0);
}

TEST_F(Render, ShaderChainsSurviveTranslucencyChanges)
{
    // Toggling translucency releases the swapchain; the shaders must stay.
    ShaderChains chains;
    chains.text = {layerShader("red-text", "fragColor = vec4(c.a, 0.0, 0.0, c.a);")};
    renderer->setShaderChains(std::move(chains));
    render("");
    renderer->releaseSwapChain();
    const QImage img = render("\x1b[?25l\u2588");
    EXPECT_PIXEL(img, cellCenter(0, 0), qRgb(255, 0, 0));
}

TEST_F(Render, BlockCursorLayerStaysUnderItsText)
{
    // The cursor on a full block: its text takes the background colour and
    // must stay over the (now green) cursor.
    const std::string content = "\x1b[2 q█ \x1b[2D";
    ShaderChains chains;
    chains.cursor = {layerShader("green-cursor", "fragColor = vec4(0.0, c.a, 0.0, c.a);")};
    renderer->setShaderChains(std::move(chains));
    const QImage img = render(content);
    EXPECT_PIXEL(img, cellCenter(0, 0), rgb(snapshot.defaultBg));

    // A bar cursor goes over the text instead.
    const QImage bar = render("\x1b[6 q");
    EXPECT_PIXEL(bar, QPoint(0, ch / 2), qRgb(0, 255, 0));
}

TEST_F(Render, MultipleCursorsMixBlocksAndBeamsWithHiddenMainCursor)
{
    const std::string content = "\x1b[?25l\x1b[2 q█ █\x1b[>1;2:1:1 q\x1b[>2;2:1:3 q";
    auto img = render(content);
    EXPECT_PIXEL(img, cellCenter(0, 0), rgb(snapshot.defaultBg));
    EXPECT_PIXEL(img, QPoint(2 * cw, ch / 2), rgb(snapshot.cursorColor));
    ShaderChains chains;
    chains.cursor = {layerShader("green-cursors", "fragColor = vec4(0.0, c.a, 0.0, c.a);")};
    renderer->setShaderChains(std::move(chains));
    img = render("");
    EXPECT_PIXEL(img, cellCenter(0, 0), rgb(snapshot.defaultBg));
    EXPECT_PIXEL(img, QPoint(2 * cw, ch / 2), qRgb(0, 255, 0));
    terminal->feed("\x1b[>0;4 q"); snapshot.capture(*terminal);
    EXPECT_TRUE(snapshot.extraCursors.empty());
}

TEST_F(Render, UnicodePlaceholderImageSurvivesSnapshotCapture)
{
    const QImage img = render("\x1b[?25l\x1b_Ga=T,f=32,s=1,v=1,i=7,p=1,U=1,c=1,r=1,q=2;/wAA/w==\x1b\\"
                             "\x1b[38;2;0;0;7m\xf4\x8e\xbb\xae\x1b[0m");
    ASSERT_EQ(snapshot.images.size(), 1u);
    EXPECT_PIXEL(img, cellCenter(0, 0), qRgb(255, 0, 0));
}

TEST_F(Render, ShapeCacheTracksFontResourceAcrossDprChanges)
{
    terminal->feed("office ffi AV ij");
    snapshot.capture(*terminal);
    for (qreal dpr : {1.0, 2.0, 1.25, 1.0}) {
        renderer->setDevicePixelRatio(dpr);
        const auto& metrics = renderer->fonts().fonts->metrics();
        const QSize size(Cols * metrics.cellWidth, Rows * metrics.cellHeight);
        WindowRenderer::Pane pane;
        pane.snapshot = &snapshot;
        pane.params.rect = QRect(QPoint(), size);
        const auto reused = renderer->renderToImage(size, {pane}, {}, snapshot.defaultBg);
        WindowRenderer fresh(*s_device, nullptr);
        fresh.setDevicePixelRatio(dpr);
        const auto expected = fresh.renderToImage(size, {pane}, {}, snapshot.defaultBg);
        EXPECT_EQ(reused, expected) << "DPR " << dpr;
    }
}

TEST_F(Render, SpriteCachePreservesFullMulticellWidth)
{
    auto& resources = renderer->fonts();
    const auto* wide = resources.glyphs.sprite(*resources.fonts, U'\u2588', 49, 2);
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(wide->width, cw * 49);
    EXPECT_EQ(wide->height, ch * 2);
    const auto* narrow = resources.glyphs.sprite(*resources.fonts, U'\u2588', 17, 2);
    ASSERT_NE(narrow, nullptr);
    EXPECT_EQ(narrow->width, cw * 17);
}

TEST_F(Render, NonNeutralBackgroundKeepsRedAndBlueChannels)
{
    const auto image = render("\x1b]11;#b03070\x07\x1b[?25l");
    EXPECT_PIXEL(image, cellCenter(10, 2), qRgb(0xb0, 0x30, 0x70));
    EXPECT_PIXEL(image, cellCenter(10, 2), rgb(snapshot.defaultBg));
}

TEST_F(Render, FullAtlasAddsPagesWithoutInvalidatingEarlierGlyphs)
{
    auto& cache = renderer->fonts().glyphs;
    cache.reset();
    render("A");
    const auto epoch = cache.epoch();
    int x = 0, y = 0;
    while (cache.grayPage().allocate(128, 128, x, y)) {}
    for (int height = 1; height <= ch; ++height)
        while (cache.grayPage().allocate(1, height, x, y)) {}
    const auto actual = render("B");
    EXPECT_GT(cache.grayPageCount(), 1u);
    EXPECT_EQ(cache.epoch(), epoch);
    cache.reset();
    const auto expected = render("");
    EXPECT_EQ(actual, expected);
}

TEST(Fonts, StaticStyleSlotsAndSymbolMappingsSelectStyledFaces)
{
    FontSpec spec;
    spec.regular.family = spec.bold.family = spec.italic.family = spec.boldItalic.family = "DejaVu Sans Mono";
    spec.symbolMap = {{U'A', U'Z', "DejaVu Sans Mono"}};
    FontSet fonts(spec, 1.0);
    if (fonts.face(fonts.primary(StyleSlot::Regular)).path.find("DejaVu") == std::string::npos) {
        GTEST_SKIP() << "DejaVu Sans Mono is not installed";
    }
    EXPECT_NE(fonts.face(fonts.primary(StyleSlot::Regular)).path, fonts.face(fonts.primary(StyleSlot::Bold)).path);
    EXPECT_NE(fonts.face(fonts.primary(StyleSlot::Regular)).path, fonts.face(fonts.primary(StyleSlot::Italic)).path);
    const int regular = fonts.faceFor(U'A', StyleSlot::Regular, false);
    const int bold = fonts.faceFor(U'A', StyleSlot::Bold, false);
    EXPECT_NE(fonts.face(regular).path, fonts.face(bold).path);
}

TEST_F(Render, ArabicLigatureUsesLeftEdgeOfItsVisualCells)
{
    AfterglowVt::TerminalOptions options;
    options.bidi = AfterglowVt::BidiMode::LeftToRight;
    AfterglowVt::Terminal rtl(Cols, Rows, host, options);
    rtl.feed("\x1b[?25l\xd9\x84\xd8\xa7"); // lam + alef
    PaneSnapshot snap;
    snap.capture(rtl);
    ASSERT_EQ(snap.logicalColumn(0, 0), 1);
    FrameBuilder builder;
    std::vector<Instance> instances;
    FrameBuilder::Ranges ranges;
    FrameBuilder::PaneParams params;
    params.rect = QRect(0, 0, Cols * cw, Rows * ch);
    auto& resources = renderer->fonts();
    if (!hasGlyph(*resources.fonts, U'\u0644')) {
        GTEST_SKIP() << "no font with Arabic letters";
    }
    builder.buildPane(snap, params, *resources.fonts, resources.glyphs, instances, ranges);
    std::vector<Instance> text;
    for (std::size_t i = ranges.textStart; i < ranges.cursorStart; ++i)
        if (instances[i].kind == std::uint32_t(InstanceKind::GrayGlyph)) text.push_back(instances[i]);
    ASSERT_EQ(text.size(), 1u); // contextual lam-alef ligature, not isolated forms
    EXPECT_LT(text.front().x, cw);
}

TEST_F(Render, ShaderUniformsUseResolvedCursorAndSelectionColors)
{
    snapshot.capture(*terminal);
    snapshot.cursorTextColor = packRgba(11, 22, 33);
    snapshot.selectionForegroundColor = packRgba(44, 55, 66);
    snapshot.selectionBackgroundColor = packRgba(77, 88, 99);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    for (const auto& [name, expected] : std::vector<std::pair<const char*, QRgb>>{
             {"uThemeCursorTextColor", qRgb(11, 22, 33)},
             {"uSelectionForegroundColor", qRgb(44, 55, 66)},
             {"uSelectionBackgroundColor", qRgb(77, 88, 99)}}) {
        const std::string body = std::string("fragColor = vec4(") + name + ".rgb, 1.0);";
        ShaderChains chains;
        chains.pane = {layerShader(name, body.c_str())};
        renderer->setShaderChains(chains);
        const auto image = renderer->renderToImage(size, {pane}, {}, snapshot.defaultBg);
        EXPECT_PIXEL(image, cellCenter(2, 2), expected);
    }
}

TEST_F(Render, MultipleBuffersCarryDepthAndSamplerHelpers)
{
    ShaderProgramSource source;
    source.id = "multipass-depth";
    source.useDepthBuffer = true;
    ShaderBufferSource first;
    first.code = "vec4 pTerminal(vec2 uv){ depthBuffer=vec4(0.25,0.5,0.75,1); return vec4(1,0,0,1); }";
    first.wrap = ShaderWrap::Repeat;
    ShaderBufferSource second;
    second.code = "#include <terminal_multipass.glsl>\nvec4 sampleIt(sampler2D s,vec2 uv){return texture(s,uv);} "
        "vec4 pTerminal(vec2 uv){depthBuffer=texture(uDepthBuffer,vec2(0.5)); return sampleIt(iChannel0,vec2(2.5));}";
    source.buffers = {first, second};
    source.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){ return texture(uDepthBuffer,vec2(0.5)); }";
    QString error;
    const auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains;
    chains.window = {program};
    renderer->setShaderChains(chains);
    const auto image = render("\x1b[?25l");
    EXPECT_PIXEL(image, cellCenter(10,2), qRgb(64,128,191));
}

TEST_F(Render, VertexHookDeformsQuadUsingSharedParameters)
{
    ShaderProgramSource source;
    source.id = "vertex-hook";
    source.vertexCode = "vec4 pVertex(vec2 uv){vec4 p=terminalQuadPosition(uv); p.x *= customParams[0].x; return p;}";
    source.params[0] = .5f;
    source.code = "vec4 pTerminal(vec2 uv){return vec4(1,0,0,1);}";
    QString error;
    const auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains;
    chains.window = {program};
    renderer->setShaderChains(chains);
    const auto image = render("\x1b[?25l");
    EXPECT_PIXEL(image, cellCenter(10,2), qRgb(255,0,0));
    // Outside the shrunk quad: the window's clear colour.
    EXPECT_PIXEL(image, cellCenter(1,2), rgb(snapshot.defaultBg));
}

TEST_F(Render, ImageOverridesAreLiveAndDefaultsReturnWithoutRecompile)
{
    ShaderProgramSource source;
    source.id = "image-live";
    source.images[0] = QImage(2,2,QImage::Format_RGBA8888);
    source.images[0].fill(Qt::red);
    source.code = "vec4 readImage(sampler2D s,vec2 uv){return texture(s,uv);} vec4 pTerminal(vec2 uv){return readImage(uTexture0,vec2(.5));}";
    QString error;
    auto program = ShaderProgram::compile(source,&error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains;
    chains.pane = {program->instance("image")};
    renderer->setShaderChains(chains);
    snapshot.capture(*terminal);
    const QSize size(Cols*cw,Rows*ch);
    WindowRenderer::Pane pane{&snapshot,{}};
    pane.params.rect = QRect(QPoint(),size);
    const auto draw = [&]{return renderer->renderToImage(size,{pane},{},snapshot.defaultBg);};
    EXPECT_PIXEL(draw(),cellCenter(10,2),qRgb(255,0,0));
    ShaderParameterValues values;
    values.imageMask = 1;
    values.images[0] = source.images[0];
    values.images[0].fill(Qt::blue);
    pane.params.shaderParameters.insert("image", values);
    EXPECT_PIXEL(draw(),cellCenter(10,2),qRgb(0,0,255));
    pane.params.shaderParameters.clear();
    EXPECT_PIXEL(draw(),cellCenter(10,2),qRgb(255,0,0));
}

TEST_F(Render, BufferRepeatAndMirrorWorkThroughSamplerHelpers)
{
    for (const auto wrap : {ShaderWrap::Repeat, ShaderWrap::Mirror}) {
        ShaderProgramSource source;
        source.id = "wrap-helper";
        ShaderBufferSource buffer;
        buffer.wrap = wrap;
        buffer.nearest = true;
        buffer.code = "vec4 pTerminal(vec2 uv){return uv.x<.5?vec4(1,0,0,1):vec4(0,1,0,1);}";
        source.buffers = {buffer};
        source.code = "#include <terminal_multipass.glsl>\n"
            "vec4 readBuffer(sampler2D s,vec2 uv){return texture(s,uv);} vec4 pTerminal(vec2 uv){return readBuffer(iChannel0,vec2(1.25,.5));}";
        QString error;
        auto program = ShaderProgram::compile(source,&error);
        ASSERT_TRUE(program) << error.toStdString();
        ShaderChains chains; chains.pane = {program}; renderer->setShaderChains(chains);
        const auto image = render("\x1b[?25l");
        EXPECT_PIXEL(image,cellCenter(10,2),wrap==ShaderWrap::Repeat?qRgb(255,0,0):qRgb(0,255,0));
    }
}

TEST_F(Render, IntermediateWindowVertexPassClearsUncoveredPixelsEveryFrame)
{
    ShaderProgramSource source;
    source.id = "vertex-intermediate";
    source.vertexCode = "vec4 pVertex(vec2 uv){vec4 p=terminalQuadPosition(uv); p.x *= iTime < 1.0 ? 1.0 : 0.5; return p;}";
    source.code = "vec4 pTerminal(vec2 uv){return vec4(1,0,0,1);}";
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains; chains.window = {program, ShaderProgram::passthrough()};
    renderer->setShaderChains(chains);
    renderer->setClockOverride(0);
    EXPECT_PIXEL(render("\x1b[?25l"),cellCenter(1,2),qRgb(255,0,0));
    renderer->setClockOverride(2);
    const auto image = render("");
    EXPECT_PIXEL(image,cellCenter(1,2),rgb(snapshot.defaultBg));
    EXPECT_PIXEL(image,cellCenter(10,2),qRgb(255,0,0));
}

TEST_F(Render, KittyTransparentColorsReplaceThePaneFill)
{
    auto img = render("\x1b[?25l\x1b]21;transparent_background_color1=red@0;transparent_background_color2=blue@.5;transparent_background_color3=green@-1\x07"
                      "\x1b[48;2;255;0;0m \x1b[48;2;0;0;255m \x1b[48;2;0;255;0m \x1b[0m", true, 180);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(qAlpha(img.pixel(cellCenter(0, 0))), 0);
    EXPECT_NEAR(qAlpha(img.pixel(cellCenter(1, 0))), 128, 2);
    EXPECT_NEAR(qAlpha(img.pixel(cellCenter(2, 0))), 180, 2);
    EXPECT_NEAR(qAlpha(img.pixel(cellCenter(3, 0))), 180, 2);
    img = render("\x1b]21;transparent_background_color1;transparent_background_color2;transparent_background_color3\x07", true, 180);
    EXPECT_EQ(qAlpha(img.pixel(cellCenter(0, 0))), 255);
    EXPECT_EQ(qAlpha(img.pixel(cellCenter(1, 0))), 255);
    EXPECT_EQ(qAlpha(img.pixel(cellCenter(2, 0))), 255);
}

TEST_F(Render, MeshInstancesUseHardwareDepthAndClearBetweenFrames)
{
    ShaderProgramSource source;
    source.id = "mesh-depth";
    source.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
    ShaderBufferSource pass;
    pass.geometry.vertexCount = 3;
    pass.geometry.instances = 2;
    pass.geometry.vertexCode = R"(
        layout(location=0) flat out int objectId;
        vec4 pMesh() {
            vec2 corners[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
            objectId=gl_InstanceIndex;
            float z=gl_InstanceIndex==0 ? 0.2 : 0.8;
            if(customParams[0].x>0.5) z=1.0-z;
            return vec4(corners[gl_VertexIndex],z,1);
        })";
    pass.code = R"(layout(location=0) flat in int objectId;
        vec4 pTerminal(vec2 uv){return objectId==0 ? vec4(0,1,0,1) : vec4(1,0,0,1);})";
    source.buffers = {pass};
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains; chains.pane = {program->instance("mesh")};
    renderer->setShaderChains(chains);
    EXPECT_PIXEL(render("\x1b[?25l"), cellCenter(10,2), qRgb(0,255,0));
    const QSize size(Cols*cw,Rows*ch);
    WindowRenderer::Pane pane{&snapshot,{}};
    pane.params.rect = QRect(QPoint(), size);
    ShaderParameterValues values; values.paramMask = 1; values.params[0] = 1;
    pane.params.shaderParameters.insert("mesh", values);
    const auto image = renderer->renderToImage(size, {pane}, {}, snapshot.defaultBg);
    EXPECT_PIXEL(image, cellCenter(10,2), qRgb(255,0,0));
    // Recreate depth attachments at a different size.
    pane.params.rect = QRect(0,0,size.width()*2,size.height()*2);
    const auto resized = renderer->renderToImage(size*2, {pane}, {}, snapshot.defaultBg);
    EXPECT_PIXEL(resized, cellCenter(10,2)*2, qRgb(255,0,0));
    PaneSnapshot otherSnapshot = snapshot;
    WindowRenderer::Pane other{&otherSnapshot,{}};
    pane.params.rect = QRect(QPoint(),size);
    other.params.rect = QRect(size.width(),0,size.width(),size.height());
    const auto split = renderer->renderToImage(QSize(size.width()*2,size.height()), {pane,other}, {}, snapshot.defaultBg);
    EXPECT_PIXEL(split, cellCenter(10,2), qRgb(255,0,0));
    EXPECT_PIXEL(split, cellCenter(10,2)+QPoint(size.width(),0), qRgb(0,255,0));
}

TEST_F(Render, WindowBackgroundKeepsMeshHistoryAcrossSplitAnimationAndFocus)
{
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    PaneSnapshot otherSnapshot = snapshot;
    ShaderProgramSource source;
    source.id = "shared-background";
    source.windowBackground = true;
    source.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
    ShaderBufferSource pass;
    pass.feedback = true;
    pass.scale = .5f;
    pass.geometry.vertexCount = 3;
    pass.geometry.vertexCode = R"(
        vec4 pMesh() {
            vec2 corners[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
            return vec4(corners[gl_VertexIndex],0.5,1);
        })";
    pass.code = R"(
        #include <terminal_multipass.glsl>
        vec4 pTerminal(vec2 uv) {
            return vec4(texture(iChannel0,uv).r+0.05,
                customParams[0].x,float(iFrame)/20.0,1);
        })";
    source.buffers = {pass};
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ASSERT_TRUE(program->windowBackground());
    ShaderChains chains; chains.background = {program};
    renderer->setShaderChains(chains);
    const QSize size(320,240);
    WindowRenderer::Pane first{&snapshot, {}}, second{&otherSnapshot, {}};
    first.params.rect = QRect(QPoint(),size);
    ShaderParameterValues values; values.paramMask=1; values.params[0]=0.75f;
    second.params.shaderParameters.insert(program->instanceId(),values);
    auto draw = [&](const std::vector<WindowRenderer::Pane>& panes, int frame) {
        renderer->setClockOverride(float(frame+1));
        return renderer->renderToImage(size,panes,{},0xff000000u);
    };
    EXPECT_PIXEL_NEAR(draw({first},0), QPoint(20,20), qRgb(13,0,0), 2);
    for (int frame=1;frame<=8;++frame) {
        const int divider=100+frame*8;
        first.params.rect=QRect(0,0,divider,240);
        second.params.rect=QRect(divider+2,0,318-divider,240);
        second.params.focused=(frame%2)==1;
        first.params.focused=!second.params.focused;
        const auto image=draw({first,second},frame);
        const auto expected=qRgb(qRound((frame+1)*.05*255),second.params.focused?191:0,qRound(frame*.05*255));
        EXPECT_PIXEL_NEAR(image,QPoint(20,20),expected,3);
        EXPECT_PIXEL_NEAR(image,QPoint(300,20),expected,3);
        EXPECT_PIXEL(image,QPoint(divider,20),qRgb(0,0,0));
    }
    // Removing the original pane must not retire the shared scene's buffers.
    second.params.rect=QRect(QPoint(),size);
    second.params.focused=true;
    EXPECT_PIXEL_NEAR(draw({second},9),QPoint(20,20),qRgb(128,191,115),3);
}

TEST_F(Render, WindowMeshCameraRemainsContinuousAcrossScaledSplits)
{
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    PaneSnapshot otherSnapshot = snapshot;
    const QSize size(320, 240);
    WindowRenderer::Pane first{&snapshot, {}}, second{&otherSnapshot, {}};
    for (float scale : {0.5f, 0.75f}) {
        ShaderProgramSource source;
        source.id = "window-camera";

        source.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
        ShaderBufferSource pass;
        pass.scale = scale;
        pass.geometry.vertexCount = 3;
        pass.geometry.vertexCode = R"(
            vec4 pMesh() {
                vec2 corners[3]=vec2[3](vec2(-0.8,-0.7),vec2(0.7,-0.4),vec2(0.1,0.8));
                vec4 p=vec4(corners[gl_VertexIndex],0.5,1);
                p.xy=(p.xy*uWindowSize+(uWindowSize-2.0*uTerminalRect.xy-uTerminalRect.zw)*p.w)/uTerminalRect.zw;
                return p;
            })";
        pass.code = R"(
            vec4 pTerminal(vec2 uv) {
                vec2 windowPixel=uTerminalRect.xy+uv*uTerminalRect.zw;
                return vec4(windowPixel/uWindowSize,0.7,1);
            })";
        source.buffers = {pass};
        QString error;
        auto program = ShaderProgram::compile(source, &error);
        ASSERT_TRUE(program) << error.toStdString();
        ASSERT_FLOAT_EQ(program->bufferSettings(0).scale, scale);
        ShaderChains chains; chains.pane = {program->instance("mesh")};
        renderer->setShaderChains(chains);
        first.params.rect = QRect(QPoint(), size);
        const auto whole = renderer->renderToImage(size, {first}, {}, 0xff000000u);
        // Unequal splits exercise offsets in both axes, independently of buffer scale.
        for (bool vertical : {false, true}) {
            first.params.rect = vertical ? QRect(0,0,320,96) : QRect(0,0,128,240);
            second.params.rect = vertical ? QRect(0,96,320,144) : QRect(128,0,192,240);
            const auto split = renderer->renderToImage(size, {first,second}, {}, 0xff000000u);
            for (const QPoint point : {QPoint(80,60), QPoint(180,72), QPoint(160,144), QPoint(20,220)}) {
                SCOPED_TRACE(::testing::Message() << "scale=" << scale << " vertical=" << vertical
                    << " point=" << point.x() << "," << point.y());
                EXPECT_PIXEL_NEAR(split, point, whole.pixel(point), 3);
            }
            // Check a known interior point to avoid a vacuous all-clear comparison.
            EXPECT_GT(qBlue(split.pixel(160,144)), 150);
        }
    }
}

TEST_F(Render, ImportedMeshUsesVertexAttributesAndOcclusion)
{
    ShaderProgramSource source;
    source.id = "obj-depth";
    source.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
    ShaderBufferSource pass;
    QString error;
    ASSERT_TRUE(ShaderProgram::loadObj(
        "v -1 -1 0.2\nv 3 -1 0.2\nv -1 3 0.2\nv -1 -1 0.8\nv 3 -1 0.8\nv -1 3 0.8\n"
        "vt 0 1\nvt 1 0\nvn 0 0 1\nf 1/1/1 2/1/1 3/1/1\nf -3/2/1 -2/2/1 -1/2/1\n", pass.geometry, &error));
    pass.geometry.vertexCode = R"(
        layout(location=0) out vec3 color;
        vec4 pMesh(){color=vec3(meshUV,meshNormal.z*0.5);return vec4(meshPosition,1);})";
    pass.code = "layout(location=0) in vec3 color;vec4 pTerminal(vec2 uv){return vec4(color,1);}";
    pass.geometry.samples = 4;
    pass.depthBuffer = true;
    pass.code.replace("return vec4(color,1);", "depthBuffer=vec4(gl_FragCoord.z);return vec4(color,1);");
    source.buffers = {pass};
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains; chains.window = {program}; renderer->setShaderChains(chains);
    EXPECT_PIXEL(render("\x1b[?25l"), cellCenter(10,2), qRgb(0,255,128));
    source.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return vec4(texture(uDepthBuffer,uv).rrr,1);}";
    program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    chains.window = {program}; renderer->setShaderChains(chains);
    EXPECT_PIXEL(render(""), cellCenter(10,2), qRgb(51,51,51));
}

TEST(ShaderGeometry, RejectsMalformedMeshesWithoutChangingPreviousGeometry)
{
    ShaderGeometry mesh;
    QString error;
    ASSERT_TRUE(ShaderProgram::loadObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\n", mesh, &error));
    ASSERT_EQ(mesh.vertexCount,3);
    EXPECT_FLOAT_EQ(mesh.vertices[0].normal[2],1.f);
    for (const auto* data : {"v nan 0 0\n", "v 0 0 0\nf 1 1 2\n", "v 0 0 0\nf 1 1 1 1\n",
            "v 0 0 0\nf 1/7 1/7 1/7\n", "v 0 0 0\nf 9999999999999999999999999999999999 1 1\n"}) {
        EXPECT_FALSE(ShaderProgram::loadObj(data,mesh,&error));
        EXPECT_EQ(mesh.vertexCount,3);
    }
}

TEST_F(Render, SceneObjectsFollowTheirParentAndLiveParameters)
{
    ShaderProgramSource source;
    source.id = "scene-objects";
    source.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
    ShaderBufferSource pass;
    QString error;
    // A quad over the left half of clip space.
    ASSERT_TRUE(ShaderProgram::loadObj("v -1 -1 0.5\nv 0 -1 0.5\nv 0 1 0.5\nv -1 1 0.5\nf 1 2 3\nf 1 3 4\n", pass.geometry, &error));
    pass.geometry.objects.resize(2);
    pass.geometry.objects[0].translate[0] = {0.0f, 0}; // the pivot moves by parameter lane 0
    pass.geometry.objects[1].parent = 0;
    pass.geometry.objects[1].vertexCount = pass.geometry.vertexCount;
    pass.geometry.vertexCode = R"(
        #include <terminal_scene.glsl>
        vec4 pMesh(){return vec4(objectPosition(meshPosition),1);})";
    pass.code = "vec4 pTerminal(vec2 uv){return vec4(1,0,0,1);}";
    source.buffers = {pass};
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    EXPECT_FALSE(program->animated());
    ShaderChains chains; chains.pane = {program->instance("scene")};
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    const QSize size(Cols*cw, Rows*ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    auto image = renderer->renderToImage(size, {pane}, {}, 0xff000000u);
    EXPECT_PIXEL(image, QPoint(size.width()/4, size.height()/2), qRgb(255,0,0));
    EXPECT_PIXEL(image, QPoint(size.width()*3/4, size.height()/2), qRgb(0,0,0));
    // A live parameter moves the pivot, and the child with it, without recompiling.
    ShaderParameterValues values; values.paramMask = 1; values.params[0] = 1.0f;
    pane.params.shaderParameters.insert("scene", values);
    image = renderer->renderToImage(size, {pane}, {}, 0xff000000u);
    EXPECT_PIXEL(image, QPoint(size.width()/4, size.height()/2), qRgb(0,0,0));
    EXPECT_PIXEL(image, QPoint(size.width()*3/4, size.height()/2), qRgb(255,0,0));
}

TEST_F(Render, BufferOverBudgetRendersSmallerInsteadOfTransparent)
{
    // A multisampled half-float mesh pass costs 80 bytes a pixel, so a
    // 2048x2048 pane asks for 320 MiB, more than the 256 MiB budget.
    if (!s_device->rhi()->supportedSampleCounts().contains(4)) GTEST_SKIP() << "no 4x multisampling";
    ShaderProgramSource source;
    source.id = "over-budget";
    source.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
    ShaderBufferSource pass;
    pass.halfFloat = true;
    QString error;
    ASSERT_TRUE(ShaderProgram::loadObj("v -1 -1 0.5\nv 1 -1 0.5\nv 1 1 0.5\nv -1 1 0.5\nf 1 2 3\nf 1 3 4\n", pass.geometry, &error));
    pass.geometry.samples = 4;
    pass.geometry.vertexCode = "vec4 pMesh(){return vec4(meshPosition,1);}";
    pass.code = "vec4 pTerminal(vec2 uv){return vec4(1,0,0,1);}";
    source.buffers = {pass};
    // Green when the buffer is smaller than asked: it shrank to fit.
    source.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return vec4(texture(iChannel0,uv).r,"
                  "textureSize(iChannel0,0).x < int(iResolution.x) ? 1.0 : 0.0,0,1);}";
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains; chains.pane = {program};
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    const QSize size(2048, 2048);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    auto image = renderer->renderToImage(size, {pane}, {}, 0xff000000u);
    EXPECT_PIXEL(image, QPoint(size.width()/2, size.height()/2), qRgb(255,255,0));
    // A smaller pane fits again and renders at full size.
    pane.params.rect = QRect(QPoint(), QSize(512, 512));
    image = renderer->renderToImage(QSize(512, 512), {pane}, {}, 0xff000000u);
    EXPECT_PIXEL(image, QPoint(256, 256), qRgb(255,0,0));
}

TEST_F(Render, PaneBlockDescribesPanesAndTheOverviewRunsOnlyWhileShown)
{
    ShaderProgramSource source;
    source.id = "overview";
    source.code = R"(#include <terminal_panes.glsl>
vec4 pTerminal(vec2 uv) {
    vec2 p = terminalPixel(uv);
    vec4 r = paneRect(highlightedPane());
    bool inside = all(greaterThanEqual(p, r.xy)) && all(lessThan(p, r.xy + r.zw));
    return vec4(inside ? 1.0 : 0.0, overviewAmount(), float(paneCount()) / 4.0 + highlightPosition() / 4.0, 1.0);
})";
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains; chains.overview = {program};
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    PaneSnapshot other = snapshot;
    const QSize size(Cols * cw, Rows * ch), frame(size.width() * 2, size.height());
    WindowRenderer::Pane left{&snapshot, {}}, right{&other, {}};
    left.params.rect = QRect(QPoint(), size);
    right.params.rect = QRect(QPoint(size.width(), 0), size);
    right.params.contentOrigin = right.params.rect.topLeft();
    right.params.highlighted = true;
    const QPoint leftCenter(size.width() / 2, size.height() / 2), rightCenter(size.width() * 3 / 2, size.height() / 2);
    // Closed: the overview chain does not run.
    auto image = renderer->renderToImage(frame, {left, right}, {}, 0xff000000u);
    EXPECT_PIXEL(image, leftCenter, rgb(snapshot.defaultBg));
    EXPECT_FALSE(renderer->overviewShowing());
    // Open: two panes, the right one highlighted, half open, the highlight at pane 1.
    const auto now = std::chrono::steady_clock::now();
    renderer->setOverview(overviewAt(true, 0.5f, 1.0f, now, now));
    EXPECT_TRUE(renderer->overviewShowing());
    EXPECT_TRUE(renderer->animating());
    image = renderer->renderToImage(frame, {left, right}, {}, 0xff000000u);
    EXPECT_PIXEL(image, rightCenter, qRgb(255, 128, 191));
    EXPECT_PIXEL(image, leftCenter, qRgb(0, 128, 191));
    // Closing keeps it running until the amount reaches zero.
    renderer->setOverview(overviewAt(false, 0.25f, 1.0f, now, now));
    EXPECT_TRUE(renderer->overviewShowing());
    renderer->setOverview(overviewAt(false, 0.0f, 1.0f, now, now));
    EXPECT_FALSE(renderer->overviewShowing());
    image = renderer->renderToImage(frame, {left, right}, {}, 0xff000000u);
    EXPECT_PIXEL(image, rightCenter, rgb(other.defaultBg));
}

TEST_F(Render, OverviewCardsSampleEveryTabInTabOrder)
{
    // One vertical stripe per card, filled with the middle of that card's pane.
    ShaderProgramSource source;
    source.id = "stripes";
    source.code = R"(#include <terminal_panes.glsl>
vec4 pTerminal(vec2 uv) {
    int card = min(int(uv.x * float(paneCount())), paneCount() - 1);
    vec3 color = paneTexel(card, vec2(0.5)).rgb;
    return vec4(uv.y < 0.5 ? color : vec3(float(paneTab(card)) / 4.0, float(uPaneTab[card].y) / 4.0, float(currentTab()) / 4.0), 1.0);
})";
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains; chains.overview = {program};
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    PaneSnapshot first = snapshot, second = snapshot, third = snapshot;
    first.defaultBg = 0xFF0000C0u;   // red
    second.defaultBg = 0xFF00C000u;  // green
    third.defaultBg = 0xFFC00000u;   // blue
    const QSize size(Cols * cw * 3, Rows * ch);
    WindowRenderer::Pane current{&second, {}};
    current.params.rect = QRect(QPoint(), size);
    current.params.focused = true;
    WindowRenderer::Pane before = current, after = current;
    before.snapshot = &first; before.params.focused = false;
    after.snapshot = &third; after.params.focused = false;
    after.params.highlighted = true; // its tab is drawn at full resolution
    const auto now = std::chrono::steady_clock::now();
    // The current tab is tab 1: tab 0 comes first, tab 2 last.
    auto overview = overviewAt(true, 1.0f, 2.0f, now, now);
    overview.currentTab = 1;
    overview.otherTabs = {tabOf(0, {before}), tabOf(2, {after})};
    renderer->setOverview(overview);
    const auto image = renderer->renderToImage(size, {current}, {}, 0xff000000u);
    const int stripe = size.width() / 3, top = size.height() / 4, bottom = size.height() * 3 / 4;
    EXPECT_PIXEL(image, QPoint(stripe / 2, top), qRgb(0xC0, 0, 0));
    EXPECT_PIXEL(image, QPoint(stripe * 3 / 2, top), qRgb(0, 0xC0, 0));
    EXPECT_PIXEL(image, QPoint(stripe * 5 / 2, top), qRgb(0, 0, 0xC0));
    // Tab indices, sources (2 the atlas, 0 the input, 1 the full-resolution tab) and the current tab.
    EXPECT_PIXEL(image, QPoint(stripe / 2, bottom), qRgb(0, 128, 64));
    EXPECT_PIXEL(image, QPoint(stripe * 3 / 2, bottom), qRgb(64, 0, 64));
    EXPECT_PIXEL(image, QPoint(stripe * 5 / 2, bottom), qRgb(128, 64, 64));
}

TEST_F(Render, TabTransitionSeesTheTabBeingLeftWhileItRuns)
{
    // Left half: the tab being left; right half: the transition's state.
    ShaderProgramSource source;
    source.id = "switch";
    source.code = R"(#include <terminal_panes.glsl>
vec4 pTerminal(vec2 uv) {
    if (uv.x < 0.5) return vec4(previousTabTexel(uv).rgb, 1.0);
    return vec4(tabTransitionProgress(), tabTransitionDirection() < 0.0 ? 1.0 : 0.0, tabTransitionActive() ? 1.0 : 0.0, 1.0);
})";
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains; chains.tabTransition = {program};
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    PaneSnapshot left = snapshot;
    left.defaultBg = 0xFFC00000u; // blue
    const QSize size(Cols * cw * 2, Rows * ch);
    WindowRenderer::Pane current{&snapshot, {}};
    current.params.rect = QRect(QPoint(), size);
    WindowRenderer::Pane leaving = current;
    leaving.snapshot = &left;
    const QPoint inLeft(size.width() / 4, size.height() / 2), inRight(size.width() * 3 / 4, size.height() / 2);
    // No switch: the transition chain does not run.
    auto image = renderer->renderToImage(size, {current}, {}, 0xff000000u);
    EXPECT_PIXEL(image, inRight, rgb(snapshot.defaultBg));
    EXPECT_FALSE(renderer->transitionShowing());
    renderer->setTabTransition(switchFrom(0.5f, -1.0f, tabOf(3, {leaving})));
    EXPECT_TRUE(renderer->animating());
    image = renderer->renderToImage(size, {current}, {}, 0xff000000u);
    EXPECT_PIXEL(image, inLeft, qRgb(0, 0, 0xC0));
    EXPECT_PIXEL(image, inRight, qRgb(128, 255, 255));
    // The overview takes precedence over a switch.
    renderer->setOverview(overviewAt(true, 1.0f, 0.0f, Clock::now()));
    EXPECT_FALSE(renderer->transitionShowing());
    renderer->setOverview({});
    renderer->setTabTransition({});
    image = renderer->renderToImage(size, {current}, {}, 0xff000000u);
    EXPECT_PIXEL(image, inRight, rgb(snapshot.defaultBg));
}

TEST_F(Render, PaneEffectsSeeOpeningPanesAndTheClosedPanesLastFrame)
{
    // Left half: the closed pane's frame; right half: card 0's opening state.
    ShaderProgramSource source;
    source.id = "life";
    source.code = R"(#include <terminal_panes.glsl>
vec4 pTerminal(vec2 uv) {
    if (uv.x < 0.5) return vec4(closingPaneActive() ? closingPaneTexel(uv).rgb : vec3(1.0), 1.0);
    return vec4(paneOpening(0), float(paneOpenEdge(0)) / 4.0, closingPaneProgress(), 1.0);
})";
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains; chains.paneEffects = {program};
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    PaneSnapshot closed = snapshot;
    closed.defaultBg = 0xFFC00000u; // blue
    const QSize size(Cols * cw * 2, Rows * ch);
    WindowRenderer::Pane open{&snapshot, {}};
    open.params.rect = QRect(QPoint(), size);
    open.params.openProgress = 0.5f;
    open.params.openEdge = 2;
    WindowRenderer::Pane ghost = open;
    ghost.snapshot = &closed;
    ghost.params.rect = QRect(QPoint(), QSize(size.width() / 2, size.height()));
    const QPoint inLeft(size.width() / 4, size.height() / 2), inRight(size.width() * 3 / 4, size.height() / 2);
    renderer->setPaneEffects({true, true, ghost, 0.25f});
    EXPECT_TRUE(renderer->paneEffectsShowing());
    auto image = renderer->renderToImage(size, {open}, {}, 0xff000000u);
    EXPECT_PIXEL(image, inLeft, qRgb(0, 0, 0xC0));
    EXPECT_PIXEL(image, inRight, qRgb(128, 128, 64));
    // A tab switch takes precedence; an inactive chain does not run.
    renderer->setTabTransition(switchFrom(0.5f, 1.0f, tabOf(1, {open})));
    EXPECT_FALSE(renderer->paneEffectsShowing());
    renderer->setTabTransition({});
    renderer->setPaneEffects({});
    open.params.openProgress = 1.0f;
    image = renderer->renderToImage(size, {open}, {}, 0xff000000u);
    EXPECT_PIXEL(image, inRight, rgb(snapshot.defaultBg));
}

TEST_F(Render, PaneContextAndPreviousContextReachPaneEffects)
{
    ShaderProgramSource source;
    source.id = "context";
    source.code = R"(#include <terminal_panes.glsl>
vec4 pTerminal(vec2 uv) {
    return vec4(paneContextChange(0), float(paneContext(0)) / 4.0,
                paneInCurrentTab(0) ? 0.5 + float(panePreviousContext(0)) / 8.0 : 0.0, 1.0);
})";
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains; chains.paneEffects = {program};
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    pane.params.context = 1;
    pane.params.previousContext = 2;
    pane.params.contextProgress = 0.5f;
    // A pane away from here keeps the chain on without asking for new frames.
    renderer->setPaneEffects({true, false, std::nullopt, 1.0f});
    EXPECT_TRUE(renderer->paneEffectsShowing());
    EXPECT_FALSE(renderer->animating());
    const auto image = renderer->renderToImage(size, {pane}, {}, 0xff000000u);
    EXPECT_PIXEL(image, QPoint(size.width() / 2, size.height() / 2), qRgb(128, 64, 191));
}

TEST(ShaderGeometry, SceneMatricesComposeParentsParametersAndSpin)
{
    std::vector<ShaderSceneObject> objects(3);
    objects[0].translate[0] = {2.0f};                  // root two units right
    objects[0].spin[2] = {90.0f};                      // turning about Z
    objects[1].parent = 0;
    objects[1].translate[1] = {0.0f, 4};               // up by parameter lane 4
    objects[2].parent = 1;
    objects[2].scale = {ShaderSceneValue{3.0f}, ShaderSceneValue{3.0f}, ShaderSceneValue{3.0f}};
    std::array<float, 32> params{};
    params[4] = 1.0f;
    TerminalSceneUniforms scene{};
    ShaderProgram::sceneMatrices(objects, params, 1.0f, scene);
    auto point = [&](int object, QVector3D p) { return QMatrix4x4(scene.uObjectMatrix[object]).transposed().map(p); };
    // After one second the root has turned 90 degrees, so the child's "up"
    // offset points left of the root.
    EXPECT_TRUE(qFuzzyCompare(point(1, {0, 0, 0}), QVector3D(1, 0, 0))) << point(1, {0, 0, 0}).x();
    EXPECT_TRUE(qFuzzyCompare(point(2, {1, 0, 0}), QVector3D(1, 3, 0))) << point(2, {1, 0, 0}).y();
    // Normals undo the scale.
    const QMatrix4x4 normal = QMatrix4x4(scene.uObjectNormalMatrix[2]).transposed();
    EXPECT_NEAR(normal.mapVector({1, 0, 0}).length(), 1.0f / 3.0f, 1e-5f);
    params[4] = 2.0f;
    ShaderProgram::sceneMatrices(objects, params, 1.0f, scene);
    EXPECT_TRUE(qFuzzyCompare(point(1, {0, 0, 0}), QVector3D(0, 0, 0)));
}

TEST(ShaderGeometry, RejectsExcessiveOrIncompleteDraws)
{
    ShaderProgramSource source;
    source.code="#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
    ShaderBufferSource pass;
    pass.code="vec4 pTerminal(vec2 uv){return vec4(1);}";
    pass.geometry.vertexCode="vec4 pMesh(){return vec4(0);}";
    QString error;
    auto compileWith = [&](const ShaderBufferSource& buffer) { source.buffers = {buffer}; return ShaderProgram::compile(source, &error); };
    for (int count : {0, 1, 4, 3*1024*1024+3}) {
        pass.geometry.vertexCount=count;
        EXPECT_FALSE(compileWith(pass));
    }
    pass.geometry.vertexCount=3;
    pass.geometry.instances=4097;
    EXPECT_FALSE(compileWith(pass));
    pass.geometry.instances=1;
    pass.geometry.vertices.resize(2);
    EXPECT_FALSE(compileWith(pass));
}

TEST(ShaderGeometry, FinalPassRejectsMeshesAndDepthOutputs)
{
    ShaderProgramSource source;
    source.code="vec4 pTerminal(vec2 uv){return vec4(1);}";
    source.geometry.vertexCode="vec4 pMesh(){return vec4(0);}";
    source.geometry.vertexCount=3;
    QString error;
    EXPECT_FALSE(ShaderProgram::compile(source,&error));
    EXPECT_FALSE(error.isEmpty());
    source.geometry = {};
    source.depthOutput = true;
    EXPECT_FALSE(ShaderProgram::compile(source,&error));
    source.depthOutput = false;
    EXPECT_TRUE(ShaderProgram::compile(source,&error)) << error.toStdString();
}

TEST(ShaderGeometry, ClockPointerAndTimeHelpersAreDetected)
{
    QString error;
    auto compileBody = [&](const char* body) {
        ShaderProgramSource source;
        source.code = QByteArray("vec4 pTerminal(vec2 uv){") + body + "}";
        auto program = ShaderProgram::compile(source, &error);
        EXPECT_TRUE(program) << error.toStdString();
        return program;
    };
    // iDate and the contract's time helpers animate, like iTime.
    EXPECT_TRUE(compileBody("return vec4(fract(iDate.w));")->animated());
    EXPECT_TRUE(compileBody("return vec4(sin(timeAngle(1.0)));")->animated());
    EXPECT_FALSE(compileBody("return vec4(1);")->animated());
    // A pack that reads only the pointer asks for frames when it moves.
    auto pointer = compileBody("return vec4(iMouse.x > 0.0 ? 1.0 : 0.0);");
    EXPECT_TRUE(pointer->readsMouse());
    EXPECT_FALSE(pointer->animated());
    ShaderChains chains;
    chains.window = {pointer};
    EXPECT_TRUE(chains.readsMouse());
}

TEST(ShaderGeometry, ObjAcceptsOptionalComponentsAndGivesFlatFacesANormal)
{
    ShaderGeometry geometry;
    QString error;
    // v with w, v with a vertex colour, vt with one and three components.
    ASSERT_TRUE(ShaderProgram::loadObj("v 0 0 0 1\nv 1 0 0 0.5 0.5 0.5\nv 0 1 0\nvt 0.5\nvt 1 1 0\nf 1/1 2/2 3/1\n", geometry, &error))
        << error.toStdString();
    EXPECT_EQ(geometry.vertexCount, 3);
    EXPECT_FLOAT_EQ(geometry.vertices[0].uv[0], 0.5f);
    EXPECT_FLOAT_EQ(geometry.vertices[0].uv[1], 0.0f);
    // A zero-area face without normals gets +Z, never a zero normal.
    ASSERT_TRUE(ShaderProgram::loadObj("v 0 0 0\nv 1 1 1\nv 2 2 2\nf 1 2 3\n", geometry, &error)) << error.toStdString();
    EXPECT_FLOAT_EQ(geometry.vertices[0].normal[2], 1.0f);
    EXPECT_FALSE(ShaderProgram::loadObj("v 0 0\nf 1 1 1\n", geometry, &error));
    EXPECT_TRUE(error.contains(QLatin1String("x y z")));
}

TEST_F(Render, MeshMultisamplingResolvesPartialEdgeCoverage)
{
    ShaderProgramSource source;
    source.id="mesh-antialiasing";
    source.code="#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return vec4(texelFetch(iChannel0,ivec2(gl_FragCoord.xy),0).r,0,0,1);}";
    ShaderBufferSource pass;
    pass.geometry.vertexCount=3;
    pass.geometry.vertexCode=R"(vec4 pMesh(){
        vec2 points[3]=vec2[3](vec2(-0.8,-0.7),vec2(0.7,-0.6),vec2(-0.45,0.8));
        return vec4(points[gl_VertexIndex],0.5,1);
    })";
    pass.code="vec4 pTerminal(vec2 uv){return vec4(1,0,0,1);}";
    for (int samples : {1,4}) {
        pass.geometry.samples=samples;
        source.buffers={pass};
        QString error;
        const auto program=ShaderProgram::compile(source,&error);
        ASSERT_TRUE(program) << error.toStdString();
        ShaderChains chains; chains.window={program}; renderer->setShaderChains(chains);
        const auto image=render("\x1b[?25l");
        int partial=0;
        for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x) {
            const int red=qRed(image.pixel(x,y));
            if(red>20 && red<235) ++partial;
        }
        if(samples==1) EXPECT_EQ(partial,0);
        else EXPECT_GT(partial,10);
    }
}


TEST_F(Render, ExceptionClosesActivePassAndAllowsNextFrame)
{
    auto* rhi = s_device->rhi();
    std::unique_ptr<QRhiTexture> texture(rhi->newTexture(QRhiTexture::RGBA8, QSize(16, 16), 1, QRhiTexture::RenderTarget));
    ASSERT_TRUE(texture->create());
    std::unique_ptr<QRhiTextureRenderTarget> target(rhi->newTextureRenderTarget({texture.get()}));
    std::unique_ptr<QRhiRenderPassDescriptor> pass(target->newCompatibleRenderPassDescriptor());
    target->setRenderPassDescriptor(pass.get());
    ASSERT_TRUE(target->create());
    for (const bool insidePass : {false, true}) {
        QRhiCommandBuffer* cb = nullptr;
        ASSERT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
        int ended = 0;
        EXPECT_THROW({
            RenderFrame frame(cb, [&] { rhi->endOffscreenFrame(); ++ended; });
            if (insidePass) frame.beginPass(target.get(), Qt::transparent, {1.f, 0});
            throw std::runtime_error("injected render failure");
        }, std::runtime_error);
        EXPECT_EQ(ended, 1);
        EXPECT_FALSE(rhi->isRecordingFrame());
        EXPECT_FALSE(render("still alive").isNull());
    }
}

TEST_F(Render, ShaderRegionsClipAnimatedBoundsToSurface)
{
    ShaderProgramSource source;
    source.id = QStringLiteral("clipped-region");
    source.code = "vec4 pTerminal(vec2 uv) { return vec4(1,0,0,1); }";
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains; chains.background = {program};
    renderer->setShaderChains(chains);
    snapshot.capture(*terminal);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    // Both a spring overshoot and a pane that has moved entirely offscreen.
    for (const QRect& bounds : {QRect(-cw, -ch, size.width() + 2*cw, size.height() + 2*ch),
                               QRect(size.width() + cw, 0, cw, ch)}) {
        pane.params.rect = bounds;
        const auto image = renderer->renderToImage(size, {pane}, {}, 0xff000000u);
        ASSERT_FALSE(image.isNull());
    }
}

TEST_F(Render, ShaderCursorTracksStylesWithoutResettingItsAge)
{
    ShaderChains chains;
    chains.window = {layerShader("cursor-styles", "fragColor = vec4(float(uCursorState.x)/4.0, float(uCursorState.y)/4.0, cursorAge()/10.0, 1.0);")};
    renderer->setShaderChains(chains);
    renderer->setClockOverride(0.f);
    render("\x1b[2 q");
    const int commands[] = {6, 4, 2, 6};
    const int styles[] = {2, 3, 0, 2};
    int previous = 0;
    for (int i = 0; i < 4; ++i) {
        const float time = float(i + 1);
        renderer->setClockOverride(time);
        auto image = render("\x1b[" + std::to_string(commands[i]) + " q");
        // Only a move sets cursorAge(): the cursor never moved, so it reads
        // as long ago (blue saturates) while the shapes still follow.
        const QRgb expected = qRgb(qRound(styles[i] * 255.0 / 4), qRound(previous * 255.0 / 4), 255);
        EXPECT_PIXEL(image, cellCenter(10, 2), expected);
        renderer->setClockOverride(time + .5f);
        EXPECT_PIXEL(render(""), cellCenter(10, 2), expected);
        previous = styles[i];
    }
}

TEST_F(Render, ShaderCursorBoxMatchesDoubleWidthAndMulticellDrawing)
{
    const QSize size(Cols * cw, Rows * ch);
    for (bool multicell : {false, true}) {
        terminal->reset();
        terminal->feed(multicell ? "\x1b]66;s=2;A\x07\x1b[2;2H" : "\x1b#6\x1b[1;4H");
        snapshot.capture(*terminal);
        const QRectF expected = multicell ? QRectF(cw, ch, 2*cw, 2*ch) : QRectF(7*cw, ch, 2*cw, ch);
        WindowRenderer::Pane pane{&snapshot, {}};
        pane.params.rect = QRect(QPoint(), size);
        pane.params.contentOrigin = QPoint(cw, ch);
        pane.params.focused = true;
        FrameBuilder builder;
        FrameBuilder::Ranges ranges;
        std::vector<Instance> instances;
        auto& resources = renderer->fonts();
        builder.buildPane(snapshot, pane.params, *resources.fonts, resources.glyphs, instances, ranges);
        ASSERT_LT(ranges.cursorStart, instances.size());
        const auto& cursor = instances[ranges.cursorStart];
        EXPECT_EQ(QRectF(cursor.x, cursor.y, cursor.w, cursor.h), expected);
        // QString::number ignores LC_NUMERIC; std::to_string would write 63,000000 in de_DE.
        auto glsl = [](double v) { return QString::number(v, 'f', 3).toStdString(); };
        const std::string body = "fragColor = all(lessThan(abs(uCursorRect - vec4("
            + glsl(expected.x()) + "," + glsl(expected.y()) + ","
            + glsl(expected.width()) + "," + glsl(expected.height())
            + ")), vec4(0.1))) ? vec4(0,1,0,1) : vec4(1,0,0,1);";
        ShaderChains chains;
        chains.window = {layerShader("cursor-box", body.c_str())};
        renderer->setShaderChains(chains);
        EXPECT_PIXEL(renderer->renderToImage(size, {pane}, {}, snapshot.defaultBg), cellCenter(10, 2), qRgb(0,255,0));
    }
}

TEST(Snapshot, SearchMatchesKeepConcealmentAndYieldToTheSelection)
{
    AfterglowVt::TerminalOptions o;
    Captured c(o);
    c.capture("\x1b[8mab\x1b[0m cd");
    const auto matches = AfterglowVt::findAll(c.terminal.screen(), U"ab");
    ASSERT_EQ(matches.size(), 1u);
    SearchHighlight search{&matches, 0, packRgba(200, 0, 0), packRgba(0, 200, 0), packRgba(1, 2, 3)};
    c.snapshot.capture(c.terminal, nullptr, search);
    EXPECT_EQ(c.snapshot.at(0, 0).bg, packRgba(0, 200, 0)); // the current match
    // The match's text colour, but concealed text stays concealed.
    EXPECT_EQ(c.snapshot.at(0, 0).fg, packRgba(1, 2, 3, 0));
    EXPECT_EQ(c.snapshot.at(2, 0).bg, 0u);
    // A selected cell shows the selection, not the match.
    AfterglowVt::Selection sel = c.word(0);
    c.snapshot.capture(c.terminal, &sel, search);
    EXPECT_NE(c.snapshot.at(0, 0).bg, packRgba(0, 200, 0));
}

TEST(Snapshot, UnderlineColourIsConcealedWithItsText)
{
    AfterglowVt::TerminalOptions o;
    Captured c(o);
    const SnapshotCell& cell = c.capture("\x1b[8;4;58:2::255:0:0mAB");
    EXPECT_EQ(cell.underlineColor >> 24, 0u);
    Captured faint(o);
    EXPECT_EQ(faint.capture("\x1b[2;4;58:2::255:0:0mAB").underlineColor >> 24, 128u);
}

TEST(Snapshot, SelectionStaysOpaqueOverATransparentBackgroundColour)
{
    AfterglowVt::TerminalOptions o;
    Captured c(o);
    // The default foreground is the inverted selection's background.
    c.capture("\x1b]21;transparent_background_color1=#d8d8d8@0\x07word");
    AfterglowVt::Selection sel = c.word(0);
    c.capture("", &sel);
    EXPECT_FALSE(c.snapshot.at(0, 0).backgroundOverride);
    EXPECT_EQ(c.snapshot.at(0, 0).bg >> 24, 255u);
}

TEST(Snapshot, PlaceholderRunsEndAtAnyOtherCell)
{
    Captured pane{AfterglowVt::TerminalOptions{}};
    pane.terminal.resize(20, 2, 10, 20);
    // A 2x1 image on a two-cell grid: each cell shows one pixel column.
    pane.capture("\x1b[?25l\x1b_Ga=T,f=32,s=2,v=1,i=7,p=1,U=1,c=2,r=1,q=2;/wAA/wD/AP8=\x1b\\"
                 "\x1b[38;2;0;0;7m\xf4\x8e\xbb\xae\xf4\x8e\xbb\xaeX\xf4\x8e\xbb\xae\x1b[0m");
    const auto& images = pane.snapshot.images;
    ASSERT_EQ(images.size(), 3u);
    EXPECT_EQ(images[0].x, 0);
    EXPECT_EQ(images[1].x, 10);
    EXPECT_EQ(images[2].x, 30);
    // The second cell continues the run; the cell after the X starts a new one.
    EXPECT_GT(images[1].srcX, images[0].srcX);
    EXPECT_EQ(images[2].srcX, images[0].srcX);
    EXPECT_EQ(pane.snapshot.at(0, 0).cp, char32_t(0));
}

TEST(Snapshot, CopiesAndAssignmentsTakeANewIdentity)
{
    PaneSnapshot a;
    PaneSnapshot b = a;
    EXPECT_NE(a.identity.value(), b.identity.value());
    const auto before = b.identity.value();
    b = a;
    EXPECT_NE(b.identity.value(), before);
    EXPECT_NE(b.identity.value(), a.identity.value());
}

TEST(GlyphCache, OversizedSpritesDrawNothingInsteadOfThrowing)
{
    FontSet fonts(FontSpec{}, 1.0);
    GlyphCache cache;
    // A device whose textures hold 64 pixels a side.
    cache.beginFrame(1000, 64);
    const GlyphEntry* huge = nullptr;
    ASSERT_NO_THROW(huge = cache.sprite(fonts, U'\u2588', 49, 7));
    ASSERT_NE(huge, nullptr);
    EXPECT_TRUE(huge->empty);
    const GlyphEntry* small = cache.sprite(fonts, U'\u2588');
    ASSERT_NE(small, nullptr);
    EXPECT_FALSE(small->empty);
}

TEST(GlyphCache, PagesUsedRecentlyStayAcrossManyRendersOverBudget)
{
    FontSet fonts(FontSpec{}, 1.0);
    GlyphCache cache;
    // Five full gray pages: 80 MiB, over the 64 MiB budget.
    int x = 0, y = 0, width = 49;
    while (cache.grayPageCount() < 5 && width > 0) {
        while (cache.grayPage(cache.grayPageCount() - 1).allocate(128, 128, x, y)) {}
        cache.beginFrame(std::numeric_limits<std::size_t>::max() / 2, 8192);
        cache.sprite(fonts, U'\u2588', width--, 7); // a new size: a new entry, on a new page
    }
    ASSERT_GT(cache.bytes(), 64u * 1024 * 1024);
    const auto epoch = cache.epoch();
    // Other windows render many times in a fraction of a second: no page of
    // this window is stale yet.
    for (int i = 0; i < 10; ++i) cache.beginFrame(1000, 8192);
    EXPECT_EQ(cache.epoch(), epoch);
}

TEST(GlyphCache, DashedLinesKeepEveryDashOrDrawSolid)
{
    for (float points : {4.0f, 6.0f, 8.0f, 11.0f, 14.0f}) {
        FontSpec spec;
        spec.sizePoints = points;
        FontSet fonts(spec, 1.0);
        GlyphCache cache;
        const GlyphEntry* e = cache.sprite(fonts, U'\u2508'); // four light dashes
        ASSERT_FALSE(e->empty);
        const QImage& page = cache.grayPage(e->page).image();
        const int row = e->v + e->height / 2;
        int runs = 0;
        bool inside = false;
        for (int col = e->u; col < e->u + e->width; ++col) {
            const bool ink = qGray(page.pixel(col, row)) > 0;
            runs += ink && !inside;
            inside = ink;
        }
        EXPECT_TRUE(runs == 4 || runs == 1) << points << " pt: " << runs << " runs";
    }
}

TEST_F(Render, OversizedImagesAreNotUploaded)
{
    auto frame = std::make_shared<AfterglowVt::ImageFrame>();
    const int limit = s_device->rhi()->resourceLimit(QRhi::TextureSizeMax);
    frame->width = std::uint32_t(limit + 1);
    frame->height = 1;
    frame->rgba.assign(std::size_t(frame->width) * 4, 0xFF);
    QRhiResourceUpdateBatch* updates = s_device->rhi()->nextResourceUpdateBatch();
    std::uint64_t serial = 0;
    EXPECT_EQ(s_device->imageTexture(frame, updates, serial), nullptr);
    updates->release();
}

TEST_F(Render, KittyImagesFarBelowGoUnderCellBackgrounds)
{
    // A red pixel over two cells at z below INT32_MIN/2: the first cell has
    // its own green background, which covers the image; the second does not.
    const QImage img = render("\x1b[?25l\x1b[2;2H\x1b[42m \x1b[0m\x1b[2;2H"
                              "\x1b_Ga=T,f=32,s=1,v=1,c=2,r=1,z=-1073741825,q=2;/wAA/w==\x1b\\");
    EXPECT_PIXEL(img, cellCenter(1, 1), qRgb(0x55, 0xcc, 0x55));
    EXPECT_PIXEL_NEAR(img, cellCenter(2, 1), qRgb(255, 0, 0), 40);
}

TEST_F(Render, DecorationsUnderABlockCursorTakeItsTextColour)
{
    // A struck-through blank under the focused block cursor: the line is drawn
    // in the cursor's text colour, so it shows on the cursor.
    const FontMetrics& m = renderer->fonts().fonts->metrics();
    const QImage img = render("\x1b[2 q\x1b[9m \x1b[0m\x1b[1D");
    const int y = int(std::lround(m.strikethroughPosition - m.underlineThickness / 2.0)) + m.underlineThickness / 2;
    EXPECT_PIXEL(img, QPoint(cw / 2, y), rgb(snapshot.defaultBg));
    EXPECT_PIXEL(img, QPoint(cw / 2, 1), rgb(snapshot.cursorColor));
}

TEST_F(Render, BlinkingTextHidesItsDecorationsInTheOffPhase)
{
    terminal->feed("\x1b[?25l\x1b[5;9;4m    ");
    snapshot.capture(*terminal);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    pane.params.textBlinkOn = false;
    const QImage off = renderer->renderToImage(size, {pane}, {}, snapshot.defaultBg);
    for (int y = 0; y < ch; ++y) EXPECT_PIXEL(off, QPoint(cw + cw / 2, y), rgb(snapshot.defaultBg));
    pane.params.textBlinkOn = true;
    const QImage on = renderer->renderToImage(size, {pane}, {}, snapshot.defaultBg);
    int ink = 0;
    for (int y = 0; y < ch; ++y) ink += on.pixel(cw + cw / 2, y) != rgb(snapshot.defaultBg);
    EXPECT_GT(ink, 0);
}

TEST_F(Render, OverviewSurvivesAChangeOfLayerChainsWhileShown)
{
    // The tab image outlives a rebuild of the post textures (new layers):
    // other tabs' cards still sample their own panes, not a stale target.
    ShaderProgramSource source;
    source.id = "cards";
    source.code = R"(#include <terminal_panes.glsl>
vec4 pTerminal(vec2 uv) {
    int card = min(int(uv.x * float(paneCount())), paneCount() - 1);
    return vec4(paneTexel(card, vec2(0.5)).rgb, 1.0);
})";
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    PaneSnapshot other = snapshot;
    other.defaultBg = 0xFF00C000u; // green
    const QSize size(Cols * cw * 2, Rows * ch);
    WindowRenderer::Pane current{&snapshot, {}};
    current.params.rect = QRect(QPoint(), size);
    current.params.focused = true;
    WindowRenderer::Pane away = current;
    away.snapshot = &other;
    away.params.focused = false;
    auto overview = overviewAt(true, 1.0f, 0.0f, Clock::now(), Clock::now());
    overview.currentTab = 0;
    overview.otherTabs = {tabOf(1, {away})};
    ShaderChains chains;
    chains.overview = {program};
    renderer->setShaderChains(chains);
    renderer->setOverview(overview);
    const QPoint secondCard(size.width() * 3 / 4, size.height() / 2);
    EXPECT_PIXEL(renderer->renderToImage(size, {current}, {}, 0xff000000u), secondCard, qRgb(0, 0xC0, 0));
    chains.text = {layerShader("identity-text", "fragColor = terminalTexel(uv);")};
    renderer->setShaderChains(chains);
    renderer->setOverview(overview);
    for (int frame = 0; frame < 2; ++frame)
        EXPECT_PIXEL(renderer->renderToImage(size, {current}, {}, 0xff000000u), secondCard, qRgb(0, 0xC0, 0));
}


int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    // Vulkan needs a real platform; the offscreen QPA has no Vulkan support.
    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QGuiApplication app(argc, argv);
    if (QGuiApplication::platformName() == QLatin1String("wayland")) {
        try {
            s_device = std::make_unique<GpuDevice>(FontSpec{});
            s_device->rhi(nullptr);
        } catch (const std::exception&) {
            s_device.reset();
        }
    }
    const int result = RUN_ALL_TESTS();
    s_device.reset();
    return result;
}
