// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Control: new panes, tabs and windows, for scripts and control clients.

#include "Control.h"

#include "../Pane.h"
#include "../PaneLaunchOptions.h"
#include "../TerminalWindow.h"

#include <QDir>
#include <QFileInfo>

#include <cmath>
#include <string_view>

namespace Afterglow {

QString Control::localDirectory(QString& cwd)
{
    if (cwd == QLatin1String("~") || cwd.startsWith(QLatin1String("~/")))
        cwd = QDir::homePath() + cwd.mid(1);
    // A leading /, not QDir::isAbsolutePath: that takes a Qt resource path
    // (":/"), which no program can start in.
    if (!cwd.isEmpty() && (!cwd.startsWith(QLatin1Char('/')) || !QFileInfo(cwd).isDir()))
        return QStringLiteral("cwd must name an existing absolute directory");
    return {};
}

namespace {

QString launchOptions(const QString& cwd, const QStringList& command, const QVariantMap& input, PaneLaunchOptions& out)
{
    out.cwd = cwd;
    out.command = command;
    if (input.contains(QStringLiteral("cwd"))) {
        if (input.value(QStringLiteral("cwd")).metaType().id() != QMetaType::QString)
            return QStringLiteral("cwd must be a string");
        out.cwd = input.value(QStringLiteral("cwd")).toString();
    }
    if (out.cwd.contains(QChar::Null))
        return QStringLiteral("cwd contains a null character");
    for (const auto& arg : command)
        if (arg.contains(QChar::Null))
            return QStringLiteral("command contains a null character");
    if (!command.isEmpty() && command.first().isEmpty())
        return QStringLiteral("command must name a program");
    if (input.contains(QStringLiteral("size"))) {
        // A number only: toDouble also reads a string such as "0.5".
        const QVariant size = input.value(QStringLiteral("size"));
        const int type = size.metaType().id();
        const bool number = type == QMetaType::Double || type == QMetaType::Float || type == QMetaType::Int
            || type == QMetaType::UInt || type == QMetaType::LongLong || type == QMetaType::ULongLong;
        bool ok = false;
        out.fraction = number ? size.toDouble(&ok) : 0;
        if (!ok || !std::isfinite(out.fraction) || out.fraction <= 0 || out.fraction >= 1)
            return QStringLiteral("size must be a fraction between 0 and 1");
    }
    if (input.contains(QStringLiteral("env"))) {
        if (input.value(QStringLiteral("env")).metaType().id() != QMetaType::QVariantMap)
            return QStringLiteral("env must be a table");
        out.environment = input.value(QStringLiteral("env")).toMap();
        for (auto it = out.environment.cbegin(); it != out.environment.cend(); ++it) {
            if (it.key().isEmpty() || it.key().contains(QLatin1Char('=')) || it.key().contains(QChar::Null))
                return QStringLiteral("invalid environment name");
            const auto& value = it.value();
            if (value.metaType().id() == QMetaType::Bool && !value.toBool())
                continue;
            if (value.metaType().id() != QMetaType::QString || value.toString().contains(QChar::Null))
                return QStringLiteral("environment values must be strings or false");
        }
    }
    return {};
}

/// The options of a new pane in @p window: launchOptions, then a refusal
/// for a session of another host, whose panes take no cwd or env of this
/// computer, then the directory, which must exist here.
QString paneOptions(const TerminalWindow& window, const QString& cwd, const QStringList& command,
                    const QVariantMap& input, PaneLaunchOptions& out)
{
    if (const QString error = launchOptions(cwd, command, input, out); !error.isEmpty())
        return error;
    if (const QString error = window.remoteLaunchError(out); !error.isEmpty())
        return error;
    return Control::localDirectory(out.cwd);
}

/// The error of a launch that made no pane: the window's words, or
/// @p fallback when it gave none.
QString launchError(const QString& error, const QString& fallback)
{
    if (error == TerminalWindow::noRoomForPane())
        return Control::NoRoomForPane;
    return error.isEmpty() ? fallback : error;
}

} // namespace

/// What the options of a control client's split, newTab and newWindow ask
/// for besides those of a script's launch (cwd, size and env), as kitten @
/// launch does.
struct Control::LaunchExtras
{
    bool focus = true;
    bool hold = false;
    QVariantMap vars;
    std::optional<TextScope> stdinScope;
    bool stdinAnsi = false;
};

std::optional<SplitTree::Direction> Control::splitDirection(const QString& name)
{
    if (name == QLatin1String("right"))
        return SplitTree::Direction::Right;
    if (name == QLatin1String("left"))
        return SplitTree::Direction::Left;
    if (name == QLatin1String("up"))
        return SplitTree::Direction::Up;
    if (name == QLatin1String("down"))
        return SplitTree::Direction::Down;
    return std::nullopt;
}

QString Control::split(quint32 id, const QString& direction, const QStringList& command, const QVariantMap& input,
                       quint32& created)
{
    return splitWith(id, direction, command, input, nullptr, created);
}

QString Control::splitWith(quint32 id, const QString& direction, const QStringList& command, const QVariantMap& input,
                           const LaunchExtras* extras, quint32& created)
{
    created = 0;
    const auto side = splitDirection(direction);
    if (!side)
        return badDirection();
    TerminalWindow* window = nullptr;
    auto* pane = find(id, &window);
    if (!pane)
        return noPane(id);
    PaneLaunchOptions options;
    if (const auto error = prepareLaunch(*window, id, {}, command, input, extras, options); !error.isEmpty())
        return error;
    Pane* const before = window->focusedPane();
    QString error;
    auto* added = window->splitPane(pane, *side, options, &error);
    if (!added)
        return launchError(error, QStringLiteral("cannot split pane"));
    if (extras)
        afterLaunch(added, before, window, *extras);
    created = added->id();
    return {};
}

void Control::split(quint32 id, const QString& direction, const QStringList& command, const QVariantMap& input,
                    PaneMade done)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    // A window of its own starts the program at once.
    if (!pane || !window->showsSession()) {
        quint32 created = 0;
        const QString error = split(id, direction, command, input, created);
        return done(error, created);
    }
    const auto side = splitDirection(direction);
    if (!side)
        return done(badDirection(), 0);
    PaneLaunchOptions options;
    if (const auto error = paneOptions(*window, {}, command, input, options); !error.isEmpty())
        return done(error, 0);
    const QString refused = window->startSessionPane(pane, side, options, [done](Pane* added, const QString& error) {
        done(added ? QString() : launchError(error, QStringLiteral("cannot split pane")), added ? added->id() : 0);
    });
    if (!refused.isEmpty())
        done(launchError(refused, {}), 0);
}

void Control::newTab(quint32 id, const QString& cwd, const QStringList& command, const QVariantMap& input,
                     PaneMade done)
{
    TerminalWindow* window = nullptr;
    Pane* from = find(id, &window);
    if (!from || !window->showsSession()) {
        quint32 created = 0;
        const QString error = newTab(id, cwd, command, input, created);
        return done(error, created);
    }
    PaneLaunchOptions options;
    if (const auto error = paneOptions(*window, cwd, command, input, options); !error.isEmpty())
        return done(error, 0);
    // Without a directory, the tab starts where the pane is, as a split does.
    const QString refused =
        window->startSessionPane(from, std::nullopt, options, [done](Pane* added, const QString& error) {
            done(added ? QString() : launchError(error, QStringLiteral("cannot open a tab")), added ? added->id() : 0);
        });
    if (!refused.isEmpty())
        done(launchError(refused, {}), 0);
}

QString Control::newTab(quint32 id, const QString& cwd, const QStringList& command, quint32& created)
{
    return newTabWith(id, cwd, command, {}, nullptr, created);
}

QString Control::newTab(quint32 id, const QString& cwd, const QStringList& command, const QVariantMap& input,
                        quint32& created)
{
    return newTabWith(id, cwd, command, input, nullptr, created);
}

QString Control::newTabWith(quint32 id, const QString& cwd, const QStringList& command, const QVariantMap& input,
                            const LaunchExtras* extras, quint32& created)
{
    created = 0;
    TerminalWindow* window = nullptr;
    Pane* from = find(id, &window);
    if (!from)
        return noPane(id);
    PaneLaunchOptions options;
    if (const auto error = prepareLaunch(*window, id, cwd, command, input, extras, options); !error.isEmpty())
        return error;
    QString error;
    // Without a directory, the tab starts where the pane is, as a split does.
    // Without focus it opens behind the current tab: no switch there and
    // back, which would run the tab transition and tabSwitched twice.
    auto* added = window->openTab(options, &error, from, !extras || extras->focus);
    if (!added)
        return launchError(error, QStringLiteral("cannot open a tab"));
    if (extras)
        afterLaunch(added, nullptr, window, *extras);
    created = added->id();
    return {};
}

QString Control::newWindow(const QString& cwd, const QStringList& command, quint32& created)
{
    return newWindowWith(cwd, command, {}, nullptr, created);
}

QString Control::newWindow(const QString& cwd, const QStringList& command, const QVariantMap& input, quint32& created)
{
    return newWindowWith(cwd, command, input, nullptr, created);
}

QString Control::newWindowWith(const QString& cwd, const QStringList& command, const QVariantMap& input,
                               const LaunchExtras* extras, quint32& created)
{
    created = 0;
    PaneLaunchOptions options;
    if (const auto error = launchOptions(cwd, command, input, options); !error.isEmpty())
        return error;
    if (const auto error = localDirectory(options.cwd); !error.isEmpty())
        return error;
    if (extras)
        options.hold = extras->hold;
    auto* window = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!window)
        return QStringLiteral("cannot open a window");
    QString error;
    auto* added = window->openTab(options, &error);
    if (!added) {
        window->deleteLater();
        return launchError(error, QStringLiteral("cannot start the program"));
    }
    if (extras)
        afterLaunch(added, nullptr, window, *extras);
    created = added->id();
    showNew(window);
    return {};
}

QString Control::readLaunchExtras(const QVariantMap& options, const QStringList& command, LaunchExtras& extras)
{
    extras = {};
    const auto flag = [&options](const char* key, bool& out) -> QString {
        const QVariant value = options.value(QLatin1String(key));
        if (!value.isValid())
            return {};
        if (value.typeId() != QMetaType::Bool)
            return QStringLiteral("%1 is true or false").arg(QLatin1String(key));
        out = value.toBool();
        return {};
    };
    for (const char* key : {"focus", "hold", "stdin_ansi"}) {
        bool& out = key == std::string_view("focus") ? extras.focus
            : key == std::string_view("hold")        ? extras.hold
                                                     : extras.stdinAnsi;
        if (const QString error = flag(key, out); !error.isEmpty())
            return error;
    }
    if (const QVariant vars = options.value(QStringLiteral("vars")); vars.isValid()) {
        if (vars.typeId() != QMetaType::QVariantMap)
            return QStringLiteral("vars must be a table");
        extras.vars = vars.toMap();
        // A new pane takes this many: refused before the pane starts, not
        // after.
        if (std::size_t(extras.vars.size()) > AfterglowVt::ITerminalHost::MaxUserVars)
            return QStringLiteral("a pane has at most %1 user variables").arg(AfterglowVt::ITerminalHost::MaxUserVars);
        for (auto it = extras.vars.cbegin(); it != extras.vars.cend(); ++it) {
            // As OSC 1337 SetUserVar takes a name.
            if (const QString error = userVarName(it.key()); !error.isEmpty())
                return error;
            if (it.value().typeId() != QMetaType::QString)
                return QStringLiteral("user variable values must be strings");
        }
    }
    if (const QVariant scope = options.value(QStringLiteral("stdin_text")); scope.isValid()) {
        extras.stdinScope = scope.typeId() == QMetaType::QString ? scopeFromName(scope.toString()) : std::nullopt;
        if (!extras.stdinScope)
            return QStringLiteral("stdin_text is a text scope: ") + TextScopeNames;
        // A shell reads its standard input as commands: each line of the
        // pane's text would run.
        if (command.isEmpty())
            return QStringLiteral("stdin_text needs a command: the shell would run the text as commands");
    }
    if (extras.stdinAnsi && !extras.stdinScope)
        return QStringLiteral("stdin_ansi needs stdin_text");
    return {};
}

QString Control::prepareLaunch(const TerminalWindow& window, quint32 source, const QString& cwd,
                               const QStringList& command, const QVariantMap& input, const LaunchExtras* extras,
                               PaneLaunchOptions& options)
{
    if (const QString error = paneOptions(window, cwd, command, input, options); !error.isEmpty())
        return error;
    if (!extras)
        return {};
    // The daemon runs the program of a session's pane, and has its
    // terminal: no hold, no input of this window and no user variable of
    // its replica reach them.
    if (window.showsSession() && (extras->hold || extras->stdinScope || !extras->vars.isEmpty()))
        return QStringLiteral("a pane of a mux session takes no hold, stdin_text or vars: the daemon runs its program");
    options.hold = extras->hold;
    if (!extras->stdinScope)
        return {};
    AfterglowVt::TextFormat format;
    format.ansi = extras->stdinAnsi;
    QString text;
    if (const QString error = readText(source, *extras->stdinScope, text, format); !error.isEmpty())
        return error;
    options.input = text.toStdString();
    return {};
}

void Control::afterLaunch(Pane* added, Pane* before, TerminalWindow* window, const LaunchExtras& extras)
{
    // readLaunchExtras held them to what a new pane takes. A false is a
    // program that set as many first: the pane runs, and Pane says it once.
    for (auto it = extras.vars.cbegin(); it != extras.vars.cend(); ++it)
        (void)added->setUserVar(it.key(), it.value().toString());
    if (!extras.focus && before && window)
        window->keepFocus(before);
}

QString Control::clientSplit(quint32 id, const QString& direction, const QStringList& command, const QVariantMap& input,
                             quint32& created)
{
    created = 0;
    LaunchExtras extras;
    if (const QString error = readLaunchExtras(input, command, extras); !error.isEmpty())
        return error;
    return splitWith(id, direction, command, input, &extras, created);
}

QString Control::clientNewTab(quint32 id, const QString& cwd, const QStringList& command, const QVariantMap& input,
                              quint32& created)
{
    created = 0;
    LaunchExtras extras;
    if (const QString error = readLaunchExtras(input, command, extras); !error.isEmpty())
        return error;
    // A tab fills the window: a size is a split's.
    if (input.contains(QStringLiteral("size")))
        return QStringLiteral("size is for split: a tab fills the window");
    return newTabWith(id, cwd, command, input, &extras, created);
}

QString Control::clientNewWindow(const QString& cwd, const QStringList& command, const QVariantMap& input,
                                 quint32& created)
{
    created = 0;
    // A new window has no pane to read: before the checks of the value,
    // which would ask for a command that cannot help.
    if (input.contains(QStringLiteral("stdin_text")))
        return QStringLiteral("newWindow reads no pane: stdin_text is for split and newTab");
    LaunchExtras extras;
    if (const QString error = readLaunchExtras(input, command, extras); !error.isEmpty())
        return error;
    // It takes the focus as a window does.
    if (!extras.focus)
        return QStringLiteral("newWindow takes the focus: focus is for split and newTab");
    if (input.contains(QStringLiteral("size")))
        return QStringLiteral("size is for split: a window has one pane");
    return newWindowWith(cwd, command, input, &extras, created);
}

QString Control::badDirection()
{
    return QStringLiteral("direction must be left, right, up or down");
}

} // namespace Afterglow
