// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Control.h"

#include <QDBusArgument>
#include <QDBusContext>
#include <QObject>

namespace Afterglow {

/// The D-Bus face of Control: io.github.fuddlesworth.Afterglow.Control at
/// /Afterglow/Control. Present on the bus only while Control.enabled is on.
class ControlBus : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.fuddlesworth.Afterglow.Control")

public:
    static constexpr const char* Interface = "io.github.fuddlesworth.Afterglow.Control";
    static constexpr const char* ObjectPath = "/Afterglow/Control";

    ControlBus(Control& control, SettingsStore& settings, QObject* parent = nullptr);
    ~ControlBus() override;

public Q_SLOTS:
    Q_SCRIPTABLE QList<Afterglow::PaneInfo> ListPanes();
    Q_SCRIPTABLE uint FocusedPane();
    Q_SCRIPTABLE void SendText(uint pane, const QString& text);
    /// @p scope: screen, scrollback, selection or last-output.
    Q_SCRIPTABLE QString GetText(uint pane, const QString& scope);
    /// @p direction: right or down. Returns the new pane.
    Q_SCRIPTABLE uint Split(uint pane, const QString& direction, const QStringList& command);
    Q_SCRIPTABLE uint NewTab(uint pane, const QString& workingDirectory, const QStringList& command);
    Q_SCRIPTABLE uint CreateWindow(const QString& workingDirectory, const QStringList& command);
    Q_SCRIPTABLE QStringList Projects();
    Q_SCRIPTABLE QVariantMap SaveProject(uint pane, const QString& name, bool overwrite);
    Q_SCRIPTABLE QVariantMap OpenProject(const QString& name, bool newInstance);
    Q_SCRIPTABLE QVariantMap MovePane(uint pane, uint target, const QString& placement);
    Q_SCRIPTABLE QVariantMap DetachTab(uint pane);
    Q_SCRIPTABLE void Focus(uint pane);
    Q_SCRIPTABLE void Close(uint pane);
    Q_SCRIPTABLE void SetTitle(uint pane, const QString& title);
    Q_SCRIPTABLE void Signal(uint pane, int channel, double value);
    Q_SCRIPTABLE void Notify(uint pane, const QString& title, const QString& body);
    Q_SCRIPTABLE void RunAction(uint pane, const QString& action);

Q_SIGNALS:
    Q_SCRIPTABLE void PaneCreated(uint pane);
    Q_SCRIPTABLE void PaneMoved(uint pane);
    Q_SCRIPTABLE void PaneClosed(uint pane);
    Q_SCRIPTABLE void TitleChanged(uint pane, const QString& title);
    Q_SCRIPTABLE void Bell(uint pane);
    /// The command text is empty unless Control.allowReadText.
    Q_SCRIPTABLE void CommandStarted(uint pane, const QString& command);
    Q_SCRIPTABLE void CommandFinished(uint pane, const QString& command, int exitCode, qlonglong durationMs);
    Q_SCRIPTABLE void StatusChanged(uint pane, const QString& state, const QString& text);
    Q_SCRIPTABLE void FocusChanged(uint pane);
    /// The value is empty unless Control.allowReadText.
    Q_SCRIPTABLE void UserVarChanged(uint pane, const QString& name, const QString& value);
    Q_SCRIPTABLE void ContextChanged(uint pane, const QString& context);

private:
    void applySettings();
    bool fail(const QString& error);
    QString commandText(const QString& command) const;

    Control& m_control;
    SettingsStore& m_settings;
    bool m_registered = false;
};

QDBusArgument& operator<<(QDBusArgument& arg, const PaneInfo& info);
const QDBusArgument& operator>>(const QDBusArgument& arg, PaneInfo& info);

} // namespace Afterglow

Q_DECLARE_METATYPE(Afterglow::PaneInfo)
