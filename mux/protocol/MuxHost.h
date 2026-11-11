// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowVt/Terminal.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// The calls that the terminal of a session pane makes to its host
// (AfterglowVt::ITerminalHost) and that the window of the session answers:
// bells, flashes, notifications, the clipboard, file transfer, drag and
// drop, downloads, pointer shapes, user variables, the commands of shell
// integration, and the reports that the terminal answers queries with. The
// daemon sends each as a Host event of the attach stream (MuxStream.h), and
// the window makes the same call on its session pane. The pane's answers to
// the program come back as its input.
//
// The title, the directory, the modes, the colors, the marks of shell
// integration (OSC 133), program status (OSC 9;4 and 7501), shell contexts
// (OSC 3008), extra cursors and region scrolls go in the state frames
// instead (AfterglowVt/StateSync.h).
//
// The layout of each call changes with ProtocolVersion (MuxProtocol.h).

namespace AfterglowMux::Host {

/// The largest call, encoded, that a window takes: a clipboard write of the
/// most that a terminal takes, and its types and names. A call larger than
/// one frame comes in parts (Attach::HostPartBytes).
inline constexpr std::size_t MaxCall = AfterglowVt::ClipboardAssembler::MaxWrite + (std::size_t(1) << 20);
/// What a waiting call costs against Mux.eventQueue on top of its bytes: its
/// string and its place in a queue. A burst of tiny calls is bounded too.
inline constexpr std::size_t CallCost = 64;

enum class Kind : std::uint8_t {
    Bell = 1,
    Flash = 2, ///< OSC 555
    Notify = 3, ///< OSC 9 and OSC 777: title and text
    DesktopNotification = 4, ///< OSC 99: notification
    ClipboardWrite = 5, ///< OSC 52 write: title is the selection list, text the data
    ClipboardRequest = 6, ///< OSC 52 read and OSC 5522: clipboard
    TerminalProtocol = 7, ///< OSC 5113 and OSC 72, and 0 for a terminal reset: number is the code, text the payload
    PointerShape = 8, ///< OSC 22: text
    UserVar = 9, ///< OSC 1337 SetUserVar: title is the name, text the value
    CommandStarted = 10, ///< OSC 133: text
    CommandFinished = 11, ///< OSC 133: number is the exit code, value the milliseconds that the command ran
    Download = 12, ///< OSC 1337 File: title is the name, text the data
    /// The answer that the terminal writes to a query (DA1, DSR, colors), in
    /// text: the window writes it after the replies to the commands that the
    /// program wrote before the query.
    Report = 13,
    /// Commands of TerminalProtocol that did not fit the queue: number is what
    /// they held for drag and drop (LostNothing, LostReset, LostPackets), value
    /// 1 when one of them was a terminal reset.
    ProtocolLost = 14,
};

/// ProtocolLost: what the lost commands held, in the order that the program
/// wrote them (DragDrop::Lost of the window).
inline constexpr int LostNothing = 0; ///< no OSC 72 packet and no terminal reset
inline constexpr int LostReset = 1; ///< a terminal reset, and no OSC 72 packet after it
inline constexpr int LostPackets = 2; ///< an OSC 72 packet after the last terminal reset

struct Call
{
    Kind kind = Kind::Bell;
    std::string title;
    std::string text;
    int number = 0;
    std::uint64_t value = 0;
    AfterglowVt::NotificationRequest notification;
    AfterglowVt::ClipboardRequest clipboard;
};

/// A call of @p kind with the fields that most kinds use.
inline Call make(Kind kind, std::string title = {}, std::string text = {}, int number = 0)
{
    Call call;
    call.kind = kind;
    call.title = std::move(title);
    call.text = std::move(text);
    call.number = number;
    return call;
}

std::string encode(const Call& call);
/// nullopt for bytes that are no call of this codec, or a call that the
/// terminal's parser never makes: text that it would refuse, an id that it
/// would strip, a value out of its range. The window answers the program
/// with what it decodes.
std::optional<Call> decode(std::string_view bytes);

} // namespace AfterglowMux::Host
