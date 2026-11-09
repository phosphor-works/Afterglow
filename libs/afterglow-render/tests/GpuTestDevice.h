// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

// The device of a test binary that draws with Vulkan, shared by the
// renderer's tests and the application's popup and pointer map tests.

#include <AfterglowRender/GpuDevice.h>

#include <QByteArray>
#include <QGuiApplication>
#include <QString>

#include <gtest/gtest.h>

#include <exception>
#include <memory>
#include <mutex>
#include <string>

namespace AfterglowRender::Testing {

/// Call before QGuiApplication. Vulkan needs a real platform, and the
/// offscreen QPA has none. Wayland first, whatever QT_QPA_PLATFORM the
/// shell set: a display that is gone (a stale WAYLAND_DISPLAY) falls back
/// to offscreen, where the tests skip, instead of aborting the binary.
inline void selectPlatform()
{
    qputenv("QT_QPA_PLATFORM", qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ? QByteArray("offscreen") : QByteArray("wayland;offscreen"));
    // Not the desktop's theme plugin, which reaches the desktop's services.
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
}

struct Device
{
    std::unique_ptr<GpuDevice> device;
    /// Why there is no device.
    std::string reason;
    /// The machine has no Wayland display or no Vulkan: the tests skip.
    /// Otherwise a device that could not be made fails them.
    bool skip = false;
};

/// Call after InitGoogleTest and QGuiApplication. Makes no device while
/// gtest only lists the tests (CTest's discovery before each test).
inline Device makeDevice()
{
    Device made;
    if (GTEST_FLAG_GET(list_tests)) {
        made.reason = "listing the tests";
        return made;
    }
    if (QGuiApplication::platformName() != QLatin1String("wayland")) {
        made.skip = true;
        made.reason = "no Wayland display";
        return made;
    }
    if (!GpuDevice::vulkanAvailable()) {
        made.skip = true;
        made.reason = "no Vulkan device";
        return made;
    }
    // Qt's reason (a VkResult) comes as a warning: it goes into the failure,
    // and on to the handler that was there before. Qt may warn from its own
    // threads: the warnings and the previous handler are read under the
    // lock, which the install holds until the previous handler is known.
    static std::mutex s_lock;
    static std::string s_warnings;
    static QtMessageHandler s_previous = nullptr;
    {
        const std::lock_guard lock(s_lock);
        s_previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& context, const QString& text) {
            QtMessageHandler previous = nullptr;
            {
                const std::lock_guard lock(s_lock);
                if (type != QtDebugMsg && type != QtInfoMsg) s_warnings += "\n" + text.toStdString();
                previous = s_previous;
            }
            if (previous) previous(type, context, text);
        });
    }
    try {
        made.device = std::make_unique<GpuDevice>(FontSpec{});
        made.device->rhi(nullptr);
    } catch (const std::exception& error) {
        made.device.reset();
        const std::lock_guard lock(s_lock);
        made.reason = std::string("this machine has a Vulkan device, and the test could not make one: ") + error.what() + s_warnings;
    }
    const std::lock_guard lock(s_lock);
    qInstallMessageHandler(s_previous);
    return made;
}

} // namespace AfterglowRender::Testing
