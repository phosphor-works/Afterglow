// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// A popup as the window draws it with its settings (popupLook): the border
// color, the title color and the dim, rendered offscreen. The native suite
// checks where a popup goes and what the pointer reaches.

#include "TerminalWindow.h"
#include "config/Settings.h"

#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/GridRenderer.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Terminal.h>

#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QVulkanFunctions>
#include <QVulkanInstance>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

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

/// Whether some pixel of @p rect is within a small distance of @p color.
bool showsColor(const QImage& image, const QRect& rect, QRgb color)
{
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        for (int x = rect.left(); x <= rect.right(); ++x) {
            const QRgb got = image.pixel(x, y);
            if (std::abs(qRed(got) - qRed(color)) + std::abs(qGreen(got) - qGreen(color)) + std::abs(qBlue(got) - qBlue(color)) <= 30)
                return true;
        }
    }
    return false;
}

/// Whether the machine has Vulkan: a loader, a driver and a physical device.
bool hasVulkanDevice()
{
    QVulkanInstance instance;
    if (!instance.create()) return false;
    std::uint32_t count = 0;
    instance.functions()->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, nullptr);
    return count > 0;
}

} // namespace

TEST(PopupLook, TheBorderTitleAndDimComeFromTheSettings)
{
    if (!s_device) {
        if (s_noVulkan) GTEST_SKIP() << s_noDevice;
        FAIL() << s_noDevice;
    }
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto write = [&](const QString& name, const QByteArray& data) -> QString {
        QFile file(directory.filePath(name));
        if (!file.open(QIODevice::WriteOnly)) {
            ADD_FAILURE() << "cannot write " << name.toStdString();
            return {};
        }
        file.write(data);
        return file.fileName();
    };
    const QString boxed = write(QStringLiteral("boxed.json"),
                                R"({"background": "#000000", "foreground": "#ffffff", "divider": "#ff0000", "popupBorder": "#0000ff"})");
    const QString plain = write(QStringLiteral("plain.json"), R"({"background": "#000000", "foreground": "#00ff00"})");
    ASSERT_FALSE(boxed.isEmpty() || plain.isEmpty());
    // The config's own border, a theme's, and the fallback of a theme
    // without one, halfway from its background to its text, which the
    // config's border does not replace. The title takes the text color, and
    // the dim darkens the pane around the popup.
    const struct
    {
        QByteArray config;
        QRgb border;
        QRgb title;
        QRgb around;
    } cases[] = {
        {R"({"Colors": {"background": "#000000", "foreground": "#ffffff", "divider": "#ff0000", "popupBorder": "#00ff00"}})",
         qRgb(0, 255, 0), qRgb(255, 255, 255), qRgb(0, 0, 0)},
        {R"({"Colors": {"theme": ")" + boxed.toUtf8() + R"("}})", qRgb(0, 0, 255), qRgb(255, 255, 255), qRgb(0, 0, 0)},
        {R"({"Colors": {"theme": ")" + plain.toUtf8() + R"(", "popupBorder": "#ff00ff"}})", qRgb(0, 127, 0), qRgb(0, 255, 0),
         qRgb(0, 0, 0)},
        {R"({"Colors": {"background": "#ffffff", "foreground": "#000000", "popupDim": 0.2}})", qRgb(128, 128, 128), qRgb(0, 0, 0),
         qRgb(204, 204, 204)},
    };
    AfterglowRender::WindowRenderer renderer(*s_device, nullptr);
    renderer.setDevicePixelRatio(1.0);
    const auto& m = renderer.fonts().fonts->metrics();
    const int cw = m.cellWidth, ch = m.cellHeight;
    for (const auto& each : cases) {
        SCOPED_TRACE(each.config.toStdString());
        SettingsStore store(write(QStringLiteral("config.json"), each.config));
        const Settings& settings = store.settings();
        EXPECT_TRUE(store.problems().isEmpty()) << store.problems().join(QLatin1Char('\n')).toStdString();
        QuietHost host;
        AfterglowVt::Terminal pane(20, 6, host, settings.terminal);
        pane.resize(20, 6, cw, ch);
        pane.feed("\x1b[?25l");
        AfterglowVt::Terminal program(8, 2, host, settings.terminal);
        program.resize(8, 2, cw, ch);
        program.feed("\x1b[?25l");
        AfterglowRender::PaneSnapshot paneSnapshot, programSnapshot;
        paneSnapshot.capture(pane);
        programSnapshot.capture(program);
        AfterglowRender::WindowRenderer::Pane drawn;
        drawn.snapshot = &paneSnapshot;
        drawn.params.rect = QRect(0, 0, 20 * cw, 6 * ch);
        drawn.params.focused = true;
        AfterglowRender::WindowRenderer::Pane::Overlay overlay;
        overlay.snapshot = &programSnapshot;
        // The window's look, at a rectangle given here: the grid of 8 by 2
        // cells from cell (5, 2), its border one cell around it.
        overlay.popup = popupLook(settings, true, "\u2588");
        overlay.popup->rect = QRect(5 * cw, 2 * ch, 8 * cw, 2 * ch);
        drawn.overlay = overlay;
        const QImage image = renderer.renderToImage(drawn.params.rect.size(), {drawn}, {}, paneSnapshot.defaultBg);
        // The left and right edges, and the bottom.
        for (const QRect& edge : {QRect(4 * cw, 2 * ch, cw, ch), QRect(13 * cw, 3 * ch, cw, ch), QRect(8 * cw, 4 * ch, cw, ch)}) {
            EXPECT_TRUE(showsColor(image, edge, each.border)) << edge.x() / cw << "," << edge.y() / ch;
            EXPECT_FALSE(showsColor(image, edge, qRgb(255, 0, 0))) << "the divider's color";
            EXPECT_FALSE(showsColor(image, edge, qRgb(255, 0, 255))) << "the config's color under a theme";
        }
        // The title, a full block, from the frame's fourth column ("╭─ █ ─").
        EXPECT_TRUE(showsColor(image, QRect((4 + 3) * cw, ch, cw, ch), each.title));
        // A corner of the pane, away from the popup.
        EXPECT_TRUE(showsColor(image, QRect(0, 0, 2, 2), each.around));
    }
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    // Vulkan needs a real platform; the offscreen QPA has no Vulkan support.
    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) qputenv("QT_QPA_PLATFORM", "offscreen");
    // Not the desktop's theme plugin, which reaches the desktop's services.
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QGuiApplication app(argc, argv);
    if (QGuiApplication::platformName() != QLatin1String("wayland")) {
        s_noVulkan = true;
        s_noDevice = "no Wayland display";
    } else if (!hasVulkanDevice()) {
        s_noVulkan = true;
        s_noDevice = "no Vulkan device";
    } else {
        try {
            s_device = std::make_unique<AfterglowRender::GpuDevice>(AfterglowRender::FontSpec{});
            if (!s_device->rhi(nullptr)) {
                s_device.reset();
                s_noDevice = "this machine has a Vulkan device, and the test could not make one";
            }
        } catch (const std::exception& error) {
            s_device.reset();
            s_noDevice = std::string("this machine has a Vulkan device, and the test could not make one: ") + error.what();
        }
    }
    const int result = RUN_ALL_TESTS();
    s_device.reset();
    return result;
}
