// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxLayout.h"
#include "MuxProtocol.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The stream of an attached session: a phosphor-channel Stream over the
// connection of the attach call, after its answer (Fn::Attach). The daemon
// writes the session's events on channel 0 and the state of each pane on a
// channel of its own (AfterglowVt::StateCodec), which PaneOpened names. Its
// first event is the session's Layout, before any PaneOpened. An attachment
// that finished before the window started its stream sends only its last
// event (TakenOver, Detached or SessionEnded). The window writes on channel
// 0 its input, its layout, its settings and its answers to Control events
// (ControlReply and ControlReplyPart); on ActionChannel what is not for a
// program (its Kill, ClosePane, Visible, Refresh, PaneForgotten and
// DownloadAnswer); and on CommandsChannel
// the bytes of the command clients that the daemon passes it. The ids of
// each direction are apart, so a pane's state channel and the action
// channel can both be 1. Integers are LEB128 varints in their shortest form.
//
// A pane's channel closes after its last frame, and then PaneClosed follows
// on channel 0. The window learns that a pane's program exited once it has
// both: the two channels do not keep an order between them.

namespace AfterglowMux::Attach {

/// The channel of the events (from the daemon) and of the input (from the
/// window). The state channels of panes start at 1.
inline constexpr std::uint32_t ControlChannel = 0;
/// The window's channel for Kill, ClosePane, Visible, Refresh, PaneForgotten
/// and DownloadAnswer: the daemon reads it also while input waits for a full
/// PTY and the control channel gets no credit.
inline constexpr std::uint32_t ActionChannel = 1;
inline constexpr const char* ActionCodec = "afterglow.mux.actions.v1";
/// The window's channel for the bytes of its command connections
/// (RelayFrame). Not the input channel: input waits while a PTY is full,
/// and the client of a run can be what keeps that PTY full.
inline constexpr std::uint32_t CommandsChannel = 2;
inline constexpr const char* CommandsCodec = "afterglow.mux.commands.v1";
/// The ids of the state channels, which the window reads, are below this:
/// the daemon gives a pane the lowest channel id that is free. The window
/// takes MaxSessionPanes channels and the events at once, closing ones
/// included, so a free id is never above MaxSessionPanes.
inline constexpr std::uint32_t MaxChannelId = std::uint32_t(MaxSessionPanes) + 1;
inline constexpr const char* EventCodec = "afterglow.mux.events.v1";
inline constexpr const char* InputCodec = "afterglow.mux.input.v1";

/// How long the daemon waits for the window to start its stream after the
/// answer to its attach call, in milliseconds. It starts right after it.
/// The window is connected already: the stream goes over the connection of
/// the call.
inline constexpr int StreamStartWaitMs = 5000;

enum class EventType : std::uint8_t {
    PaneOpened = 1, ///< a pane exists, and its state comes on `channel`
    /// the program of a pane exited, with its status; or the pane left the
    /// window, with -1: the window closed it (ClosePane), or it went to
    /// another session (Fn::BreakOut, Fn::JoinPane, or the movePane of the
    /// control target, after a paneLeft call)
    PaneClosed = 2,
    SessionEnded = 3, ///< the last program ended, or `afterglow mux kill` ended it
    TakenOver = 4, ///< another window attached: this one detaches
    Detached = 5, ///< `afterglow mux detach`: the window closes, the session runs on
    Host = 6, ///< a call of a pane's terminal to its host, or a part of one, in `payload` (MuxHost.h)
    Layout = 7, ///< the session's tabs and splits, as JSON in `payload` (MuxLayout.h): the first event
    /// the answer to a Refresh of the pane, one for each: its screens and
    /// history are whole in its first `frames` frames
    Refreshed = 8,
    /// A call of the control target that the window answers (`afterglow ctl`
    /// for the session): {"id", "fn", "args", "client"} as JSON in
    /// `payload`, with the daemon's pane IDs. "client" numbers the
    /// connection of the call, the same for its calls. The window answers
    /// with ControlReply. Calls with the ID 0 are the daemon's own, and get
    /// no answer: focus (pane), placePane (pane, beside pane, right, down,
    /// left, up or tab, command, title) for a pane that the daemon started or
    /// moved into the session, and paneLeft (pane, session) before the
    /// PaneClosed of a pane that `afterglow ctl` moved to another session.
    Control = 9,
    /// A part of a command connection that the window serves: an encoded
    /// RelayFrame in `payload`.
    Commands = 10,
};

/// The status of a Host event that holds a part of a call, not its end.
inline constexpr int HostPart = 1;
/// The bytes of a call in each Host event but its last, when the call does
/// not fit one: well under the largest frame of a channel's default window.
inline constexpr std::size_t HostPartBytes = std::size_t(1) << 20;

struct Event
{
    EventType type = EventType::PaneOpened;
    std::uint32_t pane = 0;
    /// PaneOpened: the channel of the pane's state.
    std::uint32_t channel = 0;
    /// PaneClosed: the status as waitpid gives it, or -1 when it is unknown
    /// or the pane left the window with its program running.
    /// Host: HostPart when the call goes on in the next Host event of the
    /// pane, else -1.
    int status = -1;
    /// Host: the frames of the pane's state that the daemon wrote before the
    /// call. The window makes the call once it read as many: a frame larger
    /// than a fragment of the stream can still be on its way when the call
    /// comes, and a bell goes after the text that rang it. Refreshed: the
    /// frames that hold the screens that the Refresh asked for.
    std::uint64_t frames = 0;
    /// Host: the call, encoded, or its next part. A call larger than one
    /// frame comes in parts of at most HostPartBytes. Layout and Control:
    /// their JSON. Commands: an encoded RelayFrame.
    std::string payload;
};

std::string encode(const Event& event);
std::optional<Event> decodeEvent(std::string_view frame);

enum class InputType : std::uint8_t {
    Bytes = 1, ///< bytes for the program, as a window's pane writes them to its PTY
    Resize = 2, ///< the size that the window measured for the pane
    Reset = 3, ///< the user reset the pane's terminal (Action::ResetTerminal)
    Clear = 4, ///< the user cleared the pane's screen and history (Action::ClearScreen)
    /// the user closed the window, or its last tab or pane, and Mux.onClose
    /// is kill: the session ends (ActionChannel only)
    Kill = 5,
    Visible = 6, ///< whether the user can see the pane (mode 2033), in `visible` (ActionChannel only)
    Layout = 7, ///< the window's tabs and splits, as JSON in `bytes` (MuxLayout.h), after each change; pane 0
    ClosePane = 8, ///< the user closed the pane and the window stays: its program ends (ActionChannel only)
    /// the window reads the pane's text, or draws it for a capture: the
    /// daemon sends its screens and history as they are now, also while the
    /// pane does not show, and then a Refreshed event (ActionChannel only)
    Refresh = 9,
    /// The answer to a Control event: {"id", "result"} or {"id", "error"} as
    /// JSON in `bytes`, with the daemon's pane IDs.
    ControlReply = 10,
    /// The window's settings (SessionSettings), as JSON in `bytes`, after
    /// the attach and after each change of them.
    Settings = 11,
    /// A part of a ControlReply larger than one frame, such as a screenshot:
    /// the parts, then the ControlReply with the rest, make its JSON.
    ControlReplyPart = 12,
    /// The window let go of a pane that left the session (PaneClosed after
    /// a move to another session): its ID may come into the stream again.
    /// Until then the daemon moves that pane back into this session for no
    /// one (ActionChannel only).
    PaneForgotten = 13,
    /// The answer to a DownloadBegin host call (MuxHost.h), or the stop of
    /// a download whose data comes, in `download` (ActionChannel only). Not
    /// with the input: the pane's output pauses for the answer, so the
    /// program may block in a write and read no input meanwhile, and its
    /// input that waits can hold the control channel.
    DownloadAnswer = 14,
    /// A part of the answer to an UploadRequest host call (MuxHost.h) for
    /// the program of `pane`: the parts, then the UploadAnswer with the
    /// rest, make the answer, which goes to the program in one write, or not
    /// at all when the window goes before its end. No other input of the
    /// pane comes between them. Only the answer of the window that the
    /// daemon asked last goes, and only while the program still waits: the
    /// next window asks again for an answer that the program did not get.
    UploadAnswerPart = 15,
    UploadAnswer = 16,
};

/// What a DownloadAnswer says. A download of the pane that it does not fit
/// takes nothing from it: a late Stop is not the refusal of the next
/// download.
enum class DownloadReply : std::uint8_t {
    Refuse = 0, ///< the user refused the download that asks: the pane's output goes on
    Accept = 1, ///< the user took the download that asks: its data comes as DownloadData calls
    /// The window cannot take the data of the download that streams (a
    /// write failed): the rest of it goes nowhere, and no DownloadEnd comes.
    Stop = 2,
};

/// The bytes of a ControlReply in each ControlReplyPart, when the reply does
/// not fit one frame, and of an upload's answer in each UploadAnswerPart.
inline constexpr std::size_t ControlReplyPartBytes = std::size_t(1) << 20;
/// The most bytes of one ControlReply with its parts: the daemon ends the
/// link of a window that sends more. Under the output that waits for one
/// client of the daemon.
inline constexpr std::size_t MaxControlReply = std::size_t(24) << 20;

struct Input
{
    InputType type = InputType::Bytes;
    std::uint32_t pane = 0;
    /// Bytes: for the program. Layout, Settings and ControlReply: their
    /// JSON. ControlReplyPart: a part of the JSON of a ControlReply.
    /// UploadAnswerPart and UploadAnswer: a part of the answer to an upload.
    std::string bytes;
    PaneSize size;
    bool visible = false;
    DownloadReply download = DownloadReply::Refuse; ///< DownloadAnswer
};

/// The inputs that come on ActionChannel, and never on the control channel.
constexpr bool isAction(InputType type)
{
    return type == InputType::Kill || type == InputType::ClosePane || type == InputType::Visible
        || type == InputType::Refresh || type == InputType::PaneForgotten || type == InputType::DownloadAnswer;
}

std::string encode(const Input& input);
std::optional<Input> decodeInput(std::string_view frame);

/// A client that calls the commands of scripts reaches the daemon, which
/// runs no scripts: a client in a session pane (`aglow run`, target
/// AfterglowProto::Run::Target), or a client on the remote host of an
/// `afterglow ssh` in a session pane, through the socket that the daemon
/// keeps for that ssh session. The daemon passes the connection to the
/// window that shows the session: its request lines go to the window in
/// Commands events, and the window serves them on a connection to its own
/// Afterglow, whose bytes come back on CommandsChannel. Each connection is a
/// relay, by a number that the daemon gives it.
enum class RelayPart : std::uint8_t {
    Open = 1, ///< the daemon: a new relay
    Bytes = 2, ///< bytes of the connection: whole request lines from the daemon, any bytes from the window
    End = 3, ///< the connection ended on the side that sends this
};

/// Who the client of a relay is, which decides the commands that it gets.
enum class RelayKind : std::uint8_t {
    /// A client in a session pane, of the commands target: the commands of
    /// the window's scripts. For a session of another host, only those that
    /// a script marks remote, as for an ssh session of that host.
    Commands = 0,
    /// A client on the remote host of an ssh session, of the target
    /// AfterglowProto::Remote::Target: only the commands marked remote.
    Remote = 1,
};

struct RelayFrame
{
    std::uint64_t relay = 0; ///< from 1
    RelayPart part = RelayPart::Bytes;
    std::string bytes{}; ///< Bytes: never empty
    /// Open: the daemon's ID of the pane of the client, or of the ssh
    /// session that the client's socket belongs to. Never 0.
    std::uint32_t pane = 0;
    RelayKind kind = RelayKind::Commands; ///< Open
    std::string host{}; ///< Open of RelayKind::Remote: the host of the ssh session (ev.remote.host), never empty
};

std::string encode(const RelayFrame& frame);
std::optional<RelayFrame> decodeRelay(std::string_view frame);

} // namespace AfterglowMux::Attach
