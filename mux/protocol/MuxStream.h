// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxProtocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The stream of an attached session: a phosphor-channel Stream over the
// socket that the attach call names. The daemon writes the session's events
// on channel 0 and the state of pane N on channel N (AfterglowVt::StateCodec).
// The window writes its input on channel 0. Integers are LEB128 varints.

namespace AfterglowMux::Attach {

/// The channel of the events (from the daemon) and of the input (from the
/// window). Pane ids start at 1, so no pane's state channel takes it.
inline constexpr std::uint32_t ControlChannel = 0;
inline constexpr const char* EventCodec = "afterglow.mux.events.v1";
inline constexpr const char* InputCodec = "afterglow.mux.input.v1";

/// How long the daemon waits for the window to connect to the socket of an
/// attach, in milliseconds. The window connects right after the call.
inline constexpr int ConnectWaitMs = 5000;

enum class EventType : std::uint8_t {
    PaneOpened = 1, ///< a pane exists, and its state comes on its channel
    PaneClosed = 2, ///< the program of a pane exited: status as waitpid gives it
    SessionEnded = 3, ///< the last program ended, or `afterglow mux kill` ended it
    TakenOver = 4, ///< another window attached: this one detaches
    Detached = 5, ///< `afterglow mux detach`: the window closes, the session runs on
};

struct Event
{
    EventType type = EventType::PaneOpened;
    std::uint32_t pane = 0;
    int status = 0;
};

std::string encode(const Event& event);
std::optional<Event> decodeEvent(std::string_view frame);

enum class InputType : std::uint8_t {
    Bytes = 1, ///< bytes for the program, as a window's pane writes them to its PTY
    Resize = 2, ///< the size that the window measured for the pane
};

struct Input
{
    InputType type = InputType::Bytes;
    std::uint32_t pane = 0;
    std::string bytes;
    PaneSize size;
};

std::string encode(const Input& input);
std::optional<Input> decodeInput(std::string_view frame);

} // namespace AfterglowMux::Attach
