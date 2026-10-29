// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ControlBus.h"

#include "../config/Settings.h"

#include <PhosphorDBus/Streaming.h>

#include <QDBusConnection>
#include <QDBusMetaType>

namespace Afterglow {

QDBusArgument& operator<<(QDBusArgument& arg, const PaneInfo& p)
{
    arg.beginStructure();
    arg << p.id << p.window << p.tab << p.title << p.workingDirectory << p.command << p.cols << p.rows << p.focused;
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, PaneInfo& p)
{
    arg.beginStructure();
    arg >> p.id >> p.window >> p.tab >> p.title >> p.workingDirectory >> p.command >> p.cols >> p.rows >> p.focused;
    arg.endStructure();
    return arg;
}

static_assert(PhosphorDBus::HasDBusStreaming<PaneInfo>::value, "PaneInfo needs QDBusArgument streaming");

ControlBus::ControlBus(Control& control, SettingsStore& settings, QObject* parent)
    : QObject(parent)
    , m_control(control)
    , m_settings(settings)
{
    qDBusRegisterMetaType<PaneInfo>();
    qDBusRegisterMetaType<QList<PaneInfo>>();
    connect(&control, &Control::paneCreated, this, &ControlBus::PaneCreated);
    connect(&control, &Control::paneClosed, this, &ControlBus::PaneClosed);
    connect(&control, &Control::titleChanged, this, &ControlBus::TitleChanged);
    connect(&control, &Control::bell, this, &ControlBus::Bell);
    connect(&control, &Control::focusChanged, this, &ControlBus::FocusChanged);
    connect(&control, &Control::statusChanged, this, &ControlBus::StatusChanged);
    connect(&control, &Control::commandStarted, this,
            [this](quint32 pane, const QString& c) { Q_EMIT CommandStarted(pane, commandText(c)); });
    connect(&control, &Control::commandFinished, this, [this](quint32 pane, const QString& c, int code, qint64 ms) {
        Q_EMIT CommandFinished(pane, commandText(c), code, ms);
    });
    connect(&settings, &SettingsStore::changed, this, &ControlBus::applySettings);
    applySettings();
}

ControlBus::~ControlBus()
{
    if (m_registered) {
        QDBusConnection::sessionBus().unregisterObject(QLatin1String(ObjectPath));
    }
}

void ControlBus::applySettings()
{
    const bool want = m_settings.settings().control.enabled;
    if (want == m_registered) {
        return;
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (want) {
        m_registered = bus.registerObject(QLatin1String(ObjectPath), this,
                                          QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals);
    } else {
        bus.unregisterObject(QLatin1String(ObjectPath));
        m_registered = false;
    }
}

QString ControlBus::commandText(const QString& command) const
{
    // Commands can carry secrets; they go on the bus only with read access.
    return m_settings.settings().control.allowReadText ? command : QString();
}

bool ControlBus::fail(const QString& error)
{
    if (error.isEmpty()) {
        return false;
    }
    if (calledFromDBus()) {
        sendErrorReply(QDBusError::Failed, error);
    }
    return true;
}

QList<PaneInfo> ControlBus::ListPanes()
{
    QList<PaneInfo> panes = m_control.panes();
    for (PaneInfo& p : panes) {
        p.command = commandText(p.command);
    }
    return panes;
}

uint ControlBus::FocusedPane()
{
    for (const PaneInfo& p : m_control.panes()) {
        if (p.focused) {
            return p.id;
        }
    }
    fail(QStringLiteral("no focused pane"));
    return 0;
}

void ControlBus::SendText(uint pane, const QString& text)
{
    fail(m_control.sendText(pane, text));
}

QString ControlBus::GetText(uint pane, const QString& scope)
{
    const auto s = Control::scopeFromName(scope);
    if (!s) {
        fail(QStringLiteral("scope is screen, scrollback, selection or last-output"));
        return {};
    }
    QString out;
    if (fail(m_control.readText(pane, *s, out))) {
        return {};
    }
    return out;
}

uint ControlBus::Split(uint pane, const QString& direction, const QStringList& command)
{
    if (direction != QLatin1String("right") && direction != QLatin1String("down")) {
        fail(QStringLiteral("direction is right or down"));
        return 0;
    }
    quint32 created = 0;
    fail(m_control.split(pane, direction == QLatin1String("right"), command, created));
    return created;
}

uint ControlBus::NewTab(uint pane, const QString& workingDirectory, const QStringList& command)
{
    quint32 created = 0;
    fail(m_control.newTab(pane, workingDirectory, command, created));
    return created;
}

uint ControlBus::CreateWindow(const QString& workingDirectory, const QStringList& command)
{
    quint32 created = 0;
    fail(m_control.newWindow(workingDirectory, command, created));
    return created;
}

void ControlBus::Focus(uint pane)
{
    fail(m_control.focus(pane));
}

void ControlBus::Close(uint pane)
{
    fail(m_control.close(pane));
}

void ControlBus::SetTitle(uint pane, const QString& title)
{
    fail(m_control.setTitle(pane, title));
}

void ControlBus::Signal(uint pane, int channel, double value)
{
    fail(m_control.signal(pane, channel, value));
}

void ControlBus::Notify(uint pane, const QString& title, const QString& body)
{
    fail(m_control.notify(pane, title, body));
}

void ControlBus::RunAction(uint pane, const QString& action)
{
    fail(m_control.runAction(pane, action));
}

} // namespace Afterglow
