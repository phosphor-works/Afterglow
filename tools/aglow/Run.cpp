// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// aglow run: a command of a script in a running Afterglow. The client is in
// afterglow-proto. This front end only names it and ends by the signal that
// ended the run.

#include "Cli.h"
#include "Commands.h"

#include <AfterglowProto/Run.h>

#include <csignal>

namespace Aglow {

int runRun(int argc, char** argv)
{
    AfterglowProto::Run::Environment environment;
    // "afterglow run" when afterglow runs this aglow.
    environment.program = Cli::program() + " run";
    int signal = 0;
    const int status = AfterglowProto::Run::main({argv, argv + argc}, environment, &signal);
    // A run that a signal ended ends by it: a shell's loop of runs then stops
    // at Ctrl+C. The status is for a signal that the process blocks.
    if (signal != 0) {
        std::signal(signal, SIG_DFL);
        std::raise(signal);
    }
    return status;
}

} // namespace Aglow
