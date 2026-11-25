// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>

namespace Afterglow {

struct Settings;

/// The exit status of afterglow ctl run or wait at its timeout, as
/// timeout(1) has it: the command goes on.
inline constexpr int CtlRunTimedOut = 124;
/// The exit status when the run or the wait itself failed (a pane that is
/// not at its prompt, a lost connection), as timeout(1) has it: never the
/// status of the command.
inline constexpr int CtlRunFailed = 125;

/// What afterglow ctl asks of PaneRuns, over the connection of @p fd, the
/// control target of a window or of a mux session that the caller selected.
struct CtlRun
{
    enum class Kind {
        Run, ///< run: the command at the prompt, until the next one
        Wait, ///< wait: until finished, text, quiet or closed
        SendWait, ///< send-text, paste or send-key with --wait-text
    };
    Kind kind = Kind::Run;
    /// The arguments by the names of the MCP tools run, wait and send
    /// (PaneRequest::arguments). For SendWait: those of the send, with the
    /// wait's in waitArguments.
    QJsonObject arguments;
    QJsonObject waitArguments;
    /// Print every field of the result as JSON, not the output.
    bool json = false;
    /// For messages: where the panes are ("session NAME", "this Afterglow").
    QString where;
};

/// Runs @p run over @p fd, which it closes, in an event loop until the
/// result: prints the output on standard output and the notes on standard
/// error, or every field with json. Returns the exit status: the command's
/// for a run or a wait until finished, 0 for a match, quiet or closed,
/// CtlRunTimedOut at the timeout, CtlRunFailed when the call failed.
int runPaneCommand(int fd, const CtlRun& run, const Settings& settings);

} // namespace Afterglow
