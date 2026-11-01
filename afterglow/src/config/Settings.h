// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Keybinds.h"
#include "KeyModes.h"
#include "../audio/AudioSettings.h"
#include "../shaders/ShaderRefs.h"

#include <AfterglowRender/FontSet.h>
#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/Terminal.h>

#include <PhosphorFsLoader/WatchedFileSet.h>

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace PhosphorConfig {
class JsonBackend;
class Store;
struct Schema;
} // namespace PhosphorConfig

namespace Afterglow {

struct Theme;

struct WindowSettings
{
    int padding = 6; ///< logical pixels around each pane's grid
    int initialColumns = 100;
    int initialRows = 30;
    static constexpr AfterglowVt::Rgb DefaultDivider{0x3a, 0x3a, 0x42};
    std::uint32_t dividerColor = AfterglowRender::packRgba(DefaultDivider); ///< RGBA8 in memory order
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
    /// Lower-case URL schemes that open without asking; other links ask first.
    QStringList schemes{QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("mailto"), QStringLiteral("ftp"),
                        QStringLiteral("file")};
    /// Keys that label hints, home row first. Unique, at least two.
    QString hintAlphabet = QStringLiteral("asdfjklghqweruiopzxcvbnmty");
    bool operator==(const LinkSettings&) const = default;
};

struct QuickTerminalSettings
{
    double height = 0.4; ///< fraction of the screen
    QString shortcut = QStringLiteral("Meta+`"); ///< global shortcut; empty for none
    bool animate = true; ///< slide in and out
    QString curve = QStringLiteral("spring:22,1"); ///< the slide, as AnimationSettings::splitCurve
    int duration = 150; ///< milliseconds of the slide for curves other than springs
    /// The screen edge it slides from.
    enum class Position { Top, Bottom };
    Position position = Position::Top;
    QString screen; ///< output name (QScreen::name); empty for the primary screen
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
    /// The pane overview's motion, as splitCurve; other curves take overviewDuration.
    QString overviewCurve = QStringLiteral("spring:12,0.82");
    int overviewDuration = 450;
    /// Tab switches (Shaders.tabTransition), as splitCurve.
    QString tabCurve = QStringLiteral("spring:13,0.9");
    int tabDuration = 400;
    int paneEffectDuration = 420; ///< panes opening and closing (Shaders.paneEffects)
    /// The cursor's glide, the scroll easing and the visual bell's fade, as
    /// splitCurve. Other curves take the matching duration.
    QString cursorCurve = QStringLiteral("spring:48,1");
    int cursorDuration = 150;
    QString scrollCurve = QStringLiteral("spring:26,1");
    int scrollDuration = 150;
    QString bellCurve = QStringLiteral("spring:14,1");
    int bellDuration = 150;
    bool operator==(const AnimationSettings&) const = default;
};

/// Colours of afterglow's own highlights. A theme may set each one.
struct HighlightColors
{
    AfterglowVt::Rgb searchMatch{0xe5, 0xc0, 0x7b}; ///< background of search matches
    AfterglowVt::Rgb searchCurrentMatch{0xff, 0x8c, 0x42}; ///< background of the current match
    AfterglowVt::Rgb searchMatchText{0x10, 0x10, 0x10}; ///< text of every match
    /// The visual bell's flash. A colour a program sets with OSC 21
    /// (visual_bell) wins over it.
    AfterglowVt::Rgb visualBell{0xff, 0xff, 0xff};
    double visualBellIntensity = 0.18; ///< opacity of the flash at its start, 0-1
    bool operator==(const HighlightColors&) const = default;
};

/// The default Hints.patterns: file and line references, copied.
QVariantList defaultHintPatterns();

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
    HighlightColors colors;
    double fontSizeStep = 1.0; ///< points that font_bigger and font_smaller add or take away
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
    /// Notifications one pane may show in 10 seconds; 0 for no limit.
    /// Afterglow's own notices do not count.
    int notificationRateLimit = 20;
    bool confirmUnsafePaste = true; ///< ask before pasting lines a shell would run
    bool confirmClose = true; ///< ask before closing where a program is running
    bool hideMouseWhileTyping = false;
    int scrollLines = 3; ///< lines per mouse wheel notch
    int cursorBlinkInterval = 530; ///< milliseconds per blink phase
    int textBlinkInterval = 500; ///< milliseconds per phase of blinking text (SGR 5)
    bool scrollOnOutput = false; ///< new output jumps a scrolled-back pane to the bottom
    bool scrollOnInput = true; ///< typing and pasting jump to the bottom
    /// The bell's sound: empty for none, "default" for the desktop sound
    /// theme's, or a sound file.
    QString bellSound;
    /// Variables added to the shell's environment.
    std::vector<std::pair<std::string, std::string>> environment;
    QString fileTransferPassword;
    /// Programs reading the clipboard with OSC 52: "ask", "allow" or "deny".
    QString clipboardRead = QStringLiteral("ask");
    enum class Permission { Allow, Ask, Deny };
    /// Programs writing the clipboard (OSC 52, OSC 5522).
    Permission clipboardWrite = Permission::Allow;
    /// Kitty graphics read from a file, a temporary file or shared memory
    /// (t=f, t=t, t=s): Allow or Deny.
    Permission graphicsFileTransfer = Permission::Allow;
    /// DECRQCRA checksum replies, which let any program read the screen back.
    bool allowScreenReadback = false;
    std::size_t graphicsQuota = std::size_t(320) << 20; ///< bytes of kitty images kept per terminal
    /// Each a map of pattern, action and command, checked when read.
    QVariantList hintPatterns = defaultHintPatterns();
    KeyModes keyModes;
    Keybinds keybinds = Keybinds::defaults();
    /// Shader references for each chain, applied in order: pack ids, or
    /// "<package>.<pack>" for package packs.
    ShaderRefs shaders{{}, {}, {}, {}, {}, QStringLiteral("pane-deck"), QStringLiteral("tab-switch"),
                       QStringLiteral("pane-effects")};
    /// Reference -> { parameter: value }.
    QVariantMap shaderParams;
    bool operator==(const Settings&) const = default;
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
    /// Every group, key, default and validator.
    static PhosphorConfig::Schema schema();

    const Settings& settings() const noexcept { return m_settings; }
    QString path() const { return m_path; }
    /// What the last read could not use: a file that does not parse, unknown
    /// keys, bad values, missing themes. Each is also logged.
    QStringList problems() const { return m_fileProblems + m_readProblems; }

    /// Re-reads the file. A file that cannot be read, does not parse or does
    /// not hold a JSON object keeps the current settings; a deleted file
    /// means every default.
    void reload();
    /// Writes one key to the config file and reloads. A value equal to the
    /// default removes the key. Command-line overrides keep winning over the
    /// file. False for an undeclared key, or when the file cannot be read
    /// back (it would lose what it holds) or written.
    bool write(const QString& group, const QString& key, const QVariant& value);

    /// "Group.key=value" as an override; the value is JSON when it parses
    /// (14, true, ["calt"]), else a string. Returns false if malformed or if
    /// the schema has no such key.
    static bool addOverride(QJsonObject& overrides, const QString& assignment);

Q_SIGNALS:
    /// Emitted after a reload changed anything; settings() holds the new values.
    void changed(const Settings& previous);
    void problemsChanged(const QStringList& problems);

private:
    /// Parses the file once and hands the document to the backend. False,
    /// with the reason in problems(), when the current settings must stay.
    bool loadFile();
    /// Reads the backend's document, emitting changed() if that changes anything.
    void apply();
    void watch();
    Settings read();
    void setProblems(QStringList* list, const QStringList& problems);
    /// The desktop prefers dark colours (for "light:a,dark:b" themes).
    bool darkScheme() const;

    QString m_path;
    QJsonObject m_overrides;
    std::unique_ptr<PhosphorConfig::JsonBackend> m_backend;
    std::unique_ptr<PhosphorConfig::Store> m_store;
    Settings m_settings;
    /// The last theme that parsed, kept while its file is saved half-written.
    QString m_themeFile;
    std::shared_ptr<const Theme> m_theme;
    QStringList m_fileProblems;
    QStringList m_readProblems;
    PhosphorFsLoader::WatchedFileSet m_files; ///< the config file and its theme
};

} // namespace Afterglow
