// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/GridRenderer.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Terminal.h>

#include <QGuiApplication>
#include <QImage>

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
    EXPECT_EQ(standard.snapshot.at(4, 0).bg, 0u); // ':' ends the word
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
    src.code = QByteArray(hdr ? "#define AFTERGLOW_HDR 1\n" : "")
        + "void mainImage(out vec4 fragColor, in vec2 fragCoord)\n{\n    vec4 c = texture(iChannel0, fragCoord / iResolution.xy);\n"
        + body + "\n}\n";
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
        src.id = QStringLiteral("feedback"); src.bufferFeedback = true; src.halfFloatBuffer = half; src.bufferScale = .5f;
        src.bufferCode = "void mainImage(out vec4 c,in vec2 p){c=texture(iBuffer,p/iResolution.xy)+vec4(customParams[0].x,0,0,0);c.a=1.;}";
        src.code = "void mainImage(out vec4 c,in vec2 p){c=texture(iBuffer,p/iResolution.xy);}";
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
    ShaderProgramSource src; src.id=QStringLiteral("isolation"); src.bufferFeedback=true;src.bufferScale=.375f;
    src.bufferCode="void mainImage(out vec4 c,in vec2 p){c=texture(iBuffer,p/iResolution.xy+vec2(10,0))+vec4(customParams[0].x,0,0,0);c.a=1.;}";
    src.code="void mainImage(out vec4 c,in vec2 p){c=texture(iBuffer,p/iResolution.xy);}";
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
        ShaderProgramSource src;src.id=QStringLiteral("buffer-format");src.halfFloatBuffer=half;
        src.bufferCode="void mainImage(out vec4 c,in vec2 p){c=vec4(texture(iBuffer,p/iResolution.xy).r+4.,0,0,1);}";
        src.code="void mainImage(out vec4 c,in vec2 p){c=vec4(texture(iBuffer,p/iResolution.xy).r/4.,0,0,1);}";
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
    ShaderProgramSource src; src.id=QStringLiteral("bounded");src.bufferFeedback=true;
    src.bufferCode="void mainImage(out vec4 c,in vec2 p){c=vec4(.5,0,0,1);}";
    src.code="void mainImage(out vec4 c,in vec2 p){c=texture(iBuffer,p/iResolution.xy);}";
    QString error;auto program=ShaderProgram::compile(src,&error);ASSERT_TRUE(program);
    ShaderChains chains;
    for(int i=0;i<34;++i) chains.pane.push_back(program);
    renderer->setShaderChains(chains);
    auto image=render("\x1b[?25l");
    EXPECT_EQ(image.pixelColor(cellCenter(2,2)).alpha(),0);
    image=render("");
    EXPECT_EQ(image.pixelColor(cellCenter(2,2)).alpha(),0);
    src.bufferScale=0; EXPECT_FALSE(ShaderProgram::compile(src,&error));
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
    // Readback QImage uses straight alpha.
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
#define AFTERGLOW_AUDIO 1
void mainImage(out vec4 c, in vec2 p) {
    c = vec4(getBass(), getMids(), getTreble(), 1.0);
    if (p.x > iResolution.x * 0.5)
        c = vec4(audioBar(0), audioBarSmooth(0.5), audioBar(63), 1.0);
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
    source.code.replace("#define AFTERGLOW_AUDIO 1", "#define AFTERGLOW_AUDIO 0");
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
    chains.pane = {layerShader("scrollback", "fragColor = vec4(iScrollback.xyz / 20.0, 1.0);")};
    chains.window = {layerShader("scrollback-window", "fragColor = vec4(c.r, iScrollback.yz / 20.0, 1.0);")};
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
    chains.background = {layerShader("scrollback", "fragColor = vec4(iScrollback.xyz / 20.0, 1.0);")};
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
        fragColor = max(vec4(c.rgb * 4.0, c.a), texture(iPreviousText, fragCoord / iResolution.xy) * 0.5);
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
        vec4 previous = texture(iPreviousText, fragCoord / iResolution.xy);
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
    chains.text = {layerShader("history", "fragColor = max(c, texture(iPreviousText, fragCoord / iResolution.xy) * 0.5);")};
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
    snapshot.capture(*terminal);
    other = snapshot;
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
    const QImage spaced = render("\x1b[?25l\uf015 ");
    if (inkIn(spaced, 0) == 0) {
        GTEST_SKIP() << "no font with Nerd Font icons";
    }
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
    ASSERT_TRUE(builder.buildPane(snap, params, *resources.fonts, resources.glyphs, instances, ranges));
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
             {"iCursorText", qRgb(11, 22, 33)},
             {"iSelectionForegroundColor", qRgb(44, 55, 66)},
             {"iSelectionBackgroundColor", qRgb(77, 88, 99)}}) {
        const std::string body = std::string("fragColor = vec4(") + name + ", 1.0);";
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
    first.code = "void mainImage(out vec4 c,in vec2 p){ c=vec4(1,0,0,1); depthBuffer=vec4(0.25,0.5,0.75,1); }";
    first.wrap = ShaderWrap::Repeat;
    ShaderBufferSource second;
    second.code = "vec4 sampleIt(sampler2D s,vec2 uv){return texture(s,uv);} void mainImage(out vec4 c,in vec2 p){c=sampleIt(iBuffer0,vec2(2.5)); depthBuffer=texture(iDepth,vec2(0.5));}";
    source.buffers = {first, second};
    source.code = "void mainImage(out vec4 c,in vec2 p){ c=texture(iDepth,vec2(0.5)); }";
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
    source.vertexCode = "void mainVertex(inout vec4 p,in vec2 uv){p.x *= customParams[0].x;}";
    source.params[0] = .5f;
    source.code = "void mainImage(out vec4 c,in vec2 p){c=vec4(1,0,0,1);}";
    QString error;
    const auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains;
    chains.window = {program};
    renderer->setShaderChains(chains);
    const auto image = render("\x1b[?25l");
    EXPECT_PIXEL(image, cellCenter(10,2), qRgb(255,0,0));
    EXPECT_PIXEL(image, cellCenter(1,2), qRgb(0,0,0));
}

TEST_F(Render, ImageOverridesAreLiveAndDefaultsReturnWithoutRecompile)
{
    ShaderProgramSource source;
    source.id = "image-live";
    source.images[0] = QImage(2,2,QImage::Format_RGBA8888);
    source.images[0].fill(Qt::red);
    source.code = "vec4 readImage(sampler2D s,vec2 uv){return texture(s,uv);} void mainImage(out vec4 c,in vec2 p){c=readImage(uTexture0,vec2(.5));}";
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
        buffer.code = "void mainImage(out vec4 c,in vec2 p){c=p.x<iResolution.x*.5?vec4(1,0,0,1):vec4(0,1,0,1);}";
        source.buffers = {buffer};
        source.code = "vec4 readBuffer(sampler2D s,vec2 uv){return texture(s,uv);} void mainImage(out vec4 c,in vec2 p){c=readBuffer(iBuffer0,vec2(1.25,.5));}";
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
    source.vertexCode = "void mainVertex(inout vec4 p,in vec2 uv){p.x *= iTime < 1.0 ? 1.0 : 0.5;}";
    source.code = "void mainImage(out vec4 c,in vec2 p){c=vec4(1,0,0,1);}";
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains; chains.window = {program, ShaderProgram::passthrough()};
    renderer->setShaderChains(chains);
    renderer->setClockOverride(0);
    EXPECT_PIXEL(render("\x1b[?25l"),cellCenter(1,2),qRgb(255,0,0));
    renderer->setClockOverride(2);
    const auto image = render("");
    EXPECT_PIXEL(image,cellCenter(1,2),qRgb(0,0,0));
    EXPECT_PIXEL(image,cellCenter(10,2),qRgb(255,0,0));
}
