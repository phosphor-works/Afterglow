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

    // Packed 0xAARRGGBB as the snapshot stores colours, alpha ignored.
    static QRgb rgb(std::uint32_t c) { return qRgb((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); }
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
ShaderProgramPtr layerShader(const char* id, const char* body)
{
    ShaderProgramSource src;
    src.id = QString::fromLatin1(id);
    src.code = QByteArray("void mainImage(out vec4 fragColor, in vec2 fragCoord)\n{\n    vec4 c = texture(iChannel0, fragCoord / iResolution.xy);\n")
        + body + "\n}\n";
    QString error;
    auto program = ShaderProgram::compile(src, &error);
    EXPECT_TRUE(program) << error.toStdString();
    return program;
}
} // namespace

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
