// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

class QQmlComponent;

namespace AfterglowRender {
class GpuDevice;
}

namespace Afterglow {

class AudioController;
class ScriptHost;
class SettingsStore;
class ShaderLibrary;

/// What every window shares: one GPU device, the settings, the chrome's QML
/// component, the shader library, the scripts and the audio analyzer.
struct Services
{
    AfterglowRender::GpuDevice& device;
    SettingsStore& settings;
    QQmlComponent& chrome;
    ShaderLibrary& shaders;
    ScriptHost& scripts;
    AudioController& audio;
};

} // namespace Afterglow
