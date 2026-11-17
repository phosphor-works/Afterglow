// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Keybinds.h"
#include "KeyModes.h"
#include "ModifierRemap.h"
#include "NotificationFilter.h"
#include "Theme.h"
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

struct WindowSettings
{
    int padding = 6; ///< logical pixels around each pane's grid
    int initialColumns = 100;
    int initialRows = 30;
    /// Unset, a fifth of the way from the background to the text in use:
    /// here the defaults, so that a light background without a theme does
    /// not get a dark divider.
    static inline const std::uint32_t DefaultDividerColor = [] {
        const AfterglowVt::TerminalOptions defaults;
        return AfterglowRender::packRgba(Themes::derivedDivider(defaults.defaultBackground, defaults.defaultForeground));
    }();
    std::uint32_t dividerColor = DefaultDividerColor; ///< RGBA8 in memory order
    /// Unset, halfway from the background to the text in use: here the
    /// defaults.
    static inline const std::uint32_t DefaultPopupBorderColor = [] {
        const AfterglowVt::TerminalOptions defaults;
        return AfterglowRender::packRgba(Themes::derivedPopupBorder(defaults.defaultBackground, defaults.defaultForeground));
    }();
    std::uint32_t popupBorderColor = DefaultPopupBorderColor; ///< RGBA8 in memory order
    double unfocusedDim = 0.19;
    double popupDim = 0.35; ///< over a pane behind a popup overlay
    double backgroundOpacity = 1.0; ///< of the default background
    bool blur = false; ///< blur what shows through a translucent background (KWin)
    bool paneTitles = false; ///< label each split with its title
    enum class PopupCorners { Rounded, Square };
    PopupCorners popupCorners = PopupCorners::Rounded; ///< of a popup overlay's border
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
    int tabBarHeight = 30; ///< logical pixels
    /// The history scrollbar: shown while scrolling or hovered, always, or never.
    enum class Scrollbar { Auto, Always, Never };
    Scrollbar scrollbar = Scrollbar::Auto;
    int scrollbarHideDelay = 1200; ///< milliseconds before an auto scrollbar fades
    int scrollbarWidth = 12; ///< the history scrollbar's width, logical pixels
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
    /// Milliseconds between the rows a selection scrolls while it is
    /// extended past the top or bottom of its pane.
    int autoScrollInterval = 50;
    /// The part of a finger scroll's speed that its coast keeps every 8 ms
    /// after the last finger lifts, on a touchpad or a touchscreen: 0.96, as
    /// in Kitty (momentum_scroll). 0 turns the coast off, and 1 never slows it.
    double momentumScroll = 0.96;
    /// Multiplies the distance of a finger scroll on a touchpad or a
    /// touchscreen: 1, as in Kitty (touch_scroll_multiplier). A negative value
    /// scrolls the other way. A program that reads the mouse gets the sign only.
    double touchScrollMultiplier = 1.0;
    /// Milliseconds that a finger holds still on a touchscreen before a move
    /// selects instead of scrolling: 400, as in Kitty.
    int touchSelectHold = 400;
    /// A single press inside the selection and a move drag its text out, to
    /// another pane or another application. A click there without a move
    /// clears the selection. Off, as in Kitty: a press inside the selection
    /// selects as it does elsewhere.
    bool dragSelection = false;
    /// A click on the command line of a shell that marks its prompt
    /// (OSC 133) moves the shell's cursor there. On, as in Kitty.
    bool clickMovesCursor = true;
    /// Ctrl+Shift+right-click on a command that shell integration marks
    /// (OSC 133): its output in Terminal.pager, as in Kitty
    /// (mouse_show_command_output), its output selected
    /// (mouse_select_command_output), or a plain right-click.
    enum class CommandOutputClick { Pager, Select, None };
    CommandOutputClick commandOutputClick = CommandOutputClick::Pager;
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

struct MuxSettings
{
    /// What closing the window that shows a mux session, or its last tab or
    /// pane, does. Closing another tab or pane ends only its programs.
    enum class Close { Kill, Detach };
    /// Kill ends the session and its programs, as a window that is not in a
    /// session ends its programs. Detach keeps the session running. A quit
    /// of Afterglow always detaches, and so does the window of a project
    /// that opened into the session (TerminalWindow::keepSessionForProject).
    Close onClose = Close::Kill;
    /// The bytes of host calls (MuxHost.h) that wait for a slow window: the
    /// daemon keeps a queue of this size for the session and one for each
    /// pane. In MiB in the configuration. The default equals
    /// AfterglowMux::DefaultEventQueue, which ConfigTest checks.
    std::size_t eventQueue = std::size_t(4) << 20;
    /// The variables that a session of this computer takes from the
    /// Afterglow that shows it, as tmux's update-environment: set to
    /// Afterglow's value, or removed when Afterglow has none. A window sends
    /// them when it shows the session and at each change of the settings. A
    /// name that leaves the list keeps its value in the session. The panes
    /// that start later get them. At most AfterglowMux::MaxUpdatedVariables
    /// names. The default is tmux 3.8's without MSYSTEM, WINDOWID and the
    /// XDG_ desktop names.
    QStringList updateEnvironment{QStringLiteral("DISPLAY"),       QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("XAUTHORITY"),
                                  QStringLiteral("SSH_AUTH_SOCK"), QStringLiteral("SSH_AGENT_PID"),   QStringLiteral("SSH_CONNECTION"),
                                  QStringLiteral("SSH_ASKPASS"),   QStringLiteral("KRB5CCNAME")};
    /// The command line that ssh gives the login shell of another host for
    /// its sessions (`<remoteCommand> proxy`): a name that the shell finds,
    /// or a path there, which the shell can expand. Never empty: the read
    /// gives the default for an empty value.
    QString remoteCommand = QStringLiteral("aglow-mux");
    /// Seconds that the afterglow mux commands wait for the first answer
    /// through a proxy of the daemon of another host: ssh may ask for a
    /// password or a passphrase first. A window waits
    /// AfterglowMux::CallTimeoutMs, with ssh in BatchMode, which asks for
    /// nothing.
    int signInTimeout = 120;
    /// The most notifications that each pane of a session keeps while no
    /// window shows the session, for the next window. The default equals
    /// AfterglowMux::DefaultKeptNotifications, which ConfigTest checks.
    int keptNotifications = 8;
    bool operator==(const MuxSettings&) const = default;
};

struct ControlSettings
{
    /// The "control" target on the process's socket (afterglow ctl) and,
    /// with Scripts.enabled, the "commands" target (aglow run, afterglow run)
    /// and the remote sockets that afterglow ssh forwards
    /// (Terminal.sshRemoteCommands). The mux daemon follows this, and
    /// allowReadText and eventInterval, of the window that showed a session
    /// last (sessionSettings in MuxSessions.cpp).
    bool enabled = true;
    bool allowReadText = false; ///< let it read pane contents
    /// Let a script command that runs from a remote host read pane text,
    /// and let other handlers write to such a run.
    bool allowRemoteReadText = false;
    int eventInterval = 100; ///< ms between two events of one kind for one pane, 0 sends every event
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
    int contextDuration = 1000; ///< a pane's portal to another host, user or container (Shaders.paneEffects)
    /// An overlay's fade in and out over its pane (Shaders.overlayTransition),
    /// as splitCurve. The fade stops the first time it reaches its target, so
    /// a curve that overshoots never bounces back.
    QString overlayCurve = QStringLiteral("0.4,0,0.2,1");
    int overlayDuration = 180;
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
/// The Hints.patterns entries that Afterglow uses, and the arguments of a command action.
inline constexpr qsizetype MaxHintPatterns = 16;
inline constexpr qsizetype MaxHintCommandArguments = 32;
/// The highest Scripts.commandStdinLimit, in KiB. CommandSocket sizes its
/// cap on waiting stdin from it.
inline constexpr int MaxScriptCommandStdinLimitKiB = 4096;

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
    MuxSettings mux;
    MouseSettings mouse;
    LinkSettings links;
    HighlightColors colors;
    double fontSizeStep = 1.0; ///< points that font_bigger and font_smaller add or take away
    bool scriptsAllowReadText = true; ///< scripts may read pane text, selections and semantic regions
    bool scripts = true; ///< run Luau scripts from the scripts directories
    int scriptCommandTimeout = 600000; ///< milliseconds that one command run may take
    /// Bytes of standard input that a command run may get; more refuses the
    /// run. The setting is in KiB. This field and ScriptHost::CommandLimits are in bytes.
    qint64 scriptCommandStdinLimit = qint64(1024) << 10;
    int scriptCommandRunLimit = 16; ///< command runs of one script at once
    /// Bytes of UTF-8 standard output and standard error of one command run (the setting is in KiB).
    qint64 scriptCommandOutputLimit = qint64(16384) << 10;
    int scriptCpuBudget = 250; ///< ms of handler time of one script in each second
    QString pager = QStringLiteral("less -R");
    std::size_t lastOutputLimit = std::size_t(1024) << 10; ///< bytes of the last output that show_last_output hands the pager
    /// Terminal.shellIntegration: "enabled" or "disabled", or the switches
    /// that turn parts off, as Kitty's shell_integration ("no-title no-sudo").
    QString shellIntegration = QStringLiteral("enabled");
    bool sshIntegration = true; ///< integrated shells run ssh through `afterglow ssh`
    QString sshTerminfo = QStringLiteral("install"); ///< TERM=afterglow on a host without its entry: install or fallback
    bool sshRemoteCommands = false; ///< `afterglow ssh` forwards a socket for the commands marked remote
    /// Seconds of ControlPersist of Afterglow's own ssh master for the mux
    /// sessions of another host (Ssh::sharedCommand): `afterglow mux --ssh`
    /// and the windows of those sessions. `afterglow ssh` makes another
    /// master, which this does not change.
    int sshControlPersist = 10;
    QString shell; ///< empty: the user's login shell
    QString term = QStringLiteral("xterm-256color");
    /// Where a finished selection is copied: nowhere, the primary selection
    /// (middle-click paste), or the clipboard too (Ctrl+V anywhere).
    enum class CopyOnSelect { Off, Primary, Clipboard };
    CopyOnSelect copyOnSelect = CopyOnSelect::Primary;
    enum class Bell { None, Attention, Visual, Both };
    Bell bell = Bell::Attention;
    /// OSC 9, 99 and 777 notifications, and OSC 7501 status notifications
    /// (Notifications.enabled).
    bool notifications = true;
    /// Notifications one pane may show in notificationRateWindow; 0 for no
    /// limit. Afterglow's own notices do not count (ctl notify and the notify
    /// effect of scripts among them), except the notice at the end of a
    /// script command run without a client (NotificationCenter::showLimitedNotice).
    int notificationRateLimit = 20;
    int notificationRateWindow = 10; ///< seconds that notificationRateLimit counts over
    /// Milliseconds between two program status notifications of one pane.
    int statusNotificationInterval = 3000;
    /// Notifications.filter: a notification that one of these matches is not shown.
    std::vector<NotificationFilter> notificationFilters;
    bool confirmUnsafePaste = true; ///< ask before pasting or dropping lines a shell would run
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
    int bellSoundInterval = 250; ///< the fewest milliseconds between two bell sounds
    /// Variables added to the shell's environment.
    std::vector<std::pair<std::string, std::string>> environment;
    QString fileTransferPassword;
    /// Bytes that a delta or compressed upload without a declared size can
    /// write (kitten transfer declares none). 0 sets no limit.
    qint64 fileTransferDeltaLimit = qint64(64) << 30;
    /// Milliseconds that a drag over a pane waits for the answer of a program
    /// that uses Kitty drag and drop, when the drag shows it new types.
    int dragAnswerWait = 100;
    /// Minutes that the files of a remote drag remain after its drop, for a
    /// drop target that copies them later.
    int dragTreeLifetime = 10;
    enum class Permission { Allow, Ask, Deny };
    /// Programs reading the clipboard (OSC 52, OSC 5522).
    Permission clipboardRead = Permission::Ask;
    /// Seconds after a paste in which its password lets a program read the
    /// clipboard (OSC 5522 paste events).
    int pasteReadTimeout = 10;
    /// Programs writing the clipboard (OSC 52, OSC 5522).
    Permission clipboardWrite = Permission::Allow;
    /// Files that programs send with iTerm2's protocol (OSC 1337, it2dl).
    Permission downloads = Permission::Ask;
    /// Where downloads go: empty for the desktop's download folder.
    QString downloadDirectory;
    /// Files that programs ask for with iTerm2's protocol (OSC 1337
    /// RequestUpload, it2ul): Ask opens a file picker, Deny refuses.
    Permission uploads = Permission::Ask;
    /// The largest upload, in MiB of compressed archive: its answer goes to
    /// the program's input in one piece, which holds 32 MiB.
    int uploadLimit = 16;
    static constexpr int MaxUploadLimit = 23; ///< its answer, with base64 and line ends, fits in 32 MiB
    // Terminal.graphicsFileTransfer, Terminal.allowScreenReadback and
    // Graphics.quota live in terminal, which enforces them.
    /// Each a map of pattern, action and command, checked when read.
    QVariantList hintPatterns = defaultHintPatterns();
    /// Keys that label hints and URL hints, home row first. Unique, at least two.
    QString hintAlphabet = QStringLiteral("asdfjklghqweruiopzxcvbnmty");
    KeyModes keyModes;
    Keybinds keybinds = Keybinds::defaults();
    /// Keybinds.remapModifiers: none by default, as in kitty.
    ModifierRemap modifierRemap;
    /// Shader references for each chain, applied in order: a pack's directory
    /// name (normally its id), or "<package>.<pack>" for package packs.
    ShaderRefs shaders{.overview = QStringLiteral("pane-deck"),
                       .tabTransition = QStringLiteral("tab-switch"),
                       .paneEffects = QStringLiteral("pane-effects"),
                       .overlayTransition = QStringLiteral("overlay-fade")};
    /// Reference -> { parameter: value }.
    QVariantMap shaderParams;
    bool operator==(const Settings&) const = default;
};

/// The config file (~/.config/afterglow/config.json) on phosphor-config,
/// with live reload when the file changes. The file holds only the values
/// that differ from the defaults; a missing file means every default, and so
/// does an empty one that held no settings (an empty file that held settings
/// is a save in progress and keeps them), and
/// the first write() or ensureFile() creates it, readable by the owner only.
/// A file that cannot be read at the start gives every default, but with
/// Scripts.enabled and Control.enabled off until it can be read: the file may
/// have turned them off. An empty file that follows it keeps them off too, as
/// it may be a save in progress. A -o override of either with a boolean
/// still wins. A link whose target is gone counts as such a file:
/// PhosphorConfig cannot open it.
class SettingsStore : public QObject
{
    Q_OBJECT

public:
    explicit SettingsStore(QObject* parent = nullptr);
    explicit SettingsStore(const QString& path, QObject* parent = nullptr);
    /// @p overrides ({ "Font": { "size": 14 } }) win over the file, which
    /// is still watched (afterglow -o Font.size=14).
    SettingsStore(const QString& path, const QJsonObject& overrides, QObject* parent = nullptr);
    ~SettingsStore() override;

    /// A theme from the desktop's colours follows a new application palette,
    /// and a font from the desktop follows a new desktop font.
    bool eventFilter(QObject* watched, QEvent* event) override;

    static QString defaultPath();
    /// Machine-readable reference generated from the runtime Phosphor schema.
    static QJsonObject reference();
    /// Every group, key, default and validator. The store builds its own;
    /// this one is for the tests that check the schema against the structs.
    static PhosphorConfig::Schema schema();

    const Settings& settings() const noexcept { return m_settings; }
    QString path() const { return m_path; }
    /// Creates the config file if it does not exist, holding {} and readable
    /// by the owner only, so that an editor can open it. False if it cannot.
    bool ensureFile();
    /// The files whose change reloads: the config file, every place its
    /// theme could come from and, for a plasma theme, the kdeglobals files.
    /// The set follows each that is a symbolic link itself, so a link target
    /// is not in this list. Only the tests read it: the store watches them
    /// itself.
    QStringList watchedPaths() const { return m_files.paths(); }
    /// What the last read could not use: a file that does not parse, unknown
    /// keys, bad values, missing themes. Each is logged when it appears.
    QStringList problems() const { return m_fileProblems + m_readProblems; }

    /// Re-reads the file. A file that cannot be read, does not parse or does
    /// not hold a JSON object keeps the current settings, also after an
    /// empty file kept them; a deleted file, or one that is empty or only
    /// white space and held no settings, means every default. A file that
    /// held settings and is empty now keeps them, as an editor that saves in
    /// place empties it first. Until the file is read once, an empty file
    /// keeps Scripts.enabled and Control.enabled off, as the unreadable
    /// file did.
    void reload();
    /// Writes one key to the config file and reloads. A value equal to the
    /// default removes the key. Command-line overrides keep winning over the
    /// file. False for an undeclared key, or when the file cannot be read
    /// back (it would lose what it holds) or written. A write that the file
    /// refuses still applies what the file holds.
    bool write(const QString& group, const QString& key, const QVariant& value);
    /// What the file holds now for one key, or its default: the value without
    /// command-line overrides. A write built on a current value (a shader
    /// chain that gains a pack) starts from this, so that an override never
    /// reaches the file, and an edit that is not reloaded yet is not lost.
    /// It reads the value as read() does: a value of another JSON type is
    /// the default, and a number of an integer key loses its fraction. A
    /// list keeps its entries as the file holds them; the caller skips those
    /// that read() skips. An invalid QVariant for an undeclared key.
    QVariant persisted(const QString& group, const QString& key) const;

    /// "Group.key=value" as an override; the value is JSON when it parses
    /// (14, true, ["calt"]), else a string. It replaces the file's value as a
    /// whole, also for a map such as Terminal.env. Returns false if malformed
    /// or if the schema has no such key.
    static bool addOverride(QJsonObject& overrides, const QString& assignment);

Q_SIGNALS:
    /// Emitted when settings() changed: after a reload of the file or a
    /// theme, a write(), a new desktop color scheme, palette or font.
    void changed(const Settings& previous);
    /// Emitted once for each read whose problems() differ from the last.
    void problemsChanged(const QStringList& problems);

private:
    /// Has the backend read and parse the file again (JsonBackend::reload),
    /// so that the document checked is the document used. False, with the
    /// reason in problems(), when the current settings must stay.
    bool loadFile();
    /// Reads the backend's document, emitting changed() if that changes anything.
    void apply();
    /// apply() once, after Qt has delivered the desktop's events of one change.
    void queueApply();
    void watch();
    Settings read();
    /// Emits problemsChanged() if problems() differ from the last published
    /// (or the startup) list.
    void publishProblems();
    /// The desktop prefers dark colours (for "light:a,dark:b" themes).
    bool darkScheme() const;
    /// The directory of the config file, where its themes directory is.
    QString configDirectory() const;

    QString m_path;
    QJsonObject m_overrides;
    std::unique_ptr<PhosphorConfig::JsonBackend> m_backend;
    std::unique_ptr<PhosphorConfig::Store> m_store;
    Settings m_settings;
    /// The last theme that parsed, kept while its file is saved half-written.
    QString m_themeFile;
    std::shared_ptr<const Theme> m_theme;
    /// The theme name the last read() loaded from (NAME of "plasma:NAME"),
    /// and whether it laid the desktop's colours over it. watch() follows them.
    QString m_themeName;
    bool m_plasmaTheme = false;
    /// Font.family or Font.size comes from the desktop's fixed-width font.
    bool m_desktopFont = false;
    /// An apply() for the desktop events is posted and has not run yet.
    bool m_applyQueued = false;
    /// No read of the file has worked since the start: read() keeps
    /// Scripts.enabled and Control.enabled off, which the file may set,
    /// unless an override sets them to true or false.
    bool m_neverRead = true;
    QStringList m_fileProblems;
    QStringList m_readProblems;
    QStringList m_publishedProblems;
    /// The config file, every place its theme could come from, and
    /// kdeglobals (watchedPaths()). It follows the links among them itself.
    PhosphorFsLoader::WatchedFileSet m_files;
};

} // namespace Afterglow
