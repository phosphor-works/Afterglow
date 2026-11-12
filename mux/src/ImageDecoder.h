// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowProto/Fd.h>
#include <AfterglowProto/Graphics.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace AfterglowMux {

/// The argument that makes aglow-mux the server of ImageDecoder, with
/// the daemon's process id after it.
inline constexpr std::string_view DecodeServerArgument = "--decode-images";

/// The formats that a program may send.
enum class ImageFormats : std::uint8_t {
    Png, ///< kitty graphics f=100: PNG only
    Common, ///< iTerm2 inline images: PNG, JPEG, GIF and WebP (their first frame) and BMP
};

/// Decodes the images of the programs of sessions into RGBA8 with straight
/// alpha, as the window's decodeBoundedImage does with Qt. The daemon has no
/// Qt: AfterglowImage decodes, with libpng, libjpeg-turbo and libwebp built
/// in.
///
/// Each image decodes in a process of its own, so that a corrupt file ends
/// that process, not the daemon and its sessions. Those processes fork from
/// a server that the daemon starts once, with its own executable: a fork
/// costs less than an exec, and the server's code and libraries are those
/// of the daemon's start, also after an upgrade replaced them on disk. The
/// daemon has threads, and a process with threads cannot fork without an
/// exec safely: the server has none. A decoding process ends with the
/// server, the server with the daemon.
///
/// The limits of a decoding process are the safety bounds of untrusted
/// input, as those of aglow's decoder, not settings: its CPU time and its
/// memory from the size in the image's header, no core file, no new
/// privileges, and only the system calls that decoding needs.
///
/// decode() is safe to call from any thread: the PTY reader threads of the
/// panes decode at once, each in a process of its own.
class ImageDecoder
{
public:
    /// How long decode() waits for the pixels of an image before it kills
    /// its process. The CPU limit ends a decode that computes first.
    static constexpr int WaitMs = 30000;
    /// The most bytes of an image file, and of its pixels: the store's
    /// default quota, as the window's decodeBoundedImage takes.
    static constexpr std::size_t MaxBytes = AfterglowProto::GraphicsDefaultQuota;

    /// The server is @p executable with DecodeServerArgument: the daemon
    /// passes /proc/self/exe. It starts now, with the daemon, and again
    /// with a decode after it ended. A decode waits @p waitMs for the
    /// server's answer and the pixels.
    explicit ImageDecoder(std::string executable, int waitMs = WaitMs);
    /// Ends the server, which ends the decodes that still run.
    ~ImageDecoder();
    ImageDecoder(const ImageDecoder&) = delete;
    ImageDecoder& operator=(const ImageDecoder&) = delete;

    /// Decodes @p data. False when the format is not one of @p formats, when
    /// the image is larger than MaxBytes, when the decoder refuses or fails,
    /// when it takes longer than the wait, and at once when @p cancel (a
    /// descriptor, or -1) is readable: the pane that waits for it closes.
    bool decode(std::string_view data, ImageFormats formats, std::uint32_t& width, std::uint32_t& height,
                std::vector<std::uint8_t>& rgba, int cancel);

private:
    /// Starts the server. m_mutex held.
    bool startLocked();
    /// Ends the server and reaps it. m_mutex held.
    void stopLocked();
    /// Asks the server for a process that decodes @p input into @p output,
    /// and returns a pidfd of that process. None, with the server running,
    /// when it could not fork, when @p deadline passed before the request,
    /// and when @p cancel is readable (@p cancelled). A server that does not
    /// take the request or answer it by @p deadline ends: m_socket is then
    /// closed. m_mutex held.
    AfterglowProto::UniqueFd requestLocked(ImageFormats formats, int input, int output,
                                           std::chrono::steady_clock::time_point deadline, int cancel, bool& cancelled);
    /// The server's answer to a request: the pidfd of its process, or none.
    /// m_socket closes when the server ended. m_mutex held.
    AfterglowProto::UniqueFd answerLocked();
    /// Closes the socket of a server that ended. m_mutex held.
    void serverEndedLocked();

    const std::string m_executable;
    const int m_waitMs;
    std::timed_mutex m_mutex; ///< guards the server: its socket takes one request, then its answer
    AfterglowProto::UniqueFd m_socket;
    pid_t m_server = -1;
    /// The servers that started in a row and ended before they answered a
    /// request: 0 once the server answers.
    int m_unanswered = 0;
    /// When a server last failed to start, or when two in a row ended before
    /// they answered: none starts for a minute.
    std::optional<std::chrono::steady_clock::time_point> m_failedStart;
    /// Answers that the requests of panes that closed left on the socket.
    int m_owedAnswers = 0;
    /// A line on standard error, at most once a minute for each @p last. Its
    /// own lock: a pane whose decode is late does not wait for another
    /// pane's.
    void reportOnce(std::chrono::steady_clock::time_point& last, const std::string& text);
    std::mutex m_reportMutex;
    /// Of reports of failed starts, and of the others; m_reportMutex.
    std::chrono::steady_clock::time_point m_lastStartReport;
    std::chrono::steady_clock::time_point m_lastReport;
};

/// The server of ImageDecoder: forks a decoding process for each request on
/// standard input, until the daemon @p parent closes it. Returns the exit
/// status.
int runImageServer(std::string_view parent);

} // namespace AfterglowMux
