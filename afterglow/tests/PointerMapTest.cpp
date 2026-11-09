// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The bundled packs with a pointer map (pPointerMap), CRT Curve and Tilt,
// move the pointer the way their pictures move the content, in the pane
// chain and in the window chain. A pane of black and white cells, as a
// checkerboard, goes through the pack offscreen. At each pixel of the
// picture, the cell that the map gives must have the colour drawn there.

#include "GpuTestDevice.h"
#include "shaders/ShaderLibrary.h"

#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/GridRenderer.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Terminal.h>

#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

using namespace Afterglow;

namespace {

std::unique_ptr<AfterglowRender::GpuDevice> s_device;
/// Why there is no device. A machine without Vulkan skips the test. On one
/// with Vulkan, a device that cannot be made fails it.
std::string s_noDevice;
bool s_noVulkan = false;

struct QuietHost : AfterglowVt::ITerminalHost {
    void writeToPty(std::string_view) override {}
};

constexpr int Cols = 40;
constexpr int Rows = 12;

/// Each pack at values that move the content far enough that the plain
/// pointer would miss: a test of the map, not of the identity.
QVariantMap packValues()
{
    return {{QStringLiteral("crt-curve"), QVariantMap{{QStringLiteral("curvature"), 0.3}, {QStringLiteral("vignette"), 0.0}}},
            {QStringLiteral("tilt"), QVariantMap{{QStringLiteral("glow"), 0.0}}}};
}

} // namespace

TEST(PointerMap, TheBundledMapsFindTheCellDrawnUnderThePointer)
{
    if (!s_device) {
        if (s_noVulkan) GTEST_SKIP() << s_noDevice;
        FAIL() << s_noDevice;
    }
    ShaderLibrary library;
    AfterglowRender::WindowRenderer renderer(*s_device, nullptr);
    renderer.setDevicePixelRatio(1.0);
    const auto& m = renderer.fonts().fonts->metrics();
    const int cw = m.cellWidth, ch = m.cellHeight;

    QuietHost host;
    AfterglowVt::Terminal terminal(Cols, Rows, host, {});
    terminal.resize(Cols, Rows, cw, ch);
    std::string board = "\x1b[?25l";
    for (int row = 0; row < Rows; ++row) {
        for (int col = 0; col < Cols; ++col)
            board += (row + col) % 2 ? "\x1b[48;2;255;255;255m " : "\x1b[48;2;0;0;0m ";
        if (row + 1 < Rows) board += "\r\n";
    }
    terminal.feed(board);
    AfterglowRender::PaneSnapshot snapshot;
    snapshot.capture(terminal);
    AfterglowRender::WindowRenderer::Pane pane;
    pane.snapshot = &snapshot;
    pane.params.rect = QRect(0, 0, Cols * cw, Rows * ch);
    pane.params.focused = true;
    pane.params.id = 1;

    // The packs by name, at values that move the content: not whatever else
    // the data directories hold. A bundled pack with a map joins the list.
    const QStringList mapped{QStringLiteral("crt-curve"), QStringLiteral("tilt")};
    const QStringList bundled = QDir(QStringLiteral(AFTERGLOW_SOURCE_SHADERS)).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    ASSERT_TRUE(bundled.contains(QStringLiteral("tilt"))) << AFTERGLOW_SOURCE_SHADERS;
    for (const QString& id : bundled) {
        ShaderLibrary::Refs refs;
        refs.pane = {id};
        const auto chains = library.build(refs, {});
        if (chains.pane.size() == 1 && chains.pane.front()->hasPointerMap()) {
            EXPECT_TRUE(mapped.contains(id)) << id.toStdString() << " has a pointer map: add it here, with values that move its content";
        }
    }
    for (const QString& id : mapped) {
        for (const bool window : {false, true}) {
            SCOPED_TRACE(id.toStdString() + (window ? " in the window chain" : " in the pane chain"));
            ShaderLibrary::Refs refs;
            (window ? refs.window : refs.pane) = {id};
            auto chains = library.build(refs, {{id, packValues().value(id)}});
            const auto& chain = window ? chains.window : chains.pane;
            ASSERT_EQ(chain.size(), 1u);
            ASSERT_TRUE(chain.front()->hasPointerMap());
            renderer.setShaderChains(std::move(chains));
            // The frame that draws the pack evaluates its map; the lookups
            // after it use that map.
            const QImage image = renderer.renderToImage(pane.params.rect.size(), {pane}, {}, 0xff000000u);
            ASSERT_FALSE(image.isNull());
            int checked = 0, wrong = 0, missedWithoutMap = 0;
            for (int y = 1; y < image.height(); y += 3) {
                for (int x = 1; x < image.width(); x += 3) {
                    const QPointF pixel(x + 0.5, y + 0.5);
                    const QPointF at = window ? renderer.windowContentPoint(pixel) : renderer.paneContentPoint(pixel, 1);
                    const double col = at.x() / cw, row = at.y() / ch;
                    // Near a cell's edge the picture blends two cells, and near
                    // the pane's edge the content leaves it: neither has one answer.
                    const double fx = col - std::floor(col), fy = row - std::floor(row);
                    if (std::min(fx, 1.0 - fx) < 0.25 || std::min(fy, 1.0 - fy) < 0.25) continue;
                    if (col < 1 || row < 1 || col > Cols - 1 || row > Rows - 1) continue;
                    const QRgb drawn = image.pixel(x, y);
                    const bool white = std::max({qRed(drawn), qGreen(drawn), qBlue(drawn)}) > 80;
                    const bool expected = (int(row) + int(col)) % 2;
                    ++checked;
                    if (white != expected) ++wrong;
                    const bool plain = (y / ch + x / cw) % 2;
                    if (white != plain) ++missedWithoutMap;
                }
            }
            EXPECT_GT(checked, 500);
            EXPECT_EQ(wrong, 0) << "of " << checked;
            // The plain pointer would have hit the wrong cell often.
            EXPECT_GT(missedWithoutMap, checked / 10) << "of " << checked;
        }
    }
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    AfterglowRender::Testing::selectPlatform();
    // Not the developer's packs. A build-tree binary puts the checkout's
    // packs above the system's, and XDG_DATA_DIRS stays: the Vulkan loader
    // finds its drivers there.
    QTemporaryDir home;
    if (!home.isValid()) return 1;
    qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
    qputenv("XDG_DATA_HOME", home.filePath(QStringLiteral("data-home")).toUtf8());
    QGuiApplication app(argc, argv);
    AfterglowRender::Testing::Device made = AfterglowRender::Testing::makeDevice();
    s_device = std::move(made.device);
    s_noDevice = std::move(made.reason);
    s_noVulkan = made.skip;
    const int result = RUN_ALL_TESTS();
    s_device.reset();
    return result;
}
