// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/SessionLaunch.h"

#include "config/Settings.h"
#include "mux/MuxSessions.h"

#include "MuxProtocol.h"

#include <AfterglowProto/Json.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

namespace Afterglow {

QString expandHome(const QString& path)
{
    return path == QLatin1String("~") || path.startsWith(QLatin1String("~/")) ? QDir::homePath() + path.mid(1) : path;
}

QJsonObject sessionLaunch(const Settings& settings, QString directory, const QStringList& command,
                          const InheritedEnvironment* inherited, const QVariantMap& environment)
{
    directory = expandHome(directory);
    if (directory.isEmpty()) {
        const QString setting = settings.window.workingDirectory;
        if (setting == QLatin1String("home"))
            directory = QDir::homePath();
        else if (!setting.isEmpty() && setting != QLatin1String("inherit") && QFileInfo(expandHome(setting)).isDir())
            directory = expandHome(setting);
    }
    // As TerminalWindow prepares a pane of its session.
    AfterglowMux::Launch launch =
        localLaunch(settings, directory.isEmpty() ? QDir::homePath() : directory, command, inherited, environment);
    if (directory.isEmpty())
        launch.workingDirectory.clear();
    return QJsonDocument::fromJson(QByteArray::fromStdString(AfterglowProto::Json::write(AfterglowMux::toJson(launch))))
        .object();
}

} // namespace Afterglow
