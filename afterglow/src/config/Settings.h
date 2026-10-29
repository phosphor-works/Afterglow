// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Keybinds.h"
#include "../shaders/ShaderLibrary.h"

#include <AfterglowRender/FontSet.h>
#include <AfterglowVt/Terminal.h>

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

namespace PhosphorConfig {
class JsonBackend;
class Store;
} // namespace PhosphorConfig

namespace Afterglow {

struct WindowSettings
{
    int padding = 6; ///< logical pixels around each pane's grid
    int initialColumns = 100;
    int initialRows = 30;
    std::uint32_t dividerColor = 0xFF423A3A; ///< RGBA8 in memory order
    double unfocusedDim = 0.19;
    double backgroundOpacity = 1.0; ///< of the default background
    bool blur = false; ///< blur what shows through a translucent background (KWin)
    bool paneTitles = false; ///< label each split with its title
    bool operator==(const WindowSettings&) const = default;
};

struct QuickTerminalSettings
{
    double height = 0.4; ///< fraction of the screen
    QString shortcut = QStringLiteral("Meta+`"); ///< global shortcut; empty for none
    bool animate = true; ///< slide in and out
    bool operator==(const QuickTerminalSettings&) const = default;
};

struct ControlSettings
{
    bool enabled = true; ///< the D-Bus control interface (afterglow ctl)
    bool allowReadText = false; ///< let it read pane contents
    bool operator==(const ControlSettings&) const = default;
};

struct AnimationSettings
{
    bool cursor = true; ///< the cursor glides between cells
    bool scroll = true; ///< scrolling eases instead of jumping by rows
    bool splits = true; ///< splits open, close and resize smoothly
    bool operator==(const AnimationSettings&) const = default;
};

/// Everything afterglow reads from its config file.
struct Settings
{
    AfterglowRender::FontSpec font;
    AfterglowVt::TerminalOptions terminal;
    WindowSettings window;
    AnimationSettings animation;
    QuickTerminalSettings quickTerminal;
    ControlSettings control;
    bool scripts = true; ///< run Luau scripts from the scripts directories
    QString shell; ///< empty: the user's login shell
    QString term = QStringLiteral("xterm-256color");
    bool copyOnSelect = true;
    enum class Bell { None, Attention, Visual, Both };
    Bell bell = Bell::Attention;
    bool notifications = true; ///< OSC 9 / 777 desktop notifications
    bool confirmUnsafePaste = true; ///< ask before pasting lines a shell would run
    Keybinds keybinds = Keybinds::defaults();
    /// Shader references (pack ids or file paths) for each chain, applied in order.
    ShaderLibrary::Refs shaders;
    /// Reference -> { parameter: value }.
    QVariantMap shaderParams;
};

/// The config file (~/.config/afterglow/config.json) on phosphor-config,
/// with defaults written on first run and live reload when the file changes.
class SettingsStore : public QObject
{
    Q_OBJECT

public:
    explicit SettingsStore(QObject* parent = nullptr);
    SettingsStore(const QString& path, QObject* parent = nullptr);
    ~SettingsStore() override;

    static QString defaultPath();

    const Settings& settings() const noexcept { return m_settings; }
    QString path() const { return m_path; }

    /// Re-reads the file. A file that does not parse keeps the current settings.
    void reload();

Q_SIGNALS:
    /// Emitted after a reload changed anything; settings() holds the new values.
    void changed(const Settings& previous);

private:
    void watch();
    Settings read() const;

    QString m_path;
    std::unique_ptr<PhosphorConfig::JsonBackend> m_backend;
    std::unique_ptr<PhosphorConfig::Store> m_store;
    Settings m_settings;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};

} // namespace Afterglow
