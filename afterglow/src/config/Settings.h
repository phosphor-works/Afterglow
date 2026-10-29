// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Keybinds.h"

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
    bool operator==(const WindowSettings&) const = default;
};

/// Everything afterglow reads from its config file.
struct Settings
{
    AfterglowRender::FontSpec font;
    AfterglowVt::TerminalOptions terminal;
    WindowSettings window;
    QString shell; ///< empty: the user's login shell
    QString term = QStringLiteral("xterm-256color");
    bool copyOnSelect = true;
    Keybinds keybinds = Keybinds::defaults();
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
