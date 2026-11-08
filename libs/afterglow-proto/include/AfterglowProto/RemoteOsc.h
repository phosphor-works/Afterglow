// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

/// The OSC 24569 sequence of remote commands, which the terminal answers:
/// the part of AfterglowProto/Remote.h that the parser needs, without the
/// files and sockets of `aglow remote-prune`.
namespace AfterglowProto::Remote {

/// The OSC number: AGLOW on a phone keypad.
inline constexpr int Osc = 24569;
/// sun_path holds 104 bytes on the BSDs and macOS, and 108 on Linux, with
/// the NUL that ends the path. The remote host can be any of them: a path
/// of more than 103 bytes gets no forward.
inline constexpr std::size_t MaxPathBytes = 103;

/// Why a pane has no socket to give.
enum class Error {
    Off, ///< no forward for this session: no ssh session, or remote commands are off
    Forward, ///< the ssh server refused the forward (AllowStreamLocalForwarding), or Afterglow did not confirm it in time
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

/// A path that can go in the reply: absolute, UTF-8 with no control
/// characters (isCleanText), at most MaxPathBytes.
bool validPath(std::string_view path);

} // namespace AfterglowProto::Remote
