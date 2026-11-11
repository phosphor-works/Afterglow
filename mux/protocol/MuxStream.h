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
// on channel 0 and the state of each pane on a channel of its own
// (AfterglowVt::StateCodec), which PaneOpened names. The window writes its
// input on channel 0, and its Kill on ActionChannel. The ids of each
// direction are apart, so a pane's state channel and the action channel can
// both be 1. Integers are LEB128 varints in their shortest form.
//
// A pane's channel closes after its last frame, and then PaneClosed follows
// on channel 0. The window learns that a pane's program exited once it has
// both: the two channels do not keep an order between them.

namespace AfterglowMux::Attach {

/// The channel of the events (from the daemon) and of the input (from the
/// window). The state channels of panes start at 1.
inline constexpr std::uint32_t ControlChannel = 0;
/// The window's channel for Kill: the daemon reads it also while input waits
/// for a full PTY and the control channel gets no credit.
inline constexpr std::uint32_t ActionChannel = 1;
inline constexpr const char* ActionCodec = "afterglow.mux.actions.v1";
/// The ids of the state channels, which the window reads, are below this, so
/// that a daemon that has run many panes still fits the window's stream: the
/// daemon gives a pane the lowest channel id that is free.
inline constexpr std::uint32_t MaxChannelId = 4096;
inline constexpr const char* EventCodec = "afterglow.mux.events.v1";
inline constexpr const char* InputCodec = "afterglow.mux.input.v1";

/// How long the daemon waits for the window to connect to the socket of an
/// attach, in milliseconds. The window connects right after the call.
inline constexpr int ConnectWaitMs = 5000;

enum class EventType : std::uint8_t {
    PaneOpened = 1, ///< a pane exists, and its state comes on `channel`
    PaneClosed = 2, ///< the program of a pane exited, with its status
    SessionEnded = 3, ///< the last program ended, or `afterglow mux kill` ended it
    TakenOver = 4, ///< another window attached: this one detaches
    Detached = 5, ///< `afterglow mux detach`: the window closes, the session runs on
    Host = 6, ///< a call of a pane's terminal to its host, in `payload` (MuxHost.h)
};

struct Event
{
    EventType type = EventType::PaneOpened;
    std::uint32_t pane = 0;
    /// PaneOpened: the channel of the pane's state.
    std::uint32_t channel = 0;
    /// PaneClosed: the status as waitpid gives it, or -1 when it is unknown.
    int status = -1;
    /// Host: the call, encoded.
    std::string payload;
};

std::string encode(const Event& event);
std::optional<Event> decodeEvent(std::string_view frame);

enum class InputType : std::uint8_t {
    Bytes = 1, ///< bytes for the program, as a window's pane writes them to its PTY
    Resize = 2, ///< the size that the window measured for the pane
    Reset = 3, ///< the user reset the pane's terminal (Action::ResetTerminal)
    Clear = 4, ///< the user cleared the pane's screen and history (Action::ClearScreen)
    Kill = 5, ///< the user closed the window, and Mux.onClose is kill: the session ends (ActionChannel only)
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
