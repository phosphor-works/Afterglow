// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Keybinds.h"
#include "KeyModes.h"
#include "../audio/AudioSettings.h"
#include "../shaders/ShaderLibrary.h"

#include <AfterglowRender/FontSet.h>
#include <AfterglowVt/Terminal.h>

#include <PhosphorFsLoader/WatchedFileSet.h>

#include <QJsonObject>
#include <QObject>
#include <QString>

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
    /// Where new tabs, splits and windows start: "inherit" (the focused
    /// pane's directory, or the launcher's), "home", or a path.
    QString workingDirectory = QStringLiteral("inherit");
    enum class StartMode { Normal, Maximized, Fullscreen };
    StartMode startMode = StartMode::Normal;
    bool decorations = true; ///< the compositor's title bar and borders
    bool balancePadding = false; ///< share the space a grid leaves over between opposite edges
    /// The tab bar: shown while there are several tabs, always, or never.
    enum class TabBar { Auto, Always, Never };
    TabBar tabBar = TabBar::Auto;
    /// The history scrollbar: shown while scrolling or hovered, always, or never.
    enum class Scrollbar { Auto, Always, Never };
    Scrollbar scrollbar = Scrollbar::Auto;
    int scrollbarHideDelay = 1200; ///< milliseconds before an auto scrollbar fades
    bool operator==(const WindowSettings&) const = default;
};

struct MouseSettings
{
    /// Right-click: the context menu, paste, copy a selection (or paste
    /// without one), or extend the selection to the click.
    enum class RightClick { Menu, Paste, CopyOrPaste, Extend };
    RightClick rightClick = RightClick::Menu;
    /// Middle-click pastes the primary selection, the clipboard, or nothing.
    enum class MiddleClick { Primary, Clipboard, None };
    MiddleClick middleClick = MiddleClick::Primary;
    bool operator==(const MouseSettings&) const = default;
};

struct LinkSettings
{
    bool detectUrls = true; ///< plain-text URLs, besides OSC 8 links
    QString openWith; ///< command the URL is appended to; empty for the desktop's choice
    /// Held to hover and click links; none opens them with a plain click.
    Qt::KeyboardModifiers modifier = Qt::ControlModifier;
    bool operator==(const LinkSettings&) const = default;
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
    /// The split motion: a Phosphor curve spec, such as a spring or a
    /// cubic Bézier. Springs settle by themselves; other curves take
    /// splitDuration milliseconds.
    QString splitCurve = QStringLiteral("spring:22,1");
    int splitDuration = 250;
    bool operator==(const AnimationSettings&) const = default;
};

/// Everything afterglow reads from its config file.
struct Settings
{
    AfterglowRender::FontSpec font;
    AfterglowVt::TerminalOptions terminal;
    WindowSettings window;
    AnimationSettings animation;
    AudioSettings audio;
    QuickTerminalSettings quickTerminal;
    ControlSettings control;
    MouseSettings mouse;
    LinkSettings links;
    bool scriptsAllowReadText = true;
    bool scripts = true; ///< run Luau scripts from the scripts directories
    QString pager = QStringLiteral("less -R");
    bool shellIntegration = true;
    bool sshIntegration = true; ///< integrated shells run ssh through `afterglow ssh`
    QString sshTerminfo = QStringLiteral("install"); ///< TERM=afterglow on a host without its entry: install or fallback
    QString shell; ///< empty: the user's login shell
    QString term = QStringLiteral("xterm-256color");
    /// Where a finished selection is copied: nowhere, the primary selection
    /// (middle-click paste), or the clipboard too (Ctrl+V anywhere).
    enum class CopyOnSelect { Off, Primary, Clipboard };
    CopyOnSelect copyOnSelect = CopyOnSelect::Primary;
    enum class Bell { None, Attention, Visual, Both };
    Bell bell = Bell::Attention;
    bool notifications = true; ///< OSC 9 / 777 desktop notifications
    bool confirmUnsafePaste = true; ///< ask before pasting lines a shell would run
    bool confirmClose = true; ///< ask before closing where a program is running
    bool hideMouseWhileTyping = false;
    int scrollLines = 3; ///< lines per mouse wheel notch
    int cursorBlinkInterval = 530; ///< milliseconds per blink phase
    bool scrollOnOutput = false; ///< new output jumps a scrolled-back pane to the bottom
    bool scrollOnInput = true; ///< typing and pasting jump to the bottom
    /// The bell's sound: empty for none, "default" for the desktop sound
    /// theme's, or a sound file.
    QString bellSound;
    /// Variables added to the shell's environment.
    std::vector<std::pair<std::string, std::string>> environment;
    /// The theme file the colours came from; empty when none is set or found.
    QString themePath;
    QString fileTransferPassword;
    /// Programs reading the clipboard with OSC 52: "ask", "allow" or "deny".
    QString clipboardRead = QStringLiteral("ask");
    QVariantList hintPatterns;
    KeyModes keyModes;
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
    /// @p overrides ({ "Font": { "size": 14 } }) win over the file, which
    /// is still watched (afterglow -o Font.size=14).
    SettingsStore(const QString& path, const QJsonObject& overrides, QObject* parent = nullptr);
    ~SettingsStore() override;

    static QString defaultPath();
    /// Machine-readable reference generated from the runtime Phosphor schema.
    static QJsonObject reference();

    const Settings& settings() const noexcept { return m_settings; }
    QString path() const { return m_path; }

    /// Re-reads the file. A file that does not parse keeps the current settings.
    void reload();

    /// "Group.key=value" as an override; the value is JSON when it parses
    /// (14, true, ["calt"]), else a string. Returns false if malformed.
    static bool addOverride(QJsonObject& overrides, const QString& assignment);

Q_SIGNALS:
    /// Emitted after a reload changed anything; settings() holds the new values.
    void changed(const Settings& previous);

private:
    void watch();
    Settings read() const;
    /// The desktop prefers dark colours (for "light:a,dark:b" themes).
    bool darkScheme() const;

    QString m_path;
    QJsonObject m_overrides;
    std::unique_ptr<PhosphorConfig::JsonBackend> m_backend;
    std::unique_ptr<PhosphorConfig::Store> m_store;
    Settings m_settings;
    PhosphorFsLoader::WatchedFileSet m_files; ///< the config file and its theme
};

} // namespace Afterglow
