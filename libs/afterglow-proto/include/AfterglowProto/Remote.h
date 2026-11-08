// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/RemoteOsc.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

/// Package commands from a remote host. `afterglow ssh` forwards a socket of
/// the terminal to the remote host, and `aglow run` there finds it with OSC
/// 24569: the pane that runs the ssh session answers with the socket's path
/// on the remote host, or with the reason why there is none. The pane and
/// aglow both write and read the sequence (RemoteOsc.h).
namespace AfterglowProto::Remote {

/// The only target of a forwarded socket. It has the calls of a run (list,
/// input, run, ack and cancel), for the commands that declare "remote".
inline constexpr const char* Target = "remote";
/// The random bytes of a session's token, which names its socket on both
/// hosts. It only has to be unique: the socket's permissions are the boundary.
inline constexpr std::size_t TokenBytes = 8;
/// aglow run waits this long for the answer to its query at most. The guard
/// query (DA1, or a status report inside tmux) ends the wait before that.
inline constexpr int GuardTimeoutMs = 2000;

/// The part of @p osc after "24569;", where @p osc is an OSC string without
/// its introducer and terminator. Nothing for another OSC.
std::optional<std::string_view> oscPayload(std::string_view osc);

/// @p sequences wrapped in tmux's passthrough (DCS tmux; ... ST, each ESC
/// doubled), so that tmux sends them on to the terminal that it runs in.
/// Only with allow-passthrough on, which is off by default.
std::string tmuxPassthrough(std::string_view sequences);

/// DIRECTORY/TOKEN.sock. The probe of `afterglow ssh` chooses the directory.
std::string socketPath(std::string_view directory, std::string_view token);

/// What prune() did.
struct Pruned
{
    std::vector<std::string> removed; ///< the file names, without the directory
    std::string error; ///< the directory could not be read
};
/// Removes the sockets in @p directory that belong to @p owner, are named
/// *.sock and that no process holds. They are left over from sessions that
/// ended: sshd leaves the file of a forward that was cancelled. Where the
/// kernel lists the bound sockets (/proc/net/unix on Linux), a socket there
/// stays without a connection to it: one that sshd has bound but does not
/// listen on yet is live. Elsewhere, and for a socket that the list does not
/// name, only a refused connection tells that nothing holds it. Files that
/// are not sockets or belong to another user stay. `aglow remote-prune`.
Pruned prune(const std::string& directory, uid_t owner);

} // namespace AfterglowProto::Remote
