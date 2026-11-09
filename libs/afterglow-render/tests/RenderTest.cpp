// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/RenderFrame.h>

#include "GpuTestDevice.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/GridRenderer.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Search.h>
#include <AfterglowVt/Terminal.h>

#include <QDate>
#include <QFile>
#include <QGuiApplication>
#include <QImage>

#include <QMatrix4x4>
#include <QFloat16>
#include <rhi/qrhi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
/// Why there is no device. A machine without Vulkan skips the tests that
/// draw. On one with Vulkan, a device that cannot be made fails them.
std::string s_noDevice;
bool s_noVulkan = false;

class Render : public ::testing::Test
{
protected:
    static constexpr int Cols = 20;
    static constexpr int Rows = 4;

    void SetUp() override
    {
        if (!s_device) {
            if (s_noVulkan) GTEST_SKIP() << s_noDevice;
            FAIL() << s_noDevice;
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

    /// The clear colour is the pane's background unless @p clearColor
    /// differs, which shows where the grid drew nothing.
    QImage render(const std::string& bytes, bool focused = true, std::uint8_t backgroundAlpha = 255,
                  std::optional<std::uint32_t> clearColor = std::nullopt)
    {
        terminal->feed(bytes);
        snapshot.capture(*terminal);
        return draw(focused, backgroundAlpha, clearColor);
    }

    /// Draws the snapshot again without a new capture, as a pane that holds
    /// its frame during synchronized output does.
    QImage draw(bool focused = true, std::uint8_t backgroundAlpha = 255, std::optional<std::uint32_t> clearColor = std::nullopt)
    {
        const QSize size(Cols * cw, Rows * ch);
        WindowRenderer::Pane pane;
        pane.snapshot = &snapshot;
        pane.params.rect = QRect(QPoint(0, 0), size);
        pane.params.focused = focused;
        pane.params.backgroundAlpha = backgroundAlpha;
        const std::uint32_t clear = clearColor.value_or((snapshot.defaultBg & 0x00FFFFFFu) | (std::uint32_t(backgroundAlpha) << 24));
        return renderer->renderToImage(size, {pane}, {}, clear);
    }

    // Packed 0xAABBGGRR as the snapshot stores colours, as an opaque QRgb:
    // EXPECT_PIXEL compares alpha too.
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
                      + std::abs(qBlue(_got) - qBlue(_want)) + std::abs(qAlpha(_got) - qAlpha(_want)),           \
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

WindowRenderer::Pane::Overlay overlayOf(const PaneSnapshot* snapshot, float progress)
{
    WindowRenderer::Pane::Overlay overlay;
    overlay.snapshot = snapshot;
    overlay.progress = progress;
    return overlay;
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

namespace {

/// A readback of @p format, @p width x @p height, with @p bytes as its data.
QRhiReadbackResult readbackOf(QRhiTexture::Format format, int width, int height, QByteArray bytes)
{
    QRhiReadbackResult readback;
    readback.format = format;
    readback.pixelSize = QSize(width, height);
    readback.data = std::move(bytes);
    return readback;
}

/// The premultiplied ARGB word of @p image at (@p x, @p y).
QRgb word(const QImage& image, int x, int y)
{
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied).pixel(x, y);
}

} // namespace

TEST(Readback, EightBitFormatsKeepTheirPremultipliedBytes)
{
    // Two pixels: an opaque red, and a blue at half alpha (premultiplied).
    const char rgba[] = {char(200), 10, 20, char(255), 0, 0, 64, char(128)};
    const char bgra[] = {20, 10, char(200), char(255), 64, 0, 0, char(128)};
    for (auto [format, bytes] : {std::pair{QRhiTexture::RGBA8, rgba}, std::pair{QRhiTexture::BGRA8, bgra}}) {
        QRhiReadbackResult readback = readbackOf(format, 2, 1, QByteArray(bytes, 8));
        const QImage image = WindowRenderer::readbackImage(readback, false, false);
        if (image.isNull()) {
            ADD_FAILURE() << "no image for format " << int(format);
            continue;
        }
        EXPECT_TRUE(readback.data.isEmpty()) << "the image takes the bytes";
        EXPECT_EQ(image.size(), QSize(2, 1));
        EXPECT_EQ(word(image, 0, 0), qRgba(200, 10, 20, 255)) << int(format);
        EXPECT_EQ(word(image, 1, 0), qRgba(0, 0, 64, 128)) << int(format);
    }
}

TEST(Readback, OpaqueSetsAlphaAndKeepsTheShownColours)
{
    // A surface without alpha shows the stored (premultiplied) colour. The
    // bytes are checked as they are: a conversion would set alpha by itself.
    const char bgra[] = {64, 0, 0, char(128)};
    QRhiReadbackResult words = readbackOf(QRhiTexture::BGRA8, 1, 1, QByteArray(bgra, 4));
    const QImage wordImage = WindowRenderer::readbackImage(words, false, true);
    ASSERT_FALSE(wordImage.isNull());
    EXPECT_FALSE(wordImage.hasAlphaChannel());
    if (Q_BYTE_ORDER == Q_LITTLE_ENDIAN) {
        EXPECT_EQ(wordImage.format(), QImage::Format_RGB32);
        EXPECT_EQ(reinterpret_cast<const quint32*>(wordImage.constScanLine(0))[0], 0xff000040u);
    }
    const char rgba[] = {64, 0, 0, char(128)};
    QRhiReadbackResult bytes = readbackOf(QRhiTexture::RGBA8, 1, 1, QByteArray(rgba, 4));
    const QImage byteImage = WindowRenderer::readbackImage(bytes, false, true);
    ASSERT_FALSE(byteImage.isNull());
    EXPECT_EQ(byteImage.format(), QImage::Format_RGBX8888);
    const uchar* pixel = byteImage.constScanLine(0);
    EXPECT_EQ(pixel[0], 64);
    EXPECT_EQ(pixel[1], 0);
    EXPECT_EQ(pixel[2], 0);
    EXPECT_EQ(pixel[3], 0xff);
}

TEST(Readback, YUpFlipsTheRows)
{
    const char rgba[] = {char(255), 0, 0, char(255), 0, char(255), 0, char(255)};
    QRhiReadbackResult readback = readbackOf(QRhiTexture::RGBA8, 1, 2, QByteArray(rgba, 8));
    const QImage image = WindowRenderer::readbackImage(readback, true, false);
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(word(image, 0, 0), qRgba(0, 255, 0, 255));
    EXPECT_EQ(word(image, 0, 1), qRgba(255, 0, 0, 255));
}

TEST(Readback, TenBitAndFloatFormatsBecomeEightBit)
{
    // A2B10G10R10: R in the low bits.
    const quint32 packed = 1023u | (0u << 10) | (512u << 20) | (3u << 30);
    QRhiReadbackResult tenBit = readbackOf(QRhiTexture::RGB10A2, 1, 1,
                                           QByteArray(reinterpret_cast<const char*>(&packed), sizeof packed));
    const QImage ten = WindowRenderer::readbackImage(tenBit, false, false);
    ASSERT_FALSE(ten.isNull());
    EXPECT_EQ(ten.format(), QImage::Format_ARGB32_Premultiplied);
    EXPECT_EQ(qRed(word(ten, 0, 0)), 255);
    EXPECT_EQ(qGreen(word(ten, 0, 0)), 0);
    EXPECT_NEAR(qBlue(word(ten, 0, 0)), 128, 1);
    EXPECT_EQ(qAlpha(word(ten, 0, 0)), 255);

    const std::array<qfloat16, 4> halves = {qfloat16(1.0f), qfloat16(0.5f), qfloat16(0.0f), qfloat16(1.0f)};
    QRhiReadbackResult half = readbackOf(QRhiTexture::RGBA16F, 1, 1,
                                         QByteArray(reinterpret_cast<const char*>(halves.data()), sizeof halves));
    const QImage floats = WindowRenderer::readbackImage(half, false, false);
    ASSERT_FALSE(floats.isNull());
    EXPECT_EQ(qRed(word(floats, 0, 0)), 255);
    EXPECT_NEAR(qGreen(word(floats, 0, 0)), 128, 1);
    EXPECT_EQ(qBlue(word(floats, 0, 0)), 0);

    const std::array<float, 4> singles = {0.0f, 0.5f, 1.0f, 1.0f};
    QRhiReadbackResult single = readbackOf(QRhiTexture::RGBA32F, 1, 1,
                                           QByteArray(reinterpret_cast<const char*>(singles.data()), sizeof singles));
    const QImage wide = WindowRenderer::readbackImage(single, false, false);
    ASSERT_FALSE(wide.isNull());
    EXPECT_EQ(qRed(word(wide, 0, 0)), 0);
    EXPECT_NEAR(qGreen(word(wide, 0, 0)), 128, 1);
    EXPECT_EQ(qBlue(word(wide, 0, 0)), 255);
}

TEST(Readback, OtherFormatsAndShortDataGiveANullImage)
{
    QRhiReadbackResult red = readbackOf(QRhiTexture::R8, 2, 2, QByteArray(4, char(255)));
    EXPECT_TRUE(WindowRenderer::readbackImage(red, false, false).isNull());
    QRhiReadbackResult shortData = readbackOf(QRhiTexture::RGBA8, 2, 2, QByteArray(12, char(255)));
    EXPECT_TRUE(WindowRenderer::readbackImage(shortData, false, false).isNull());
    QRhiReadbackResult empty = readbackOf(QRhiTexture::RGBA8, 0, 0, {});
    EXPECT_TRUE(WindowRenderer::readbackImage(empty, false, false).isNull());
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
            int ink = 0, intermediate = 0, translucent = 0;
            for (int y = 0; y < img.height(); ++y) {
                for (int x = 0; x < img.width(); ++x) {
                    const QRgb c = img.pixel(x, y);
                    translucent += qAlpha(c) != 255;
                    ink += qRed(c) > 0;
                    intermediate += qRed(c) > 0 && qRed(c) < 255;
                }
            }
            EXPECT_EQ(translucent, 0);
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
    // A count and the first pixel that differs, not a failure for each pixel.
    int mismatches = 0;
    QPoint first;
    for (int y = 0; y < rgb.height(); ++y) {
        for (int x = 0; x < rgb.width(); ++x) {
            const QRgb a = rgb.pixel(x, y), b = bgr.pixel(x, y);
            if (qRed(a) == qBlue(b) && qGreen(a) == qGreen(b) && qBlue(a) == qRed(b)) continue;
            if (mismatches++ == 0) first = QPoint(x, y);
        }
    }
    EXPECT_EQ(mismatches, 0) << "first at " << first.x() << "," << first.y() << ": rgb " << std::hex << rgb.pixel(first)
                             << " bgr " << bgr.pixel(first);
}

TEST_F(FontRendering, SubpixelCompositesEachChannelOverColoredBackgrounds)
{
    for (const auto aa : {Antialiasing::Rgb, Antialiasing::Bgr}) {
        for (const float gamma : {1.0f, 1.6f}) {
            mode(aa, gamma);
            const QImage coverage = sample();
            const QImage colored = sample(" MWi Il -> fi", "220;90;40;48;2;30;120;200");
            ASSERT_EQ(coverage.size(), colored.size());
            // The text has ink, with channels that differ at its edges: a
            // sample without text would match the background everywhere.
            EXPECT_GT(coloredEdges(coverage), 0);
            int mismatches = 0;
            QPoint first;
            for (int y = 0; y < colored.height(); ++y) {
                for (int x = 0; x < colored.width(); ++x) {
                    const QRgb c = coverage.pixel(x, y), actual = colored.pixel(x, y);
                    const bool near = std::abs(qRed(actual) - (30 + (220 - 30) * qRed(c) / 255.0)) <= 2
                        && std::abs(qGreen(actual) - (120 + (90 - 120) * qGreen(c) / 255.0)) <= 2
                        && std::abs(qBlue(actual) - (200 + (40 - 200) * qBlue(c) / 255.0)) <= 2 && qAlpha(actual) == 255;
                    if (!near && mismatches++ == 0) first = QPoint(x, y);
                }
            }
            EXPECT_EQ(mismatches, 0) << "first at " << first.x() << "," << first.y() << ": coverage " << std::hex
                                     << coverage.pixel(first) << " composited " << colored.pixel(first);
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

TEST_F(FontRendering, DockTextKeepsSubpixelUnderShadersButNotWhenTranslucent)
{
    // Docks draw into the window after the chains, so no chain resamples
    // their text: it stays subpixel under the text, pane and window chains
    // and the overview. A translucent window has no opaque target, so the
    // docks fall back too.
    mode(Antialiasing::Rgb);
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    AfterglowVt::Terminal dockTerminal(Cols, 1, host);
    dockTerminal.resize(Cols, 1, cw, ch);
    dockTerminal.feed("\x1b[?25l\x1b[0;38;2;255;255;255;48;2;0;0;0m\x1b[2J MWi Il -> fi");
    PaneSnapshot dockSnapshot;
    dockSnapshot.capture(dockTerminal);
    const QSize size(Cols * cw, (Rows + 1) * ch);
    const QRect dockRect(0, Rows * ch, Cols * cw, ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(0, 0, Cols * cw, Rows * ch);
    WindowRenderer::Pane dock{&dockSnapshot, {}};
    dock.params.rect = dockRect;
    dock.params.contentOrigin = dockRect.topLeft();
    auto dockEdges = [&] {
        renderer->setDocks({dock});
        return coloredEdges(renderer->renderToImage(size, {pane}, {}, packRgba(0, 0, 0)).copy(dockRect));
    };
    EXPECT_GT(dockEdges(), 0);
    for (int layer = 0; layer < 4; ++layer) {
        ShaderChains chains;
        if (layer == 0) chains.text = {ShaderProgram::passthrough()};
        if (layer == 1) chains.pane = {ShaderProgram::passthrough()};
        if (layer == 2) chains.window = {ShaderProgram::passthrough()};
        if (layer == 3) chains.overview = {ShaderProgram::passthrough()};
        renderer->setShaderChains(chains);
        renderer->setOverview(layer == 3 ? overviewAt(true, 1.0f, 0.0f, Clock::now()) : WindowRenderer::Overview{});
        SCOPED_TRACE(layer);
        EXPECT_GT(dockEdges(), 0);
    }
    renderer->setOverview({});
    renderer->setShaderChains({});
    renderer->setTranslucent(true);
    EXPECT_EQ(dockEdges(), 0);
    renderer->setTranslucent(false);
    EXPECT_GT(dockEdges(), 0);
}

TEST_F(FontRendering, PaneDockTextIsSubpixelOnlyWithoutChains)
{
    // A pane dock is part of its pane: a chain that resamples the pane
    // resamples its docks too, so their text falls back to grayscale under
    // any chain (layer, pane, window, overview) and in a translucent window,
    // and is subpixel otherwise.
    mode(Antialiasing::Rgb);
    terminal->feed("\x1b[?25l\x1b[2J");
    snapshot.capture(*terminal);
    AfterglowVt::Terminal dockTerminal(Cols, 1, host);
    dockTerminal.resize(Cols, 1, cw, ch);
    dockTerminal.feed("\x1b[?25l\x1b[0;38;2;255;255;255;48;2;0;0;0m\x1b[2J MWi Il -> fi");
    PaneSnapshot dockSnapshot;
    dockSnapshot.capture(dockTerminal);
    const QSize size(Cols * cw, Rows * ch);
    const QRect dockRect(0, (Rows - 1) * ch, Cols * cw, ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    pane.docks.push_back({&dockSnapshot, dockRect});
    auto dockEdges = [&] {
        return coloredEdges(renderer->renderToImage(size, {pane}, {}, packRgba(0, 0, 0)).copy(dockRect));
    };
    EXPECT_GT(dockEdges(), 0);
    for (int layer = 0; layer < 5; ++layer) {
        ShaderChains chains;
        if (layer == 0) chains.background = {ShaderProgram::passthrough()};
        if (layer == 1) chains.text = {ShaderProgram::passthrough()};
        if (layer == 2) chains.pane = {ShaderProgram::passthrough()};
        if (layer == 3) chains.window = {ShaderProgram::passthrough()};
        if (layer == 4) chains.overview = {ShaderProgram::passthrough()};
        renderer->setShaderChains(chains);
        renderer->setOverview(layer == 4 ? overviewAt(true, 1.0f, 0.0f, Clock::now()) : WindowRenderer::Overview{});
        SCOPED_TRACE(layer);
        EXPECT_EQ(dockEdges(), 0);
    }
    renderer->setOverview({});
    renderer->setShaderChains({});
    EXPECT_GT(dockEdges(), 0);
    renderer->setTranslucent(true);
    EXPECT_EQ(dockEdges(), 0);
    renderer->setTranslucent(false);
    EXPECT_GT(dockEdges(), 0);
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
    // Colour ink, which the white on black sample has only from the emoji.
    EXPECT_GT(coloredEdges(original), 0);
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
    // A green clear: the background is the grid's.
    const QImage img = render("\x1b[?25l", true, 255, 0xFF00FF00u);
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

TEST_F(Render, LegacyComputingSpritesFillTheirParts)
{
    // Symbols for Legacy Computing and its Supplement, drawn to the cell:
    // a sample of each family, checked at the centres of its parts.
    const QRgb fg = qRgb(240, 240, 240);
    std::u32string text = U"\U0001FB00\U0001FB3B\U0001CD00\U0001CDE5\U0001FB3C\U0001FB41\U0001FB6C\U0001FB68"
                          U"\U0001CEA0\U0001CE95\U0001FB82\U0001CC2F\U0001FB8C\U0001FBA2\U0001FB70\U0001FBE6";
    std::string utf8;
    for (char32_t cp : text) {
        utf8 += QString::fromUcs4(&cp, 1).toStdString();
    }
    const QImage img = render("\x1b[?25l\x1b[38;2;240;240;240m" + utf8);
    const QRgb bg = rgb(snapshot.defaultBg);
    // fx and fy are fractions of the cell. Lit: nearer the foreground.
    auto lit = [&](int cell, double fx, double fy) {
        const QRgb p = img.pixel(int(cell * cw + fx * cw), int(fy * ch));
        auto dist = [&](QRgb q) { return std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q)); };
        return dist(fg) < dist(bg);
    };
    // A grid's part centres, bit 0 at the upper left, row by row.
    auto expectGrid = [&](int cell, unsigned mask, int cols, int rows) {
        for (int i = 0; i < cols * rows; ++i) {
            EXPECT_EQ(lit(cell, (i % cols + 0.5) / cols, (i / cols + 0.5) / rows), bool(mask & (1u << i)))
                << "cell " << cell << " part " << i;
        }
    };
    expectGrid(0, 0b000001, 2, 3); // U+1FB00 BLOCK SEXTANT-1
    expectGrid(1, 0b111110, 2, 3); // U+1FB3B BLOCK SEXTANT-23456
    expectGrid(2, 0b00000100, 2, 4); // U+1CD00 BLOCK OCTANT-3
    expectGrid(3, 0b11111110, 2, 4); // U+1CDE5 BLOCK OCTANT-2345678
    // U+1FB3C: lower left, from the lower third of the left edge to the
    // centre of the bottom edge.
    EXPECT_TRUE(lit(4, 0.05, 0.95));
    EXPECT_FALSE(lit(4, 0.5, 0.5));
    EXPECT_FALSE(lit(4, 0.45, 0.75));
    // U+1FB41: lower right, cut off from the upper third of the left edge to
    // the centre of the top edge.
    EXPECT_FALSE(lit(5, 0.05, 0.05));
    EXPECT_TRUE(lit(5, 0.5, 0.5));
    EXPECT_TRUE(lit(5, 0.6, 0.05));
    // U+1FB6C left triangle to the centre; U+1FB68 the three others.
    EXPECT_TRUE(lit(6, 0.1, 0.5));
    EXPECT_FALSE(lit(6, 0.9, 0.5));
    EXPECT_FALSE(lit(6, 0.5, 0.1));
    EXPECT_FALSE(lit(7, 0.1, 0.5));
    EXPECT_TRUE(lit(7, 0.9, 0.5));
    EXPECT_TRUE(lit(7, 0.5, 0.1));
    EXPECT_TRUE(lit(7, 0.5, 0.9));
    expectGrid(8, 0b1100000000000000, 4, 4); // U+1CEA0 right half, lower quarter
    expectGrid(9, 0b0000000000100000, 4, 4); // U+1CE95 a sixteenth
    expectGrid(10, 0b00000011, 2, 4); // U+1FB82 UPPER ONE QUARTER BLOCK: the first of four rows
    // U+1CC2F separated quadrants: all four parts, with gaps between them
    // and at the edges of the cell.
    expectGrid(11, 0b1111, 2, 2);
    EXPECT_FALSE(lit(11, 0.5, 0.5));
    EXPECT_FALSE(lit(11, 0.0, 0.25));
    // U+1FB8C left half medium shade: half coverage.
    EXPECT_PIXEL_NEAR(img, QPoint(12 * cw + cw / 4, ch / 2),
                      qRgb((qRed(fg) + qRed(bg)) / 2, (qGreen(fg) + qGreen(bg)) / 2, (qBlue(fg) + qBlue(bg)) / 2), 60);
    EXPECT_FALSE(lit(12, 0.75, 0.5));
    // U+1FBA2 a line from the middle of the left edge to the centre of the
    // bottom edge.
    EXPECT_FALSE(lit(13, 0.75, 0.25));
    EXPECT_FALSE(lit(13, 0.1, 0.1));
    // U+1FB70 VERTICAL ONE EIGHTH BLOCK-2: the second of eight columns.
    {
        const int x0 = int(std::lround(cw / 8.0));
        const int x1 = int(std::lround(cw * 2 / 8.0));
        ASSERT_GT(x1, x0);
        EXPECT_PIXEL(img, QPoint(13 * cw + cw + x0, ch / 2), fg);
        EXPECT_PIXEL(img, QPoint(13 * cw + cw + x1, ch / 2), bg);
    }
    // U+1FBE6 middle left quarter.
    EXPECT_TRUE(lit(15, 0.25, 0.5));
    EXPECT_FALSE(lit(15, 0.25, 0.1));
    EXPECT_FALSE(lit(15, 0.75, 0.5));
}

TEST_F(Render, LegacyComputingDiagonalsMeetTheBoxDrawingLines)
{
    // U+2500, then U+1FBA2 and U+1FBA3, then U+2500: the diagonals start at
    // the middle of the edge, where the horizontal line ends.
    const QImage img = render("\x1b[?25l\x1b[38;2;240;240;240m─\U0001FBA2\U0001FBA3─");
    const QRgb fg = qRgb(240, 240, 240);
    int row = -1;
    for (int y = 0; y < ch && row < 0; ++y) {
        if (img.pixel(cw / 2, y) == fg) row = y;
    }
    ASSERT_GE(row, 0);
    // At the edge, each diagonal covers the row of the horizontal line (in
    // part: the steep line is antialiased) and nothing above it.
    const int bg = qGray(rgb(snapshot.defaultBg));
    for (const int x : {cw, 3 * cw - 1}) {
        EXPECT_GT(qGray(img.pixel(x, row)), bg + 40) << "column " << x;
        EXPECT_LE(qGray(img.pixel(x, row - 2)), bg + 6) << "column " << x;
    }
}

TEST_F(Render, ScaledLegacyComputingSpritesKeepTheirParts)
{
    // A 2x2 OSC 66 block draws the sprite at twice the size.
    const QImage img = render("\x1b[?25l\x1b[38;2;240;240;240m\x1b]66;s=2;\U0001FB00\U0001CD00\x07");
    const QRgb fg = qRgb(240, 240, 240);
    const QRgb bg = rgb(snapshot.defaultBg);
    // fx and fy are fractions of the 2x2 block.
    auto at = [&](int block, double fx, double fy) { return QPoint(int((2 * block + 2 * fx) * cw), int(2 * fy * ch)); };
    EXPECT_PIXEL(img, at(0, 0.25, 1 / 6.0), fg); // U+1FB00 BLOCK SEXTANT-1: the upper left part
    EXPECT_PIXEL(img, at(0, 0.75, 1 / 6.0), bg);
    EXPECT_PIXEL(img, at(0, 0.25, 0.5), bg);
    EXPECT_PIXEL(img, at(1, 0.25, 3 / 8.0), fg); // U+1CD00 BLOCK OCTANT-3: second row, left
    EXPECT_PIXEL(img, at(1, 0.25, 1 / 8.0), bg);
    EXPECT_PIXEL(img, at(1, 0.75, 3 / 8.0), bg);
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
    const std::string enc = AfterglowProto::base64Encode(rgba);
    const QImage img = render("\x1b[?25l\x1b[2;2H\x1b_Ga=T,f=32,s=2,v=2,c=2,r=2,q=2;" + enc + "\x1b\\");
    // Each image pixel covers one cell of the 2x2-cell placement at (1, 1).
    // The image is filtered, so cell centres off texel centres blend a little.
    EXPECT_PIXEL_NEAR(img, cellCenter(1, 1), qRgb(255, 0, 0), 40);
    EXPECT_PIXEL_NEAR(img, cellCenter(2, 1), qRgb(0, 255, 0), 40);
    EXPECT_PIXEL_NEAR(img, cellCenter(1, 2), qRgb(0, 0, 255), 40);
    EXPECT_PIXEL_NEAR(img, cellCenter(2, 2), qRgb(255, 255, 255), 40);
    EXPECT_PIXEL(img, cellCenter(0, 0), rgb(snapshot.defaultBg));
}

TEST_F(Render, IndexedImagesTakeTheColoursOfThePane)
{
    // kitty f=8 and f=16: palette indices, drawn in the pane's colours.
    using AfterglowProto::base64Encode;
    const std::string red = "\x1b]4;1;rgb:ff/00/00\x1b\\";
    // Four 1x1 images, one cell each, side by side: palette 1, the cursor
    // colour (252), palette 1 at alpha 0 (f=16), the default background (248).
    std::string bytes = "\x1b[?25l" + red + "\x1b]12;rgb:12/34/56\x1b\\\x1b[1;1H";
    // Selection and cursor text colours of their own, so that 250, 251 and
    // 253 differ from the defaults and from each other.
    bytes += "\x1b]17;rgb:20/40/60\x1b\\\x1b]19;rgb:60/40/20\x1b\\\x1b]21;cursor_text=#0a141e\x1b\\";
    bytes += "\x1b_Ga=T,f=8,s=1,v=1,c=1,r=1,q=2;" + base64Encode(std::string("\x01", 1)) + "\x1b\\";
    bytes += "\x1b[1;2H\x1b_Ga=T,f=8,s=1,v=1,c=1,r=1,q=2;" + base64Encode(std::string("\xfc", 1)) + "\x1b\\";
    bytes += "\x1b[1;3H\x1b_Ga=T,f=16,s=1,v=1,c=1,r=1,q=2;" + base64Encode(std::string("\x01\x00", 2)) + "\x1b\\";
    bytes += "\x1b[1;4H\x1b_Ga=T,f=8,s=1,v=1,c=1,r=1,q=2;" + base64Encode(std::string("\xf8", 1)) + "\x1b\\";
    // Palette 1 at half alpha (f=16).
    bytes += "\x1b[1;5H\x1b_Ga=T,f=16,s=1,v=1,c=1,r=1,q=2;" + base64Encode(std::string("\x01\x80", 2)) + "\x1b\\";
    // The other special entries, one cell each on the second row: the
    // default foreground (249), the selection colours (250, 251), the
    // cursor text (253), link hover (254) and the reserved entry (255).
    const std::string special("\xf9\xfa\xfb\xfd\xfe\xff", 6);
    for (std::size_t i = 0; i < special.size(); ++i) {
        bytes += "\x1b[2;" + std::to_string(i + 1) + "H\x1b_Ga=T,f=8,s=1,v=1,c=1,r=1,q=2;"
            + base64Encode(special.substr(i, 1)) + "\x1b\\";
    }
    // A pane at half opacity: the default background of the image stays opaque.
    QImage img = render(bytes, true, 128, 0x00000000u);
    const QRgb pane = img.pixel(cellCenter(10, 0)); // no image there
    EXPECT_LT(qAlpha(pane), 255);
    EXPECT_PIXEL_NEAR(img, cellCenter(0, 0), qRgb(255, 0, 0), 12);
    EXPECT_PIXEL_NEAR(img, cellCenter(1, 0), qRgb(0x12, 0x34, 0x56), 12);
    EXPECT_PIXEL_NEAR(img, cellCenter(2, 0), pane, 6); // alpha 0: only the pane shows
    EXPECT_EQ(qAlpha(img.pixel(cellCenter(3, 0))), 255);
    EXPECT_PIXEL_NEAR(img, cellCenter(3, 0), rgb(snapshot.defaultBg), 12);
    // Half alpha over the half-transparent pane: 0.5 + 0.5 x 0.5 of coverage.
    const QRgb half = img.pixel(cellCenter(4, 0));
    EXPECT_NEAR(qAlpha(half), 191, 8);
    EXPECT_GT(qRed(half), qRed(pane) + 40);
    ASSERT_EQ(snapshot.selectionBackgroundColor, packRgba(0x20, 0x40, 0x60));
    ASSERT_EQ(snapshot.selectionForegroundColor, packRgba(0x60, 0x40, 0x20));
    ASSERT_EQ(snapshot.cursorTextColor, packRgba(0x0a, 0x14, 0x1e));
    const std::uint32_t expected[] = {snapshot.defaultFg,      snapshot.selectionBackgroundColor,
                                      snapshot.selectionForegroundColor, snapshot.cursorTextColor,
                                      snapshot.defaultFg,      snapshot.defaultFg};
    for (int i = 0; i < 6; ++i) {
        SCOPED_TRACE(int(std::uint8_t(special[std::size_t(i)])));
        EXPECT_PIXEL_NEAR(img, cellCenter(i, 1), rgb(expected[i]), 12);
    }
    // A new palette entry redraws the image in the new colour.
    img = render("\x1b]4;1;rgb:00/ff/00\x1b\\", true, 128, 0x00000000u);
    EXPECT_PIXEL_NEAR(img, cellCenter(0, 0), qRgb(0, 255, 0), 12);
    // Reverse video swaps the default colours, and 248 and 249 with them.
    const std::uint32_t foreground = snapshot.defaultFg;
    img = render("\x1b[?5h", true, 128, 0x00000000u);
    ASSERT_EQ(snapshot.defaultBg, foreground);
    EXPECT_PIXEL_NEAR(img, cellCenter(3, 0), rgb(foreground), 12);
}

TEST_F(Render, AHeldFrameKeepsTheImageThatASynchronizedUpdateReplaces)
{
    // kitty 0.49.2: an image replaced during a synchronized update vanished
    // for a frame. The pane draws its held snapshot until the update ends.
    const std::string place = "\x1b[2;2H\x1b_Ga=T,f=24,s=1,v=1,i=1,c=2,r=1,q=2;";
    QImage img = render("\x1b[?25l" + place + "/wAA\x1b\\"); // red
    EXPECT_PIXEL_NEAR(img, cellCenter(1, 1), qRgb(255, 0, 0), 40);
    terminal->feed("\x1b[?2026h" + place + "AAD/\x1b\\"); // blue, inside the update
    img = draw();
    EXPECT_PIXEL_NEAR(img, cellCenter(1, 1), qRgb(255, 0, 0), 40);
    EXPECT_PIXEL_NEAR(img, cellCenter(2, 1), qRgb(255, 0, 0), 40);
    img = render("\x1b[?2026l");
    EXPECT_PIXEL_NEAR(img, cellCenter(1, 1), qRgb(0, 0, 255), 40);
    EXPECT_PIXEL_NEAR(img, cellCenter(2, 1), qRgb(0, 0, 255), 40);
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
    // A failure, and a program that draws, so that the caller goes on to
    // fail its checks instead of crashing on a null chain entry.
    if (!program) {
        ADD_FAILURE() << id << ": " << error.toStdString();
        return ShaderProgram::passthrough();
    }
    return program;
}

/// A pack with the panes contract (terminal_panes.glsl): @p body is the
/// whole of pTerminal.
ShaderProgramPtr panesShader(const char* id, const char* body)
{
    ShaderProgramSource source;
    source.id = QString::fromLatin1(id);
    source.code = QByteArray("#include <terminal_panes.glsl>\nvec4 pTerminal(vec2 uv) {\n") + body + "\n}\n";
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    if (!program) {
        ADD_FAILURE() << id << ": " << error.toStdString();
        return ShaderProgram::passthrough();
    }
    return program;
}
} // namespace

namespace {
ShaderProgramPtr pointerMapProgram(const char* id, const char* map)
{
    ShaderProgramSource src;
    src.id = QString::fromLatin1(id);
    src.code = QByteArray("vec4 pTerminal(vec2 uv) { return terminalTexel(uv); }\n") + map;
    QString error;
    auto program = ShaderProgram::compile(src, &error);
    EXPECT_TRUE(program) << error.toStdString();
    return program ? program->instance(QString::fromLatin1(id)) : nullptr;
}
} // namespace

TEST_F(Render, PointerMapsFindTheContentUnderThePointer)
{
    // A map that mirrors the canvas: the content at x came from width - x.
    auto mirror = pointerMapProgram("mirror", "vec2 pPointerMap(vec2 uv) { return vec2(1.0 - uv.x, uv.y); }\n");
    ASSERT_TRUE(mirror && mirror->hasPointerMap());
    auto none = pointerMapProgram("none", "");
    ASSERT_TRUE(none && !none->hasPointerMap());
    // A stage with its own main() has no generated entry, so no map.
    ShaderProgramSource own;
    own.id = QStringLiteral("own-main");
    // Such a stage has no prologue either: it declares what it uses.
    own.code = "#version 450\nlayout(location = 0) out vec4 fragColor;\nvec2 pPointerMap(vec2 uv) { return uv; }\n"
               "void main() { fragColor = vec4(pPointerMap(vec2(0.5)), 0.0, 1.0); }\n";
    QString error;
    auto ownMain = ShaderProgram::compile(own, &error);
    ASSERT_TRUE(ownMain) << error.toStdString();
    EXPECT_FALSE(ownMain->hasPointerMap());

    const QSize window(200, 100);
    ShaderChains chains;
    chains.window = {none, mirror};
    renderer->setShaderChains(chains);
    EXPECT_TRUE(renderer->hasPointerMaps());
    auto near = [](QPointF a, QPointF b) { return std::abs(a.x() - b.x()) < 0.5 && std::abs(a.y() - b.y()) < 0.5; };
    EXPECT_TRUE(near(renderer->windowContentPoint({10, 20}, window, {}), {190, 20}));
    EXPECT_TRUE(near(renderer->windowContentPoint({150, 80}, window, {}), {50, 80}));
    // A pane chain maps inside its pane's rectangle.
    chains = {};
    chains.pane = {mirror};
    renderer->setShaderChains(chains);
    EXPECT_TRUE(near(renderer->windowContentPoint({10, 20}, window, {}), {10, 20})); // the window chain has no map
    EXPECT_TRUE(near(renderer->paneContentPoint({110, 60}, QRect(100, 50, 200, 100), {}), {290, 60}));
    // The canvas size: a shift of 10 pixels on any canvas.
    auto shift = pointerMapProgram("shift", "vec2 pPointerMap(vec2 uv) { return uv + vec2(10.0 / iResolution.x, 0.0); }\n");
    chains = {};
    chains.window = {shift};
    renderer->setShaderChains(chains);
    EXPECT_TRUE(near(renderer->windowContentPoint({50, 50}, QSize(300, 200), {}), {60, 50}));
    EXPECT_TRUE(near(renderer->windowContentPoint({50, 50}, QSize(800, 600), {}), {60, 50}));
    // Live parameter values: the map is evaluated again for new ones.
    auto scaled = pointerMapProgram("scaled", "vec2 pPointerMap(vec2 uv) { return vec2(uv.x * customParams[0].x, uv.y); }\n");
    chains = {};
    chains.window = {scaled};
    renderer->setShaderChains(chains);
    ShaderParameterMap parameters;
    ShaderParameterValues half;
    half.paramMask = 1;
    half.params[0] = 0.5f;
    parameters.insert(scaled->instanceId(), half);
    EXPECT_TRUE(near(renderer->windowContentPoint({100, 50}, window, parameters), {50, 50}));
    half.params[0] = 0.25f;
    parameters.insert(scaled->instanceId(), half);
    EXPECT_TRUE(near(renderer->windowContentPoint({100, 50}, window, parameters), {25, 50}));
    // Without maps, the point itself.
    chains = {};
    chains.window = {none};
    renderer->setShaderChains(chains);
    EXPECT_FALSE(renderer->hasPointerMaps());
    EXPECT_EQ(renderer->windowContentPoint({33, 44}, window, {}), QPointF(33, 44));
}

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
        // A grey clear: black is the shader's, not a pane that was not drawn.
        auto image = renderer->renderToImage(size, {pane}, {}, 0xFF808080u);
        EXPECT_PIXEL(image, cellCenter(1, 1), qRgb(64, 191, 0));
        pane.params.shaderParameters.clear();
        image = renderer->renderToImage(size, {pane}, {}, 0xFF808080u);
        EXPECT_PIXEL(image, cellCenter(1, 1), qRgb(0, 0, 0));
    }
}

TEST_F(Render, DocksAreDrawnAfterEveryShaderChain)
{
    // Each chain paints everything magenta. A dock below the pane keeps its
    // own red: it goes into the window after the chains, the layered path,
    // the overview and the pane effects included.
    auto source = layerShader("magenta", "fragColor = vec4(1.0, 0.0, 1.0, 1.0);");
    ASSERT_TRUE(source);
    auto program = source->instance(QStringLiteral("magenta"));
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    AfterglowVt::Terminal dockTerminal(Cols, 1, host);
    dockTerminal.resize(Cols, 1, cw, ch);
    dockTerminal.feed("\x1b[?25l\x1b[41m\x1b[2K");
    PaneSnapshot dockSnapshot;
    dockSnapshot.capture(dockTerminal);
    const QSize size(Cols * cw, (Rows + 1) * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(0, 0, Cols * cw, Rows * ch);
    WindowRenderer::Pane dock{&dockSnapshot, {}};
    dock.params.rect = QRect(0, Rows * ch, Cols * cw, ch);
    dock.params.contentOrigin = dock.params.rect.topLeft();
    const QPoint inDock(cw / 2, Rows * ch + ch / 2);
    enum class Chain { None, Text, Pane, Window, Overview, PaneEffects };
    for (const Chain kind : {Chain::None, Chain::Text, Chain::Pane, Chain::Window, Chain::Overview, Chain::PaneEffects}) {
        ShaderChains chains;
        if (kind == Chain::Text) chains.text = {program};
        if (kind == Chain::Pane) chains.pane = {program};
        if (kind == Chain::Window) chains.window = {program};
        if (kind == Chain::Overview) chains.overview = {program};
        if (kind == Chain::PaneEffects) chains.paneEffects = {program};
        renderer->setShaderChains(chains);
        renderer->setOverview(kind == Chain::Overview ? overviewAt(true, 1.0f, 0.0f, Clock::now()) : WindowRenderer::Overview{});
        renderer->setPaneEffects({kind == Chain::PaneEffects, false, std::nullopt, 1.0f});
        renderer->setDocks({dock});
        SCOPED_TRACE(int(kind));
        const QImage image = renderer->renderToImage(size, {pane}, {}, 0xFF000000u);
        EXPECT_PIXEL(image, inDock, rgb(dockSnapshot.cells[0].bg));
        if (kind != Chain::None) EXPECT_PIXEL(image, cellCenter(1, 1), qRgb(255, 0, 255));
    }
}

TEST_F(Render, DocksAreDrawnOnlyInTheFrameThatSetsThem)
{
    // The renderer keeps no dock pointer past a frame: a frame without its
    // own setDocks() shows the clear colour where the dock was.
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    AfterglowVt::Terminal dockTerminal(Cols, 1, host);
    dockTerminal.resize(Cols, 1, cw, ch);
    dockTerminal.feed("\x1b[?25l\x1b[41m\x1b[2K");
    auto dockSnapshot = std::make_unique<PaneSnapshot>();
    dockSnapshot->capture(dockTerminal);
    const QSize size(Cols * cw, (Rows + 1) * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(0, 0, Cols * cw, Rows * ch);
    WindowRenderer::Pane dock{dockSnapshot.get(), {}};
    dock.params.rect = QRect(0, Rows * ch, Cols * cw, ch);
    dock.params.contentOrigin = dock.params.rect.topLeft();
    const QPoint inDock(cw / 2, Rows * ch + ch / 2);
    const QRgb red = rgb(dockSnapshot->cells[0].bg);
    renderer->setDocks({dock});
    EXPECT_PIXEL(renderer->renderToImage(size, {pane}, {}, 0xFF000000u), inDock, red);
    // As the window frees a dock between frames.
    dockSnapshot.reset();
    EXPECT_PIXEL(renderer->renderToImage(size, {pane}, {}, 0xFF000000u), inDock, qRgb(0, 0, 0));
}

TEST_F(Render, DockImagesAreUploadedWithTheirDock)
{
    // A green 1x1 kitty image in a dock that no pane shows: its texture is
    // uploaded for the dock itself, with and without a chain.
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    AfterglowVt::Terminal dockTerminal(Cols, 1, host);
    dockTerminal.resize(Cols, 1, cw, ch);
    dockTerminal.feed("\x1b[?25l\x1b[41m\x1b[2K\x1b[1;2H\x1b_Ga=T,f=32,s=1,v=1,c=1,r=1,C=1,q=2;AP8A/w==\x1b\\");
    PaneSnapshot dockSnapshot;
    dockSnapshot.capture(dockTerminal);
    ASSERT_EQ(dockSnapshot.images.size(), 1u);
    const QSize size(Cols * cw, (Rows + 1) * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(0, 0, Cols * cw, Rows * ch);
    WindowRenderer::Pane dock{&dockSnapshot, {}};
    dock.params.rect = QRect(0, Rows * ch, Cols * cw, ch);
    dock.params.contentOrigin = dock.params.rect.topLeft();
    for (bool shaded : {false, true}) {
        ShaderChains chains;
        if (shaded) chains.window = {ShaderProgram::passthrough()};
        renderer->setShaderChains(chains);
        renderer->setDocks({dock});
        SCOPED_TRACE(shaded);
        const QImage image = renderer->renderToImage(size, {pane}, {}, 0xFF000000u);
        EXPECT_PIXEL_NEAR(image, QPoint(cw + cw / 2, Rows * ch + ch / 2), qRgb(0, 255, 0), 40);
        EXPECT_PIXEL(image, QPoint(cw / 2, Rows * ch + ch / 2), rgb(dockSnapshot.cells[0].bg));
    }
}

TEST_F(Render, PaneDocksAreDrawnInsideTheirPaneAndUnderItsChains)
{
    // A red pane dock on the pane's last row, with a green image in its
    // second cell, and a red window dock below the pane. The pane dock is
    // part of the pane: drawn over the pane's grid, cut at the pane's edge,
    // and a chain passes it through or halves it as it does the pane. The
    // window dock comes after every chain and keeps its red.
    auto source = layerShader("halve", "fragColor = vec4(c.rgb * 0.5, c.a);");
    ASSERT_TRUE(source);
    auto halve = source->instance(QStringLiteral("halve"));
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    AfterglowVt::Terminal dockTerminal(Cols, 1, host);
    dockTerminal.resize(Cols, 1, cw, ch);
    dockTerminal.feed("\x1b[?25l\x1b[41m\x1b[2K\x1b[1;2H\x1b_Ga=T,f=32,s=1,v=1,c=1,r=1,C=1,q=2;AP8A/w==\x1b\\");
    PaneSnapshot dockSnapshot;
    dockSnapshot.capture(dockTerminal);
    ASSERT_EQ(dockSnapshot.images.size(), 1u);
    AfterglowVt::Terminal windowDockTerminal(Cols, 1, host);
    windowDockTerminal.resize(Cols, 1, cw, ch);
    windowDockTerminal.feed("\x1b[?25l\x1b[41m\x1b[2K");
    PaneSnapshot windowDockSnapshot;
    windowDockSnapshot.capture(windowDockTerminal);
    const QRgb red = rgb(dockSnapshot.cells[0].bg);
    // The pane is one column narrower than the window, and the dock as wide
    // as the window: its last column is cut.
    const QSize size(Cols * cw, (Rows + 1) * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(0, 0, (Cols - 1) * cw, Rows * ch);
    pane.docks.push_back({&dockSnapshot, QRect(0, (Rows - 1) * ch, Cols * cw, ch)});
    WindowRenderer::Pane windowDock{&windowDockSnapshot, {}};
    windowDock.params.rect = QRect(0, Rows * ch, Cols * cw, ch);
    windowDock.params.contentOrigin = windowDock.params.rect.topLeft();
    const QPoint inDock(cw / 2, (Rows - 1) * ch + ch / 2);
    const QPoint onImage(cw + cw / 2, (Rows - 1) * ch + ch / 2);
    const QPoint pastPane((Cols - 1) * cw + cw / 2, (Rows - 1) * ch + ch / 2);
    const QPoint inWindowDock(cw / 2, Rows * ch + ch / 2);
    auto halved = [](QRgb c) { return qRgb(qRed(c) / 2, qGreen(c) / 2, qBlue(c) / 2); };
    enum class Chain { None, Pane, Window };
    for (const Chain kind : {Chain::None, Chain::Pane, Chain::Window}) {
        for (const bool halving : {false, true}) {
            if (kind == Chain::None && halving) continue;
            const ShaderProgramPtr program = halving ? halve : ShaderProgramPtr(ShaderProgram::passthrough());
            ShaderChains chains;
            if (kind == Chain::Pane) chains.pane = {program};
            if (kind == Chain::Window) chains.window = {program};
            renderer->setShaderChains(chains);
            renderer->setDocks({windowDock});
            SCOPED_TRACE(testing::Message() << "chain " << int(kind) << (halving ? " halving" : " passing through"));
            const QImage image = renderer->renderToImage(size, {pane}, {}, 0xFF000000u);
            auto shaded = [&](QRgb c) { return halving ? halved(c) : c; };
            EXPECT_PIXEL(image, inDock, shaded(red));
            EXPECT_PIXEL_NEAR(image, onImage, shaded(qRgb(0, 255, 0)), 40);
            EXPECT_PIXEL(image, cellCenter(1, 1), shaded(rgb(snapshot.defaultBg)));
            EXPECT_PIXEL(image, pastPane, qRgb(0, 0, 0));
            EXPECT_PIXEL(image, inWindowDock, rgb(windowDockSnapshot.cells[0].bg));
        }
    }
}

TEST_F(Render, PaneDocksHideThePanesContentInEveryLayer)
{
    // The pane's first and last rows are white full blocks, with a block
    // cursor and a green image on the last row, and a red pane dock covers
    // that row. In one pass the dock's background covers them. In the
    // layered path each layer is a texture of its own, where nothing of the
    // dock covers the pane's text, cursor or image: they must keep out of
    // the dock there too. A top dock does the same for row 0, shifted up
    // into it by a smooth scroll.
    std::string blocks;
    for (int i = 0; i < Cols; ++i) blocks += "\xe2\x96\x88";
    const std::string last = std::to_string(Rows);
    terminal->feed("\x1b[2J\x1b[38;2;255;255;255m\x1b[1;1H" + blocks + "\x1b[" + last + ";1H" + blocks + "\x1b["
                   + last + ";6H\x1b_Ga=T,f=32,s=1,v=1,c=1,r=1,C=1,q=2;AP8A/w==\x1b\\\x1b[" + last + ";3H");
    snapshot.capture(*terminal);
    ASSERT_EQ(snapshot.images.size(), 1u);
    ASSERT_TRUE(snapshot.cursorVisible);
    AfterglowVt::Terminal dockTerminal(Cols, 1, host);
    dockTerminal.resize(Cols, 1, cw, ch);
    dockTerminal.feed("\x1b[?25l\x1b[41m\x1b[2K");
    PaneSnapshot dockSnapshot;
    dockSnapshot.capture(dockTerminal);
    const QRgb red = rgb(dockSnapshot.cells[0].bg);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    pane.params.focused = true;
    enum class Chain { None, Background, Text, Cursor, Pane };
    auto frame = [&](Chain kind) {
        ShaderChains chains;
        if (kind == Chain::Background) chains.background = {ShaderProgram::passthrough()};
        if (kind == Chain::Text) chains.text = {ShaderProgram::passthrough()};
        if (kind == Chain::Cursor) chains.cursor = {ShaderProgram::passthrough()};
        if (kind == Chain::Pane) chains.pane = {ShaderProgram::passthrough()};
        renderer->setShaderChains(chains);
        return renderer->renderToImage(size, {pane}, {}, 0xFF000000u);
    };
    const int lastRow = Rows - 1;
    for (const Chain kind : {Chain::None, Chain::Background, Chain::Text, Chain::Cursor, Chain::Pane}) {
        SCOPED_TRACE(int(kind));
        pane.params.contentOrigin = QPoint();
        pane.params.scrollShift = 0;
        pane.docks.clear();
        pane.docks.push_back({&dockSnapshot, QRect(0, lastRow * ch, size.width(), ch)});
        QImage image = frame(kind);
        EXPECT_PIXEL(image, cellCenter(0, 0), qRgb(255, 255, 255));
        EXPECT_PIXEL(image, cellCenter(0, lastRow), red);
        EXPECT_PIXEL(image, cellCenter(2, lastRow), red); // the cursor
        EXPECT_PIXEL(image, cellCenter(5, lastRow), red); // the image
        EXPECT_PIXEL(image, cellCenter(Cols - 1, lastRow), red);

        // Row 0 is drawn from half a row into the dock down to half a row below it.
        pane.params.contentOrigin = QPoint(0, ch);
        pane.params.scrollShift = float(ch / 2);
        pane.docks.clear();
        pane.docks.push_back({&dockSnapshot, QRect(0, 0, size.width(), ch)});
        image = frame(kind);
        EXPECT_PIXEL(image, QPoint(cw / 2, ch * 3 / 4), red);
        EXPECT_PIXEL(image, QPoint((Cols - 1) * cw + cw / 2, ch * 3 / 4), red);
        EXPECT_PIXEL(image, QPoint(cw / 2, ch + ch / 4), qRgb(255, 255, 255));
    }
}

TEST_F(Render, PaneDockTextBlinksWithItsPane)
{
    // Blinking text in a pane dock follows the pane's blink phase.
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    AfterglowVt::Terminal dockTerminal(Cols, 1, host);
    dockTerminal.resize(Cols, 1, cw, ch);
    dockTerminal.feed("\x1b[?25l\x1b[5;38;2;255;255;255m\xe2\x96\x88");
    PaneSnapshot dockSnapshot;
    dockSnapshot.capture(dockTerminal);
    ASSERT_TRUE(dockSnapshot.hasBlinkingText);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    pane.docks.push_back({&dockSnapshot, QRect(0, (Rows - 1) * ch, size.width(), ch)});
    const QPoint onText = cellCenter(0, Rows - 1);
    EXPECT_PIXEL(renderer->renderToImage(size, {pane}, {}, 0xFF000000u), onText, qRgb(255, 255, 255));
    pane.params.textBlinkOn = false;
    EXPECT_PIXEL(renderer->renderToImage(size, {pane}, {}, 0xFF000000u), onText, rgb(dockSnapshot.defaultBg));
    pane.params.textBlinkOn = true;
    EXPECT_PIXEL(renderer->renderToImage(size, {pane}, {}, 0xFF000000u), onText, qRgb(255, 255, 255));
}

TEST_F(Render, PaneDockMovedByARowIsDrawnAtItsNewPlace)
{
    // The same dock snapshot, unchanged, at another place: its cached
    // instances are built again for the new rectangle.
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    AfterglowVt::Terminal dockTerminal(Cols, 1, host);
    dockTerminal.resize(Cols, 1, cw, ch);
    dockTerminal.feed("\x1b[?25l\x1b[41m\x1b[2K");
    PaneSnapshot dockSnapshot;
    dockSnapshot.capture(dockTerminal);
    const QRgb red = rgb(dockSnapshot.cells[0].bg);
    const QRgb background = rgb(snapshot.defaultBg);
    ASSERT_NE(red, background);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    const QRect bottom(0, (Rows - 1) * ch, size.width(), ch);
    pane.docks.push_back({&dockSnapshot, bottom});
    QImage image = renderer->renderToImage(size, {pane}, {}, 0xFF000000u);
    EXPECT_PIXEL(image, cellCenter(3, Rows - 1), red);
    EXPECT_PIXEL(image, cellCenter(3, Rows - 2), background);
    pane.docks.clear();
    pane.docks.push_back({&dockSnapshot, bottom.translated(0, -ch)});
    image = renderer->renderToImage(size, {pane}, {}, 0xFF000000u);
    EXPECT_PIXEL(image, cellCenter(3, Rows - 2), red);
    EXPECT_PIXEL(image, cellCenter(3, Rows - 1), background);
}

TEST_F(Render, PaneDocksShowInOtherTabsTheTabBeingLeftAndTheClosedPane)
{
    // A red dock on row 0 of a blue pane that is not in the current tab:
    // an overview card of another tab, the tab a switch leaves, and a
    // closed pane's last frame each show it in their image.
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    PaneSnapshot blue = snapshot;
    blue.defaultBg = 0xFFC00000u;
    AfterglowVt::Terminal dockTerminal(Cols, 1, host);
    dockTerminal.resize(Cols, 1, cw, ch);
    dockTerminal.feed("\x1b[?25l\x1b[41m\x1b[2K");
    PaneSnapshot dockSnapshot;
    dockSnapshot.capture(dockTerminal);
    const QRgb red = rgb(dockSnapshot.cells[0].bg);
    const QSize size(Cols * cw * 2, Rows * ch);
    WindowRenderer::Pane current{&snapshot, {}};
    current.params.rect = QRect(QPoint(), size);
    current.params.focused = true;
    WindowRenderer::Pane other{&blue, {}};
    other.params.rect = current.params.rect;
    other.docks.push_back({&dockSnapshot, QRect(0, 0, size.width(), ch)});
    const QPoint inDock(size.width() / 4, ch / 2), belowDock(size.width() / 4, ch * 5 / 2);

    {
        SCOPED_TRACE("overview card of another tab");
        auto program = panesShader("card", "return vec4(paneTexel(1, uv).rgb, 1.0);");
        ASSERT_TRUE(program);
        ShaderChains chains;
        chains.overview = {program};
        renderer->setShaderChains(chains);
        auto overview = overviewAt(true, 1.0f, 0.0f, Clock::now(), Clock::now());
        overview.otherTabs = {tabOf(1, {other})};
        renderer->setOverview(overview);
        const QImage image = renderer->renderToImage(size, {current}, {}, 0xff000000u);
        EXPECT_PIXEL(image, inDock, red);
        EXPECT_PIXEL(image, belowDock, qRgb(0, 0, 0xC0));
        renderer->setOverview({});
    }
    {
        SCOPED_TRACE("the tab being left");
        auto program = panesShader("leaving", "return vec4(previousTabTexel(uv).rgb, 1.0);");
        ASSERT_TRUE(program);
        ShaderChains chains;
        chains.tabTransition = {program};
        renderer->setShaderChains(chains);
        renderer->setTabTransition(switchFrom(0.5f, 1.0f, tabOf(1, {other})));
        const QImage image = renderer->renderToImage(size, {current}, {}, 0xff000000u);
        EXPECT_PIXEL(image, inDock, red);
        EXPECT_PIXEL(image, belowDock, qRgb(0, 0, 0xC0));
        renderer->setTabTransition({});
    }
    {
        SCOPED_TRACE("the closed pane");
        auto program = panesShader("closed", "return vec4(closingPaneActive() ? closingPaneTexel(uv).rgb : vec3(1.0), 1.0);");
        ASSERT_TRUE(program);
        ShaderChains chains;
        chains.paneEffects = {program};
        renderer->setShaderChains(chains);
        WindowRenderer::Pane ghost = other;
        ghost.params.rect = QRect(QPoint(), QSize(size.width() / 2, size.height()));
        ghost.docks.clear();
        ghost.docks.push_back({&dockSnapshot, QRect(0, 0, size.width() / 2, ch)});
        renderer->setPaneEffects({true, true, ghost, 0.25f});
        const QImage image = renderer->renderToImage(size, {current}, {}, 0xff000000u);
        EXPECT_PIXEL(image, inDock, red);
        EXPECT_PIXEL(image, belowDock, qRgb(0, 0, 0xC0));
        renderer->setPaneEffects({});
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
    // The right pane adds its own step every frame, and none of the left
    // pane's resets touch it: 13 a frame in its 8-bit buffer.
    ShaderParameterValues own;own.paramMask=1;own.params[0]=.05f;
    right.params.shaderParameters.insert(program->instanceId(),own);
    const QPoint inRight=cellCenter(2,2)+QPoint(size.width(),0);
    renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    auto image=renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    EXPECT_PIXEL(image,cellCenter(2,2),qRgb(51,0,0));
    EXPECT_PIXEL(image,inRight,qRgb(26,0,0));
    left.params.shaderParameters.clear();
    left.params.rect.setWidth(size.width()-cw); // Geometry reset.
    image=renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    EXPECT_PIXEL(image,cellCenter(2,2),qRgb(0,0,0));
    EXPECT_PIXEL(image,inRight,qRgb(39,0,0));
    left.params.shaderParameters.insert(program->instanceId(),values);
    renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    left.params.shaderParameters.clear(); PaneSnapshot newTab=snapshot;left.snapshot=&newTab;
    image=renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    EXPECT_PIXEL(image,cellCenter(2,2),qRgb(0,0,0));
    EXPECT_PIXEL(image,inRight,qRgb(65,0,0));
    left.params.shaderParameters.insert(program->instanceId(),values);
    renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    left.params.shaderParameters.clear();newTab.defaultBg=0xFF102030u;
    image=renderer->renderToImage(frame,{left,right},{},0xFF000000u);
    EXPECT_PIXEL(image,cellCenter(2,2),qRgb(0,0,0));
    EXPECT_PIXEL(image,inRight,qRgb(91,0,0));
}

TEST_F(Render, BufferFormatRetainsHdrAndNonfeedbackClears)
{
    for (bool half : {false,true}) {
        ShaderProgramSource src;src.id=QStringLiteral("buffer-format");
        ShaderBufferSource buffer; buffer.halfFloat=half;
        // Red goes past 1, which only a half-float buffer keeps. Green stays
        // below 1 in both: a buffer that kept its last frame would double it.
        buffer.code="#include <terminal_multipass.glsl>\n"
            "vec4 pTerminal(vec2 uv){vec4 t=texture(iChannel0,uv);return vec4(t.r+4.,t.g+.25,0,1);}";
        src.buffers={buffer};
        src.code="#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){vec4 t=texture(iChannel0,uv);return vec4(t.r/4.,t.g,0,1);}";
        QString error;auto program=ShaderProgram::compile(src,&error);ASSERT_TRUE(program);
        ShaderChains chains;chains.pane={program};renderer->setShaderChains(chains);
        auto image=render("\x1b[?25l");
        EXPECT_PIXEL(image,cellCenter(2,2),qRgb(half ? 255 : 64,64,0));
        image=render(""); // No feedback: input remains clear on every pass.
        EXPECT_PIXEL(image,cellCenter(2,2),qRgb(half ? 255 : 64,64,0));
    }
}

TEST_F(Render, FeedbackBufferGoesOnWhileAnHdrEventChainStartsAndEnds)
{
    // The HDR overview changes the window textures' format; the always-on
    // window chain's feedback buffer has its own and keeps accumulating.
    ShaderProgramSource src;
    src.id = QStringLiteral("feedback-under-hdr");
    ShaderBufferSource buffer; buffer.feedback = true;
    buffer.code = "#include <terminal_multipass.glsl>\n"
        "vec4 pTerminal(vec2 uv){vec4 c=texture(iChannel0,uv)+vec4(customParams[0].x,0,0,0);c.a=1.;return c;}";
    src.buffers = {buffer};
    src.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
    QString error;
    auto compiled = ShaderProgram::compile(src, &error);
    ASSERT_TRUE(compiled) << error.toStdString();
    auto program = compiled->instance(QStringLiteral("feedback-under-hdr"));
    ShaderChains chains;
    chains.window = {program};
    chains.overview = {layerShader("hdr-overview", "", true)};
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    ShaderParameterValues values; values.paramMask = 1; values.params[0] = .1f;
    pane.params.shaderParameters.insert(program->instanceId(), values);
    auto frameAt = [&](float seconds) {
        renderer->setClockOverride(seconds);
        return renderer->renderToImage(size, {pane}, {}, 0xFF000000u);
    };
    EXPECT_PIXEL(frameAt(1.f), cellCenter(2, 2), qRgb(26, 0, 0));
    EXPECT_PIXEL_NEAR(frameAt(2.f), cellCenter(2, 2), qRgb(51, 0, 0), 3);
    const auto now = Clock::now();
    renderer->setOverview(overviewAt(true, 1.0f, 0.0f, now, now));
    ASSERT_TRUE(renderer->overviewShowing());
    EXPECT_PIXEL_NEAR(frameAt(3.f), cellCenter(2, 2), qRgb(77, 0, 0), 4);
    renderer->setOverview(overviewAt(false, 0.0f, 0.0f, now, now));
    ASSERT_FALSE(renderer->overviewShowing());
    EXPECT_PIXEL_NEAR(frameAt(4.f), cellCenter(2, 2), qRgb(102, 0, 0), 5);
    renderer->setClockOverride(std::nullopt);
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
    // A grey clear: black is the shader's, not a pane that was not drawn.
    auto draw = [&] { return renderer->renderToImage(QSize(size.width()*2, size.height()), {left, right}, {}, 0xFF808080u); };
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

TEST_F(Render, IdleAudioEventChainBindsNoSpectrumBeforeItRuns)
{
    // A device of its own: the shared one may already hold a spectrum texture.
    GpuDevice device{FontSpec{}};
    device.rhi(nullptr);
    ASSERT_EQ(device.audioTexture(), nullptr);
    WindowRenderer fresh(device, nullptr);
    ShaderProgramSource source;
    source.id = QStringLiteral("audio-overview");
    source.code = R"(#include <terminal_audio.glsl>
vec4 pTerminal(vec2 uv) { return vec4(audioBar(0), 0.0, 1.0, 1.0); })";
    QString error;
    auto overview = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(overview) << error.toStdString();
    ASSERT_TRUE(overview->requiresAudio());
    ShaderChains chains;
    chains.window = {layerShader("red-window", "fragColor = vec4(1.0, 0.0, 0.0, 1.0);")};
    chains.overview = {overview};
    fresh.setShaderChains(chains);
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    // Closed: the window chain runs, and the audio slot binds the input
    // texture, because no spectrum texture exists yet.
    QImage image = fresh.renderToImage(size, {pane}, {}, 0xff000000u);
    EXPECT_PIXEL(image, cellCenter(1, 0), qRgb(255, 0, 0));
    EXPECT_EQ(device.audioTexture(), nullptr);
    // Open: the frame that runs the overview creates and fills the spectrum.
    std::array<float, GpuDevice::AudioBars> spectrum;
    spectrum.fill(1.0f);
    device.setAudioSpectrum(spectrum);
    fresh.setOverview(overviewAt(true, 1.0f, 0.0f, Clock::now()));
    image = fresh.renderToImage(size, {pane}, {}, 0xff000000u);
    EXPECT_NE(device.audioTexture(), nullptr);
    EXPECT_PIXEL(image, cellCenter(1, 0), qRgb(255, 0, 255));
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

TEST_F(Render, AgeOnlyShadersSettleThenWakeOnTheNextEvent)
{
    // Event ages are this program's only clock: the window renders every
    // frame while the youngest age it reads is below the settle time.
    ShaderChains chains;
    chains.window = {layerShader("cursor-age", "fragColor = vec4(c.rgb * clamp(cursorAge(), 0.0, 1.0), 1.0);")};
    ASSERT_FALSE(chains.window[0]->animated());
    EXPECT_EQ(chains.window[0]->eventAges(), unsigned(CursorAge));
    EXPECT_FLOAT_EQ(chains.window[0]->settleSeconds(), 2.0f);
    renderer->setShaderChains(chains);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    pane.params.focused = true;
    auto draw = [&](double time, const std::string& output) {
        terminal->feed(output);
        snapshot.capture(*terminal);
        renderer->setClockOverride(time);
        renderer->renderToImage(size, {pane}, {}, 0xFF000000u);
    };
    draw(0, ""); // a pane seen for the first time has no event yet
    EXPECT_FALSE(renderer->animating());
    draw(1, "\x1b[3;5H");
    EXPECT_TRUE(renderer->animating());
    draw(2.9, "");
    EXPECT_TRUE(renderer->animating());
    draw(3.1, "");
    EXPECT_FALSE(renderer->animating());
    draw(10, "\x1b[1;1H");
    EXPECT_TRUE(renderer->animating());
    draw(12.1, "");
    EXPECT_FALSE(renderer->animating());

    // Only the ages a program reads wake it, after its own settle time.
    ShaderProgramSource source;
    source.id = QStringLiteral("focus-age");
    source.code = "vec4 pTerminal(vec2 uv) { return terminalTexel(uv) * clamp(focusAge(), 0.0, 1.0); }\n";
    source.settleSeconds = 0.5f;
    QString error;
    auto focus = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(focus) << error.toStdString();
    EXPECT_EQ(focus->eventAges(), unsigned(FocusAge));
    chains.window = {focus};
    renderer->setShaderChains(chains);
    draw(20, "\x1b[2;2H");
    EXPECT_FALSE(renderer->animating());
    pane.params.focused = false;
    draw(21, "");
    EXPECT_TRUE(renderer->animating());
    draw(21.6, "");
    EXPECT_FALSE(renderer->animating());

    // A signal's age counts from when the script set it.
    chains.window = {layerShader("signal-age", "fragColor = c * clamp(signalAge(3), 0.0, 1.0);")};
    EXPECT_EQ(chains.window[0]->eventAges(), unsigned(SignalAge));
    renderer->setShaderChains(chains);
    pane.params.signals[3] = {1.0f, std::chrono::steady_clock::now() - std::chrono::seconds(5)};
    draw(30, "");
    EXPECT_FALSE(renderer->animating());
    pane.params.signals[3].at = std::chrono::steady_clock::now();
    draw(30.1, "");
    EXPECT_TRUE(renderer->animating());
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
    // Resize reallocates and clears the history texture: a history that
    // holds half a block shows none of it at the new size.
    terminal->feed("\x1b[H█");
    newTab.capture(*terminal);
    renderer->renderToImage(frame, {left, right}, {}, 0xFF000000u);
    terminal->feed("\x1b[2J");
    newTab.capture(*terminal);
    img = renderer->renderToImage(frame, {left, right}, {}, 0xFF000000u);
    EXPECT_PIXEL(img, cellCenter(0, 0), qRgb(128, 0, 0));
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
    // A green clear: the background is the grid's.
    const auto image = render("\x1b]11;#b03070\x07\x1b[?25l", true, 255, 0xFF00FF00u);
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
    source.id = QStringLiteral("multipass-depth");
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
    source.id = QStringLiteral("vertex-hook");
    source.vertexCode = "vec4 pVertex(vec2 uv){vec4 p=terminalQuadPosition(uv); p.x *= customParams[0].x; return p;}";
    source.params[0] = .5f;
    source.code = "vec4 pTerminal(vec2 uv){return vec4(1,0,0,1);}";
    QString error;
    const auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    const auto instance = program->instance(QStringLiteral("vertex-hook"));
    ShaderChains chains;
    chains.window = {instance};
    renderer->setShaderChains(chains);
    // A green clear, apart from the pane's background.
    const auto image = render("\x1b[?25l", true, 255, 0xFF00FF00u);
    EXPECT_PIXEL(image, cellCenter(10,2), qRgb(255,0,0));
    // Outside the shrunk quad: the window's clear colour.
    EXPECT_PIXEL(image, cellCenter(1,2), qRgb(0,255,0));
    // A live value of the same parameter reaches the vertex stage on the
    // next frame: the quad covers the window.
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    ShaderParameterValues values; values.paramMask = 1; values.params[0] = 1.0f;
    pane.params.shaderParameters.insert(instance->instanceId(), values);
    const auto full = renderer->renderToImage(size, {pane}, {}, 0xFF00FF00u);
    EXPECT_PIXEL(full, cellCenter(1,2), qRgb(255,0,0));
    EXPECT_PIXEL(full, cellCenter(10,2), qRgb(255,0,0));
}

TEST_F(Render, ImageOverridesAreLiveAndDefaultsReturnWithoutRecompile)
{
    ShaderProgramSource source;
    source.id = QStringLiteral("image-live");
    source.images[0] = QImage(2,2,QImage::Format_RGBA8888);
    source.images[0].fill(Qt::red);
    source.code = "vec4 readImage(sampler2D s,vec2 uv){return texture(s,uv);} vec4 pTerminal(vec2 uv){return readImage(uTexture0,vec2(.5));}";
    QString error;
    auto program = ShaderProgram::compile(source,&error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains;
    chains.pane = {program->instance(QStringLiteral("image"))};
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
    pane.params.shaderParameters.insert(QStringLiteral("image"), values);
    EXPECT_PIXEL(draw(),cellCenter(10,2),qRgb(0,0,255));
    pane.params.shaderParameters.clear();
    EXPECT_PIXEL(draw(),cellCenter(10,2),qRgb(255,0,0));
}

TEST_F(Render, BufferRepeatAndMirrorWorkThroughSamplerHelpers)
{
    for (const auto wrap : {ShaderWrap::Repeat, ShaderWrap::Mirror}) {
        ShaderProgramSource source;
        source.id = QStringLiteral("wrap-helper");
        ShaderBufferSource buffer;
        buffer.wrap = wrap;
        buffer.nearest = true;
        // The buffer holds its own x in red: at 1.25, Repeat reads 0.25,
        // Mirror 0.75, and ClampToEdge, the default, about 1.
        buffer.code = "vec4 pTerminal(vec2 uv){return vec4(uv.x,0,0,1);}";
        source.buffers = {buffer};
        source.code = "#include <terminal_multipass.glsl>\n"
            "vec4 readBuffer(sampler2D s,vec2 uv){return texture(s,uv);} vec4 pTerminal(vec2 uv){return readBuffer(iChannel0,vec2(1.25,.5));}";
        QString error;
        auto program = ShaderProgram::compile(source,&error);
        ASSERT_TRUE(program) << error.toStdString();
        ShaderChains chains; chains.pane = {program}; renderer->setShaderChains(chains);
        const auto image = render("\x1b[?25l");
        EXPECT_PIXEL(image,cellCenter(10,2),wrap==ShaderWrap::Repeat?qRgb(64,0,0):qRgb(191,0,0));
    }
}

TEST_F(Render, IntermediateWindowVertexPassClearsUncoveredPixelsEveryFrame)
{
    ShaderProgramSource source;
    source.id = QStringLiteral("vertex-intermediate");
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
    source.id = QStringLiteral("mesh-depth");
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
    ShaderChains chains; chains.pane = {program->instance(QStringLiteral("mesh"))};
    renderer->setShaderChains(chains);
    EXPECT_PIXEL(render("\x1b[?25l"), cellCenter(10,2), qRgb(0,255,0));
    const QSize size(Cols*cw,Rows*ch);
    WindowRenderer::Pane pane{&snapshot,{}};
    pane.params.rect = QRect(QPoint(), size);
    ShaderParameterValues values; values.paramMask = 1; values.params[0] = 1;
    pane.params.shaderParameters.insert(QStringLiteral("mesh"), values);
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
    source.id = QStringLiteral("shared-background");
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
        source.id = QStringLiteral("window-camera");

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
        ShaderChains chains; chains.pane = {program->instance(QStringLiteral("mesh"))};
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
    source.id = QStringLiteral("obj-depth");
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
    source.id = QStringLiteral("scene-objects");
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
    ShaderChains chains; chains.pane = {program->instance(QStringLiteral("scene"))};
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    const QSize size(Cols*cw, Rows*ch);
    WindowRenderer::Pane pane{&snapshot, {}};
    pane.params.rect = QRect(QPoint(), size);
    auto image = renderer->renderToImage(size, {pane}, {}, 0xff000000u);
    // Outside the mesh the buffer stays clear: transparent.
    EXPECT_PIXEL(image, QPoint(size.width()/4, size.height()/2), qRgb(255,0,0));
    EXPECT_PIXEL(image, QPoint(size.width()*3/4, size.height()/2), qRgba(0,0,0,0));
    // A live parameter moves the pivot, and the child with it, without recompiling.
    ShaderParameterValues values; values.paramMask = 1; values.params[0] = 1.0f;
    pane.params.shaderParameters.insert(QStringLiteral("scene"), values);
    image = renderer->renderToImage(size, {pane}, {}, 0xff000000u);
    EXPECT_PIXEL(image, QPoint(size.width()/4, size.height()/2), qRgba(0,0,0,0));
    EXPECT_PIXEL(image, QPoint(size.width()*3/4, size.height()/2), qRgb(255,0,0));
}

TEST_F(Render, BufferOverBudgetRendersSmallerInsteadOfTransparent)
{
    // A multisampled half-float mesh pass costs 80 bytes a pixel, so a
    // 2048x2048 pane asks for 320 MiB, more than the 256 MiB budget.
    if (!s_device->rhi()->supportedSampleCounts().contains(4)) GTEST_SKIP() << "no 4x multisampling";
    ShaderProgramSource source;
    source.id = QStringLiteral("over-budget");
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

TEST_F(Render, ABufferThatDidNotFitGrowsBackWhenTheBudgetHasRoom)
{
    // A half-float pass costs 16 bytes a pixel (two sides), so a 4096x4096
    // pane fills the 256 MiB budget. A second pane then gets what is left:
    // nothing (a transparent buffer), or a smaller buffer. Once the first
    // pane is gone, its buffer expires and the second grows to full size.
    ShaderProgramSource source;
    source.id = QStringLiteral("grow-back");
    ShaderBufferSource pass;
    pass.halfFloat = true;
    pass.code = "vec4 pTerminal(vec2 uv){return vec4(1,0,0,1);}";
    source.buffers = {pass};
    // Red from the buffer, green when the buffer is smaller than asked.
    source.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return vec4(texture(iChannel0,uv).r,"
                  "textureSize(iChannel0,0).x < int(iResolution.x) ? 1.0 : 0.0,0,1);}";
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    PaneSnapshot other = snapshot;
    const QRgb full = qRgb(255, 0, 0), smaller = qRgb(255, 255, 0), blocked = qRgb(0, 255, 0);
    // The first pane's width, and the second pane's side.
    const struct { int firstWidth; int secondSide; QRgb before; } cases[] = {
        {4096, 64, blocked},
        {4000, 1024, smaller},
    };
    for (const auto& each : cases) {
        SCOPED_TRACE(testing::Message() << "second pane " << each.secondSide);
        // A renderer of its own, with an empty budget.
        renderer = std::make_unique<WindowRenderer>(*s_device, nullptr);
        ShaderChains chains; chains.pane = {program};
        renderer->setShaderChains(chains);
        WindowRenderer::Pane first{&snapshot, {}}, second{&other, {}};
        first.params.rect = QRect(0, 0, each.firstWidth, 4096);
        second.params.rect = QRect(each.firstWidth, 0, each.secondSide, each.secondSide);
        second.params.contentOrigin = second.params.rect.topLeft();
        const QSize frame(each.firstWidth + each.secondSide, 4096);
        const QPoint inSecond = second.params.rect.center();
        auto image = renderer->renderToImage(frame, {first, second}, {}, 0xff000000u);
        EXPECT_PIXEL(image, QPoint(each.firstWidth / 2, 2048), full);
        EXPECT_PIXEL(image, inSecond, each.before);
        // The first pane's buffer goes once a frame has not used it.
        for (int frames = 0; frames < 2; ++frames) image = renderer->renderToImage(frame, {second}, {}, 0xff000000u);
        EXPECT_PIXEL(image, inSecond, full);
    }
}

TEST_F(Render, PaneBlockDescribesPanesAndTheOverviewRunsOnlyWhileShown)
{
    ShaderProgramSource source;
    source.id = QStringLiteral("overview");
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
    source.id = QStringLiteral("stripes");
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

TEST_F(Render, ClosingOverviewKeepsItsTargetInAnotherTabHighlighted)
{
    // The overview closes onto a card in tab 1 while tab 0, and its focus,
    // stay current until the close ends.
    ShaderProgramSource source;
    source.id = QStringLiteral("close-target");
    source.code = R"(#include <terminal_panes.glsl>
vec4 pTerminal(vec2 uv) {
    int lit = highlightedPane();
    float sharp = lit >= 0 ? float(uPaneTab[lit].y) / 4.0 : 1.0;
    return vec4(float(lit + 1) / 4.0, float(focusedPane() + 1) / 4.0, overviewOpen() ? 1.0 : sharp, 1.0);
})";
    QString error;
    auto program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ShaderChains chains; chains.overview = {program};
    renderer->setShaderChains(chains);
    terminal->feed("\x1b[?25l"); snapshot.capture(*terminal);
    PaneSnapshot other = snapshot;
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane current{&snapshot, {}};
    current.params.rect = QRect(QPoint(), size);
    current.params.focused = true;
    WindowRenderer::Pane target{&other, {}};
    target.params.rect = current.params.rect;
    target.params.highlighted = true;
    auto overview = overviewAt(false, 0.5f, 1.0f, Clock::now(), Clock::now());
    overview.otherTabs = {tabOf(1, {target})};
    renderer->setOverview(overview);
    ASSERT_TRUE(renderer->overviewShowing());
    const auto image = renderer->renderToImage(size, {current}, {}, 0xff000000u);
    // Highlighted card 1, focused card 0, closing, and the target's tab at full resolution.
    EXPECT_PIXEL(image, QPoint(size.width() / 2, size.height() / 2), qRgb(128, 64, 64));
}

TEST_F(Render, TabTransitionSeesTheTabBeingLeftWhileItRuns)
{
    // Left half: the tab being left; right half: the transition's state.
    ShaderProgramSource source;
    source.id = QStringLiteral("switch");
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
    // The overview takes precedence over a switch: the frame shows no switch.
    renderer->setOverview(overviewAt(true, 1.0f, 0.0f, Clock::now()));
    EXPECT_FALSE(renderer->transitionShowing());
    image = renderer->renderToImage(size, {current}, {}, 0xff000000u);
    EXPECT_PIXEL(image, inLeft, rgb(snapshot.defaultBg));
    EXPECT_PIXEL(image, inRight, rgb(snapshot.defaultBg));
    renderer->setOverview({});
    renderer->setTabTransition({});
    image = renderer->renderToImage(size, {current}, {}, 0xff000000u);
    EXPECT_PIXEL(image, inRight, rgb(snapshot.defaultBg));
}

TEST_F(Render, PaneEffectsSeeOpeningPanesAndTheClosedPanesLastFrame)
{
    // Left half: the closed pane's frame; right half: card 0's opening state.
    ShaderProgramSource source;
    source.id = QStringLiteral("life");
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
    // A tab switch takes precedence: the chain still runs first, under the
    // switch, but the closed pane's frame is not drawn for it. An inactive
    // chain does not run.
    renderer->setTabTransition(switchFrom(0.5f, 1.0f, tabOf(1, {open})));
    EXPECT_FALSE(renderer->paneEffectsShowing());
    image = renderer->renderToImage(size, {open}, {}, 0xff000000u);
    EXPECT_PIXEL(image, inLeft, qRgb(255, 255, 255));
    renderer->setTabTransition({});
    renderer->setPaneEffects({});
    open.params.openProgress = 1.0f;
    image = renderer->renderToImage(size, {open}, {}, 0xff000000u);
    EXPECT_PIXEL(image, inRight, rgb(snapshot.defaultBg));
}

TEST_F(Render, PaneContextAndPreviousContextReachPaneEffects)
{
    ShaderProgramSource source;
    source.id = QStringLiteral("context");
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
    // A comment that mentions the clock is not a read of it.
    EXPECT_FALSE(compileBody("// iTime\n/* timeAngle(1.0) */ return vec4(1);")->animated());
    // Event ages are not a clock: they settle (PostChain::animated()).
    auto ages = compileBody("return vec4(cursorAge() + focusAge() + signalAge(0));");
    EXPECT_FALSE(ages->animated());
    EXPECT_EQ(ages->eventAges(), unsigned(CursorAge | FocusAge | SignalAge));
    EXPECT_EQ(compileBody("return vec4(signalValue(0));")->eventAges(), 0u);
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
    source.id=QStringLiteral("mesh-antialiasing");
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

TEST_F(Render, AFrameThatFailsSubmitsTheBatchItStaged)
{
    // The uploads of a staged batch are counted as done when they are
    // staged: a frame that throws before its pass submits them anyway.
    auto* rhi = s_device->rhi();
    std::unique_ptr<QRhiTexture> texture(rhi->newTexture(QRhiTexture::RGBA8, QSize(4, 4), 1, QRhiTexture::UsedAsTransferSource));
    ASSERT_TRUE(texture->create());
    auto filled = [](QColor color) {
        QImage image(4, 4, QImage::Format_RGBA8888);
        image.fill(color);
        return image;
    };
    auto readBack = [&] {
        QRhiCommandBuffer* cb = nullptr;
        EXPECT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
        QRhiReadbackResult result;
        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        updates->readBackTexture({texture.get()}, &result);
        cb->resourceUpdate(updates);
        EXPECT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
        return result.data.size() >= 4 ? qRgb(quint8(result.data[0]), quint8(result.data[1]), quint8(result.data[2])) : QRgb(0);
    };
    QRhiCommandBuffer* cb = nullptr;
    ASSERT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
    QRhiResourceUpdateBatch* red = rhi->nextResourceUpdateBatch();
    red->uploadTexture(texture.get(), filled(Qt::red));
    cb->resourceUpdate(red);
    ASSERT_EQ(rhi->endOffscreenFrame(), QRhi::FrameOpSuccess);
    ASSERT_EQ(readBack(), qRgb(255, 0, 0));
    ASSERT_EQ(rhi->beginOffscreenFrame(&cb), QRhi::FrameOpSuccess);
    EXPECT_THROW({
        RenderFrame frame(cb, [&] { rhi->endOffscreenFrame(); });
        QRhiResourceUpdateBatch* green = rhi->nextResourceUpdateBatch();
        green->uploadTexture(texture.get(), filled(Qt::green));
        // A second batch merges into the first, after it.
        QRhiResourceUpdateBatch* blue = rhi->nextResourceUpdateBatch();
        blue->uploadTexture(texture.get(), filled(Qt::blue));
        EXPECT_EQ(frame.stage(green), green);
        EXPECT_EQ(frame.stage(blue), green);
        throw std::runtime_error("injected render failure");
    }, std::runtime_error);
    EXPECT_FALSE(rhi->isRecordingFrame());
    EXPECT_EQ(readBack(), qRgb(0, 0, 255));
}

TEST_F(Render, ADateOverrideReplacesTheWallClocksDate)
{
    ShaderChains chains;
    chains.pane = {layerShader("date", "fragColor = vec4(iDate.y / 12.0, iDate.z / 31.0, iDate.w / 86400.0, 1.0);")};
    renderer->setShaderChains(chains);
    // June 31 does not exist: the fields go to the shader as given.
    renderer->setDateOverride(std::array<float, 4>{2026.0f, 6.0f, 31.0f, 21600.0f});
    EXPECT_PIXEL(render("\x1b[?25l"), cellCenter(2, 2), qRgb(128, 255, 64));
    // Unset: the wall clock's month again.
    renderer->setDateOverride(std::nullopt);
    const QImage image = render("");
    const int month = QDate::currentDate().month();
    EXPECT_NEAR(qRed(image.pixel(cellCenter(2, 2))), month / 12.0 * 255, 3);
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
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    const QPoint corners[] = {QPoint(0, 0), QPoint(size.width() - 1, 0), QPoint(0, size.height() - 1),
                              QPoint(size.width() - 1, size.height() - 1), QPoint(size.width() / 2, size.height() / 2)};
    // A spring overshoot: the pane's red covers the whole surface.
    pane.params.rect = QRect(-cw, -ch, size.width() + 2*cw, size.height() + 2*ch);
    auto image = renderer->renderToImage(size, {pane}, {}, 0xff00ff00u);
    ASSERT_FALSE(image.isNull());
    for (const QPoint& at : corners) EXPECT_PIXEL(image, at, qRgb(255, 0, 0));
    // A pane that has moved entirely offscreen: only the clear colour.
    pane.params.rect = QRect(size.width() + cw, 0, cw, ch);
    image = renderer->renderToImage(size, {pane}, {}, 0xff00ff00u);
    ASSERT_FALSE(image.isNull());
    for (const QPoint& at : corners) EXPECT_PIXEL(image, at, qRgb(0, 255, 0));
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

TEST(Snapshot, MovesKeepTheIdentityAndLeaveANewOne)
{
    // A snapshot handed on by move (an overlay's last frame) keeps the
    // renderer's caches and buffers that go with it.
    PaneSnapshot a;
    const auto value = a.identity.value();
    PaneSnapshot b = std::move(a);
    EXPECT_EQ(b.identity.value(), value);
    EXPECT_NE(a.identity.value(), value);
    PaneSnapshot c;
    const auto replaced = c.identity.value();
    c = std::move(b);
    EXPECT_EQ(c.identity.value(), value);
    EXPECT_NE(b.identity.value(), value);
    EXPECT_NE(b.identity.value(), replaced);
    EXPECT_NE(b.identity.value(), a.identity.value());
}

TEST(SnapshotImages, IndexedFramesKeepTheirColoursUntilTheColoursChange)
{
    NullHost host;
    AfterglowVt::Terminal terminal(10, 4, host);
    terminal.resize(10, 4, 8, 16);
    terminal.feed("\x1b_Ga=T,f=8,s=2,v=1,i=1,q=2;" + AfterglowProto::base64Encode(std::string("\x01\x02", 2)) + "\x1b\\");
    PaneSnapshot snap;
    snap.capture(terminal);
    ASSERT_EQ(snap.images.size(), 1u);
    const AfterglowVt::FramePtr first = snap.images[0].frame;
    ASSERT_FALSE(first->indexed());
    ASSERT_EQ(first->rgba.size(), 8u);
    EXPECT_EQ(first->rgba[0], terminal.paletteColor(1).r);
    EXPECT_EQ(first->rgba[3], 255);
    // Text changes, the colours do not: the same RGBA frame, so the same texture.
    terminal.feed("hello");
    snap.capture(terminal);
    EXPECT_EQ(snap.images[0].frame, first);
    terminal.feed("\x1b]4;2;rgb:01/02/03\x1b\\");
    snap.capture(terminal);
    ASSERT_NE(snap.images[0].frame, first);
    EXPECT_EQ(snap.images[0].frame->rgba[4], 1);
    EXPECT_EQ(snap.images[0].frame->rgba[6], 3);
    // A second image: when one of the two goes, only its colours go, and
    // the other keeps its RGBA frame.
    terminal.feed("\x1b[3;1H\x1b_Ga=T,f=8,s=1,v=1,i=2,q=2;" + AfterglowProto::base64Encode(std::string("\x03", 1))
                  + "\x1b\\");
    snap.capture(terminal);
    ASSERT_EQ(snap.images.size(), 2u);
    EXPECT_EQ(snap.coloredFrames.size(), 2u);
    const auto second = std::find_if(snap.images.begin(), snap.images.end(),
                                     [](const AfterglowVt::VisiblePlacement& image) { return image.imageId == 2; });
    ASSERT_NE(second, snap.images.end());
    const AfterglowVt::FramePtr kept = second->frame;
    terminal.feed("\x1b_Ga=d,d=I,i=1\x1b\\");
    snap.capture(terminal);
    ASSERT_EQ(snap.images.size(), 1u);
    EXPECT_EQ(snap.images[0].frame, kept);
    EXPECT_EQ(snap.coloredFrames.size(), 1u);
    // When nothing shows the image, its colours go.
    terminal.feed("\x1b_Ga=d,d=A\x1b\\");
    snap.capture(terminal);
    EXPECT_TRUE(snap.coloredFrames.empty());
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

TEST(GlyphCache, LegacyComputingSpritesAreTheFamiliesThatFillCells)
{
    // Every character that the sprites take is drawn, and every other one
    // in the two blocks goes to the font: a sprite has no font fallback.
    auto drawn = [](char32_t cp) {
        return (cp >= 0x1FB00 && cp <= 0x1FB92) || cp == 0x1FB94 || (cp >= 0x1FB9A && cp <= 0x1FBAE)
            || cp == 0x1FBCE || cp == 0x1FBCF || (cp >= 0x1FBD0 && cp <= 0x1FBDF) || (cp >= 0x1FBE4 && cp <= 0x1FBE7)
            || (cp >= 0x1CC21 && cp <= 0x1CC2F) || (cp >= 0x1CD00 && cp <= 0x1CDE5) || (cp >= 0x1CE51 && cp <= 0x1CEAF);
    };
    FontSet fonts(FontSpec{}, 1.0);
    GlyphCache cache;
    cache.beginFrame(1000, 8192);
    std::vector<char32_t> blocks;
    for (char32_t cp = 0x1FB00; cp <= 0x1FBFF; ++cp) blocks.push_back(cp);
    for (char32_t cp = 0x1CC00; cp <= 0x1CEBF; ++cp) blocks.push_back(cp);
    for (char32_t cp : blocks) {
        const GlyphEntry* e = cache.sprite(fonts, cp);
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(!e->empty, drawn(cp)) << std::hex << std::uint32_t(cp);
        if (e->empty) continue;
        const QImage& page = cache.grayPage(e->page).image();
        int coverage = 0;
        for (int y = e->v; y < e->v + e->height; ++y) {
            for (int x = e->u; x < e->u + e->width; ++x) coverage += qAlpha(page.pixel(x, y)) > 0;
        }
        EXPECT_GT(coverage, 0) << std::hex << std::uint32_t(cp);
    }
}

TEST(GlyphCache, OverBudgetOnlyAPageUnusedForStaleAfterDiscardsTheCache)
{
    FontSet fonts(FontSpec{}, 1.0);
    GlyphCache cache;
    // The renders' clock is the test's, so a slow setup does not age the pages.
    const auto start = std::chrono::steady_clock::now();
    // Five full gray pages: 80 MiB, over the 64 MiB budget.
    int x = 0, y = 0, width = 49;
    while (cache.grayPageCount() < 5 && width > 0) {
        while (cache.grayPage(cache.grayPageCount() - 1).allocate(128, 128, x, y)) {}
        cache.beginFrame(std::numeric_limits<std::size_t>::max() / 2, 8192, start);
        cache.sprite(fonts, U'\u2588', width--, 7); // a new size: a new entry, on a new page
    }
    ASSERT_GT(cache.bytes(), 64u * 1024 * 1024);
    const auto epoch = cache.epoch();
    // Other windows render many times in a fraction of a second: no page of
    // this window is stale yet.
    for (int i = 0; i < 10; ++i) cache.beginFrame(1000, 8192, start + GlyphCache::StaleAfter / 2);
    EXPECT_EQ(cache.epoch(), epoch);
    // No render used the older pages for longer than StaleAfter: start over.
    cache.beginFrame(1000, 8192, start + GlyphCache::StaleAfter * 2);
    EXPECT_NE(cache.epoch(), epoch);
    EXPECT_EQ(cache.grayPageCount(), 1u);
}

TEST(GlyphCache, DirtyRegionsMergePerShelfAndAWholePageReplacesThem)
{
    AtlasPage page(QImage::Format_Grayscale8, 256, 256);
    int x = 0, y = 0;
    // Two glyphs of different heights on one shelf: one region.
    ASSERT_TRUE(page.allocate(10, 12, x, y));
    page.markDirty(QRect(x, y, 10, 12));
    ASSERT_TRUE(page.allocate(8, 10, x, y));
    ASSERT_EQ(y, 0);
    page.markDirty(QRect(x, y, 8, 10));
    // A taller glyph opens a shelf of its own: a second region.
    ASSERT_TRUE(page.allocate(10, 40, x, y));
    ASSERT_GT(y, 0);
    page.markDirty(QRect(x, y, 10, 40));
    auto dirty = page.takeDirty();
    ASSERT_EQ(dirty.size(), 2u);
    EXPECT_EQ(dirty[0], QRect(0, 0, 19, 12));
    EXPECT_EQ(dirty[1], QRect(0, 13, 10, 40));
    // A region that covers the others (a texture made again) replaces them.
    page.markDirty(QRect(0, 0, 4, 4));
    page.markDirty(QRect(0, 20, 4, 4));
    page.markDirty(page.image().rect());
    dirty = page.takeDirty();
    ASSERT_EQ(dirty.size(), 1u);
    EXPECT_EQ(dirty[0], page.image().rect());
    // Past the limit a region joins its nearest one, not a box around all.
    for (int row = 0; row <= 32; ++row) page.markDirty(QRect(0, row * 4, 2, 2));
    dirty = page.takeDirty();
    ASSERT_EQ(dirty.size(), 32u);
    for (const QRect& rect : dirty) EXPECT_LE(rect.height(), 6);
}

TEST(GlyphCache, DashedLinesKeepEveryDashOrDrawSolid)
{
    // Four dashes and their gaps need 8 pixels: narrower cells draw solid.
    // The sizes give both.
    bool solid = false, dashed = false;
    for (float points : {4.0f, 6.0f, 8.0f, 11.0f, 14.0f}) {
        FontSpec spec;
        spec.sizePoints = points;
        FontSet fonts(spec, 1.0);
        const int expected = fonts.metrics().cellWidth >= 8 ? 4 : 1;
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
        EXPECT_EQ(runs, expected) << points << " pt, " << fonts.metrics().cellWidth << " pixels wide";
        (expected == 4 ? dashed : solid) = true;
    }
    EXPECT_TRUE(solid && dashed);
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

TEST_F(Render, FramesWithoutWholeRgbaAreNotUploaded)
{
    // An indexed frame the snapshot did not colour, and an RGBA frame shorter
    // than its size: neither is read past its data.
    auto indexed = std::make_shared<AfterglowVt::ImageFrame>();
    indexed->width = 2;
    indexed->height = 2;
    indexed->format = AfterglowVt::PixelFormat::Indexed;
    indexed->indices.assign(4, 1);
    auto shortFrame = std::make_shared<AfterglowVt::ImageFrame>();
    shortFrame->width = 2;
    shortFrame->height = 2;
    shortFrame->rgba.assign(4, 0xFF);
    // An indexed frame is refused for its format, even with RGBA of the
    // right length, and an empty frame for its size.
    auto indexedWithRgba = std::make_shared<AfterglowVt::ImageFrame>(*indexed);
    indexedWithRgba->rgba.assign(16, 0xFF);
    auto empty = std::make_shared<AfterglowVt::ImageFrame>();
    QRhiResourceUpdateBatch* updates = s_device->rhi()->nextResourceUpdateBatch();
    std::uint64_t serial = 0;
    EXPECT_EQ(s_device->imageTexture(indexed, updates, serial), nullptr);
    EXPECT_EQ(s_device->imageTexture(shortFrame, updates, serial), nullptr);
    EXPECT_EQ(s_device->imageTexture(indexedWithRgba, updates, serial), nullptr);
    EXPECT_EQ(s_device->imageTexture(empty, updates, serial), nullptr);
    updates->release();
}

TEST_F(Render, KittyImagesFarBelowGoUnderCellBackgrounds)
{
    // A red pixel over two cells at z below INT32_MIN/2: the first cell has
    // its own green background, which covers the image; the second does not.
    const QImage img = render("\x1b[?25l\x1b[2;2H\x1b[42m \x1b[0m\x1b[2;2H"
                              "\x1b_Ga=T,f=32,s=1,v=1,c=2,r=1,z=-1073741825,q=2;/wAA/w==\x1b\\");
    EXPECT_PIXEL(img, cellCenter(1, 1), rgb(packRgba(AfterglowVt::TerminalOptions{}.basePalette[2])));
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
    source.id = QStringLiteral("cards");
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

namespace {

/// The terminal of an overlay's program: of its own, the size of the pane.
struct OverlayTerminal
{
    NullHost host;
    AfterglowVt::Terminal terminal;
    PaneSnapshot snapshot;

    OverlayTerminal(int cols, int rows, int cw, int ch, const std::string& bytes)
        : terminal(cols, rows, host)
    {
        terminal.resize(cols, rows, cw, ch);
        terminal.feed(bytes);
        snapshot.capture(terminal);
    }
};

/// The overlay transition pack, as it ships.
ShaderProgramPtr overlayFade()
{
    QFile file(QStringLiteral(AFTERGLOW_OVERLAY_FADE_PACK));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    ShaderProgramSource source;
    source.id = QStringLiteral("overlay-fade");
    source.code = file.readAll();
    QString error;
    ShaderProgramPtr program = ShaderProgram::compile(source, &error);
    EXPECT_TRUE(program) << error.toStdString();
    return program;
}

QRgb halfway(QRgb a, QRgb b)
{
    return qRgb((qRed(a) + qRed(b)) / 2, (qGreen(a) + qGreen(b)) / 2, (qBlue(a) + qBlue(b)) / 2);
}

} // namespace

TEST_F(Render, AnOverlayTransitionCrossFadesThePaneAndItsOverlayThroughThePaneAndLayerChains)
{
    // The pane: a red cell. The overlay: a green cell elsewhere, on a blue background.
    render("\x1b[?25l\x1b[48;2;220;0;0m \x1b[0m");
    OverlayTerminal overlay(Cols, Rows, cw, ch, "\x1b[?25l\x1b]11;rgb:00/00/c0\x1b\\\x1b[2;3H\x1b[48;2;0;200;0m \x1b[0m");
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(QPoint(0, 0), size);
    pane.params.focused = true;
    const ShaderProgramPtr fade = overlayFade();
    ASSERT_TRUE(fade);
    // A shader that swaps red and green: both images go through it.
    const ShaderProgramPtr swap = layerShader("swap", "fragColor = c.grba;");
    ASSERT_TRUE(swap);
    // As a pane shader, and as a background shader: then the pane under the
    // overlay takes the layered path, with its own composite.
    for (const bool layered : {false, true}) {
        SCOPED_TRACE(layered ? "background chain" : "pane chain");
        ShaderChains chains;
        (layered ? chains.background : chains.pane) = {swap};
        chains.overlayTransition = {fade};
        renderer->setShaderChains(chains);
        auto frame = [&](std::optional<float> progress, bool overlayAlone = false) {
            WindowRenderer::Pane drawn = pane;
            if (overlayAlone) drawn.snapshot = &overlay.snapshot;
            else if (progress) drawn.overlay = overlayOf(&overlay.snapshot, *progress);
            return renderer->renderToImage(size, {drawn}, {}, snapshot.defaultBg);
        };
        const QImage plain = frame(std::nullopt);
        const QImage alone = frame(std::nullopt, true);
        const QRgb raw = rgb(snapshot.defaultBg);
        const QRgb host = qRgb(0, 220, 0), cell = qRgb(200, 0, 0), blue = qRgb(0, 0, 0xc0),
                   background = qRgb(qGreen(raw), qRed(raw), qBlue(raw));
        EXPECT_PIXEL(plain, cellCenter(0, 0), host);
        EXPECT_PIXEL(alone, cellCenter(2, 1), cell);
        // At 0 the pane alone, at 1 the overlay alone, each with its shaders.
        EXPECT_TRUE(frame(0.0f) == plain);
        EXPECT_TRUE(frame(1.0f) == alone);
        // Between, both at once, in proportion.
        const QImage middle = frame(0.5f);
        EXPECT_PIXEL(middle, cellCenter(0, 0), halfway(host, blue));
        EXPECT_PIXEL(middle, cellCenter(2, 1), halfway(background, cell));
        const QImage quarter = frame(0.25f);
        EXPECT_PIXEL_NEAR(quarter, cellCenter(0, 0), qRgb(0, 165, 48), 8);
        // Once it has ended, nothing of it runs: the frame is the pane's again.
        EXPECT_TRUE(frame(std::nullopt) == plain);
    }
}

TEST_F(Render, AnOverlayFadingInOnePaneLeavesTheOtherPaneAsItWas)
{
    render("\x1b[?25l\x1b[48;2;220;0;0m \x1b[0m");
    PaneSnapshot other = snapshot;
    OverlayTerminal overlay(Cols, Rows, cw, ch, "\x1b[?25l\x1b]11;rgb:00/00/c0\x1b\\");
    const QSize paneSize(Cols * cw, Rows * ch), size(paneSize.width() * 2, paneSize.height());
    WindowRenderer::Pane left;
    left.snapshot = &snapshot;
    left.params.rect = QRect(QPoint(0, 0), paneSize);
    left.params.focused = true;
    WindowRenderer::Pane right;
    right.snapshot = &other;
    right.params.rect = QRect(QPoint(paneSize.width(), 0), paneSize);
    right.params.contentOrigin = right.params.rect.topLeft();
    const ShaderProgramPtr fade = overlayFade();
    ASSERT_TRUE(fade);
    const ShaderProgramPtr swap = layerShader("swap", "fragColor = c.grba;");
    ASSERT_TRUE(swap);
    for (const bool layered : {false, true}) {
        SCOPED_TRACE(layered ? "background chain" : "pane chain");
        ShaderChains chains;
        (layered ? chains.background : chains.pane) = {swap};
        chains.overlayTransition = {fade};
        renderer->setShaderChains(chains);
        const QImage plain = renderer->renderToImage(size, {left, right}, {}, snapshot.defaultBg);
        EXPECT_PIXEL(plain, cellCenter(0, 0), qRgb(0, 220, 0));
        for (const float progress : {0.25f, 0.5f, 0.75f}) {
            SCOPED_TRACE(testing::Message() << "progress " << progress);
            WindowRenderer::Pane fading = left;
            fading.overlay = overlayOf(&overlay.snapshot, progress);
            const QImage image = renderer->renderToImage(size, {fading, right}, {}, snapshot.defaultBg);
            EXPECT_TRUE(image.copy(right.params.rect) == plain.copy(right.params.rect));
            // The swapped red of the pane, to the overlay's blue.
            EXPECT_PIXEL_NEAR(image, cellCenter(0, 0), qRgb(0, qRound(220 * (1 - progress)), qRound(0xc0 * progress)), 8);
        }
    }
}

TEST_F(Render, AnOverlaySwitchesAtHalfwayWhileTheTabImageIsTaken)
{
    // The overview with another tab, a tab switch and a closing pane draw into
    // the tab image, which the overlay transition needs too: the overlay
    // switches at once, the pane until halfway and the overlay after.
    render("\x1b[?25l\x1b[48;2;220;0;0m \x1b[0m");
    PaneSnapshot away = snapshot;
    away.defaultBg = 0xFF00C000u;
    OverlayTerminal overlay(Cols, Rows, cw, ch, "\x1b[?25l\x1b]11;rgb:00/00/c0\x1b\\");
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(QPoint(0, 0), size);
    pane.params.focused = true;
    WindowRenderer::Pane other = pane;
    other.snapshot = &away;
    other.params.focused = false;
    const ShaderProgramPtr fade = overlayFade();
    ASSERT_TRUE(fade);
    const ShaderProgramPtr pass = layerShader("pass", "");
    ASSERT_TRUE(pass);
    // Pane effects show the closed pane's last frame on the right half.
    const ShaderProgramPtr ghost = panesShader("ghost", "return uv.x > 0.5 ? texture(uTabImage, uv) : terminalTexel(uv);");
    ASSERT_TRUE(ghost);
    ShaderChains chains;
    chains.overlayTransition = {fade};
    chains.overview = {pass};
    chains.tabTransition = {pass};
    chains.paneEffects = {ghost};
    renderer->setShaderChains(chains);
    for (int event = 0; event < 3; ++event) {
        SCOPED_TRACE(event == 0 ? "overview" : event == 1 ? "tab switch" : "closing pane");
        if (event == 0) {
            auto overview = overviewAt(true, 1.0f, 0.0f, Clock::now(), Clock::now());
            overview.otherTabs = {tabOf(1, {other})};
            renderer->setOverview(overview);
        } else if (event == 1) {
            renderer->setTabTransition(switchFrom(0.5f, 1.0f, tabOf(1, {other})));
        } else {
            // The closed pane had an overlay too: its last frame is the overlay's.
            WindowRenderer::Pane closed = other;
            closed.overlay = overlayOf(&overlay.snapshot, 0.75f);
            renderer->setPaneEffects({true, true, closed, 0.5f});
        }
        for (const float progress : {0.25f, 0.75f}) {
            SCOPED_TRACE(testing::Message() << "progress " << progress);
            WindowRenderer::Pane drawn = pane;
            drawn.overlay = overlayOf(&overlay.snapshot, progress);
            const QImage image = renderer->renderToImage(size, {drawn}, {}, snapshot.defaultBg);
            EXPECT_PIXEL(image, cellCenter(0, 0), progress < 0.5f ? qRgb(220, 0, 0) : qRgb(0, 0, 0xc0));
            if (event == 2) EXPECT_PIXEL(image, cellCenter(Cols - 3, 2), qRgb(0, 0, 0xc0));
        }
        renderer->setOverview({});
        renderer->setTabTransition({});
        renderer->setPaneEffects({});
    }
}

TEST_F(Render, TheTabImageOfAnOverlayFadeGoesAfterTheFade)
{
    // After the fade the tab image holds nothing: a pane effect that reads it
    // then gets the window, not the pane that was under the overlay.
    render("\x1b[?25l\x1b[48;2;220;0;0m \x1b[0m");
    OverlayTerminal overlay(Cols, Rows, cw, ch, "\x1b[?25l\x1b]11;rgb:00/00/c0\x1b\\");
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(QPoint(0, 0), size);
    pane.params.focused = true;
    const ShaderProgramPtr fade = overlayFade();
    ASSERT_TRUE(fade);
    ShaderChains chains;
    // Always on, so the chains stay prepared between the frames.
    chains.pane = {layerShader("plain", "")};
    chains.overlayTransition = {fade};
    // Green marks that the pane effects ran: the overlay alone is blue too.
    chains.paneEffects = {panesShader("tab-image", "vec4 t = texture(uTabImage, uv); return vec4(t.r, 1.0, t.b, 1.0);")};
    renderer->setShaderChains(chains);
    WindowRenderer::Pane drawn = pane;
    drawn.overlay = overlayOf(&overlay.snapshot, 0.5f);
    renderer->renderToImage(size, {drawn}, {}, snapshot.defaultBg);
    renderer->renderToImage(size, {pane}, {}, snapshot.defaultBg);
    // The overlay alone, as a pane: what the window shows now.
    drawn = pane;
    drawn.snapshot = &overlay.snapshot;
    renderer->setPaneEffects({true, true, std::nullopt, 1.0f});
    EXPECT_PIXEL(renderer->renderToImage(size, {drawn}, {}, snapshot.defaultBg), cellCenter(0, 0), qRgb(0, 255, 0xc0));
}

TEST_F(Render, AWindowBackgroundBufferStepsOncePerFrameDuringAnOverlayFade)
{
    // The window's one background buffer set: the pane under the overlay and
    // the overlay draw through it in the same frame, and it steps once.
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    const PaneSnapshot copy = snapshot; // the same colours, an identity of its own
    ShaderProgramSource source;
    source.id = QStringLiteral("stepping-background");
    source.windowBackground = true;
    source.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
    ShaderBufferSource pass;
    pass.feedback = true;
    pass.scale = .5f;
    pass.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return vec4(texture(iChannel0,uv).r+0.05,0,0,1);}";
    source.buffers = {pass};
    QString error;
    const ShaderProgramPtr program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    ASSERT_TRUE(program->windowBackground());
    const ShaderProgramPtr fade = overlayFade();
    ASSERT_TRUE(fade);
    ShaderChains chains;
    chains.background = {program};
    chains.overlayTransition = {fade};
    renderer->setShaderChains(chains);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(QPoint(0, 0), size);
    pane.params.focused = true;
    auto draw = [&](std::optional<float> progress, int frame) {
        renderer->setClockOverride(float(frame + 1));
        WindowRenderer::Pane drawn = pane;
        if (progress) drawn.overlay = overlayOf(&copy, *progress);
        return renderer->renderToImage(size, {drawn}, {}, 0xff000000u);
    };
    const auto steps = [](double count) { return qRgb(qRound(count * 0.05 * 255), 0, 0); };
    EXPECT_PIXEL_NEAR(draw(std::nullopt, 0), cellCenter(2, 2), steps(1), 3);
    for (int frame = 1; frame <= 3; ++frame) {
        // The pane under the overlay shows the last frame's step, the overlay this one's.
        EXPECT_PIXEL_NEAR(draw(0.5f, frame), cellCenter(2, 2), steps(frame + 0.5), 3);
    }
    EXPECT_PIXEL_NEAR(draw(std::nullopt, 4), cellCenter(2, 2), steps(5), 3);
    // After a reload the set does not exist: the pane under the overlay makes
    // it and steps it once, and the overlay takes that step.
    renderer->setShaderChains(chains);
    EXPECT_PIXEL_NEAR(draw(0.5f, 5), cellCenter(2, 2), steps(1), 3);
}

namespace {

/// A shader that keeps what the text history holds, as a trail pack does.
/// In the text chain it keeps every trail, in the pane chain the last frame's.
ShaderProgramPtr keepHistory()
{
    return layerShader("keep", "fragColor = max(c, texture(uPreviousText, uv));");
}

} // namespace

TEST_F(Render, ThePaneUnderAFadingOverlayKeepsItsOwnTextHistory)
{
    // The pane under the overlay keeps its own trails through the fade, and
    // the overlay's text never comes out of it.
    const ShaderProgramPtr fade = overlayFade();
    ASSERT_TRUE(fade);
    const ShaderProgramPtr keep = keepHistory();
    ASSERT_TRUE(keep);
    // The pane's snapshot is taken by each frame below; the first now, for its colours.
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    OverlayTerminal overlay(Cols, Rows, cw, ch, "\x1b[?25l\x1b[2;6H\x1b[38;2;0;200;0m█");
    ASSERT_EQ(overlay.snapshot.defaultBg, snapshot.defaultBg);
    const QRgb red = qRgb(220, 0, 0);
    WindowRenderer::Pane drawn;
    drawn.snapshot = &snapshot;
    drawn.params.rect = QRect(QPoint(0, 0), QSize(Cols * cw, Rows * ch));
    drawn.params.focused = true;
    // A red block at a cell of the top row: the same snapshot, so the same pane.
    auto frame = [&](int col, std::optional<float> progress) {
        terminal->feed("\x1b[2J\x1b[1;" + std::to_string(col + 1) + "H█");
        snapshot.capture(*terminal);
        drawn.overlay.reset();
        if (progress) drawn.overlay = overlayOf(&overlay.snapshot, *progress);
        return renderer->renderToImage(drawn.params.rect.size(), {drawn}, {}, snapshot.defaultBg);
    };
    for (const bool paneChain : {false, true}) {
        SCOPED_TRACE(paneChain ? "pane chain" : "text chain");
        ShaderChains chains;
        (paneChain ? chains.pane : chains.text) = {keep};
        chains.overlayTransition = {fade};
        renderer->setShaderChains(chains);
        terminal->feed("\x1b[?25l\x1b[38;2;220;0;0m");
        // The pane chain's input has the background under the text: a trail
        // there is the brighter of the two in each channel.
        const QRgb background = rgb(snapshot.defaultBg);
        const QRgb trail = paneChain ? qRgb(std::max(qRed(red), qRed(background)), std::max(qGreen(red), qGreen(background)),
                                            std::max(qBlue(red), qBlue(background)))
                                     : red;
        frame(0, std::nullopt);
        const QImage plain = frame(1, std::nullopt);
        EXPECT_PIXEL(plain, cellCenter(0, 0), trail);
        EXPECT_PIXEL(plain, cellCenter(1, 0), red);
        // The first frame of the fade starts from the frame's history; the
        // next one reads what the pane under the overlay drew in the first.
        frame(2, 0.01f);
        const QImage image = frame(3, 0.02f);
        EXPECT_PIXEL_NEAR(image, cellCenter(2, 0), trail, 8);
        EXPECT_PIXEL_NEAR(image, cellCenter(3, 0), red, 8);
        // The text chain keeps every trail, the pane chain the last frame's.
        if (!paneChain) EXPECT_PIXEL_NEAR(image, cellCenter(0, 0), red, 8);
        else EXPECT_PIXEL_NEAR(image, cellCenter(1, 0), background, 8);
        EXPECT_PIXEL_NEAR(image, cellCenter(5, 1), plain.pixel(cellCenter(5, 1)), 8);
    }
}

TEST_F(Render, TheTrailsOfThePaneUnderAnOverlayOutlastItsFadeOut)
{
    // Once the overlay has faded out, the pane's own history goes on from
    // what it drew under the overlay, without the overlay's text.
    const ShaderProgramPtr fade = overlayFade();
    ASSERT_TRUE(fade);
    const ShaderProgramPtr keep = keepHistory();
    ASSERT_TRUE(keep);
    ShaderChains chains;
    chains.text = {keep};
    chains.overlayTransition = {fade};
    renderer->setShaderChains(chains);
    // The pane's snapshot is taken by each frame below; the first now, for its colours.
    terminal->feed("\x1b[?25l");
    snapshot.capture(*terminal);
    OverlayTerminal overlay(Cols, Rows, cw, ch, "\x1b[?25l\x1b[2;6H\x1b[38;2;0;200;0m█");
    ASSERT_EQ(overlay.snapshot.defaultBg, snapshot.defaultBg);
    const QRgb red = qRgb(220, 0, 0);
    WindowRenderer::Pane drawn;
    drawn.snapshot = &snapshot;
    drawn.params.rect = QRect(QPoint(0, 0), QSize(Cols * cw, Rows * ch));
    drawn.params.focused = true;
    auto frame = [&](int col, std::optional<float> progress) {
        terminal->feed("\x1b[2J\x1b[1;" + std::to_string(col + 1) + "H█");
        snapshot.capture(*terminal);
        drawn.overlay.reset();
        if (progress) drawn.overlay = overlayOf(&overlay.snapshot, *progress);
        return renderer->renderToImage(drawn.params.rect.size(), {drawn}, {}, snapshot.defaultBg);
    };
    terminal->feed("\x1b[?25l\x1b[38;2;220;0;0m");
    frame(0, std::nullopt);
    frame(1, 1.0f); // the overlay alone: the pane is not drawn
    // The fade out starts after the overlay showed whole: the pane under it
    // has no history yet, so the overlay's block at (5,1), which the frame's
    // own history holds, leaves no trail there. The fade adds 2% of it.
    const QImage start = frame(2, 0.02f);
    EXPECT_PIXEL_NEAR(start, cellCenter(5, 1), rgb(snapshot.defaultBg), 8);
    frame(3, 0.01f);
    const QImage image = frame(4, std::nullopt);
    EXPECT_PIXEL(image, cellCenter(2, 0), red);
    EXPECT_PIXEL(image, cellCenter(3, 0), red);
    EXPECT_PIXEL(image, cellCenter(4, 0), red);
    EXPECT_PIXEL(image, cellCenter(5, 1), rgb(snapshot.defaultBg));
}

TEST_F(Render, TheWindowChainSeesThePaneUntilItsOverlayIsHalfwayIn)
{
    render("\x1b[?25l");
    OverlayTerminal overlay(Cols, Rows, cw, ch, "\x1b[?25l\x1b]11;rgb:00/00/c0\x1b\\");
    ASSERT_NE(overlay.snapshot.defaultBg, snapshot.defaultBg);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(QPoint(0, 0), size);
    pane.params.focused = true;
    const ShaderProgramPtr fade = overlayFade();
    ASSERT_TRUE(fade);
    ShaderChains chains;
    chains.window = {layerShader("theme", "fragColor = vec4(uBackgroundColor.rgb, 1.0);")};
    chains.overlayTransition = {fade};
    renderer->setShaderChains(chains);
    for (const float progress : {0.25f, 0.75f}) {
        SCOPED_TRACE(testing::Message() << "progress " << progress);
        WindowRenderer::Pane drawn = pane;
        drawn.overlay = overlayOf(&overlay.snapshot, progress);
        const QImage image = renderer->renderToImage(size, {drawn}, {}, snapshot.defaultBg);
        EXPECT_PIXEL(image, cellCenter(3, 2), progress < 0.5f ? rgb(snapshot.defaultBg) : qRgb(0, 0, 0xc0));
    }
}

TEST_F(Render, APaneWithAnOverlayKeepsItsOwnScrollbackInputs)
{
    // The overlay's terminal has no history: the chains still count the pane's.
    render("\x1b[?25l1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8");
    ASSERT_GT(snapshot.scrollbackLines, 0u);
    ASSERT_LE(snapshot.scrollbackLines, 16u);
    OverlayTerminal overlay(Cols, Rows, cw, ch, "\x1b[?25l");
    ASSERT_EQ(overlay.snapshot.scrollbackLines, 0u);
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(QPoint(0, 0), size);
    pane.params.focused = true;
    pane.overlay = overlayOf(&overlay.snapshot, 1.0f);
    ShaderChains chains;
    chains.pane = {layerShader("rows", "fragColor = vec4(uScrollback.x / 16.0, uScrollback.y / 16.0, 0.0, 1.0);")};
    renderer->setShaderChains(chains);
    const int expected = qRound(double(snapshot.scrollbackLines) / 16.0 * 255.0);
    EXPECT_PIXEL(renderer->renderToImage(size, {pane}, {}, snapshot.defaultBg), cellCenter(3, 2), qRgb(expected, expected, 0));
}

TEST_F(Render, WithoutAnOverlayTransitionAnOverlaySwitchesAtOnce)
{
    // No Shaders.overlayTransition, or a pack that did not compile: never
    // blank, never a blend, the pane until halfway and the overlay after.
    render("\x1b[?25l\x1b[48;2;220;0;0m \x1b[0m");
    OverlayTerminal overlay(Cols, Rows, cw, ch, "\x1b[?25l\x1b[48;2;0;200;0m \x1b[0m");
    const QSize size(Cols * cw, Rows * ch);
    WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(QPoint(0, 0), size);
    for (const bool chains : {false, true}) {
        // With other chains, and with none at all.
        ShaderChains set;
        if (chains) set.pane = {layerShader("plain", "")};
        renderer->setShaderChains(set);
        for (const float progress : {0.0f, 0.25f, 0.49f, 0.5f, 0.75f, 1.0f}) {
            WindowRenderer::Pane drawn = pane;
            drawn.overlay = overlayOf(&overlay.snapshot, progress);
            SCOPED_TRACE(testing::Message() << "progress " << progress << (chains ? " with a pane chain" : ""));
            const QImage image = renderer->renderToImage(size, {drawn}, {}, snapshot.defaultBg);
            EXPECT_PIXEL(image, cellCenter(0, 0), progress < 0.5f ? qRgb(220, 0, 0) : qRgb(0, 200, 0));
            EXPECT_EQ(qAlpha(image.pixel(cellCenter(5, 2))), 255);
        }
    }
}

class PopupRender : public Render
{
protected:
    /// The pane: red, with a white X in column 0 and in columns 4 to 11,
    /// under the popup and its border. The popup: a 6 by 2 grid at cell
    /// (5, 1), blue, with a green cell at its own (0, 0).
    void SetUp() override
    {
        Render::SetUp();
        if (IsSkipped() || HasFatalFailure()) return;
        std::string fill = "\x1b[?25l\x1b[48;2;220;0;0m\x1b[38;2;255;255;255m";
        for (int row = 0; row < Rows; ++row) fill += "X   " + std::string(8, 'X') + std::string(Cols - 12, ' ');
        render(fill);
        popupTerminal = std::make_unique<AfterglowVt::Terminal>(6, 2, popupHost);
        popupTerminal->resize(6, 2, cw, ch);
        popupTerminal->feed("\x1b[?25l\x1b]11;rgb:00/00/c0\x1b\\\x1b[48;2;0;200;0m \x1b[0m");
        popupSnapshot.capture(*popupTerminal);
        size = QSize(Cols * cw, Rows * ch);
        pane.snapshot = &snapshot;
        pane.params.rect = QRect(QPoint(0, 0), size);
        pane.params.focused = true;
    }

    WindowRenderer::Pane withPopup(bool border, float progress = 1.0f) const
    {
        WindowRenderer::Pane drawn = pane;
        WindowRenderer::Pane::Overlay overlay;
        overlay.snapshot = &popupSnapshot;
        overlay.progress = progress;
        WindowRenderer::Pane::Overlay::Popup popup;
        popup.rect = QRect(5 * cw, ch, 6 * cw, 2 * ch);
        popup.border = border;
        popup.title = "T";
        popup.borderColor = packRgba(0, 255, 255);
        popup.titleColor = packRgba(255, 255, 0);
        popup.dim = 0.5f;
        overlay.popup = popup;
        drawn.overlay = overlay;
        return drawn;
    }

    QImage frame(const WindowRenderer::Pane& drawn) { return renderer->renderToImage(size, {drawn}, {}, snapshot.defaultBg); }

    /// Whether every pixel of @p cell is @p color.
    static bool cellIs(const QImage& image, QRect cell, QRgb color)
    {
        for (int y = cell.top(); y <= cell.bottom(); ++y)
            for (int x = cell.left(); x <= cell.right(); ++x) {
                const QRgb got = image.pixel(x, y);
                if (std::abs(qRed(got) - qRed(color)) + std::abs(qGreen(got) - qGreen(color)) + std::abs(qBlue(got) - qBlue(color)) > 6)
                    return false;
            }
        return true;
    }
    QRect cell(int x, int y) const { return QRect(x * cw, y * ch, cw, ch); }
    /// Whether some pixel of @p rect is near @p color.
    static bool shows(const QImage& image, QRect rect, QRgb color)
    {
        for (int y = rect.top(); y <= rect.bottom(); ++y)
            for (int x = rect.left(); x <= rect.right(); ++x) {
                const QRgb got = image.pixel(x, y);
                if (std::abs(qRed(got) - qRed(color)) + std::abs(qGreen(got) - qGreen(color)) + std::abs(qBlue(got) - qBlue(color)) <= 30)
                    return true;
            }
        return false;
    }

    NullHost popupHost;
    std::unique_ptr<AfterglowVt::Terminal> popupTerminal;
    PaneSnapshot popupSnapshot;
    QSize size;
    WindowRenderer::Pane pane;
};

TEST_F(PopupRender, APopupDrawsOverItsPaneWhichShowsDarkerAroundIt)
{
    const QRgb red = qRgb(220, 0, 0), dimmed = qRgb(110, 0, 0), blue = qRgb(0, 0, 0xc0), green = qRgb(0, 200, 0);
    // One pass, the layered path (the pane's text in a layer of its own,
    // which a popup's background does not cover), and a pane chain.
    for (const int path : {0, 1, 2}) {
        ShaderChains chains;
        if (path == 1) chains.text = {ShaderProgram::passthrough()};
        if (path == 2) chains.pane = {layerShader("plain", "")};
        renderer->setShaderChains(chains);
        SCOPED_TRACE(testing::Message() << "path " << path);
        const QImage plain = frame(pane);
        EXPECT_PIXEL(plain, cellCenter(2, 0), red);
        EXPECT_TRUE(shows(plain, cell(0, 0), qRgb(255, 255, 255)));
        for (const bool border : {true, false}) {
            SCOPED_TRACE(testing::Message() << (border ? "with" : "without") << " a border");
            const QImage image = frame(withPopup(border));
            // The pane around it shows, through a black layer at half.
            EXPECT_PIXEL(image, cellCenter(2, 0), dimmed);
            EXPECT_PIXEL(image, cellCenter(Cols - 1, Rows - 1), dimmed);
            EXPECT_TRUE(shows(image, cell(0, 0), qRgb(128, 128, 128))) << "the pane's text, darker";
            EXPECT_FALSE(shows(image, cell(0, 0), qRgb(255, 255, 255)));
            // The popup's grid as it is, none of the pane's text in it.
            EXPECT_TRUE(cellIs(image, cell(5, 1), green));
            for (int x = 6; x < 11; ++x) EXPECT_TRUE(cellIs(image, cell(x, 1), blue)) << x;
            for (int x = 5; x < 11; ++x) EXPECT_TRUE(cellIs(image, cell(x, 2), blue)) << x;
            if (border) {
                // A line in its colour on the popup's background, the title in
                // the top edge, and nothing of the pane's text under them.
                for (const QRect& edge : {cell(4, 1), cell(11, 2), cell(9, 0), cell(8, 3), cell(4, 0), cell(11, 3)}) {
                    EXPECT_TRUE(shows(image, edge, qRgb(0, 255, 255))) << edge.x() / cw << "," << edge.y() / ch;
                    EXPECT_TRUE(shows(image, edge, blue)) << edge.x() / cw << "," << edge.y() / ch;
                    EXPECT_FALSE(shows(image, edge, red)) << edge.x() / cw << "," << edge.y() / ch;
                    EXPECT_FALSE(shows(image, edge, qRgb(255, 255, 255))) << edge.x() / cw << "," << edge.y() / ch;
                }
                EXPECT_TRUE(shows(image, cell(7, 0), qRgb(255, 255, 0))) << "the title";
                EXPECT_TRUE(cellIs(image, cell(6, 0), blue)) << "a space before the title";
                EXPECT_TRUE(cellIs(image, cell(8, 0), blue)) << "a space after the title";
            } else {
                // The pane shows right up to the grid, darker.
                for (const QRect& around : {cell(4, 1), cell(11, 2), cell(8, 0), cell(8, 3)}) {
                    EXPECT_TRUE(shows(image, around, dimmed)) << around.x() / cw << "," << around.y() / ch;
                    EXPECT_FALSE(shows(image, around, blue)) << around.x() / cw << "," << around.y() / ch;
                }
            }
        }
        // Without the darkening the pane around it is as it was.
        WindowRenderer::Pane bright = withPopup(true);
        bright.overlay->popup->dim = 0;
        EXPECT_PIXEL(frame(bright), cellCenter(2, 0), red);
    }
}

TEST_F(PopupRender, TheOverlayTransitionBlendsThePaneAndThePaneWithItsPopup)
{
    const ShaderProgramPtr fade = overlayFade();
    ASSERT_TRUE(fade);
    ShaderChains chains;
    chains.overlayTransition = {fade};
    renderer->setShaderChains(chains);
    const QImage plain = frame(pane);
    // The second image: the whole pane as it shows with the popup, as
    // without a transition.
    renderer->setShaderChains({});
    const QImage shown = frame(withPopup(true));
    renderer->setShaderChains(chains);
    EXPECT_TRUE(frame(withPopup(true, 0.0f)) == plain);
    EXPECT_TRUE(frame(withPopup(true, 1.0f)) == shown);
    const QImage middle = frame(withPopup(true, 0.5f));
    // The pane, between itself and darker; the popup, between the pane and itself.
    EXPECT_PIXEL(middle, cellCenter(2, 0), qRgb(165, 0, 0));
    EXPECT_PIXEL(middle, cell(6, 1).topLeft(), qRgb(110, 0, 0x60));
    EXPECT_PIXEL(middle, cell(5, 1).topLeft(), qRgb(110, 100, 0));
}

TEST_F(PopupRender, APanesBufferStepsOncePerFrameWhileItsPopupFades)
{
    // The pane under a fading popup and the pane with it carry one snapshot:
    // its buffer set steps once a frame, not once for each.
    ShaderProgramSource source;
    source.id = QStringLiteral("stepping-pane");
    source.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return texture(iChannel0,uv);}";
    ShaderBufferSource pass;
    pass.feedback = true;
    pass.scale = .5f;
    pass.code = "#include <terminal_multipass.glsl>\nvec4 pTerminal(vec2 uv){return vec4(texture(iChannel0,uv).r+0.05,0,0,1);}";
    source.buffers = {pass};
    QString error;
    const ShaderProgramPtr program = ShaderProgram::compile(source, &error);
    ASSERT_TRUE(program) << error.toStdString();
    const ShaderProgramPtr fade = overlayFade();
    ASSERT_TRUE(fade);
    ShaderChains chains;
    chains.pane = {program};
    chains.overlayTransition = {fade};
    renderer->setShaderChains(chains);
    auto draw = [&](std::optional<float> progress, int n) {
        renderer->setClockOverride(float(n + 1));
        WindowRenderer::Pane drawn = progress ? withPopup(true, *progress) : pane;
        if (drawn.overlay) drawn.overlay->popup->dim = 0;
        return frame(drawn);
    };
    const auto steps = [](double count) { return qRgb(qRound(count * 0.05 * 255), 0, 0); };
    EXPECT_PIXEL_NEAR(draw(std::nullopt, 0), cellCenter(2, 0), steps(1), 3);
    for (int n = 1; n <= 3; ++n) {
        SCOPED_TRACE(n);
        EXPECT_PIXEL_NEAR(draw(0.5f, n), cellCenter(2, 0), steps(n + 1), 3);
    }
    EXPECT_PIXEL_NEAR(draw(std::nullopt, 4), cellCenter(2, 0), steps(5), 3);
}

TEST_F(PopupRender, ThePanesCursorIsUnfocusedOnlyWhileItsPopupRuns)
{
    // The pane's cursor at (1, 1), beside the popup.
    terminal->feed("\x1b[2;2H\x1b[?25h");
    snapshot.capture(*terminal);
    const QRgb red = qRgb(220, 0, 0);
    WindowRenderer::Pane running = withPopup(true);
    running.overlay->popup->dim = 0;
    // A box: the cell's background shows in its middle, and its outline
    // in the cell's edges.
    const QImage box = frame(running);
    EXPECT_PIXEL(box, cellCenter(1, 1), red);
    EXPECT_FALSE(cellIs(box, cell(1, 1), red)) << "no cursor at all";
    // One that ended fades out: the pane has the keyboard again, and its
    // cursor is a focused block.
    WindowRenderer::Pane ended = running;
    ended.overlay->running = false;
    const QImage image = frame(ended);
    EXPECT_FALSE(shows(image, QRect(cellCenter(1, 1), QSize(1, 1)), red));
}

TEST_F(PopupRender, TheCursorInputsAreThePopupsWhileItRunsAndThePanesOnceItEnds)
{
    // The pane's cursor at its (1, 1), the popup's at its own (1, 0), which
    // is the pane's (6, 1).
    terminal->feed("\x1b[2;2H\x1b[?25h");
    snapshot.capture(*terminal);
    popupTerminal->feed("\x1b[?25h");
    popupSnapshot.capture(*popupTerminal);
    auto glsl = [](double v) { return QString::number(v, 'f', 3).toStdString(); };
    auto cursorAt = [&](int x, int y) {
        const std::string body = "fragColor = all(lessThan(abs(uCursorRect - vec4(" + glsl(x * cw) + "," + glsl(y * ch) + "," + glsl(cw)
            + "," + glsl(ch) + ")), vec4(0.1))) ? vec4(0,1,0,1) : vec4(1,0,0,1);";
        ShaderChains chains;
        chains.pane = {layerShader("cursor-at", body.c_str())};
        renderer->setShaderChains(chains);
    };
    // The pane away from the frame's corner: the inputs are in the pane's
    // own pixels, not the frame's.
    const QPoint offset(2 * cw, ch);
    const QSize bigger = size + QSize(offset.x(), offset.y());
    WindowRenderer::Pane running = withPopup(true);
    running.params.rect.translate(offset);
    running.params.contentOrigin = running.params.rect.topLeft();
    running.overlay->popup->rect.translate(offset);
    cursorAt(6, 1);
    EXPECT_PIXEL(renderer->renderToImage(bigger, {running}, {}, snapshot.defaultBg), cellCenter(4, 1), qRgb(0, 255, 0));
    WindowRenderer::Pane ended = running;
    ended.overlay->running = false;
    cursorAt(1, 1);
    EXPECT_PIXEL(renderer->renderToImage(bigger, {ended}, {}, snapshot.defaultBg), cellCenter(4, 1), qRgb(0, 255, 0));
}

TEST_F(PopupRender, ATitleThatDoesNotFitIsCutAndAZeroWidthCharacterTakesNoCell)
{
    const QRgb yellow = qRgb(255, 255, 0), blue = qRgb(0, 0, 0xc0);
    // The frame is 8 cells wide from cell 4: room for 2 title cells, here
    // one and an ellipsis, between a space each side.
    WindowRenderer::Pane cut = withPopup(true);
    cut.overlay->popup->title = "ABCDEFGHIJ";
    const QImage image = frame(cut);
    EXPECT_TRUE(cellIs(image, cell(6, 0), blue));
    EXPECT_TRUE(shows(image, cell(7, 0), yellow));
    EXPECT_FALSE(cellIs(image, cell(8, 0), blue)) << "the ellipsis, small dots in the title's colour";
    EXPECT_TRUE(cellIs(image, cell(9, 0), blue));
    // A zero-width space with nothing to join takes no cell.
    WindowRenderer::Pane joined = withPopup(true);
    joined.overlay->popup->title = "\u200BT";
    const QImage title = frame(joined);
    EXPECT_TRUE(shows(title, cell(7, 0), yellow));
    EXPECT_TRUE(cellIs(title, cell(8, 0), blue));
}

TEST_F(PopupRender, SquareCornersAndATitleInVisualOrder)
{
    const QRgb yellow = qRgb(255, 255, 0), cyan = qRgb(0, 255, 255);
    // A square corner goes through the middle of its cell.
    WindowRenderer::Pane square = withPopup(true);
    square.overlay->popup->roundedCorners = false;
    const QImage corners = frame(square);
    for (const QRect& corner : {cell(4, 0), cell(11, 0), cell(4, 3), cell(11, 3)})
        EXPECT_TRUE(shows(corners, QRect(corner.center() - QPoint(1, 1), QSize(3, 3)), cyan)) << corner.x() / cw << "," << corner.y() / ch;
    // An alef, then a full block, which takes the right-to-left direction of
    // the title: with bidi the block shows first.
    WindowRenderer::Pane mixed = withPopup(true);
    mixed.overlay->popup->title = "\u05D0\u2588";
    EXPECT_TRUE(cellIs(frame(mixed), cell(8, 0), yellow));
    mixed.overlay->popup->bidi = AfterglowVt::BidiMode::Auto;
    EXPECT_TRUE(cellIs(frame(mixed), cell(7, 0), yellow));
}

TEST_F(PopupRender, APopupPastItsPaneIsClippedToThePane)
{
    // A frame wider than the pane, and a popup that goes past its right edge.
    const QSize wide((Cols + 4) * cw, Rows * ch);
    WindowRenderer::Pane drawn = withPopup(true);
    drawn.overlay->popup->rect = QRect((Cols - 3) * cw, ch, 6 * cw, 2 * ch);
    const QImage image = renderer->renderToImage(wide, {drawn}, {}, 0xff000000u);
    EXPECT_TRUE(cellIs(image, cell(Cols - 3, 1), qRgb(0, 200, 0)));
    EXPECT_TRUE(cellIs(image, cell(Cols - 1, 2), qRgb(0, 0, 0xc0)));
    for (int x = Cols; x < Cols + 4; ++x) {
        EXPECT_TRUE(cellIs(image, cell(x, 1), qRgb(0, 0, 0))) << x;
        EXPECT_TRUE(cellIs(image, cell(x, 0), qRgb(0, 0, 0))) << x;
    }
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    Testing::selectPlatform();
    QGuiApplication app(argc, argv);
    Testing::Device made = Testing::makeDevice();
    s_device = std::move(made.device);
    s_noDevice = std::move(made.reason);
    s_noVulkan = made.skip;
    const int result = RUN_ALL_TESTS();
    s_device.reset();
    return result;
}
