// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

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
/// aglow both write and read the sequence here.
namespace AfterglowProto::Remote {

/// The OSC number: AGLOW on a phone keypad.
inline constexpr int Osc = 24569;
/// The only target of a forwarded socket. It has the calls of a run (list,
/// input, run, ack and cancel), for the commands that declare "remote".
inline constexpr const char* Target = "remote";
/// A Unix socket path holds 104 bytes on the BSDs and macOS, and 108 on
/// Linux. The remote host can be any of them: a longer path gets no forward.
inline constexpr std::size_t MaxPathBytes = 104;
/// The random bytes of a session's token, which names its socket on both
/// hosts. It only has to be unique: the socket's permissions are the boundary.
inline constexpr std::size_t TokenBytes = 8;
/// aglow run waits this long for the answer to its query at most. The guard
/// query (DA1, or a status report inside tmux) ends the wait before that.
inline constexpr int GuardTimeoutMs = 2000;

/// Why a pane has no socket to give.
enum class Error {
    Off, ///< no forward for this session: no ssh session, or remote commands are off
    Forward, ///< the ssh server refused the forward (AllowStreamLocalForwarding)
    Version, ///< the aglow on the remote host speaks another protocol version
    Length, ///< the socket path on the remote host would be over MaxPathBytes
};
std::string_view errorName(Error error);
std::optional<Error> errorFromName(std::string_view name);

/// A pane's answer: the remote socket path, else the reason for none.
struct Answer
{
    std::string path; ///< empty when there is none
    Error error = Error::Off;
    bool operator==(const Answer&) const = default;
};

/// The query of aglow run: OSC 24569 ; q=remote ST.
std::string query();
/// Whether @p payload, the part of an OSC 24569 after "24569;", is the query.
bool isQuery(std::string_view payload);
/// The pane's reply to the query: OSC 24569 ; path=PATH, or ; error=REASON.
/// It ends with BEL when @p bel, as the query did, else with ST. A path that
/// validPath() refuses answers error=off.
std::string reply(const Answer& answer, bool bel = false);
/// The answer in @p payload, the part of an OSC 24569 reply after "24569;".
/// Nothing when it is no reply: the query itself, an unknown reason, a path
/// that validPath() refuses.
std::optional<Answer> parseReply(std::string_view payload);

/// A path that can go in the reply: absolute, with no control characters,
/// at most MaxPathBytes.
bool validPath(std::string_view path);

/// @p sequences wrapped in tmux's passthrough (DCS tmux; ... ST, each ESC
/// doubled), so that tmux sends them on to the terminal that it runs in.
/// Only with allow-passthrough on, which is off by default.
std::string tmuxPassthrough(std::string_view sequences);

/// The remote directory of the sockets: $XDG_RUNTIME_DIR/afterglow when
/// @p runtime is an absolute path, else @p home/.local/state/afterglow/run.
/// Empty when @p home is not absolute either.
std::string directory(std::string_view runtime, std::string_view home);
/// DIRECTORY/TOKEN.sock.
std::string socketPath(std::string_view directory, std::string_view token);

/// What prune() did.
struct Pruned
{
    std::vector<std::string> removed; ///< the file names, without the directory
    std::string error; ///< the directory could not be read
};
/// Removes the sockets in @p directory that belong to @p owner, are named
/// *.sock and refuse a connection. They are left over from sessions that
/// ended: sshd leaves the file of a forward that was cancelled. A socket that
/// accepts a connection belongs to a live session and stays, and so do files
/// that are not sockets or belong to another user. `aglow remote-prune`.
Pruned prune(const std::string& directory, uid_t owner);

} // namespace AfterglowProto::Remote
