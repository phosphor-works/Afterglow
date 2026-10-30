// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScriptHost.h"
#include "ScriptTasks.h"
#include "ScriptFiles.h"
#include "ScriptPackage.h"
#include "ScriptState.h"
#include <QCryptographicHash>
#include <QJsonDocument>

#include <PhosphorScripting/LuauEngine.h>
#include <PhosphorScripting/LuauWatchdog.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <cmath>
#include <deque>
#include <limits>
#include <map>

Q_LOGGING_CATEGORY(lcScripts, "afterglow.scripts", QtInfoMsg)

namespace Afterglow {

using PhosphorScripting::LuauEngine;

struct ScriptHost::Script
{
    QString id;
    QString owner;
    bool retiring = false;
    std::deque<qint64> callbacks;
    QString name;
    QString path;
    QByteArray source;
    bool package = false;
    QVariantMap packageInfo;
    ScriptState::Snapshot storage;
    int stateVersion = 1;
    bool migrateState = false;
    QVariant initialEffects;
    std::unique_ptr<LuauEngine> engine;
    int module = -1;
    QList<ScriptAction> actions;
    int failures = 0;
    quint64 calls = 0, handlerErrors = 0, effectErrors = 0;
    quint64 droppedCallbacks = 0, droppedEffects = 0, droppedResults = 0;
    qint64 lastCallNs = 0, maxCallNs = 0, totalCallNs = 0;
    QString lastError;
    bool disabled = false;
    std::deque<qint64> effects; ///< when recent effects ran, for the budgets
    std::deque<qint64> structural;
    std::deque<qint64> results;
    int pendingResults = 0;
    LuauEngine::CallOutcome invoke(const QString& function, const QVariantList& args, int timeoutMs)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        auto outcome = engine->callModule(module, function, args, timeoutMs);
        ++calls;
        lastCallNs = elapsed.nsecsElapsed();
        maxCallNs = std::max(maxCallNs, lastCallNs);
        totalCallNs += lastCallNs;
        return outcome;
    }
};

namespace {

constexpr int ReloadDelayMs = 200;

bool validId(const QString& id)
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
    return re.match(id).hasMatch();
}

QString str(const QVariantMap& m, const char* key)
{
    const QVariant v = m.value(QLatin1String(key));
    return v.isValid() ? v.toString() : QString();
}

/// A command given as a string or a list of strings.
QStringList command(const QVariant& v)
{
    if (v.typeId() == QMetaType::QString) {
        return v.toString().isEmpty() ? QStringList{} : QStringList{v.toString()};
    }
    QStringList out;
    for (const QVariant& part : v.toList()) {
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
    {"cancel", false, false, {"name"}, false},
    {"exec", false, false, {"name", "cwd"}, true, {"timeout_ms", "max_bytes"}},
    {"read_file", false, false, {"name", "path"}, false, {"timeout_ms", "max_bytes"}},
    {"watch_file", false, false, {"name", "path"}, false},
    {"set_actions", false, false, {}, false},
    {"select", true, false, {"title"}, false},
    {"prompt", true, false, {"title", "text"}, false},
    {"status", true, false, {"text", "tab_title"}, false},
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
    {"shaders", true, false, {}, false},
    {"shader_presets", true, false, {"instance"}, false},
    {"shader_preset", true, false, {"instance", "preset", "scope"}, false},
    {"save_shader_preset", true, false, {"instance", "name"}, false},
    {"scene_state", true, false, {}, false},
    {"shader_parameters", true, false, {"instance", "scope"}, false},
};

bool number(const QVariant& v)
{
    return v.typeId() == QMetaType::Double || v.typeId() == QMetaType::Int || v.typeId() == QMetaType::LongLong;
}

bool validSignal(const QVariant& channel, const QVariant& value)
{
    const double c = channel.toDouble(), v = value.toDouble();
    return number(channel) && number(value) && std::isfinite(c) && c == std::floor(c) && c >= 0 && c < 8
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
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(ReloadDelayMs);
    connect(&m_reloadTimer, &QTimer::timeout, this, &ScriptHost::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_reloadTimer, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_reloadTimer, qOverload<>(&QTimer::start));
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
    if (m_enabled) {
        reload();
    }
}

void ScriptHost::setEnabled(bool enabled)
{
    if (enabled == m_enabled) {
        return;
    }
    m_enabled = enabled;
    reload();
}

void ScriptHost::reload()
{
    m_reloadTimer.stop();
    std::vector<ScriptPtr> loaded;
    m_packageWatches.clear();
    m_loadErrors.clear();
    if (m_enabled) {
        // id -> path, later directories winning.
        std::map<QString, std::pair<int, QString>> chosen;
        int priority = 0;
        for (const QString& dir : std::as_const(m_searchPaths)) {
            const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.luau")}, QDir::Files | QDir::Readable,
                                                                QDir::Name);
            for (const QFileInfo& f : files) {
                if (!f.fileName().endsWith(QLatin1String(".d.luau"))) {
                    chosen[f.completeBaseName()] = {priority, f.absoluteFilePath()};
                }
            }
            for (const auto& folder : QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
                m_packageWatches << folder.absoluteFilePath();
                const auto manifest = folder.absoluteFilePath() + QStringLiteral("/package.json");
                if (QFileInfo::exists(manifest)) chosen[folder.fileName()] = {priority, manifest};
            }
            ++priority;
        }
        std::vector<std::pair<QString, std::pair<int, QString>>> ordered(chosen.begin(), chosen.end());
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
            return a.second.first < b.second.first;
        });
        for (const auto& [base, entry] : ordered) {
            const auto& path = entry.second;
            ScriptPackage package;
            const bool isPackage = QFileInfo(path).fileName() == QLatin1String("package.json");
            QByteArray source;
            QString failure;
            if (isPackage) {
                failure = ScriptPackage::read(path, package);
                m_packageWatches << package.watched;
                if (failure.isEmpty() && package.id != base) failure = QStringLiteral("Package id must match its directory name");
                source = package.source;
                const QVariantMap identity{{QStringLiteral("id"), package.id}, {QStringLiteral("version"), package.version},
                    {QStringLiteral("state_version"), package.stateVersion}, {QStringLiteral("assets"), package.assets}, {QStringLiteral("config"), package.config}};
                source += "\n-- metadata: " + QCryptographicHash::hash(QJsonDocument::fromVariant(identity).toJson(), QCryptographicHash::Sha256).toHex();
            } else {
                failure = readScriptFile(path, 1024 * 1024, source);
            }
            auto same = std::find_if(m_scripts.begin(), m_scripts.end(),
                                     [&](const ScriptPtr& s) { return s->path == path && s->source == source; });
            ScriptPtr script;
            if (failure.isEmpty()) script = same != m_scripts.end() ? *same : load(path, source, isPackage ? &package : nullptr);
            else { m_loadErrors[path] = failure; qCWarning(lcScripts).noquote() << path << failure; }
            if (!script) {
                // A broken edit keeps its last working instance and resources.
                const auto old = std::find_if(m_scripts.begin(), m_scripts.end(), [&](const ScriptPtr& s) { return s->path == path; });
                if (old == m_scripts.end()) continue;
                script = *old;
            }
            auto clash = std::find_if(loaded.begin(), loaded.end(), [&](const ScriptPtr& s) { return s->id == script->id; });
            if (clash != loaded.end()) {
                qCWarning(lcScripts) << path << "uses the id" << script->id << "of" << (*clash)->path << "and replaces it";
                *clash = script;
            } else {
                loaded.push_back(script);
            }
        }
    }
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
                qCWarning(lcScripts).noquote() << script->id << "migration not committed:" << error;
                const auto old = std::find_if(m_scripts.begin(), m_scripts.end(), [&](const ScriptPtr& s) { return s->path == script->path; });
                if (old == m_scripts.end()) { it = loaded.erase(it); continue; }
                script = *old;
            } else { script->storage = committed; script->migrateState = false; }
        }
        ++it;
    }
    QList<ScriptAction> before = actions();
    const auto previous = m_scripts;
    for (const auto& script : previous) {
        if (std::find(loaded.begin(), loaded.end(), script) == loaded.end()) retire(script);
    }
    m_scripts = std::move(loaded);
    watch();
    for (const auto& script : m_scripts) {
        if (std::find(previous.begin(), previous.end(), script) == previous.end()) {
            if (script->package) applyResult(script, script->initialEffects, 0);
            else emitOwned(script, QStringLiteral("on_load"), {}, 0);
        }
    }
    const QList<ScriptAction> after = actions();
    const bool changed = before.size() != after.size()
        || !std::equal(before.cbegin(), before.cend(), after.cbegin(),
                       [](const ScriptAction& a, const ScriptAction& b) { return a.token == b.token && a.title == b.title; });
    if (changed) {
        Q_EMIT actionsChanged();
    }
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
    script->module = script->engine->loadModule(chunk, source, &error);
    if (script->module < 0) {
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
    if (!validId(script->id)) {
        m_loadErrors[path] = QStringLiteral("Invalid script id");
        qCWarning(lcScripts) << path << "has an unusable id" << script->id << "(letters, digits, _ and - only)";
        return nullptr;
    }
    static quint64 nextOwner = 0;
    script->owner = script->id + QLatin1Char('@') + QString::number(++nextOwner);
    script->name = str(meta, "name");
    if (script->name.isEmpty()) {
        script->name = script->id;
    }
    const bool runs = script->engine->hasFunction(script->module, QStringLiteral("run_action"));
    for (const QVariant& v : meta.value(QStringLiteral("actions")).toList()) {
        const QVariantMap a = v.toMap();
        const QString id = str(a, "id");
        if (!validId(id)) {
            qCWarning(lcScripts) << path << "has an action with an unusable id" << id;
            continue;
        }
        if (!runs) {
            qCWarning(lcScripts) << path << "lists actions but has no run_action function";
            break;
        }
        const QString title = str(a, "title");
        script->actions.append({script->id + QLatin1Char('.') + id, title.isEmpty() ? id : title});
    }
    if (package) {
        script->package = true;
        script->stateVersion = package->stateVersion;
        script->packageInfo = {{QStringLiteral("id"), package->id}, {QStringLiteral("version"), package->version},
            {QStringLiteral("api_version"), 1}, {QStringLiteral("assets"), package->assets}, {QStringLiteral("home"), QDir::homePath()}, {QStringLiteral("config"), package->config}};
        error = ScriptState(m_stateDirectory).read(script->id, script->storage);
        if (error.isEmpty() && script->storage.version > script->stateVersion) error = QStringLiteral("Stored state is newer than this package");
        if (error.isEmpty() && script->storage.version > 0 && script->storage.version < script->stateVersion) {
            if (!script->engine->hasFunction(script->module, QStringLiteral("migrate"))) error = QStringLiteral("State migration function is missing");
            else {
                const auto migrated = script->invoke(QStringLiteral("migrate"),
                    {script->storage.value, script->storage.version, script->stateVersion}, ActionTimeoutMs);
                if (migrated.status != LuauEngine::CallStatus::Ok || migrated.result.typeId() != QMetaType::QVariantMap)
                    error = QStringLiteral("Migration failed: ") + migrated.message;
                else {
                    script->storage.value = migrated.result.toMap();
                    error = ScriptState::validate(script->storage.value);
                    script->migrateState = error.isEmpty();
                }
            }
        }
        if (error.isEmpty() && script->engine->hasFunction(script->module, QStringLiteral("on_load"))) {
            const QVariantMap storage{{QStringLiteral("value"), script->storage.value}, {QStringLiteral("version"), script->stateVersion},
                {QStringLiteral("revision"), script->storage.revision + (script->migrateState ? 1 : 0)}};
            const auto initialized = script->invoke(QStringLiteral("on_load"),
                {QVariantMap{{QStringLiteral("terminal"), state()}, {QStringLiteral("package"), script->packageInfo}, {QStringLiteral("storage"), storage}}}, ActionTimeoutMs);
            if (initialized.status != LuauEngine::CallStatus::Ok) error = QStringLiteral("Initialization failed: ") + initialized.message;
            else script->initialEffects = initialized.result;
        }
        if (!error.isEmpty()) { m_loadErrors[path] = error; qCWarning(lcScripts).noquote() << path << error; return nullptr; }
    }
    qCInfo(lcScripts).noquote() << "loaded" << script->id << "from" << path;
    return script;
}

void ScriptHost::watch()
{
    if (const QStringList watched = m_watcher.files() + m_watcher.directories(); !watched.isEmpty()) {
        m_watcher.removePaths(watched);
    }
    if (!m_enabled) {
        return;
    }
    for (const QString& dir : std::as_const(m_searchPaths)) {
        // Watch the nearest existing directory, so creating the scripts
        // directory itself is noticed too.
        QFileInfo info(dir);
        while (!info.exists() && info.absolutePath() != info.absoluteFilePath()) {
            info = QFileInfo(info.absolutePath());
        }
        if (info.isDir()) {
            m_watcher.addPath(info.absoluteFilePath());
        }
    }
    m_packageWatches.removeDuplicates();
    for (auto path : std::as_const(m_packageWatches)) {
        QFileInfo info(path);
        while (!info.exists() && info.absolutePath() != info.absoluteFilePath()) info = QFileInfo(info.absolutePath());
        if (info.exists()) m_watcher.addPath(info.absoluteFilePath());
    }
    for (const ScriptPtr& s : m_scripts) {
        m_watcher.addPath(s->path); // editors that save in place change only the file
    }
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
        result << QVariantMap{{QStringLiteral("id"), script->id}, {QStringLiteral("path"), script->path},
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
            {QStringLiteral("dropped_results"), script->droppedResults}};
    }
    for (auto it = m_loadErrors.begin(); it != m_loadErrors.end(); ++it) if (!active.contains(it.key()))
        result << QVariantMap{{QStringLiteral("path"), it.key()}, {QStringLiteral("status"), QStringLiteral("failed")}, {QStringLiteral("error"), it.value()}};
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

QList<ScriptHost::ScriptAction> ScriptHost::actions() const
{
    QList<ScriptAction> out;
    for (const ScriptPtr& s : m_scripts) {
        if (!s->disabled) {
            out.append(s->actions);
        }
    }
    return out;
}

bool ScriptHost::runAction(const QString& token, quint32 pane)
{
    const auto dot = token.indexOf(QLatin1Char('.'));
    if (dot <= 0) {
        return false;
    }
    const QString id = token.left(dot);
    // Copied: a reload during the call must not free the script under it.
    const std::vector<ScriptPtr> scripts = m_scripts;
    for (const ScriptPtr& s : scripts) {
        if (s->id != id || s->disabled) {
            continue;
        }
        auto it = std::find_if(s->actions.cbegin(), s->actions.cend(), [&](const ScriptAction& a) { return a.token == token; });
        if (it == s->actions.cend()) {
            return false;
        }
        QVariantMap ev{{QStringLiteral("terminal"), state()}};
        if (pane) {
            ev.insert(QStringLiteral("pane"), m_callbacks.paneInfo ? m_callbacks.paneInfo(pane) : QVariantMap{});
        }
        call(s, QStringLiteral("run_action"), {token.mid(dot + 1), ev}, ActionTimeoutMs, pane);
        return true;
    }
    return false;
}

void ScriptHost::dispatch(const QString& handler, quint32 pane, QVariantMap event)
{
    if (m_scripts.empty()) {
        return;
    }
    event.insert(QStringLiteral("terminal"), state());
    if (!event.contains(QStringLiteral("pane"))) {
        QVariantMap info = m_callbacks.paneInfo ? m_callbacks.paneInfo(pane) : QVariantMap{};
        if (info.isEmpty()) {
            info.insert(QStringLiteral("id"), pane); // closed already
        }
        event.insert(QStringLiteral("pane"), info);
    }
    const std::vector<ScriptPtr> scripts = m_scripts;
    for (const ScriptPtr& s : scripts) {
        if (!s->disabled && s->engine->hasFunction(s->module, handler)) {
            call(s, handler, {event}, EventTimeoutMs, pane);
        }
    }
}

QVariantMap ScriptHost::state() const
{
    QVariantMap snapshot = m_callbacks.state ? m_callbacks.state() : QVariantMap{};
    snapshot.insert(QStringLiteral("version"), StateVersion);
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
    if (!script->disabled && script->engine->hasFunction(script->module, QStringLiteral("on_unload")))
        call(script, QStringLiteral("on_unload"), {QVariantMap{{QStringLiteral("terminal"), state()}}}, EventTimeoutMs, 0);
    m_tasks->clear(script->owner);
    if (m_callbacks.cleanup) m_callbacks.cleanup(script->owner);
}

void ScriptHost::emitOwned(const ScriptPtr& script, const QString& handler, QVariantMap event, quint32 pane)
{
    if (script->disabled || script->retiring || std::find(m_scripts.begin(), m_scripts.end(), script) == m_scripts.end()
        || !script->engine->hasFunction(script->module, handler)) return;
    const qint64 now = m_clock.elapsed();
    while (!script->callbacks.empty() && now - script->callbacks.front() >= 1000) script->callbacks.pop_front();
    if (script->callbacks.size() >= 64) { ++script->droppedCallbacks; return; }
    script->callbacks.push_back(now);
    event.insert(QStringLiteral("terminal"), state());
    if (pane && m_callbacks.paneInfo) event.insert(QStringLiteral("pane"), m_callbacks.paneInfo(pane));
    call(script, handler, {event}, EventTimeoutMs, pane);
}

void ScriptHost::call(const ScriptPtr& script, const QString& function, const QVariantList& args, int timeoutMs,
                      quint32 eventPane)
{
    const auto out = script->invoke(function, args, timeoutMs);
    if (out.status != LuauEngine::CallStatus::Ok) {
        const QString why = out.status == LuauEngine::CallStatus::TimedOut
            ? QStringLiteral("took longer than %1 ms").arg(timeoutMs)
            : out.message;
        ++script->handlerErrors;
        script->lastError = function + QStringLiteral(": ") + why;
        qCWarning(lcScripts).noquote() << script->id << function << "failed:" << why;
        if (++script->failures >= MaxFailures) {
            script->disabled = true;
            retire(script);
            qCWarning(lcScripts).noquote() << script->id << "is disabled after" << MaxFailures
                                           << "failures in a row, until its file changes";
            if (!script->actions.isEmpty()) {
                Q_EMIT actionsChanged();
            }
        }
        return;
    }
    script->failures = 0;
    applyResult(script, out.result, eventPane);
}

void ScriptHost::applyResult(const ScriptPtr& script, const QVariant& result, quint32 eventPane)
{
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
            const QString error = QStringLiteral("a handler may return at most %1 effects; no effects applied").arg(MaxEffectsPerCall);
            qCWarning(lcScripts).noquote() << script->id << error;
            ++script->effectErrors;
            script->droppedEffects += effects.size();
            script->lastError = error;
            Q_EMIT effectApplied(script->id, {}, error);
            return;
        }
        for (const QVariant& v : effects) {
            if (script->disabled || std::find(m_scripts.begin(), m_scripts.end(), script) == m_scripts.end()) {
                break;
            }
            applyEffect(script, v.toMap(), eventPane);
        }
        return;
    }
    const QString error = QStringLiteral("a handler returned a %1, not effects").arg(QString::fromLatin1(result.typeName()));
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
        }
    }
    // Rebuilt from the known fields only, with their types settled.
    QVariantMap effect{{QStringLiteral("kind"), kind}};
    EffectResult result;
    QString& error = result.error;
    result.code = QStringLiteral("invalid_effect");
    const QVariant requestValue = raw.value(QStringLiteral("request_id"));
    const QString request = requestValue.toString();
    const bool validRequest = requestValue.typeId() == QMetaType::QString && !request.isEmpty() && request.size() <= 64;
    if (requestValue.isValid() && !validRequest) {
        error = QStringLiteral("request_id must be a nonempty string of at most 64 characters");
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
        if (info->command) {
            effect.insert(QStringLiteral("command"), command(raw.value(QStringLiteral("command"))));
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
            const auto value = (launch ? options : raw).value(QStringLiteral("env"));
            if (value.isValid() && value.typeId() != QMetaType::QVariantMap
                && !(value.typeId() == QMetaType::QVariantList && value.toList().isEmpty()))
                error = QStringLiteral("env must be an object");
            const auto env = value.toMap();
            qsizetype bytes = 0;
            for (auto it = env.begin(); it != env.end(); ++it) {
                const auto& v = it.value();
                bytes += it.key().toUtf8().size() + v.toString().toUtf8().size();
                if (it.key().isEmpty() || it.key().contains(QLatin1Char('=')) || it.key().contains(QChar::Null)
                    || !((v.typeId() == QMetaType::QString && !v.toString().contains(QChar::Null))
                         || (v.typeId() == QMetaType::Bool && !v.toBool())))
                    error = QStringLiteral("env needs valid variable names and string values or false to remove");
            }
            if (env.size() > 256 || bytes > 65536) error = QStringLiteral("Environment overrides exceed their limit");
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
            effect.insert(QStringLiteral("values"), values);
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
            if (!std::isfinite(target) || target < 0 || target > std::numeric_limits<quint32>::max() || target != std::floor(target))
                error = QStringLiteral("target must be a pane ID or zero");
        }
        if (kind == QLatin1String("set_actions") || kind == QLatin1String("select")) {
            const auto key = kind == QLatin1String("select") ? QStringLiteral("items") : QStringLiteral("actions");
            const auto entries = raw.value(key).toList();
            const int limit = kind == QLatin1String("select") ? 500 : 32;
            QVariantList checked;
            QSet<QString> ids;
            if (kind == QLatin1String("set_actions") && !script->engine->hasFunction(script->module, QStringLiteral("run_action"))) error = QStringLiteral("set_actions requires run_action");
            if (entries.size() > limit) error = QStringLiteral("Too many entries");
            for (const auto& item : entries) {
                const auto map = item.toMap();
                const auto id = str(map, "id"), title = str(map, "title");
                if (id.isEmpty() || id.size() > 64 || title.size() > 512 || ids.contains(id)
                    || (kind == QLatin1String("set_actions") && !validId(id))) { error = QStringLiteral("Invalid or duplicate entry"); break; }
                ids.insert(id); checked << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("title"), title.isEmpty() ? id : title}};
            }
            effect.insert(key, checked);
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
        if (kind == QLatin1String("signals")) {
            const auto values = raw.value(QStringLiteral("values")).toList();
            QVariantList checked;
            QSet<int> seen;
            if (values.isEmpty() || values.size() > 8) {
                error = QStringLiteral("a signal batch needs 1 to 8 channels");
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
        } else if (kind == QLatin1String("action") && effect.value(QStringLiteral("name")).toString().startsWith(QLatin1String("script:"))) {
            error = QStringLiteral("scripts cannot run script actions");
        } else if (kind == QLatin1String("signal")
                   && !validSignal(effect.value(QStringLiteral("channel")), effect.value(QStringLiteral("value")))) {
            error = QStringLiteral("invalid signal channel/value");
        } else if (info->pane && (!m_callbacks.paneInfo || m_callbacks.paneInfo(effect.value(QStringLiteral("pane")).toUInt()).isEmpty())) {
            result.code = QStringLiteral("no_pane");
            error = QStringLiteral("no pane %1").arg(effect.value(QStringLiteral("pane")).toUInt());
        } else if (!withinBudget(*script, structural)) {
            ++script->droppedEffects;
            result.code = QStringLiteral("rate_limited");
            error = QStringLiteral("too many effects; %1 dropped").arg(kind);
        } else if (kind == QLatin1String("state_read") || kind == QLatin1String("state_write")) {
            ScriptState::Snapshot saved;
            ScriptState store(m_stateDirectory);
            result.code = QStringLiteral("state_error");
            error = kind == QLatin1String("state_read") ? store.read(script->id, saved)
                : store.write(script->id, script->stateVersion, effect.value(QStringLiteral("revision")).toInt(), effect.value(QStringLiteral("value")).toMap(), saved);
            if (error.isEmpty() && saved.version > script->stateVersion) error = QStringLiteral("Stored state is newer than this script");
            if (error.isEmpty()) result.data = {{QStringLiteral("value"), saved.value}, {QStringLiteral("version"), saved.version ? saved.version : script->stateVersion},
                                               {QStringLiteral("revision"), saved.revision}};
        } else if (kind == QLatin1String("log")) {
            qCInfo(lcScripts).noquote() << script->id + QLatin1Char(':') << effect.value(QStringLiteral("message")).toString();
        } else if (kind == QLatin1String("set_actions")) {
            script->actions.clear();
            for (const auto& value : effect.value(QStringLiteral("actions")).toList()) {
                const auto item = value.toMap();
                script->actions.append({script->id + QLatin1Char('.') + str(item, "id"), str(item, "title")});
            }
            Q_EMIT actionsChanged();
        } else if (kind == QLatin1String("timer") || kind == QLatin1String("cancel") || kind == QLatin1String("exec")
                   || kind == QLatin1String("read_file") || kind == QLatin1String("watch_file")) {
            result = m_tasks->apply(script->owner, effect,
                [this, weak = std::weak_ptr<Script>(script), request, kind, eventPane](EffectResult finished) {
                    if (auto script = weak.lock()) {
                        if (!finished.error.isEmpty()) { ++script->effectErrors; script->lastError = finished.error; }
                        if (!request.isEmpty()) queueResult(script, request, kind, finished, eventPane);
                    }
                }, [this, weak = std::weak_ptr<Script>(script), eventPane](const QString& handler, QVariantMap event) {
                    if (auto script = weak.lock()) emitOwned(script, handler, std::move(event), eventPane);
                });
        } else if (m_callbacks.applyOwned || m_callbacks.apply) {
            result = m_callbacks.applyOwned ? m_callbacks.applyOwned(script->owner, effect,
                [this, weak = std::weak_ptr<Script>(script), request, kind, eventPane](EffectResult finished) {
                    if (auto script = weak.lock()) {
                        if (!finished.error.isEmpty()) { ++script->effectErrors; script->lastError = finished.error; }
                        if (!request.isEmpty()) queueResult(script, request, kind, finished, eventPane);
                    }
                }) : m_callbacks.apply(effect);
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
        queueResult(script, request, kind, result, eventPane);
    }
}

void ScriptHost::queueResult(const ScriptPtr& script, const QString& request, const QString& kind,
                             const EffectResult& result, quint32 eventPane)
{
    if (script->disabled || !script->engine->hasFunction(script->module, QStringLiteral("on_result"))) {
        return;
    }
    const qint64 now = m_clock.elapsed();
    while (!script->results.empty() && now - script->results.front() >= 1000) {
        script->results.pop_front();
    }
    if (script->pendingResults >= MaxPendingResults || int(script->results.size()) >= MaxResultsPerSecond) {
        ++script->droppedResults;
        script->lastError = QStringLiteral("Too many operation results");
        // Include refusals in this budget: a handler that retries a rejected
        // request from on_result must not create an endless queued feedback loop.
        script->disabled = true;
        retire(script);
        qCWarning(lcScripts) << script->id << "disabled: too many operation results";
        Q_EMIT actionsChanged();
        return;
    }
    script->results.push_back(now);
    ++script->pendingResults;
    QVariantMap event{{QStringLiteral("request_id"), request}, {QStringLiteral("kind"), kind},
                      {QStringLiteral("ok"), result.error.isEmpty()}, {QStringLiteral("data"), result.data}};
    if (!result.error.isEmpty()) {
        event.insert(QStringLiteral("error"), QVariantMap{{QStringLiteral("code"), result.code},
                                                          {QStringLiteral("message"), result.error}});
    }
    // Queued after application. Never re-enter a VM, and never deliver an old
    // instance's completions to its replacement following a reload.
    QTimer::singleShot(0, this, [this, weak = std::weak_ptr<Script>(script), event = std::move(event), eventPane]() mutable {
        const auto script = weak.lock();
        if (!script) {
            return;
        }
        --script->pendingResults;
        if (script->disabled || std::find(m_scripts.begin(), m_scripts.end(), script) == m_scripts.end()) {
            return;
        }
        event.insert(QStringLiteral("terminal"), state());
        QVariantMap pane = m_callbacks.paneInfo ? m_callbacks.paneInfo(eventPane) : QVariantMap{};
        if (eventPane) {
            if (pane.isEmpty()) {
                pane.insert(QStringLiteral("id"), eventPane);
            }
            event.insert(QStringLiteral("pane"), pane);
        }
        call(script, QStringLiteral("on_result"), {event}, EventTimeoutMs, eventPane);
    });
}

bool ScriptHost::withinBudget(Script& script, bool structural)
{
    const qint64 now = m_clock.elapsed();
    auto admit = [now](std::deque<qint64>& times, qint64 window, int limit) {
        while (!times.empty() && now - times.front() >= window) {
            times.pop_front();
        }
        return int(times.size()) < limit;
    };
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
