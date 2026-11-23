// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mux/MuxRoute.h"

#include "shell/Ssh.h"

namespace Afterglow {

std::optional<AfterglowMux::Route> muxRoute(const QString& host, const QString& remoteCommand, int controlPersist,
                                            const AfterglowMux::Environment& environment, QString* error,
                                            Ssh::Config* resolved)
{
    const auto config = Ssh::resolve(host, environment, error);
    if (!config)
        return std::nullopt;
    const QByteArrayList ssh = Ssh::sharedCommand(host, *config, controlPersist, error);
    if (ssh.isEmpty())
        return std::nullopt;
    if (resolved)
        *resolved = *config;
    AfterglowMux::Route route;
    for (const QByteArray& word : ssh)
        route.proxy.push_back(word.toStdString());
    // ssh joins the words of the command for the shell of the login there,
    // which finds the program in its PATH, or expands a path such as
    // ~/.local/bin/aglow-mux.
    route.proxy.push_back(remoteCommand.toLocal8Bit().toStdString());
    route.host = host.toStdString();
    return route;
}

} // namespace Afterglow
