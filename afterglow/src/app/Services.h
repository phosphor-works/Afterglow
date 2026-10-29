// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

class QQmlEngine;

namespace AfterglowRender {
class GpuDevice;
}

namespace Afterglow {

class SettingsStore;
class ShaderLibrary;

/// What every window shares: one GPU device, the settings, the QML engine
/// for the chrome and the shader library.
struct Services
{
    AfterglowRender::GpuDevice& device;
    SettingsStore& settings;
    QQmlEngine& engine;
    ShaderLibrary& shaders;
};

} // namespace Afterglow
