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
// bells, notifications, the clipboard, file transfer and drag and drop. The
// daemon sends each as a Host event of the attach stream (MuxStream.h), and
// the window makes the same call on its session pane. The pane's answers to
// the program come back as its input.
//
// The state of the terminal (title, directory, modes, colors) goes in the
// state frames instead, and the daemon answers what the terminal answers
// itself (DA1, DSR, color and size queries).

namespace AfterglowMux::Host {

/// The largest call, encoded: what one frame of the attach stream takes. A
/// larger call (a clipboard write of many megabytes) gets the answer that a
/// session without a window gives.
inline constexpr std::size_t MaxCall = (std::size_t(4) << 20) - 256;

enum class Kind : std::uint8_t {
    Bell = 1,
    Flash = 2, ///< OSC 555
    Notify = 3, ///< OSC 9 and OSC 777: text and title
    DesktopNotification = 4, ///< OSC 99: notification
    ClipboardWrite = 5, ///< OSC 52 write: text is the selection list, title the data
    ClipboardRequest = 6, ///< OSC 52 read and OSC 5522: clipboard
    TerminalProtocol = 7, ///< OSC 5113 and OSC 72: number is the code, text the payload
    PointerShape = 8, ///< OSC 22: text
    UserVar = 9, ///< OSC 1337 SetUserVar: title is the name, text the value
    CommandStarted = 10, ///< OSC 133: text
    CommandFinished = 11, ///< OSC 133: number is the exit code
    Download = 12, ///< OSC 1337 File: title is the name, text the data
};

struct Call
{
    Kind kind = Kind::Bell;
    std::string title;
    std::string text;
    int number = 0;
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
/// nullopt for bytes that are no call of this codec.
std::optional<Call> decode(std::string_view bytes);

} // namespace AfterglowMux::Host
