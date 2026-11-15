// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mux/MuxRoute.h"

#include "shell/Ssh.h"

namespace Afterglow {

std::optional<AfterglowMux::Route> muxRoute(const QString& host, QString* error)
{
    const QByteArrayList ssh = Ssh::sharedCommand(host, error);
    if (ssh.isEmpty()) return std::nullopt;
    AfterglowMux::Route route;
    for (const QByteArray& word : ssh) route.proxy.push_back(word.toStdString());
    // As the shell of the login finds it there.
    route.proxy.emplace_back("aglow-mux");
    route.host = host.toStdString();
    return route;
}

} // namespace Afterglow
