// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScriptHost.h"

#include <PhosphorScripting/LuauEngine.h>
#include <PhosphorScripting/LuauWatchdog.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStandardPaths>

#include <deque>
#include <map>

Q_LOGGING_CATEGORY(lcScripts, "afterglow.scripts", QtInfoMsg)

namespace Afterglow {

using PhosphorScripting::LuauEngine;

struct ScriptHost::Script
{
    QString id;
    QString name;
    QString path;
    QByteArray source;
    std::unique_ptr<LuauEngine> engine;
    int module = -1;
    QList<ScriptAction> actions;
    int failures = 0;
    bool disabled = false;
    std::deque<qint64> effects; ///< when recent effects ran, for the budgets
    std::deque<qint64> structural;
};

namespace {

constexpr int ReloadDelayMs = 200;

bool validId(const QString& id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
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
};

} // namespace

ScriptHost::ScriptHost(Callbacks callbacks, QObject* parent)
    : QObject(parent)
    , m_callbacks(std::move(callbacks))
    , m_watchdog(std::make_shared<PhosphorScripting::LuauWatchdog>())
    , m_searchPaths(defaultSearchPaths())
{
    QFile prelude(QStringLiteral(":/afterglow/scripting/afterglow.luau"));
    if (prelude.open(QIODevice::ReadOnly)) {
        m_prelude = prelude.readAll();
    } else {
        qCWarning(lcScripts) << "the script prelude is missing from the build";
    }
    m_clock.start();
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(ReloadDelayMs);
    connect(&m_reloadTimer, &QTimer::timeout, this, &ScriptHost::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_reloadTimer, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_reloadTimer, qOverload<>(&QTimer::start));
}

ScriptHost::~ScriptHost() = default;

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
    if (m_enabled) {
        // id -> path, later directories winning.
        std::map<QString, QString> chosen;
        for (const QString& dir : std::as_const(m_searchPaths)) {
            const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.luau")}, QDir::Files | QDir::Readable,
                                                                QDir::Name);
            for (const QFileInfo& f : files) {
                if (!f.fileName().endsWith(QLatin1String(".d.luau"))) {
                    chosen[f.completeBaseName()] = f.absoluteFilePath();
                }
            }
        }
        for (const auto& [base, path] : chosen) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QByteArray source = file.readAll();
            // An unchanged script keeps its state, including being disabled.
            auto same = std::find_if(m_scripts.begin(), m_scripts.end(),
                                     [&](const ScriptPtr& s) { return s->path == path && s->source == source; });
            ScriptPtr script = same != m_scripts.end() ? *same : load(path, source);
            if (!script) {
                continue;
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
    QList<ScriptAction> before = actions();
    m_scripts = std::move(loaded);
    watch();
    const QList<ScriptAction> after = actions();
    const bool changed = before.size() != after.size()
        || !std::equal(before.cbegin(), before.cend(), after.cbegin(),
                       [](const ScriptAction& a, const ScriptAction& b) { return a.token == b.token && a.title == b.title; });
    if (changed) {
        Q_EMIT actionsChanged();
    }
}

ScriptHost::ScriptPtr ScriptHost::load(const QString& path, const QByteArray& source) const
{
    auto script = std::make_shared<Script>();
    script->path = path;
    script->source = source;
    script->engine = std::make_unique<LuauEngine>(m_watchdog, MemoryCapBytes);
    QString error;
    const QString chunk = QStringLiteral("@") + QFileInfo(path).fileName();
    if (!script->engine->init(&error) || !script->engine->runPrelude(QStringLiteral("@afterglow"), m_prelude, &error)) {
        qCWarning(lcScripts) << "cannot start the script engine:" << error;
        return nullptr;
    }
    script->engine->sandbox();
    script->module = script->engine->loadModule(chunk, source, &error);
    if (script->module < 0) {
        qCWarning(lcScripts).noquote() << path << "did not load:" << error;
        return nullptr;
    }

    const QVariantMap meta = script->engine->moduleField(script->module, QStringLiteral("metadata")).toMap();
    script->id = str(meta, "id");
    if (script->id.isEmpty()) {
        script->id = QFileInfo(path).completeBaseName();
    }
    if (!validId(script->id)) {
        qCWarning(lcScripts) << path << "has an unusable id" << script->id << "(letters, digits, _ and - only)";
        return nullptr;
    }
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
        QVariantMap ev;
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

void ScriptHost::call(const ScriptPtr& script, const QString& function, const QVariantList& args, int timeoutMs,
                      quint32 eventPane)
{
    const LuauEngine::CallOutcome out = script->engine->callModule(script->module, function, args, timeoutMs);
    if (out.status != LuauEngine::CallStatus::Ok) {
        const QString why = out.status == LuauEngine::CallStatus::TimedOut
            ? QStringLiteral("took longer than %1 ms").arg(timeoutMs)
            : out.message;
        qCWarning(lcScripts).noquote() << script->id << function << "failed:" << why;
        if (++script->failures >= MaxFailures) {
            script->disabled = true;
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
    if (!result.isValid()) {
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
        for (const QVariant& v : result.toList()) {
            applyEffect(script, v.toMap(), eventPane);
        }
        return;
    }
    const QString error = QStringLiteral("a handler returned a %1, not effects").arg(QString::fromLatin1(result.typeName()));
    qCWarning(lcScripts).noquote() << script->id << error;
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
    QString error;
    if (!info) {
        error = QStringLiteral("unknown effect \"%1\"").arg(kind);
    } else {
        if (info->pane) {
            const quint32 pane = raw.value(QStringLiteral("pane")).toUInt();
            effect.insert(QStringLiteral("pane"), pane ? pane : eventPane);
        }
        for (const char* key : info->strings) {
            effect.insert(QLatin1String(key), str(raw, key));
        }
        if (info->command) {
            effect.insert(QStringLiteral("command"), command(raw.value(QStringLiteral("command"))));
        }
        QString badNumber;
        for (const char* key : info->numbers) {
            const QVariant v = raw.value(QLatin1String(key));
            const bool numeric = !v.isValid() || v.typeId() == QMetaType::Double || v.typeId() == QMetaType::Int
                || v.typeId() == QMetaType::LongLong;
            if (!numeric) {
                badNumber = QLatin1String(key);
            }
            effect.insert(QLatin1String(key), v.toDouble());
        }
        if (!badNumber.isEmpty()) {
            error = QStringLiteral("%1: %2 must be a number").arg(kind, badNumber);
        } else if (info->pane && effect.value(QStringLiteral("pane")).toUInt() == 0) {
            error = QStringLiteral("%1 needs a pane").arg(kind);
        } else if (kind == QLatin1String("split") && effect.value(QStringLiteral("direction")) != QLatin1String("right")
                   && effect.value(QStringLiteral("direction")) != QLatin1String("down")) {
            error = QStringLiteral("split direction must be \"right\" or \"down\"");
        } else if (kind == QLatin1String("action") && effect.value(QStringLiteral("name")).toString().startsWith(QLatin1String("script:"))) {
            error = QStringLiteral("scripts cannot run script actions");
        } else if (!withinBudget(*script, info->structural)) {
            error = QStringLiteral("too many effects; %1 dropped").arg(kind);
        } else if (kind == QLatin1String("log")) {
            qCInfo(lcScripts).noquote() << script->id + QLatin1Char(':') << effect.value(QStringLiteral("message")).toString();
        } else if (m_callbacks.apply) {
            error = m_callbacks.apply(effect);
        }
    }
    if (!error.isEmpty()) {
        qCWarning(lcScripts).noquote() << script->id + QLatin1Char(':') << error;
    }
    Q_EMIT effectApplied(script->id, effect, error);
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
