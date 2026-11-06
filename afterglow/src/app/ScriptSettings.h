// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../config/Settings.h"
#include "../scripting/ScriptHost.h"

#include <QObject>

namespace Afterglow {

/// The Scripts.command* limits as the script host takes them.
inline ScriptHost::CommandLimits commandLimits(const Settings& settings)
{
    return {settings.scriptCommandTimeout, settings.scriptCommandStdinLimit, settings.scriptCommandRunLimit,
            settings.scriptCommandOutputLimit};
}

/// Applies Scripts.enabled and the command limits to @p scripts now, and on
/// each change of @p settings. Call it once the CommandSocket exists, which
/// follows the same settings: its slot then runs first. So when
/// Scripts.enabled goes off, the runs of `aglow run` fail with that
/// reason before the scripts stop, which would end them as "the script
/// stopped".
inline void followScriptSettings(ScriptHost& scripts, SettingsStore& settings)
{
    const auto apply = [&scripts, &settings] {
        scripts.setCommandLimits(commandLimits(settings.settings()));
        scripts.setEnabled(settings.settings().scripts);
    };
    apply();
    QObject::connect(&settings, &SettingsStore::changed, &scripts, apply);
}

} // namespace Afterglow
