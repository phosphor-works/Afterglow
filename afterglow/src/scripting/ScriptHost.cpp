// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScriptHost.h"
#include "OverlaySpec.h"
#include "ScriptTasks.h"
#include "ScriptFiles.h"
#include "ScriptPackage.h"
#include "ScriptState.h"
#include "../config/Keybinds.h"
#include <QCryptographicHash>
#include <QJsonDocument>

#include <PhosphorFsLoader/WatchedDirectorySet.h>
#include <PhosphorScripting/LuauEngine.h>
#include <PhosphorScripting/LuauWatchdog.h>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <utility>

Q_LOGGING_CATEGORY(lcScripts, "afterglow.scripts", QtInfoMsg)

namespace Afterglow {

using PhosphorScripting::LuauEngine;

struct ScriptHost::Script
{
    QString id;
    QString owner;
    bool retiring = false;
    std::deque<qint64> callbacks, taskCallbacks;
    QString name;
    QString path;
    QByteArray source;
    bool package = false;
    QList<ScriptPackage::Module> modules; ///< a package's module files, for error positions
    QVariantMap packageInfo;
    QString shaderDirectory;
    QVariantMap suggests;
    QHash<QString, int> pulses;
    ScriptState::Snapshot storage;
    int stateVersion = 1;
    bool migrateState = false;
    QVariant initialEffects;
    qint64 initialFlowsBefore = 0, initialFlowsAfter = 0; ///< the flows that on_load registered
    std::unique_ptr<LuauEngine> engine;
    int module = -1;
    QList<ScriptCommand> commands; ///< the manifest's, then those of set_commands
    /// A command run: where its output goes, and how it ends.
    struct Run
    {
        QString title;
        CommandSink sink;
        quint32 pane = 0;
        qint64 outputBytes = 0;
        qint64 outputLimit = 0; ///< Scripts.commandOutputLimit when the run started
        int status = 0;
        QString errors; ///< stderr of a run with no client, for its notification
        std::unique_ptr<QTimer> timer; ///< Scripts.commandTimeout
    };
    std::map<quint64, std::shared_ptr<Run>> runs;
    /// The run of each flow (the await runtime's waiting handler), so that a
    /// result that resumes a run's handler runs as that run. A run's handler
    /// waits for one flow at a time, so a run has one entry at most.
    QHash<qint64, quint64> flowRuns;
    /// Runs that ended while their handler waited: it resumed with
    /// "cancelled" and may still run, as no run, until it returns.
    QSet<quint64> endedRuns;
    /// Ended runs whose "cancelled" resume has not run yet. Results for
    /// their flows that were queued before it are dropped: the handler
    /// resumes once, with "cancelled", and starts nothing new in between.
    QSet<quint64> cancelsDue;
    int failures = 0;
    quint64 calls = 0, handlerErrors = 0, effectErrors = 0;
    quint64 droppedCallbacks = 0, droppedEffects = 0, droppedResults = 0;
    qint64 lastCallNs = 0, maxCallNs = 0, totalCallNs = 0;
    QString lastError;
    bool disabled = false;
    QString disabledReason; ///< the stderr of the runs that its disable ended
    std::deque<qint64> effects; ///< when recent effects ran, for the budgets
    std::deque<qint64> structural;
    std::deque<qint64> docks;
    std::deque<qint64> results;
    int pendingResults = 0;
    /// Sleeps that have not ended, by timer, with their request IDs.
    QHash<QTimer*, QString> sleeps;
    int callTimeoutMs = CommandCallTimeoutMs; ///< of the running handler; its awaited results resume with it
    /// The last flow ID that the await runtime gave.
    qint64 flowCount() const { return engine->moduleField(module, QStringLiteral("__flows")).toLongLong(); }
    /// Calls @p function. @p before: the flow count before the call.
    LuauEngine::CallOutcome invoke(const QString& function, const QVariantList& args, int timeoutMs, qint64* before = nullptr)
    {
        const qint64 flowsBefore = flowCount();
        if (before) *before = flowsBefore;
        QElapsedTimer elapsed;
        elapsed.start();
        auto outcome = engine->callModule(module, function, args, timeoutMs);
        ++calls;
        lastCallNs = elapsed.nsecsElapsed();
        maxCallNs = std::max(maxCallNs, lastCallNs);
        totalCallNs += lastCallNs;
        // The await runtime registers a flow before the engine converts the
        // effects it waits for. When the call fails after that (the effects
        // are too large to convert), no result will resume the flow, and it
        // would hold a waiting slot for good.
        if (outcome.status != LuauEngine::CallStatus::Ok && flowCount() > flowsBefore) {
            engine->callModule(module, QStringLiteral("__abandon"), {flowsBefore}, timeoutMs);
        }
        return outcome;
    }
};

namespace {

/// Request IDs the prelude's await runtime gives the effects a handler waits for.
const QString AwaitPrefix = QStringLiteral("afterglow.await:");

/// A pulse's effect: a name packs share, or a number a pack defines.
int pulseEffect(const QString& name)
{
    if (name == QLatin1String("portal")) return 1;
    if (name == QLatin1String("flash")) return 2;
    if (name == QLatin1String("shake")) return 3;
    bool ok = false;
    const int number = name.toInt(&ok);
    return ok && number >= 1 && number <= 255 ? number : 0;
}

bool number(const QVariant& v)
{
    return v.typeId() == QMetaType::Double || v.typeId() == QMetaType::Int || v.typeId() == QMetaType::LongLong;
}

/// A whole number in [low, high].
bool integer(const QVariant& v, double low, double high)
{
    const double d = v.toDouble();
    return number(v) && std::isfinite(d) && d == std::floor(d) && d >= low && d <= high;
}

/// A dock color: a palette index, or "#rrggbb". Returns the checked value,
/// or an invalid one. An absent color is the default and stays absent.
QVariant dockColor(const QVariant& v, bool& ok)
{
    if (!v.isValid()) return {};
    if (integer(v, 0, 255)) return v.toInt();
    const QString text = v.toString();
    // Each digit by hand: toUInt would also take "0x", a sign and spaces.
    if (v.typeId() == QMetaType::QString && text.size() == 7 && text.startsWith(QLatin1Char('#'))
        && std::all_of(text.begin() + 1, text.end(), [](QChar c) {
               const char16_t u = c.unicode();
               return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
           }))
        return text.toLower();
    ok = false;
    return {};
}

/// A Luau array, which is empty when the table was: an empty table has no
/// array part, so it arrives as an empty map.
std::optional<QVariantList> luauArray(const QVariant& v)
{
    if (v.typeId() == QMetaType::QVariantList) return v.toList();
    if (!v.isValid() || (v.typeId() == QMetaType::QVariantMap && v.toMap().isEmpty())) return QVariantList{};
    return std::nullopt;
}

constexpr int MaxDockRows = 8;
constexpr int MaxDockColumns = 80;
constexpr int MaxDockLines = 256;
constexpr int MaxDockRuns = 128; ///< in one line
constexpr int MaxDockAreaRuns = 1024; ///< in all the lines of a dock
constexpr int MaxDockItem = 64; ///< characters
constexpr qsizetype MaxDockText = 16384; ///< UTF-8 bytes of a dock's texts and items together

/// Checks a dock effect and copies its fields into @p effect in the form
/// DockArea reads. Returns an error, or empty.
QString checkDock(const QVariantMap& raw, QVariantMap& effect)
{
    // The helper passes the spec through whole, so a misspelled field is
    // refused here, as a misspelled run field is, instead of being dropped.
    static constexpr QLatin1String Fields[] = {QLatin1String("kind"), QLatin1String("pane"), QLatin1String("request_id"),
        QLatin1String("id"), QLatin1String("edge"), QLatin1String("size"), QLatin1String("tab_bar"), QLatin1String("fill"),
        QLatin1String("lines"), QLatin1String("scope")};
    for (auto it = raw.keyBegin(); it != raw.keyEnd(); ++it) {
        if (std::none_of(std::begin(Fields), std::end(Fields), [&](QLatin1String field) { return *it == field; }))
            return QStringLiteral("Unknown dock field %1").arg(*it);
    }
    const QString edge = raw.value(QStringLiteral("edge")).toString();
    const bool side = edge == QLatin1String("left") || edge == QLatin1String("right");
    if (!side && edge != QLatin1String("top") && edge != QLatin1String("bottom"))
        return QStringLiteral("Dock edge must be top, bottom, left or right");
    const QVariant size = raw.value(QStringLiteral("size"), 1);
    if (!integer(size, 1, side ? MaxDockColumns : MaxDockRows))
        return side ? QStringLiteral("A left or right dock is 1 to %1 columns wide").arg(MaxDockColumns)
                    : QStringLiteral("A top or bottom dock is 1 to %1 rows high").arg(MaxDockRows);
    const QVariant tabBar = raw.value(QStringLiteral("tab_bar"), false);
    if (tabBar.typeId() != QMetaType::Bool) return QStringLiteral("tab_bar must be a boolean");
    if (tabBar.toBool() && edge != QLatin1String("top")) return QStringLiteral("Only a top dock can replace the tab bar");
    if (tabBar.toBool() && raw.value(QStringLiteral("scope")) == QLatin1String("pane"))
        return QStringLiteral("Only a window dock can replace the tab bar");
    bool ok = true;
    const QVariant fill = dockColor(raw.value(QStringLiteral("fill")), ok);
    if (!ok) return QStringLiteral("Dock colors are palette indices 0 to 255 or \"#rrggbb\"");
    const auto lines = luauArray(raw.value(QStringLiteral("lines")));
    if (!lines) return QStringLiteral("Dock lines must be a list");
    if (side && lines->size() > MaxDockLines)
        return QStringLiteral("A left or right dock has at most %1 lines").arg(MaxDockLines);
    if (!side && lines->size() > size.toInt()) return QStringLiteral("A top or bottom dock has at most one line for each row");
    // Counted as the runs are read, so a dock that is too large stops at the
    // limit instead of converting all of its text first.
    qsizetype bytes = 0;
    auto withinText = [&bytes](const QString& s) {
        // Each UTF-16 unit is at least one UTF-8 byte: a long string is over without a conversion.
        bytes += s.size() > MaxDockText ? s.size() : s.toUtf8().size();
        return bytes <= MaxDockText;
    };
    const QString tooMuchText = QStringLiteral("Dock text and items exceed %1 bytes").arg(MaxDockText);
    QVariantList checkedLines;
    qsizetype runCount = 0;
    for (const QVariant& line : *lines) {
        const auto runs = luauArray(line);
        if (!runs || runs->size() > MaxDockRuns) return QStringLiteral("Each dock line is a list of at most %1 runs").arg(MaxDockRuns);
        runCount += runs->size();
        if (runCount > MaxDockAreaRuns) return QStringLiteral("A dock has at most %1 runs").arg(MaxDockAreaRuns);
        QVariantList checkedRuns;
        for (const QVariant& value : *runs) {
            if (value.typeId() != QMetaType::QVariantMap) return QStringLiteral("Each dock run is a table");
            const QVariantMap run = value.toMap();
            QVariantMap checked;
            for (auto it = run.begin(); it != run.end(); ++it) {
                const QString& key = it.key();
                const QVariant& v = it.value();
                if (key == QLatin1String("text")) {
                    if (v.typeId() != QMetaType::QString) return QStringLiteral("Dock run text must be a string");
                    if (!withinText(v.toString())) return tooMuchText;
                    checked.insert(key, v.toString());
                } else if (key == QLatin1String("fg") || key == QLatin1String("bg")) {
                    checked.insert(key, dockColor(v, ok));
                    if (!ok) return QStringLiteral("Dock colors are palette indices 0 to 255 or \"#rrggbb\"");
                } else if (key == QLatin1String("bold") || key == QLatin1String("italic") || key == QLatin1String("underline")
                           || key == QLatin1String("spacer")) {
                    if (v.typeId() != QMetaType::Bool) return QStringLiteral("Dock run %1 must be a boolean").arg(key);
                    checked.insert(key, v.toBool());
                } else if (key == QLatin1String("item")) {
                    const QString item = v.toString();
                    // Characters, not UTF-16 units: a surrogate pair is one.
                    const auto length = item.size() - std::count_if(item.begin(), item.end(), [](QChar c) { return c.isLowSurrogate(); });
                    if (v.typeId() != QMetaType::QString || item.isEmpty() || length > MaxDockItem
                        || std::any_of(item.begin(), item.end(), [](QChar c) { return c.category() == QChar::Other_Control; }))
                        return QStringLiteral("A dock item is a string of 1 to %1 characters, with no control characters").arg(MaxDockItem);
                    if (!withinText(item)) return tooMuchText;
                    checked.insert(key, item);
                } else {
                    return QStringLiteral("Unknown dock run field %1").arg(key);
                }
            }
            // DockArea draws a spacer as blank cells and never reads its text.
            if (checked.value(QStringLiteral("spacer")).toBool() && checked.contains(QStringLiteral("text")))
                return QStringLiteral("A spacer run has no text");
            checkedRuns.append(checked);
        }
        checkedLines.append(QVariant(checkedRuns));
    }
    effect.insert(QStringLiteral("edge"), edge);
    effect.insert(QStringLiteral("size"), size.toInt());
    effect.insert(QStringLiteral("tab_bar"), tabBar.toBool());
    if (fill.isValid()) effect.insert(QStringLiteral("fill"), fill);
    effect.insert(QStringLiteral("lines"), checkedLines);
    return {};
}

/// What a handler returned, as Luau names it.
QString luauType(const QVariant& value)
{
    switch (value.typeId()) {
    case QMetaType::Bool: return QStringLiteral("boolean");
    case QMetaType::Int: case QMetaType::LongLong: case QMetaType::Double: return QStringLiteral("number");
    case QMetaType::QString: return QStringLiteral("string");
    default: return QStringLiteral("value");
    }
}

/// Text from a script, safe for a log line: bounded, and with no control
/// characters that could drive the terminal that shows the log. @p lines
/// keeps line breaks, for a notice, which shows them as such.
QString loggable(QString text, int limit, bool lines = false)
{
    if (text.size() > limit) {
        text.truncate(limit);
        if (text.back().isHighSurrogate()) text.chop(1);
        text += QStringLiteral("…");
    }
    for (QChar& c : text) {
        const char16_t u = c.unicode();
        if ((u < 0x20 && u != u'\t' && !(lines && u == u'\n')) || (u >= 0x7f && u < 0xa0)) c = QChar::ReplacementCharacter;
    }
    return text;
}

/// The last @p limit UTF-16 units of @p text, without half of a surrogate pair at the start.
QString tail(const QString& text, qsizetype limit)
{
    QString out = text.right(limit);
    if (!out.isEmpty() && out.front().isLowSurrogate()) out.remove(0, 1);
    return out;
}

/// A byte limit as the setting gives it: in KiB, which it always is outside tests.
QString byteLimit(qint64 bytes)
{
    return bytes % 1024 == 0 ? QStringLiteral("%1 KiB").arg(bytes >> 10) : QStringLiteral("%1 bytes").arg(bytes);
}

QString str(const QVariantMap& m, const char* key)
{
    const QVariant v = m.value(QLatin1String(key));
    return v.isValid() ? v.toString() : QString();
}

/// A command given as a string or a list of strings: nothing for another
/// value, which a conversion would turn into a program name such as "1" or "".
std::optional<QStringList> command(const QVariant& v)
{
    if (v.typeId() == QMetaType::QString) {
        return v.toString().isEmpty() ? QStringList{} : QStringList{v.toString()};
    }
    const auto parts = luauArray(v);
    if (!parts) return std::nullopt;
    QStringList out;
    for (const QVariant& part : *parts) {
        if (part.typeId() != QMetaType::QString) return std::nullopt;
        out.append(part.toString());
    }
    return out;
}

/// Which fields each kind of effect carries. "pane" falls back to the event's pane.
struct EffectKind
{
    const char* kind;
    bool pane;
    bool structural;
    std::initializer_list<const char*> strings;
    bool command;
    std::initializer_list<const char*> numbers = {};
};

const EffectKind Kinds[] = {
    {"state_read", false, false, {}, false},
    {"state_write", false, false, {}, false, {"revision"}},
    {"open_directory", false, true, {"cwd", "layout"}, false},
    {"save_project", true, false, {"name"}, false},
    {"open_project", false, true, {"name"}, false},
    {"list_projects", false, false, {}, false},
    {"move_pane", true, true, {"placement"}, false, {"target"}},
    {"detach_tab", true, true, {}, false},
    {"timer", false, false, {"name"}, false, {"delay_ms", "repeat_ms"}},
    {"sleep", false, false, {}, false, {"delay_ms"}},
    {"cancel", false, false, {"name"}, false},
    {"exec", false, false, {"name", "cwd"}, true, {"timeout_ms", "max_bytes"}},
    {"read_file", false, false, {"name", "path"}, false, {"timeout_ms", "max_bytes"}},
    {"watch_file", false, false, {"name", "path"}, false},
    {"set_commands", false, false, {}, false},
    {"output", false, false, {"run", "text"}, false},
    {"output_error", false, false, {"run", "text"}, false},
    {"exit", false, false, {"run"}, false, {"code"}},
    {"select", true, false, {"title"}, false},
    {"prompt", true, false, {"title", "text"}, false},
    {"overlay", true, false, {}, true},
    {"status", true, false, {"text", "tab_title"}, false},
    {"dock", true, false, {"id"}, false},
    {"undock", true, false, {"id"}, false},
    {"override", true, false, {}, false},
    {"read_text", true, false, {"scope"}, false, {"max_bytes"}},
    {"semantic_regions", true, false, {}, false},
    {"read_region", true, false, {"region"}, false, {"max_bytes"}},
    {"send_text", true, false, {"text"}, false},
    {"notify", true, false, {"title", "body"}, false},
    {"split", true, true, {"direction"}, true},
    {"new_tab", true, true, {"cwd"}, true},
    {"new_window", false, true, {"cwd"}, true},
    {"focus", true, false, {}, false},
    {"close", true, true, {}, false},
    {"set_title", true, false, {"title"}, false},
    {"copy", false, false, {"text"}, false},
    {"open_url", false, false, {"url"}, false},
    {"action", true, false, {"name"}, false},
    {"log", false, false, {"message"}, false},
    {"signal", true, false, {}, false, {"channel", "value"}},
    {"signals", true, false, {"scope"}, false},
    {"key_mode", true, false, {"label"}, false},
    {"end_key_mode", false, false, {}, false},
    {"overview", true, false, {"operation"}, false},
    {"tab_menu", true, false, {}, false},
    {"move_tab", true, false, {}, false, {"index"}},
    {"overview_move", true, false, {}, false, {"target", "step"}},
    {"pulse", true, false, {"effect"}, false, {"duration_ms", "color"}},
    {"shaders", true, false, {}, false},
    {"shader_presets", true, false, {"instance"}, false},
    {"shader_preset", true, false, {"instance", "preset", "scope"}, false},
    {"save_shader_preset", true, false, {"instance", "name"}, false},
    {"scene_state", true, false, {}, false},
    {"shader_parameters", true, false, {"instance", "scope"}, false},
};

/// Kinds outside the effect budget. A sleep is bounded by MaxSleeping and its
/// result counts as one result. Ending a key mode, cancelling a task or removing a
/// dock only gives something back, and must work when a burst has spent the
/// budget. An undock removes at most the docks that budgeted effects made. A
/// command's output and status stay with its own client, which
/// Scripts.commandOutputLimit bounds: a command that prints a line for each
/// item must not run into the budget of the effects that act on the terminal.
bool budgetFree(const QString& kind)
{
    return kind == QLatin1String("sleep") || kind == QLatin1String("end_key_mode") || kind == QLatin1String("cancel")
        || kind == QLatin1String("undock") || kind == QLatin1String("output") || kind == QLatin1String("output_error")
        || kind == QLatin1String("exit");
}

/// The flow ID of a request ID that the await runtime gave, else -1.
qint64 flowOf(const QString& request)
{
    if (!request.startsWith(AwaitPrefix)) return -1;
    const QString rest = request.mid(AwaitPrefix.size());
    bool ok = false;
    const qint64 flow = rest.left(rest.indexOf(QLatin1Char(':'))).toLongLong(&ok);
    return ok ? flow : -1;
}

/// The run that the await runtime reports as finished, in the call's result.
const QString RunDone = QStringLiteral("__run_done");

/// The channels a signal can set: the size of the shaders' uSignal array
/// (TerminalUniformsTail in ShaderProgram.h, Pane::SignalCount). This
/// library does not link the renderer to read it from there.
constexpr int SignalChannels = 8;

bool validSignal(const QVariant& channel, const QVariant& value)
{
    const double c = channel.toDouble(), v = value.toDouble();
    return number(channel) && number(value) && std::isfinite(c) && c == std::floor(c) && c >= 0 && c < SignalChannels
        && std::isfinite(v) && std::abs(v) <= std::numeric_limits<float>::max();
}

} // namespace

ScriptHost::ScriptHost(Callbacks callbacks, QObject* parent)
    : QObject(parent)
    , m_callbacks(std::move(callbacks))
    , m_watchdog(std::make_shared<PhosphorScripting::LuauWatchdog>())
    , m_searchPaths(defaultSearchPaths())
    , m_stateDirectory(QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/afterglow/scripts"))
{
    QFile prelude(QStringLiteral(":/afterglow/scripting/afterglow.luau"));
    if (prelude.open(QIODevice::ReadOnly)) {
        m_prelude = prelude.readAll();
    } else {
        qCWarning(lcScripts) << "the script prelude is missing from the build";
    }
    m_tasks = std::make_unique<ScriptTasks>();
    m_clock.start();
}

ScriptHost::~ScriptHost()
{
    for (const auto& script : m_scripts) retire(script);
}

QStringList ScriptHost::defaultSearchPaths()
{
    QStringList dirs;
    // standardLocations lists the most important first; scripts want the reverse.
    const QStringList data = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (auto it = data.crbegin(); it != data.crend(); ++it) {
        dirs.append(*it + QStringLiteral("/afterglow/scripts"));
    }
    dirs.append(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                + QStringLiteral("/afterglow/scripts"));
    return dirs;
}

void ScriptHost::setStateDirectory(const QString& directory)
{
    if (m_enabled) {
        qCWarning(lcScripts) << "State directory cannot change while scripts are enabled";
        return;
    }
    m_stateDirectory = directory;
}

void ScriptHost::setSearchPaths(const QStringList& dirs)
{
    m_searchPaths = dirs;
    if (m_directories) {
        m_directories->setDirectories(m_searchPaths, PhosphorFsLoader::LiveReload::On);
    }
}

void ScriptHost::setEnabled(bool enabled)
{
    if (enabled == m_enabled) {
        return;
    }
    m_enabled = enabled;
    if (m_scanning) {
        // A scan's own effects changed the setting (a reload_config action in
        // on_load). The directory set is still inside that scan, so it must
        // not be destroyed or replaced until the scan has returned.
        QTimer::singleShot(0, this, [this] { applyEnabled(); });
        return;
    }
    applyEnabled();
}

void ScriptHost::applyEnabled()
{
    if (m_enabled == bool(m_directories)) {
        return;
    }
    if (m_enabled) {
        // The set scans now and again on every change, recovering watches
        // for directories that do not exist yet.
        m_directories = std::make_unique<PhosphorFsLoader::WatchedDirectorySet>(
            static_cast<PhosphorFsLoader::IScanStrategy&>(*this));
        m_directories->setDirectories(m_searchPaths, PhosphorFsLoader::LiveReload::On);
    } else {
        m_directories.reset();
        performScan({});
    }
}

void ScriptHost::reload()
{
    if (m_directories) m_directories->rescanNow();
    else performScan({});
}

QStringList ScriptHost::performScan(const QStringList& directories)
{
    const bool outer = !m_scanning;
    m_scanning = true;
    struct Done { bool& scanning; bool outer; ~Done() { if (outer) scanning = false; } } done{m_scanning, outer};
    std::vector<ScriptPtr> loaded;
    QStringList watched;
    QHash<QString, Memo> memos;
    m_loadErrors.clear();
    const auto wasSuperseded = std::exchange(m_superseded, {});
    m_stateFailures.clear();
    QHash<QString, QString> supersededIds; // path -> the id it lost, for the warning
    auto supersede = [&](const QString& path, const QString& id, const QString& holder) {
        m_superseded.insert(path, holder);
        supersededIds.insert(path, id);
    };
    if (m_enabled) {
        // Every script file and package folder, lowest priority first. A
        // package follows the files of its directory, so it wins an id clash
        // with a file there, as a later directory wins over an earlier one.
        QStringList candidates;
        for (const QString& dir : directories) {
            // Unreadable files too: their load fails, which diagnostics show,
            // and the watch sees a change of their permissions.
            const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.luau")}, QDir::Files, QDir::Name);
            for (const QFileInfo& f : files) {
                if (!f.fileName().endsWith(QLatin1String(".d.luau"))) {
                    watched << f.absoluteFilePath();
                    candidates << f.absoluteFilePath();
                }
            }
            // A symlinked package (a checkout linked in) loads like any other:
            // its paths are checked against where the link points.
            for (const auto& folder : QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                watched << folder.absoluteFilePath();
                const auto manifest = folder.absoluteFilePath() + QStringLiteral("/package.json");
                if (QFileInfo::exists(manifest)) candidates << manifest;
            }
        }
        // Highest priority first, so a candidate whose id a winner already
        // holds is known to lose without running its code again.
        QHash<QString, QString> holders; // id -> path
        for (auto it = candidates.crbegin(); it != candidates.crend(); ++it) {
            const QString& path = *it;
            ScriptPackage package;
            const bool isPackage = QFileInfo(path).fileName() == QLatin1String("package.json");
            QByteArray source;
            QString failure;
            if (isPackage) {
                failure = ScriptPackage::read(path, package);
                watched << package.watched;
                if (failure.isEmpty() && package.id != QFileInfo(path).absoluteDir().dirName())
                    failure = QStringLiteral("Package id must match its directory name");
                // The manifest names the id, so a losing package never runs. It
                // shares the winner's state file, whose every write would load it again.
                if (failure.isEmpty() && holders.contains(package.id)) {
                    supersede(path, package.id, holders.value(package.id));
                    continue;
                }
                source = package.source;
                // Everything a running instance copied from the manifest: a
                // change to any of it must replace the instance.
                QVariantMap pulses;
                for (auto p = package.pulses.cbegin(); p != package.pulses.cend(); ++p) pulses.insert(p.key(), p.value());
                QVariantList commands;
                for (const ScriptCommand& c : std::as_const(package.commands))
                    commands.append(QVariantList{c.name, c.summary, c.usage, c.palette, c.input, c.remote});
                const QVariantMap identity{{QStringLiteral("id"), package.id}, {QStringLiteral("version"), package.version},
                    {QStringLiteral("state_version"), package.stateVersion}, {QStringLiteral("assets"), package.assets},
                    {QStringLiteral("config"), package.config}, {QStringLiteral("shaders"), package.shaderDirectory},
                    {QStringLiteral("packs"), package.shaderPacks}, {QStringLiteral("suggests"), package.suggests},
                    {QStringLiteral("pulses"), pulses}, {QStringLiteral("commands"), commands}};
                source += "\n-- metadata: " + QCryptographicHash::hash(QJsonDocument::fromVariant(identity).toJson(), QCryptographicHash::Sha256).toHex();
            } else {
                failure = readScriptFile(path, MaxScriptBytes, source);
            }
            // A package's load also reads its stored state.
            QByteArray stamp = QCryptographicHash::hash(source, QCryptographicHash::Sha256);
            if (isPackage) {
                const QFileInfo state(QDir(m_stateDirectory).filePath(package.id + QStringLiteral(".json")));
                stamp += QByteArray::number(state.exists() ? state.size() : -1) + ':'
                    + QByteArray::number(state.lastModified().toMSecsSinceEpoch());
            }
            auto same = std::find_if(m_scripts.begin(), m_scripts.end(),
                                     [&](const ScriptPtr& s) { return s->path == path && s->source == source; });
            const auto memo = m_memos.constFind(path);
            const bool known = memo != m_memos.cend() && memo->stamp == stamp;
            ScriptPtr script;
            if (!failure.isEmpty()) {
                m_loadErrors[path] = failure;
                qCWarning(lcScripts).noquote() << path << loggable(failure, MaxLogCharacters);
            } else if (same != m_scripts.end()) {
                script = *same;
            } else if (known && !memo->error.isEmpty()) {
                m_loadErrors[path] = memo->error; // unchanged and still broken
                if (memo->state) m_stateFailures.insert(path);
                // A broken edit of a superseded script: still superseded.
                if (!memo->id.isEmpty() && holders.contains(memo->id)) supersede(path, memo->id, holders.value(memo->id));
                memos.insert(path, *memo);
            } else if (known && holders.contains(memo->id)) {
                supersede(path, memo->id, holders.value(memo->id)); // unchanged and still superseded
                memos.insert(path, *memo);
                continue;
            } else {
                script = load(path, source, isPackage ? &package : nullptr);
                if (!script) memos.insert(path, {stamp, {}, m_loadErrors.value(path), m_stateFailures.contains(path)});
            }
            if (!script) {
                if (isPackage && m_stateFailures.contains(path)) {
                    // Fixing or removing the state file retries the package.
                    // The file exists: its own watch sees an edit, a
                    // replacement and a removal, and the writes of other
                    // packages to the directory do not rescan.
                    watched << QDir(m_stateDirectory).filePath(package.id + QStringLiteral(".json"));
                }
                // A broken edit keeps its last working instance and resources.
                const auto old = std::find_if(m_scripts.begin(), m_scripts.end(), [&](const ScriptPtr& s) { return s->path == path; });
                if (old == m_scripts.end()) continue;
                script = *old;
            }
            if (const auto holder = holders.constFind(script->id); holder != holders.cend()) {
                supersede(path, script->id, *holder);
                // A broken edit of a superseded script keeps its error for diagnostics.
                memos.insert(path, {stamp, script->id, m_loadErrors.value(path), m_stateFailures.contains(path)});
                continue;
            }
            holders.insert(script->id, path);
            loaded.push_back(script);
        }
        std::reverse(loaded.begin(), loaded.end());
    }
    m_memos = std::move(memos);
    // Commit migrations only for the selected replacement, after compilation and
    // initialization succeeded. A conflict retains the old live instance.
    for (auto it = loaded.begin(); it != loaded.end();) {
        auto& script = *it;
        if (script->migrateState) {
            ScriptState::Snapshot committed;
            const auto error = ScriptState(m_stateDirectory).write(script->id, script->stateVersion,
                script->storage.revision, script->storage.value, committed);
            if (!error.isEmpty()) {
                m_loadErrors[script->path] = error;
                // Fixing the state file, or removing it, retries the migration.
                watched << QDir(m_stateDirectory).filePath(script->id + QStringLiteral(".json"));
                qCWarning(lcScripts).noquote() << script->id << "migration not committed:" << error;
                // The live instance keeps running: the one at this path, or
                // the one with this id that a higher-priority path replaced.
                auto old = std::find_if(m_scripts.begin(), m_scripts.end(), [&](const ScriptPtr& s) { return s->path == script->path; });
                if (old == m_scripts.end()) {
                    old = std::find_if(m_scripts.begin(), m_scripts.end(), [&](const ScriptPtr& s) {
                        return s->id == script->id && std::find(loaded.begin(), loaded.end(), s) == loaded.end();
                    });
                }
                if (old == m_scripts.end()) { it = loaded.erase(it); continue; }
                script = *old;
                m_superseded.remove(script->path); // it keeps running, so it is not superseded
            } else { script->storage = committed; script->migrateState = false; }
        }
        ++it;
    }
    // Warns once, when a path starts to lose its id, not on every rescan; and
    // only now, since a failed commit above keeps a superseded path running.
    for (auto it = m_superseded.cbegin(); it != m_superseded.cend(); ++it) {
        if (!wasSuperseded.contains(it.key()))
            qCWarning(lcScripts) << it.key() << "uses the id" << supersededIds.value(it.key()) << "of" << it.value()
                                 << "and is not loaded";
    }
    const QList<ScriptCommand> before = commands();
    const auto previous = m_scripts;
    for (const auto& script : previous) {
        if (std::find(loaded.begin(), loaded.end(), script) == loaded.end()) retire(script);
    }
    m_scripts = std::move(loaded);
    // A package's packs exist before its on_load effects run, which may use them.
    publishPackageShaders();
    // Iterates a copy: an on_load effect can rescan, or turn scripting off.
    const auto current = m_scripts;
    for (const auto& script : current) {
        if (std::find(previous.begin(), previous.end(), script) == previous.end()) {
            if (script->package) {
                script->callTimeoutMs = CommandCallTimeoutMs;
                const QVariant effects = std::exchange(script->initialEffects, QVariant());
                // The effects of its on_load, which ran in load(), and the flows it registered there.
                const CallContext outer = std::exchange(m_call, {script.get(), 0, false, false, script->initialFlowsBefore,
                                                                 script->initialFlowsAfter});
                applyResult(script, effects, 0);
                m_call = outer;
            } else {
                emitOwned(script, QStringLiteral("on_load"), {}, 0);
            }
        }
    }
    // Once for the whole scan: its on_load effects and disables announce nothing.
    if (outer && commands() != before) Q_EMIT commandsChanged();
    for (const ScriptPtr& s : m_scripts) {
        watched << s->path; // editors that save in place change only the file
    }
    publishPackageShaders(); // an on_load can have failed and disabled its package
    watched.removeDuplicates();
    return watched;
}

void ScriptHost::announceCommands(const QList<ScriptCommand>& before)
{
    // A scan announces its change once, at its end.
    if (!m_scanning && commands() != before) Q_EMIT commandsChanged();
}

void ScriptHost::publishPackageShaders()
{
    QMap<QString, QString> shaders;
    for (const ScriptPtr& s : m_scripts) {
        if (!s->shaderDirectory.isEmpty() && !s->disabled) shaders.insert(s->id, s->shaderDirectory);
    }
    if (shaders != m_packageShaders) {
        m_packageShaders = shaders;
        Q_EMIT packageShadersChanged(m_packageShaders);
    }
}

void ScriptHost::disable(const ScriptPtr& script, const QString& reason)
{
    const QList<ScriptCommand> before = commands();
    script->disabled = true;
    script->disabledReason = reason;
    retire(script);
    announceCommands(before);
    publishPackageShaders();
}

ScriptHost::ScriptPtr ScriptHost::load(const QString& path, const QByteArray& source, const ScriptPackage* package) const
{
    auto script = std::make_shared<Script>();
    script->path = path;
    script->source = source;
    script->engine = std::make_unique<LuauEngine>(m_watchdog, MemoryCapBytes);
    QString error;
    const QString chunk = QStringLiteral("@") + QFileInfo(path).fileName();
    if (!script->engine->init(&error) || !script->engine->runPrelude(QStringLiteral("@afterglow"), m_prelude, &error)) {
        m_loadErrors[path] = error;
        qCWarning(lcScripts) << "cannot start the script engine:" << error;
        return nullptr;
    }
    script->engine->sandbox();
    // Each module of a package parses alone first. In the one chunk, a block
    // that a module leaves open takes the end of its factory, so the parser
    // reports it in a later module or at the end of the chunk, if at all.
    // Alone, the error names the module's file and line, as for a single
    // file. The prefix returns before the module's code, and shares its
    // first line.
    bool parsed = true;
    for (qsizetype i = 0; package && parsed && i < package->modules.size(); ++i) {
        const ScriptPackage::Module& module = package->modules[i];
        const int checked = script->engine->loadModule(QStringLiteral("@") + module.file,
                                                       "do return {} end " + package->source.mid(module.offset, module.size), &error);
        parsed = checked >= 0;
        script->engine->releaseModule(checked);
    }
    // The prelude wraps the module so its handlers can await. The prefix shares
    // the first line, so error positions keep their line numbers.
    if (parsed)
        script->module = script->engine->loadModule(chunk, "return afterglow.__wrap((function(...) " + source + "\nend)(...))", &error);
    if (!parsed || script->module < 0) {
        if (package && parsed) error = ScriptPackage::locate(package->modules, error);
        // A script's own error (its module code calls error()) can be of any length and hold anything.
        error = loggable(error, MaxLogCharacters);
        m_loadErrors[path] = error;
        qCWarning(lcScripts).noquote() << path << "did not load:" << error;
        return nullptr;
    }

    const QVariantMap meta = script->engine->moduleField(script->module, QStringLiteral("metadata")).toMap();
    script->id = package ? package->id : str(meta, "id");
    if (package && !str(meta, "id").isEmpty() && str(meta, "id") != package->id) {
        m_loadErrors[path] = QStringLiteral("Entry metadata id differs from manifest");
        qCWarning(lcScripts) << path << "entry metadata id differs from manifest"; return nullptr;
    }
    if (script->id.isEmpty()) {
        script->id = QFileInfo(path).completeBaseName();
    }
    if (!validScriptId(script->id)) {
        m_loadErrors[path] = QStringLiteral("The script id %1 must be %2").arg(loggable(script->id, 64), ScriptIdRule);
        qCWarning(lcScripts).noquote() << path << m_loadErrors.value(path);
        return nullptr;
    }
    static quint64 nextOwner = 0;
    script->owner = script->id + QLatin1Char('@') + QString::number(++nextOwner);
    script->name = str(meta, "name");
    if (script->name.isEmpty()) {
        script->name = script->id;
    }
    // A package declares its commands in package.json, a single file in its metadata.
    error.clear();
    const QVariant declared = meta.value(QStringLiteral("commands"));
    if (package && declared.isValid()) error = QStringLiteral("A package declares its commands in package.json");
    else if (package) script->commands = package->commands;
    else if (declared.isValid()) error = readCommands(declared, script->commands);
    if (error.isEmpty() && !script->commands.isEmpty()
        && !script->engine->hasFunction(script->module, QStringLiteral("run_command")))
        error = QStringLiteral("A script with commands needs a run_command function");
    if (!error.isEmpty()) {
        m_loadErrors[path] = loggable(error, MaxLogCharacters);
        qCWarning(lcScripts).noquote() << path << m_loadErrors.value(path);
        return nullptr;
    }
    for (ScriptCommand& command : script->commands) command.script = script->id;
    if (package) {
        script->package = true;
        script->modules = package->modules;
        script->shaderDirectory = package->shaderDirectory;
        script->suggests = package->suggests;
        script->pulses = package->pulses;
        script->stateVersion = package->stateVersion;
        script->packageInfo = {{QStringLiteral("id"), package->id}, {QStringLiteral("version"), package->version},
            {QStringLiteral("api_version"), 1}, {QStringLiteral("assets"), package->assets}, {QStringLiteral("home"), QDir::homePath()}, {QStringLiteral("config"), package->config}};
        error = ScriptState(m_stateDirectory).read(script->id, script->storage);
        if (error.isEmpty() && script->storage.version > script->stateVersion) error = QStringLiteral("Stored state is newer than this package");
        if (error.isEmpty() && script->storage.version > 0 && script->storage.version < script->stateVersion) {
            if (!script->engine->hasFunction(script->module, QStringLiteral("migrate"))) error = QStringLiteral("State migration function is missing");
            else {
                const auto migrated = script->invoke(QStringLiteral("migrate"),
                    {script->storage.value, script->storage.version, script->stateVersion}, CommandCallTimeoutMs);
                if (migrated.status != LuauEngine::CallStatus::Ok)
                    error = QStringLiteral("Migration failed: ") + ScriptPackage::locate(package->modules, migrated.message);
                else if (migrated.result.typeId() != QMetaType::QVariantMap)
                    error = QStringLiteral("Migration must return an object table");
                else {
                    script->storage.value = migrated.result.toMap();
                    error = ScriptState::validate(script->storage.value);
                    script->migrateState = error.isEmpty();
                }
            }
        }
        // Read or migrated, the error came from the stored state: fixing or
        // removing its file retries the package.
        if (!error.isEmpty()) m_stateFailures.insert(path);
        if (error.isEmpty() && script->engine->hasFunction(script->module, QStringLiteral("on_load"))) {
            const QVariantMap storage{{QStringLiteral("value"), script->storage.value}, {QStringLiteral("version"), script->stateVersion},
                {QStringLiteral("revision"), script->storage.revision + (script->migrateState ? 1 : 0)}};
            const auto initialized = script->invoke(QStringLiteral("on_load"),
                {QVariantMap{{QStringLiteral("terminal"), state()}, {QStringLiteral("package"), script->packageInfo}, {QStringLiteral("storage"), storage}}},
                CommandCallTimeoutMs, &script->initialFlowsBefore);
            if (initialized.status != LuauEngine::CallStatus::Ok)
                error = QStringLiteral("Initialization failed: ") + ScriptPackage::locate(package->modules, initialized.message);
            else script->initialEffects = initialized.result;
            script->initialFlowsAfter = script->flowCount();
        }
        if (!error.isEmpty()) {
            m_loadErrors[path] = loggable(error, MaxLogCharacters);
            qCWarning(lcScripts).noquote() << path << m_loadErrors.value(path);
            return nullptr;
        }
    }
    qCInfo(lcScripts).noquote() << "loaded" << script->id << "from" << path;
    return script;
}

QVariantList ScriptHost::packageSuggestions() const
{
    QVariantList out;
    for (const ScriptPtr& s : m_scripts) {
        if (s->disabled) continue;
        for (auto it = s->suggests.cbegin(); it != s->suggests.cend(); ++it) {
            out.append(QVariantMap{{QStringLiteral("package"), s->id}, {QStringLiteral("key"), it.key()},
                                   {QStringLiteral("ref"), s->id + QLatin1Char('.') + it.value().toString()}});
        }
    }
    return out;
}

QStringList ScriptHost::scriptIds() const
{
    QStringList ids;
    for (const ScriptPtr& s : m_scripts) {
        ids.append(s->id);
    }
    return ids;
}

QVariantList ScriptHost::diagnostics() const
{
    QVariantList result;
    QSet<QString> active;
    for (const auto& script : m_scripts) {
        active.insert(script->path);
        result << QVariantMap{{QStringLiteral("id"), script->id}, {QStringLiteral("name"), script->name}, {QStringLiteral("path"), script->path},
            {QStringLiteral("version"), script->packageInfo.value(QStringLiteral("version"))},
            {QStringLiteral("status"), script->disabled ? QStringLiteral("disabled") : m_loadErrors.contains(script->path) ? QStringLiteral("last-good") : QStringLiteral("active")},
            {QStringLiteral("error"), m_loadErrors.value(script->path)},
            {QStringLiteral("calls"), script->calls}, {QStringLiteral("handler_errors"), script->handlerErrors},
            {QStringLiteral("effect_errors"), script->effectErrors}, {QStringLiteral("last_error"), script->lastError},
            {QStringLiteral("last_call_ms"), script->lastCallNs / 1e6},
            {QStringLiteral("max_call_ms"), script->maxCallNs / 1e6},
            {QStringLiteral("total_call_ms"), script->totalCallNs / 1e6},
            {QStringLiteral("dropped_callbacks"), script->droppedCallbacks},
            {QStringLiteral("dropped_effects"), script->droppedEffects},
            {QStringLiteral("dropped_results"), script->droppedResults},
            {QStringLiteral("waiting"), script->engine->moduleField(script->module, QStringLiteral("__waiting")).toInt()}};
    }
    for (auto it = m_loadErrors.begin(); it != m_loadErrors.end(); ++it) if (!active.contains(it.key()))
        result << QVariantMap{{QStringLiteral("path"), it.key()}, {QStringLiteral("status"), QStringLiteral("failed")}, {QStringLiteral("error"), it.value()}};
    for (auto it = m_superseded.begin(); it != m_superseded.end(); ++it)
        result << QVariantMap{{QStringLiteral("path"), it.key()}, {QStringLiteral("status"), QStringLiteral("superseded")},
                              {QStringLiteral("error"), QStringLiteral("Its id belongs to %1").arg(it.value())}};
    return result;
}

bool ScriptHost::isDisabled(const QString& id) const
{
    for (const ScriptPtr& s : m_scripts) {
        if (s->id == id) {
            return s->disabled;
        }
    }
    return false;
}

QList<ScriptCommand> ScriptHost::commands() const
{
    QList<ScriptCommand> out;
    for (const ScriptPtr& s : m_scripts) {
        if (!s->disabled && !s->retiring) out.append(s->commands);
    }
    return out;
}

quint64 ScriptHost::startCommand(const CommandRequest& request, CommandSink sink, QString* error, bool* unknown)
{
    auto refuse = [&](const QString& why, bool missing = false) -> quint64 {
        if (error) *error = why;
        if (unknown) *unknown = missing;
        return 0;
    };
    // Copied: a reload during the call must not free the script under it.
    const std::vector<ScriptPtr> scripts = m_scripts;
    // A disabled script is retired for good, but stays in the list until its
    // file changes: it is found, so that the refusal can say why.
    const auto found = std::find_if(scripts.begin(), scripts.end(), [&](const ScriptPtr& s) {
        return s->id == request.script && (!s->retiring || s->disabled);
    });
    const ScriptPtr script = found == scripts.end() ? nullptr : *found;
    // Its commands are withdrawn, but it is there: the user can find out why.
    if (script && script->disabled)
        return refuse(QStringLiteral("Script %1 is disabled. afterglow ctl scripts shows why").arg(script->id));
    const auto command = script ? std::find_if(script->commands.cbegin(), script->commands.cend(),
                                               [&](const ScriptCommand& c) { return c.name == request.command; })
                                : QList<ScriptCommand>::const_iterator();
    // A remote host sees only the commands that declare remote: for it, the others do not exist.
    const bool remote = request.source == QLatin1String("remote");
    if (!script || command == script->commands.cend() || (remote && !command->remote))
        return refuse(QStringLiteral("No command %1 in %2").arg(request.command, request.script), true);
    if (int(script->runs.size()) >= m_limits.runs)
        return refuse(QStringLiteral("Script %1 runs %2 commands already, its limit (Scripts.commandRunLimit)")
                          .arg(script->id).arg(m_limits.runs));
    if (request.args.size() > MaxCommandArgs) return refuse(QStringLiteral("A command takes at most %1 arguments").arg(MaxCommandArgs));
    for (const QString& arg : request.args) {
        if (arg.size() > MaxCommandArgLength)
            return refuse(QStringLiteral("A command argument has at most %1 UTF-16 units").arg(MaxCommandArgLength));
    }
    const quint64 handle = ++m_nextRun;
    const QString id = QString::number(handle);
    auto run = std::make_shared<Script::Run>();
    run->title = command->title();
    run->sink = std::move(sink);
    run->pane = request.pane;
    // The limits that the run started with: a settings change does not move them.
    run->outputLimit = m_limits.outputBytes;
    run->timer = std::make_unique<QTimer>();
    run->timer->setSingleShot(true);
    connect(run->timer.get(), &QTimer::timeout, this, [this, handle, timeout = m_limits.timeoutMs] {
        cancelCommand(handle, CommandTimedOut,
                      QStringLiteral("The command ran for %1 seconds, its limit (Scripts.commandTimeout)").arg(timeout / 1000.0));
    });
    run->timer->start(m_limits.timeoutMs);
    script->runs.emplace(handle, run);
    m_runScripts.insert(handle, script);

    QVariantMap ev{{QStringLiteral("args"), request.args}, {QStringLiteral("source"), request.source},
                   {QStringLiteral("run"), id}, {QStringLiteral("terminal"), state()}};
    QVariantMap pane;
    if (request.pane) {
        pane = eventPane(request.pane);
        ev.insert(QStringLiteral("pane"), pane);
    }
    ev.insert(QStringLiteral("cwd"), request.cwd.isEmpty() ? pane.value(QStringLiteral("cwd")).toString() : request.cwd);
    if (remote) ev.insert(QStringLiteral("remote"), QVariantMap{{QStringLiteral("host"), request.remoteHost}});
    // Whole: CommandSocket refuses a run with more than Scripts.commandStdinLimit.
    if (command->input && request.input) ev.insert(QStringLiteral("stdin"), *request.input);
    call(script, QStringLiteral("__command"), {id, request.command, ev}, CommandCallTimeoutMs, request.pane, handle);
    return handle;
}

QString ScriptHost::runCommand(const QString& token, quint32 pane, const QString& source)
{
    const auto dot = token.indexOf(QLatin1Char('.'));
    if (dot < 0) return QStringLiteral("A script command is <script id>.<command>, not %1").arg(token);
    QString error;
    startCommand({token.left(dot), token.mid(dot + 1), {}, {}, pane, std::nullopt, source, {}}, {}, &error);
    return error;
}

void ScriptHost::cancelCommand(quint64 run, int status, const QString& message)
{
    if (const ScriptPtr script = m_runScripts.value(run).lock()) finishRun(script, run, status, message, true);
}

void ScriptHost::finishRun(const ScriptPtr& script, quint64 handle, int status, const QString& message, bool waiting)
{
    const auto it = script->runs.find(handle);
    if (it == script->runs.end()) return;
    const std::shared_ptr<Script::Run> run = it->second;
    script->runs.erase(it);
    m_runScripts.remove(handle);
    run->timer.reset();
    const QString text = message.isEmpty() || message.endsWith(QLatin1Char('\n')) ? message : message + QLatin1Char('\n');
    if (run->sink.finished) {
        if (!text.isEmpty() && run->sink.error) run->sink.error(text, {});
        run->sink.finished(status);
    } else {
        // No client: its stderr and the reason it ended show as a
        // notification: on the run's pane, or one that belongs to no pane
        // when the run has none.
        const QString errors = (run->errors + text).trimmed();
        if ((status != 0 || !errors.isEmpty()) && m_callbacks.notice) {
            m_callbacks.notice(run->pane, run->title,
                               errors.isEmpty() ? QStringLiteral("Exited with status %1").arg(status)
                                                : loggable(tail(errors, MaxLogCharacters), MaxLogCharacters, true));
        }
    }
    QList<qint64> flows;
    for (auto flow = script->flowRuns.cbegin(); flow != script->flowRuns.cend(); ++flow) {
        if (flow.value() == handle) flows << flow.key();
    }
    if (!waiting || script->retiring || script->disabled) {
        for (const qint64 flow : std::as_const(flows)) script->flowRuns.remove(flow);
        return;
    }
    // The handler may wait for something that will now never resume it. It
    // resumes once with "cancelled", so that its cleanup (a pcall, a busy
    // flag) runs. Its output and exit go nowhere: the client has its status
    // already. Not now: the run can end inside an effect of a handler, or of
    // another script, and a VM must not run inside its own effects. Its flow
    // keeps its run until then, so that a result queued before the end is
    // recognized and dropped (cancelsDue). Queued before what stops below
    // queues its own results, so the handler learns why the run ended.
    script->endedRuns.insert(handle);
    script->cancelsDue.insert(handle);
    QTimer::singleShot(0, this, [this, weak = std::weak_ptr<Script>(script), handle, pane = run->pane,
                                 reason = message.isEmpty() ? QStringLiteral("The run was canceled") : message.trimmed()] {
        const ScriptPtr script = weak.lock();
        if (script) script->cancelsDue.remove(handle);
        // Its handler returned or failed meanwhile: nothing waits.
        if (!script || !script->endedRuns.contains(handle)) return;
        if (script->disabled || script->retiring || std::find(m_scripts.begin(), m_scripts.end(), script) == m_scripts.end()) {
            script->endedRuns.remove(handle);
            script->flowRuns.removeIf([handle](QHash<qint64, quint64>::iterator flow) { return flow.value() == handle; });
            return;
        }
        // The result events of the effects that it waited for, as a result has them.
        QVariantMap context{{QStringLiteral("terminal"), state()}};
        if (pane) context.insert(QStringLiteral("pane"), eventPane(pane));
        call(script, QStringLiteral("__cancel_run"), {QString::number(handle), reason, context}, CommandCallTimeoutMs, pane, handle);
    });
    // What the handler waits for stops: its dialogs and overlays close, and
    // its processes, file reads and sleeps end and free their slots.
    for (const qint64 flow : std::as_const(flows)) {
        const QString prefix = AwaitPrefix + QString::number(flow) + QLatin1Char(':');
        if (m_callbacks.closeRunWaits) m_callbacks.closeRunWaits(script->owner, prefix);
        m_tasks->cancel(script->owner, prefix);
        stopSleeps(*script, prefix);
    }
}

void ScriptHost::stopSleeps(Script& script, const QString& prefix)
{
    for (auto it = script.sleeps.begin(); it != script.sleeps.end();) {
        if (it.value().startsWith(prefix)) {
            delete it.key();
            it = script.sleeps.erase(it);
        } else {
            ++it;
        }
    }
}

bool ScriptHost::endedRun(const Script& script, quint64 run) const
{
    return script.endedRuns.contains(run);
}

ScriptHost::EffectResult ScriptHost::writeRun(const ScriptPtr& script, quint64 handle, const QString& text, bool error,
                                              std::function<void(bool delivered)> sent)
{
    const auto it = script->runs.find(handle);
    if (it == script->runs.end()) return {QStringLiteral("The run has ended"), {}, QStringLiteral("no_run")};
    const std::shared_ptr<Script::Run> run = it->second;
    const auto& write = error ? run->sink.error : run->sink.output;
    const bool client = run->sink.toClient(error);
    // A run with no client drops its stdout, which costs nothing. Its
    // stderr is kept for the notification.
    if (client || error) {
        const qint64 bytes = text.toUtf8().size();
        if (run->outputBytes + bytes > run->outputLimit) {
            const QString why = QStringLiteral("The command's output is over %1, its limit (Scripts.commandOutputLimit)")
                                    .arg(byteLimit(run->outputLimit));
            finishRun(script, handle, 1, why, true);
            return {why, {}, QStringLiteral("operation_failed")};
        }
        run->outputBytes += bytes;
    }
    if (!client) {
        // Kept for the notification, at most what it shows.
        if (error) run->errors = tail(run->errors + text, MaxLogCharacters);
        if (sent) sent(true);
        return {{}, {}, {}, true};
    }
    // Once: whatever a sink that refuses the text does, the result is reported once.
    auto once = std::make_shared<std::function<void(bool)>>(std::move(sent));
    auto report = [once](bool delivered) {
        if (auto callback = std::exchange(*once, {})) callback(delivered);
    };
    if (!write(text, report)) {
        finishRun(script, handle, 1, {}, true);
        report(false);
        return {QStringLiteral("The run's output could not go to its client"), {}, QStringLiteral("cancelled"), true};
    }
    return {{}, {}, {}, true};
}

void ScriptHost::dispatchTo(const QString& owner, const QString& handler, quint32 pane, QVariantMap event, bool limited)
{
    for (const ScriptPtr& script : m_scripts) {
        if (script->owner == owner) {
            // Only a call that would run counts.
            if (limited && !script->disabled && !script->retiring && script->engine->hasFunction(script->module, handler)
                && !admitCallback(*script))
                return;
            // A copy: a reload during the call must not free the script under it.
            const ScriptPtr target = script;
            emitOwned(target, handler, std::move(event), pane);
            return;
        }
    }
}

void ScriptHost::dispatch(const QString& handler, quint32 pane, QVariantMap event)
{
    // Copied: a reload during a call must not free the script under it. The
    // snapshot is built only when some script handles the event and has room
    // in its callback budget, because terminal output can raise events
    // (titles) faster than anyone reads them, and the snapshot costs a
    // directory and a process lookup for each pane.
    std::vector<ScriptPtr> targets;
    for (const ScriptPtr& s : m_scripts) {
        if (!s->disabled && !s->retiring && s->engine->hasFunction(s->module, handler) && admitCallback(*s)) targets.push_back(s);
    }
    if (targets.empty()) {
        return;
    }
    event.insert(QStringLiteral("terminal"), state());
    if (pane && !event.contains(QStringLiteral("pane"))) {
        event.insert(QStringLiteral("pane"), eventPane(pane));
    }
    for (const ScriptPtr& s : targets) {
        // An earlier call can disable or replace a later script.
        if (s->disabled || s->retiring) continue;
        call(s, handler, {event}, EventTimeoutMs, pane);
    }
}

QVariantMap ScriptHost::eventPane(quint32 pane) const
{
    QVariantMap info = m_callbacks.paneInfo ? m_callbacks.paneInfo(pane) : QVariantMap{};
    if (info.isEmpty()) {
        info.insert(QStringLiteral("id"), pane); // closed already
    }
    return info;
}

bool ScriptHost::admitCallback(Script& script, bool task)
{
    std::deque<qint64>& calls = task ? script.taskCallbacks : script.callbacks;
    const qint64 now = m_clock.elapsed();
    while (!calls.empty() && now - calls.front() >= 1000) calls.pop_front();
    if (int(calls.size()) >= MaxCallbacksPerSecond) {
        ++script.droppedCallbacks;
        return false;
    }
    calls.push_back(now);
    return true;
}

QVariantMap ScriptHost::state() const
{
    QVariantMap snapshot = m_callbacks.state ? m_callbacks.state() : QVariantMap{};
    snapshot.insert(QStringLiteral("version"), SnapshotVersion);
    for (const char* key : {"windows", "tabs", "panes"}) {
        if (!snapshot.contains(QLatin1String(key))) {
            snapshot.insert(QLatin1String(key), QVariantList{});
        }
    }
    for (const char* key : {"active_window", "active_tab", "active_pane"}) {
        if (!snapshot.contains(QLatin1String(key))) {
            snapshot.insert(QLatin1String(key), 0);
        }
    }
    return snapshot;
}

void ScriptHost::retire(const ScriptPtr& script)
{
    if (script->retiring) return;
    script->retiring = true;
    // Its runs end before on_unload: none of them can finish after this.
    while (!script->runs.empty()) {
        finishRun(script, script->runs.begin()->first, 1,
                  script->disabled ? script->disabledReason
                                   : QStringLiteral("The script stopped: it was changed, removed or turned off"),
                  false);
    }
    if (!script->disabled && script->engine->hasFunction(script->module, QStringLiteral("on_unload")))
        call(script, QStringLiteral("on_unload"), {QVariantMap{{QStringLiteral("terminal"), state()}}}, EventTimeoutMs, 0);
    m_tasks->clear(script->owner);
    stopSleeps(*script, {});
    if (m_callbacks.cleanup) m_callbacks.cleanup(script->owner);
}

void ScriptHost::emitOwned(const ScriptPtr& script, const QString& handler, QVariantMap event, quint32 pane)
{
    if (script->disabled || script->retiring || std::find(m_scripts.begin(), m_scripts.end(), script) == m_scripts.end()
        || !script->engine->hasFunction(script->module, handler)) return;
    // Keys, dock clicks, hovers and layouts, the start, end and cancel of a
    // dock drag and on_load are not counted here: dropping a key would leave
    // a key mode on while its keys vanish, and a dropped hover or layout
    // would leave a script with an old state. Those that can come without
    // limit (wheel turns, drag moves, accessibility presses) were counted by
    // dispatchTo. Timers and watchers have a budget of their own,
    // so a title storm that spends the events' budget does not stop them.
    const bool task = handler == QLatin1String("on_timer") || handler == QLatin1String("on_file_changed");
    if (task && !admitCallback(*script, true)) return;
    event.insert(QStringLiteral("terminal"), state());
    if (pane) event.insert(QStringLiteral("pane"), eventPane(pane));
    call(script, handler, {event}, EventTimeoutMs, pane);
}

void ScriptHost::call(const ScriptPtr& script, const QString& function, const QVariantList& args, int timeoutMs,
                      quint32 eventPane, quint64 run)
{
    // Restored on return: a call that an effect makes (on_unload) must not
    // change the time limit that the outer handler's awaited results get.
    struct Restore
    {
        int& value;
        int saved;
        ~Restore() { value = saved; }
    } restore{script->callTimeoutMs, std::exchange(script->callTimeoutMs, timeoutMs)};
    const bool ended = run && endedRun(*script, run);
    // The handler of an ended run stops being tracked once it returns, fails
    // or waits for nothing more.
    auto forgetEnded = [&script, run] {
        script->endedRuns.remove(run);
        script->flowRuns.removeIf([run](QHash<qint64, quint64>::iterator flow) { return flow.value() == run; });
    };
    qint64 flowsBefore = 0;
    const auto out = script->invoke(function, args, timeoutMs, &flowsBefore);
    if (out.status != LuauEngine::CallStatus::Ok) {
        // The script's own error text can be of any length and hold anything.
        const QString why = out.status == LuauEngine::CallStatus::TimedOut
            ? QStringLiteral("took longer than %1 ms").arg(timeoutMs)
            : loggable(ScriptPackage::locate(script->modules, out.message), MaxLogCharacters);
        // Whatever the owner holds must not trap the user, also after a
        // failure that does not count.
        if (m_callbacks.failed) m_callbacks.failed(script->owner);
        if (ended) {
            // Often the "cancelled" error passed on: not a failure of the script.
            forgetEnded();
            qCInfo(lcScripts).noquote() << script->id << "run_command stopped after its run ended:" << why;
            return;
        }
        ++script->handlerErrors;
        const QString name = function == QLatin1String("__resume") ? QStringLiteral("a handler after await")
            : function == QLatin1String("__command")                ? QStringLiteral("run_command")
                                                                    : function;
        script->lastError = name + QStringLiteral(": ") + why;
        qCWarning(lcScripts).noquote() << script->id << name << "failed:" << why;
        // An unloading script goes away anyway: its failure disables nothing.
        if (script->retiring) return;
        // The handler has stopped: its run ends, with the error on stderr.
        if (run) {
            finishRun(script, run, 1,
                      (function == QLatin1String("__command") ? QStringLiteral("The command failed: ")
                                                              : QStringLiteral("The command failed after a wait: "))
                          + why,
                      false);
        }
        if (++script->failures >= MaxFailures) {
            qCWarning(lcScripts).noquote() << script->id << "is disabled after" << MaxFailures
                                           << "failures in a row, until its file changes";
            disable(script, QStringLiteral("The script was disabled after %1 failures in a row").arg(MaxFailures));
        }
        return;
    }
    script->failures = 0;
    // Effects that the handler waits for belong to its run, and the runtime
    // reports the end of a run's handler in the result.
    const CallContext outer = std::exchange(m_call, {script.get(), run, ended, false, flowsBefore, script->flowCount()});
    applyResult(script, out.result, eventPane);
    const bool done = m_call.done;
    m_call = outer;
    // Also a run that ended inside its own effects (its output limit, a
    // client that left): a handler that returned leaves nothing to resume.
    if (run && endedRun(*script, run)) {
        if (done || std::find(script->flowRuns.cbegin(), script->flowRuns.cend(), run) == script->flowRuns.cend()) forgetEnded();
    } else if (run && done) {
        const auto it = script->runs.find(run);
        if (it != script->runs.end()) finishRun(script, run, it->second->status, {}, false);
    }
}

void ScriptHost::applyResult(const ScriptPtr& script, const QVariant& value, quint32 eventPane)
{
    QVariant result = value;
    const bool own = m_call.script == script.get();
    // {__run_done = run, result = effects}: the run's handler has returned.
    // Only the run of this call can end here, so a script that makes one up
    // ends nothing but itself.
    if (own && m_call.run && result.typeId() == QMetaType::QVariantMap) {
        const QVariantMap map = result.toMap();
        if (map.value(RunDone).toString() == QString::number(m_call.run)) {
            m_call.done = true;
            result = map.value(QStringLiteral("result"));
        }
    }
    if (script->retiring || !result.isValid()) {
        return;
    }
    if (result.typeId() == QMetaType::QVariantMap) {
        const QVariantMap map = result.toMap();
        if (map.isEmpty()) {
            return; // {} reads as an empty table either way
        }
        applyEffect(script, map, eventPane);
        return;
    }
    if (result.typeId() == QMetaType::QVariantList) {
        const auto effects = result.toList();
        if (effects.size() > MaxEffectsPerCall) {
            const QString error = QStringLiteral("A handler may return at most %1 effects. No effects applied").arg(MaxEffectsPerCall);
            qCWarning(lcScripts).noquote() << script->id << error;
            ++script->effectErrors;
            script->droppedEffects += effects.size();
            script->lastError = error;
            Q_EMIT effectApplied(script->id, {}, error);
            return;
        }
        // A run that ends inside the list (its output limit, a client that
        // left) takes the rest of the list with it: those effects were its.
        const quint64 run = own && !m_call.ended ? m_call.run : 0;
        for (qsizetype i = 0; i < effects.size(); ++i) {
            if (script->disabled || std::find(m_scripts.begin(), m_scripts.end(), script) == m_scripts.end()) {
                break;
            }
            if (run && !script->runs.count(run)) {
                const auto dropped = std::count_if(effects.begin() + i, effects.end(), [](const QVariant& v) { return v.isValid(); });
                script->droppedEffects += dropped;
                qCInfo(lcScripts).noquote() << script->id << "dropped" << dropped << "effects of run" << run << "after it ended";
                break;
            }
            const QVariant& v = effects[i];
            if (!v.isValid()) {
                continue; // a nil in the list, as in {cond and effect or nil, other}
            }
            applyEffect(script, v.toMap(), eventPane);
        }
        return;
    }
    const QString error = QStringLiteral("a handler returned a %1, not effects").arg(luauType(result));
    qCWarning(lcScripts).noquote() << script->id << error;
    ++script->effectErrors;
    script->lastError = error;
    Q_EMIT effectApplied(script->id, {}, error);
}

void ScriptHost::applyEffect(const ScriptPtr& script, const QVariantMap& raw, quint32 eventPane)
{
    const QString kind = str(raw, "kind");
    const EffectKind* info = nullptr;
    for (const EffectKind& k : Kinds) {
        if (kind == QLatin1String(k.kind)) {
            info = &k;
            break;
        }
    }
    // Rebuilt from the known fields only, with their types settled.
    QVariantMap effect{{QStringLiteral("kind"), kind}};
    EffectResult result;
    QString& error = result.error;
    result.code = QStringLiteral("invalid_effect");
    const QVariant requestValue = raw.value(QStringLiteral("request_id"));
    const QString request = requestValue.toString();
    const bool own = m_call.script == script.get();
    // The await runtime tags the effects of a flow in the call that
    // registers it. A tag for another flow is made up: its result would
    // resume a handler that waits for something else, with a forged result.
    const qint64 flow = flowOf(request);
    const bool forged = request.startsWith(AwaitPrefix) && !(own && flow > m_call.flowsBefore && flow <= m_call.flowsAfter);
    const bool validRequest = requestValue.typeId() == QMetaType::QString && !request.isEmpty() && request.size() <= 64 && !forged;
    // What a run's handler waits for resumes it as that run, also when the
    // effect is refused below: the refusal is its result. The handler waits
    // for this flow only now.
    if (validRequest && flow >= 0 && own && m_call.run) {
        script->flowRuns.removeIf([&](QHash<qint64, quint64>::iterator f) { return f.value() == m_call.run && f.key() != flow; });
        script->flowRuns.insert(flow, m_call.run);
    }
    if (forged) {
        error = QStringLiteral("Request IDs that start with %1 belong to afterglow.await").arg(AwaitPrefix);
    } else if (requestValue.isValid() && !validRequest) {
        // exec and read_file take their task name as the request ID: name what the script gave.
        const bool task = (kind == QLatin1String("exec") || kind == QLatin1String("read_file"))
            && requestValue == raw.value(QStringLiteral("name"));
        error = task ? QStringLiteral("task name must contain 1 to 64 characters")
                     : QStringLiteral("request_id must be a nonempty string of at most 64 characters");
    } else if (!info) {
        error = QStringLiteral("unknown effect \"%1\"").arg(kind);
    } else {
        if (validRequest) {
            effect.insert(QStringLiteral("request_id"), request);
        }
        if (info->pane) {
            const QVariant value = raw.value(QStringLiteral("pane"));
            const double pane = value.toDouble();
            if (value.isValid() && (!number(value) || !std::isfinite(pane) || pane < 1
                                   || pane > std::numeric_limits<quint32>::max() || pane != std::floor(pane))) {
                error = QStringLiteral("pane must be a positive integer ID");
            }
            effect.insert(QStringLiteral("pane"), value.isValid() && error.isEmpty() ? quint32(pane) : eventPane);
        }
        for (const char* key : info->strings) {
            effect.insert(QLatin1String(key), str(raw, key));
        }
        if (str(effect, "cwd").contains(QChar::Null)) error = QStringLiteral("cwd must be a string without NUL");
        if (info->command) {
            const auto argv = command(raw.value(QStringLiteral("command")));
            if (!argv) error = QStringLiteral("command must be a string or a list of strings");
            effect.insert(QStringLiteral("command"), argv.value_or(QStringList{}));
        }
        const bool launch = kind == QLatin1String("split") || kind == QLatin1String("new_tab")
            || kind == QLatin1String("new_window");
        QVariantMap options;
        if (launch) {
            const auto value = raw.value(QStringLiteral("options"));
            if (value.isValid() && value.typeId() != QMetaType::QVariantMap
                && !(value.typeId() == QMetaType::QVariantList && value.toList().isEmpty()))
                error = QStringLiteral("Launch options must be an object");
            options = value.toMap();
            for (auto it = options.begin(); it != options.end(); ++it) {
                if (it.key() == QLatin1String("cwd")) {
                    if (it.value().typeId() != QMetaType::QString || it.value().toString().contains(QChar::Null))
                        error = QStringLiteral("cwd must be a string without NUL");
                } else if (it.key() == QLatin1String("size")) {
                    const auto size = it.value().toDouble();
                    if (!number(it.value()) || !std::isfinite(size) || size <= 0 || size >= 1)
                        error = QStringLiteral("size must be a fraction between zero and one");
                } else if (it.key() != QLatin1String("env")) error = QStringLiteral("Unknown launch option %1").arg(it.key());
            }
        }
        if (launch || kind == QLatin1String("exec")) {
            QVariantMap env;
            if (const QString problem = EnvironmentOverrides::read((launch ? options : raw).value(QStringLiteral("env")), env);
                !problem.isEmpty())
                error = problem;
            if (launch) effect.insert(QStringLiteral("options"), options);
            else effect.insert(QStringLiteral("env"), env);
        }
        QString badNumber;
        for (const char* key : info->numbers) {
            const QVariant v = raw.value(QLatin1String(key));
            const bool numeric = !v.isValid() || number(v);
            if (!numeric) {
                badNumber = QLatin1String(key);
            }
            if (v.isValid()) effect.insert(QLatin1String(key), v.toDouble());
        }
        // A package names its packs' pulse effects.
        if (kind == QLatin1String("pulse")) {
            if (const int number = script->pulses.value(str(effect, "effect")); number > 0)
                effect.insert(QStringLiteral("effect"), QString::number(number));
        }
        if (kind == QLatin1String("read_text") || kind == QLatin1String("read_region")) {
            const QVariant limit = raw.value(QStringLiteral("max_bytes"), 65536);
            if (!integer(limit, 1, MaxReadTextBytes))
                error = QStringLiteral("max_bytes must be a whole number from 1 to %1").arg(MaxReadTextBytes);
            // The helpers' default, also for an effect built by hand.
            effect.insert(QStringLiteral("max_bytes"), limit.toDouble());
        }
        // Pane text that a run's handler reads can go to its client, a
        // process outside the window: Control.allowReadText decides then.
        if ((kind == QLatin1String("read_text") || kind == QLatin1String("read_region")
             || kind == QLatin1String("semantic_regions"))
            && own && m_call.run && !m_call.ended) {
            const auto run = script->runs.find(m_call.run);
            if (run != script->runs.end() && run->second->sink.finished) effect.insert(QStringLiteral("client_run"), true);
        }
        if (kind == QLatin1String("shader_parameters") || kind == QLatin1String("shader_preset")) {
            const auto values = raw.value(QStringLiteral("values"));
            const auto scope = effect.value(QStringLiteral("scope")).toString();
            if (kind == QLatin1String("shader_parameters") && (values.typeId() != QMetaType::QVariantMap || values.toMap().size() > 52))
                error = QStringLiteral("Shader values must be an object with at most 52 parameters");
            if (scope != QLatin1String("pane") && scope != QLatin1String("tab") && scope != QLatin1String("window"))
                error = QStringLiteral("Shader scope must be pane, tab or window");
            const auto instance = effect.value(QStringLiteral("instance")).toString();
            if (instance.isEmpty() || instance.size() > 128) error = QStringLiteral("Invalid shader instance ID");
            const auto transition = raw.value(QStringLiteral("transition"));
            if (transition.isValid() && transition.typeId() != QMetaType::QVariantMap)
                error = QStringLiteral("Transition must be an object table");
            effect.insert(QStringLiteral("transition"), transition.toMap());
            if (kind == QLatin1String("shader_parameters")) effect.insert(QStringLiteral("values"), values);
        }
        if (kind == QLatin1String("state_write")) {
            const auto value = raw.value(QStringLiteral("value"));
            const auto revision = effect.value(QStringLiteral("revision"), -1).toDouble();
            if (value.typeId() != QMetaType::QVariantMap) error = QStringLiteral("State value must be an object table");
            else error = ScriptState::validate(value.toMap());
            if (!effect.contains(QStringLiteral("revision")) || !std::isfinite(revision) || revision < 0
                || revision >= std::numeric_limits<int>::max() || revision != std::floor(revision)) error = QStringLiteral("State write needs an integer revision");
            effect.insert(QStringLiteral("value"), value);
        }
        if (kind == QLatin1String("save_project") || kind == QLatin1String("open_project")) {
            const auto key = kind == QLatin1String("save_project") ? QStringLiteral("overwrite") : QStringLiteral("new_instance");
            const auto value = raw.value(key);
            if (value.isValid() && value.metaType().id() != QMetaType::Bool) error = key + QStringLiteral(" must be boolean");
            effect.insert(key, value.toBool());
        }
        if (kind == QLatin1String("move_pane")) {
            const double target = effect.value(QStringLiteral("target"), 0).toDouble();
            const QString placement = str(effect, "placement");
            const bool beside = placement == QLatin1String("left") || placement == QLatin1String("right")
                || placement == QLatin1String("up") || placement == QLatin1String("down");
            const bool away = placement == QLatin1String("new-tab") || placement == QLatin1String("new-window");
            if (!std::isfinite(target) || target < 0 || target > std::numeric_limits<quint32>::max() || target != std::floor(target))
                error = QStringLiteral("target must be a pane ID or zero");
            else if (!beside && !away)
                error = QStringLiteral("placement must be left, right, up, down, new-tab or new-window");
            else if (beside == (target == 0))
                error = QStringLiteral("left, right, up and down need a target pane. new-tab and new-window take none");
        }
        if (kind == QLatin1String("overlay")) {
            // The helper passes the options through whole: OverlaySpec, the
            // rules of the control socket's overlay call too, sees their own
            // types and refuses a misspelled one. The spec that it reads goes
            // on to the window, so a long stdin is converted once.
            const QVariant value = raw.value(QStringLiteral("options"));
            if (value.isValid() && !value.isNull() && value.typeId() != QMetaType::QVariantMap
                && !(value.typeId() == QMetaType::QVariantList && value.toList().isEmpty()))
                error = QStringLiteral("Overlay options must be an object");
            else if (error.isEmpty()) {
                QString problem;
                if (auto spec = OverlaySpec::read(effect.value(QStringLiteral("command")).toStringList(), value.toMap(), &problem))
                    effect.insert(QStringLiteral("spec"), QVariant::fromValue(std::move(*spec)));
                else
                    error = problem;
            }
        }
        if (kind == QLatin1String("select")) {
            const auto entries = raw.value(QStringLiteral("items")).toList();
            QVariantList checked;
            QSet<QString> ids;
            if (entries.size() > 500) error = QStringLiteral("Too many entries");
            if (entries.isEmpty()) error = QStringLiteral("A selector needs at least one item");
            for (const auto& item : entries) {
                const auto map = item.toMap();
                const auto id = str(map, "id"), title = str(map, "title");
                if (id.isEmpty() || id.size() > 64 || title.size() > 512 || ids.contains(id)) { error = QStringLiteral("Invalid or duplicate entry"); break; }
                ids.insert(id); checked << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("title"), title.isEmpty() ? id : title}};
            }
            effect.insert(QStringLiteral("items"), checked);
        }
        if (kind == QLatin1String("set_commands")) {
            // The shape and the rules of the manifest's commands table.
            QList<ScriptCommand> runtime;
            QList<ScriptCommand> manifest;
            for (const ScriptCommand& c : std::as_const(script->commands)) {
                if (!c.runtime) manifest.append(c);
            }
            if (!script->engine->hasFunction(script->module, QStringLiteral("run_command")))
                error = QStringLiteral("set_commands needs a run_command function");
            else if (const QString problem = readCommands(raw.value(QStringLiteral("commands")), runtime); !problem.isEmpty())
                error = problem;
            else if (manifest.size() + runtime.size() > MaxScriptCommands)
                error = QStringLiteral("A script has at most %1 commands").arg(MaxScriptCommands);
            QVariantMap checked;
            for (ScriptCommand& command : runtime) {
                if (error.isEmpty() && std::any_of(manifest.cbegin(), manifest.cend(), [&](const ScriptCommand& c) { return c.name == command.name; }))
                    error = QStringLiteral("Command %1 is in the manifest already").arg(command.name);
                checked.insert(command.name, QVariantMap{{QStringLiteral("summary"), command.summary},
                                                         {QStringLiteral("usage"), command.usage},
                                                         {QStringLiteral("palette"), command.palette},
                                                         {QStringLiteral("stdin"), command.input},
                                                         {QStringLiteral("remote"), command.remote}});
            }
            effect.insert(QStringLiteral("commands"), checked);
        }
        if (kind == QLatin1String("output") || kind == QLatin1String("output_error") || kind == QLatin1String("exit")) {
            if (raw.value(QStringLiteral("run")).typeId() != QMetaType::QString)
                error = QStringLiteral("%1 needs the run of a run_command event (ev.run)").arg(kind);
            else if (kind != QLatin1String("exit") && raw.value(QStringLiteral("text")).typeId() != QMetaType::QString)
                error = QStringLiteral("%1 needs text").arg(kind);
            else if (kind == QLatin1String("exit") && !integer(raw.value(QStringLiteral("code")), 0, 255))
                error = QStringLiteral("An exit status is a whole number from 0 to 255");
        }
        if (kind == QLatin1String("override")) {
            const auto values = raw.value(QStringLiteral("values")).toMap();
            if (values.size() > 3) error = QStringLiteral("Too many overrides");
            for (auto it = values.begin(); it != values.end(); ++it) {
                if (it.key() == QLatin1String("cursorBlink")) {
                    if (it.value().typeId() != QMetaType::Bool) error = QStringLiteral("cursorBlink must be a boolean");
                } else if (it.key() == QLatin1String("backgroundOpacity") || it.key() == QLatin1String("unfocusedDim")) {
                    const double value = it.value().toDouble();
                    if (!number(it.value()) || !std::isfinite(value) || value < 0 || value > 1) error = QStringLiteral("Opacity overrides must be between 0 and 1");
                } else error = QStringLiteral("Unknown override %1").arg(it.key());
            }
            effect.insert(QStringLiteral("values"), values);
        }
        if ((kind == QLatin1String("select") || kind == QLatin1String("prompt"))
            && (str(effect, "title").size() > 512 || str(effect, "text").size() > 4096))
            error = QStringLiteral("Dialog text exceeds its length limit");
        if (kind == QLatin1String("status") && (str(effect, "text").size() > 512 || str(effect, "tab_title").size() > 128))
            error = QStringLiteral("Status text exceeds its length limit");
        if (kind == QLatin1String("dock") || kind == QLatin1String("undock")) {
            const QVariant id = raw.value(QStringLiteral("id"));
            const QVariant scope = raw.value(QStringLiteral("scope"), QStringLiteral("window"));
            if (id.typeId() != QMetaType::QString || !validName(id.toString()) || id.toString().size() > 32)
                error = QStringLiteral("A dock id is a string of 1 to 32 ASCII letters, digits, - or _");
            else if (scope != QLatin1String("window") && scope != QLatin1String("pane"))
                error = QStringLiteral("Dock scope must be window or pane");
            else if (kind == QLatin1String("dock"))
                if (const QString problem = checkDock(raw, effect); !problem.isEmpty()) error = problem;
            effect.insert(QStringLiteral("scope"), scope.toString());
        }
        if (kind == QLatin1String("signals")) {
            const auto values = raw.value(QStringLiteral("values")).toList();
            QVariantList checked;
            QSet<int> seen;
            if (values.isEmpty() || values.size() > SignalChannels) {
                error = QStringLiteral("a signal batch needs 1 to %1 channels").arg(SignalChannels);
            }
            for (const auto& value : values) {
                const auto entry = value.toMap();
                const auto channel = entry.value(QStringLiteral("channel"));
                const auto amount = entry.value(QStringLiteral("value"));
                if (!validSignal(channel, amount) || seen.contains(channel.toInt())) {
                    error = QStringLiteral("invalid or duplicate signal channel/value");
                    break;
                }
                seen.insert(channel.toInt());
                checked.append(QVariantMap{{QStringLiteral("channel"), channel.toInt()}, {QStringLiteral("value"), amount.toDouble()}});
            }
            effect.insert(QStringLiteral("values"), checked);
            const QString scope = str(effect, "scope");
            if (scope != QLatin1String("pane") && scope != QLatin1String("tab") && scope != QLatin1String("window")) {
                error = QStringLiteral("signal scope must be pane, tab or window");
            }
        }
        // An undock may name a pane that has closed or moved: a window area
        // stays in the window it was docked in, where Control finds it. A
        // pane area goes with its pane and closes with it (no_pane then).
        const bool paneMustExist = info->pane && kind != QLatin1String("undock");
        const bool structural = info->structural || (kind == QLatin1String("action")
            && (!m_callbacks.structuralAction || m_callbacks.structuralAction(str(effect, "name"))));
        if (!error.isEmpty()) {
            // A validation failure must never reach the host callbacks.
        } else if (!badNumber.isEmpty()) {
            error = QStringLiteral("%1: %2 must be a number").arg(kind, badNumber);
        } else if (info->pane && effect.value(QStringLiteral("pane")).toUInt() == 0) {
            error = QStringLiteral("%1 needs a pane").arg(kind);
        } else if (kind == QLatin1String("split") && effect.value(QStringLiteral("direction")) != QLatin1String("right")
                   && effect.value(QStringLiteral("direction")) != QLatin1String("down")
                   && effect.value(QStringLiteral("direction")) != QLatin1String("left")
                   && effect.value(QStringLiteral("direction")) != QLatin1String("up")) {
            error = QStringLiteral("split direction must be right, down, left or up");
        } else if (kind == QLatin1String("key_mode") && !script->engine->hasFunction(script->module, QStringLiteral("on_key"))) {
            error = QStringLiteral("key_mode requires an on_key handler");
        } else if (kind == QLatin1String("key_mode") && str(effect, "label").size() > 160) {
            error = QStringLiteral("Key mode label exceeds 160 characters");
        } else if (kind == QLatin1String("overview") && !QStringList{QStringLiteral("open"), QStringLiteral("close"), QStringLiteral("choose")}.contains(str(effect, "operation"))) {
            error = QStringLiteral("overview operation must be open, close or choose");
        } else if (kind == QLatin1String("overview_move") && !(integer(effect.value(QStringLiteral("step"), 0), -32, 32)
                                                              && integer(effect.value(QStringLiteral("target"), 0), 0, std::numeric_limits<quint32>::max()))) {
            error = QStringLiteral("overview_move needs a pane or a whole step of at most 32 cards");
        } else if (kind == QLatin1String("move_tab") && !integer(effect.value(QStringLiteral("index")), 0, std::numeric_limits<int>::max())) {
            error = QStringLiteral("move_tab needs a tab index of 0 or more");
        } else if (kind == QLatin1String("pulse") && pulseEffect(str(effect, "effect")) == 0) {
            error = QStringLiteral("pulse effect must be portal, flash, shake or a number from 1 to 255");
        } else if (kind == QLatin1String("pulse") && !(effect.value(QStringLiteral("duration_ms")).toDouble() >= 50
                                                      && effect.value(QStringLiteral("duration_ms")).toDouble() <= 10000
                                                      && integer(effect.value(QStringLiteral("color")), 0, 255))) {
            error = QStringLiteral("pulse needs 50 to 10000 ms and a palette color from 0 to 255");
        } else if (kind == QLatin1String("sleep") && !request.startsWith(AwaitPrefix)) {
            error = QStringLiteral("sleep must be awaited: use afterglow.sleep");
        } else if (kind == QLatin1String("sleep") && !(effect.value(QStringLiteral("delay_ms")).toDouble() >= 0
                                                      && effect.value(QStringLiteral("delay_ms")).toDouble() <= MaxSleepMs)) {
            error = QStringLiteral("sleep needs 0 to %1 ms").arg(MaxSleepMs);
        } else if (kind == QLatin1String("action") && effect.value(QStringLiteral("name")).toString().startsWith(Keybinds::ScriptPrefix)) {
            error = QStringLiteral("Scripts cannot run script commands");
        } else if (kind == QLatin1String("signal")
                   && !validSignal(effect.value(QStringLiteral("channel")), effect.value(QStringLiteral("value")))) {
            error = QStringLiteral("invalid signal channel/value");
        } else if (paneMustExist
                   && (!m_callbacks.paneInfo || m_callbacks.paneInfo(effect.value(QStringLiteral("pane")).toUInt()).isEmpty())) {
            result.code = QStringLiteral("no_pane");
            error = QStringLiteral("no pane %1").arg(effect.value(QStringLiteral("pane")).toUInt());
        } else if (!budgetFree(kind) && !withinBudget(*script, kind, structural)) {
            ++script->droppedEffects;
            result.code = QStringLiteral("rate_limited");
            error = QStringLiteral("Too many effects. %1 dropped").arg(kind);
        } else if (kind == QLatin1String("state_read") || kind == QLatin1String("state_write")) {
            ScriptState::Snapshot saved;
            ScriptState store(m_stateDirectory);
            result.code = QStringLiteral("state_error");
            error = kind == QLatin1String("state_read") ? store.read(script->id, saved)
                : store.write(script->id, script->stateVersion, effect.value(QStringLiteral("revision")).toInt(), effect.value(QStringLiteral("value")).toMap(), saved);
            if (error.isEmpty() && saved.version > script->stateVersion) error = QStringLiteral("Stored state is newer than this script");
            if (error.isEmpty()) result.data = {{QStringLiteral("value"), saved.value}, {QStringLiteral("version"), saved.version ? saved.version : script->stateVersion},
                                               {QStringLiteral("revision"), saved.revision}};
        } else if (kind == QLatin1String("sleep")) {
            if (script->sleeps.size() >= MaxSleeping) {
                result.code = QStringLiteral("busy");
                error = QStringLiteral("Too many sleeps");
            } else {
                result.deferred = true;
                // Kept, so that the end of a run stops the sleep its handler waits for.
                auto* timer = new QTimer(this);
                timer->setSingleShot(true);
                script->sleeps.insert(timer, request);
                connect(timer, &QTimer::timeout, this,
                        [this, timer, weak = std::weak_ptr<Script>(script), request, kind, eventPane, timeout = script->callTimeoutMs] {
                            timer->deleteLater();
                            const auto script = weak.lock();
                            if (script && script->sleeps.remove(timer)) queueResult(script, request, kind, {}, eventPane, timeout);
                        });
                timer->start(qRound(effect.value(QStringLiteral("delay_ms")).toDouble()));
            }
        } else if (kind == QLatin1String("log")) {
            qCInfo(lcScripts).noquote() << script->id + QLatin1Char(':')
                                        << loggable(effect.value(QStringLiteral("message")).toString(), MaxLogCharacters);
        } else if (kind == QLatin1String("set_commands")) {
            QList<ScriptCommand> commands;
            for (const ScriptCommand& c : std::as_const(script->commands)) {
                if (!c.runtime) commands.append(c);
            }
            const QVariantMap runtime = effect.value(QStringLiteral("commands")).toMap();
            for (auto it = runtime.cbegin(); it != runtime.cend(); ++it) {
                const auto item = it.value().toMap();
                commands.append({script->id, it.key(), str(item, "summary"), str(item, "usage"), str(item, "palette"),
                                 item.value(QStringLiteral("stdin")).toBool(), true, item.value(QStringLiteral("remote")).toBool()});
            }
            const QList<ScriptCommand> before = this->commands();
            script->commands = commands;
            announceCommands(before);
        } else if (kind == QLatin1String("output") || kind == QLatin1String("output_error") || kind == QLatin1String("exit")) {
            // A script reaches its own runs only.
            const quint64 handle = str(effect, "run").toULongLong();
            const auto run = script->runs.find(handle);
            result.code = QStringLiteral("no_run");
            if (own && m_call.run == handle && endedRun(*script, handle)) {
                // Its own handler resumed with "cancelled": the client has its
                // status already. Another handler is told that the run has ended.
            } else if (run == script->runs.end()) {
                // Another script's live run is none of this one's either.
                error = QStringLiteral("Run %1 has ended, or is not a run of this script").arg(loggable(str(effect, "run"), 64));
            } else if (kind == QLatin1String("exit")) {
                run->second->status = int(effect.value(QStringLiteral("code")).toDouble());
            } else {
                // Its result comes once the text has gone to the client, which
                // can be later for a client that reads slowly, or once the
                // text was dropped. Text that went to a client is not counted
                // toward the results each second: the run's output and time
                // limits bound it. A run with no client drops its stdout at
                // once, so a handler that awaits it in a loop would resume on
                // each pass of the event loop: text that no client takes is
                // counted, and so is an empty text.
                const QString text = str(effect, "text");
                const bool counted = text.isEmpty() || !run->second->sink.toClient(kind == QLatin1String("output_error"));
                result = writeRun(script, handle, text, kind == QLatin1String("output_error"),
                    [this, weak = std::weak_ptr<Script>(script), request, kind, eventPane, counted,
                     timeout = script->callTimeoutMs](bool delivered) {
                        const auto script = weak.lock();
                        if (!script || request.isEmpty()) return;
                        queueResult(script, request, kind,
                                    delivered ? EffectResult{}
                                              : EffectResult{QStringLiteral("The run has ended"), {}, QStringLiteral("cancelled")},
                                    eventPane, timeout, counted || !delivered);
                    });
            }
        } else if (kind == QLatin1String("timer") || kind == QLatin1String("cancel") || kind == QLatin1String("exec")
                   || kind == QLatin1String("read_file") || kind == QLatin1String("watch_file")) {
            result = m_tasks->apply(script->owner, effect, completion(script, request, kind, eventPane),
                [this, weak = std::weak_ptr<Script>(script), eventPane](const QString& handler, QVariantMap event) {
                    if (auto script = weak.lock()) emitOwned(script, handler, std::move(event), eventPane);
                });
        } else if (m_callbacks.applyOwned || m_callbacks.apply) {
            if (kind == QLatin1String("pulse")) {
                // Named or numbered, the window takes the effect's number.
                effect.insert(QStringLiteral("effect"), QString::number(pulseEffect(str(effect, "effect"))));
            }
            result = m_callbacks.applyOwned ? m_callbacks.applyOwned(script->owner, effect, completion(script, request, kind, eventPane))
                                            : m_callbacks.apply(effect);
            if (!result.error.isEmpty() && result.code.isEmpty()) {
                result.code = QStringLiteral("operation_failed");
            }
        } else {
            result.code = QStringLiteral("unavailable");
            error = QStringLiteral("no effect handler is available");
        }
    }
    if (!error.isEmpty()) {
        ++script->effectErrors;
        script->lastError = error;
        qCWarning(lcScripts).noquote() << script->id + QLatin1Char(':') << error;
    }
    Q_EMIT effectApplied(script->id, effect, error);
    if (validRequest && !result.deferred) {
        queueResult(script, request, kind, result, eventPane, script->callTimeoutMs);
    }
}

std::function<void(ScriptHost::EffectResult)> ScriptHost::completion(const ScriptPtr& script, const QString& request,
                                                                     const QString& kind, quint32 eventPane)
{
    return [this, weak = std::weak_ptr<Script>(script), request, kind, eventPane, timeout = script->callTimeoutMs](EffectResult finished) {
        if (auto script = weak.lock()) {
            if (!finished.error.isEmpty()) { ++script->effectErrors; script->lastError = finished.error; }
            if (!request.isEmpty()) queueResult(script, request, kind, finished, eventPane, timeout);
        }
    };
}

void ScriptHost::queueResult(const ScriptPtr& script, const QString& request, const QString& kind,
                             const EffectResult& result, quint32 eventPane, int timeoutMs, bool counted)
{
    // An awaited result resumes its handler, under that handler's time limit.
    const QString handler = request.startsWith(AwaitPrefix) ? QStringLiteral("__resume") : QStringLiteral("on_result");
    if (script->disabled || script->retiring || !script->engine->hasFunction(script->module, handler)) {
        return;
    }
    const qint64 now = m_clock.elapsed();
    while (!script->results.empty() && now - script->results.front() >= 1000) {
        script->results.pop_front();
    }
    const bool queued = script->pendingResults >= MaxPendingResults;
    if (queued || (counted && int(script->results.size()) >= MaxResultsPerSecond)) {
        ++script->droppedResults;
        script->lastError = QStringLiteral("Too many operation results");
        // Include refusals in this budget: a handler that retries a rejected
        // request from on_result must not create an endless queued feedback loop.
        qCWarning(lcScripts) << script->id << "disabled: too many operation results";
        disable(script, queued ? QStringLiteral("The script was disabled: %1 operation results waited for it").arg(MaxPendingResults)
                               : QStringLiteral("The script was disabled: it got more than %1 operation results in one second")
                                     .arg(MaxResultsPerSecond));
        return;
    }
    if (counted) script->results.push_back(now);
    ++script->pendingResults;
    QVariantMap event{{QStringLiteral("request_id"), request}, {QStringLiteral("kind"), kind},
                      {QStringLiteral("ok"), result.error.isEmpty()}, {QStringLiteral("data"), result.data}};
    if (!result.error.isEmpty()) {
        event.insert(QStringLiteral("error"), QVariantMap{{QStringLiteral("code"), result.code},
                                                          {QStringLiteral("message"), result.error}});
    }
    // Queued after application. Never re-enter a VM, and never deliver an old
    // instance's completions to its replacement following a reload.
    QTimer::singleShot(0, this, [this, weak = std::weak_ptr<Script>(script), event = std::move(event), eventPane, handler, timeoutMs]() mutable {
        const auto script = weak.lock();
        if (!script) {
            return;
        }
        --script->pendingResults;
        if (script->disabled || std::find(m_scripts.begin(), m_scripts.end(), script) == m_scripts.end()) {
            return;
        }
        // A result that resumes a run's handler runs as that run, while it lasts.
        quint64 run = handler == QLatin1String("__resume") ? script->flowRuns.value(flowOf(event.value(QStringLiteral("request_id")).toString())) : 0;
        // Queued before the run ended: its "cancelled" resume, still due,
        // answers this flow instead.
        if (run && script->cancelsDue.contains(run)) return;
        if (run && !script->runs.count(run) && !endedRun(*script, run)) run = 0;
        event.insert(QStringLiteral("terminal"), state());
        if (eventPane) {
            event.insert(QStringLiteral("pane"), this->eventPane(eventPane));
        }
        call(script, handler, {event}, handler == QLatin1String("on_result") ? EventTimeoutMs : timeoutMs, eventPane, run);
    });
}

bool ScriptHost::withinBudget(Script& script, const QString& kind, bool structural)
{
    const qint64 now = m_clock.elapsed();
    auto admit = [now](std::deque<qint64>& times, qint64 window, int limit) {
        while (!times.empty() && now - times.front() >= window) {
            times.pop_front();
        }
        return int(times.size()) < limit;
    };
    if (kind == QLatin1String("dock")) {
        if (!admit(script.docks, 1000, MaxDockEffectsPerSecond)) return false;
        script.docks.push_back(now);
        return true;
    }
    if (!admit(script.effects, 1000, MaxEffectsPerSecond) || (structural && !admit(script.structural, 10000, MaxStructuralPer10s))) {
        return false;
    }
    script.effects.push_back(now);
    if (structural) {
        script.structural.push_back(now);
    }
    return true;
}

} // namespace Afterglow
