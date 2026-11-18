// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowProto/Fd.h>
#include <AfterglowProto/Graphics.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/types.h>

namespace AfterglowMux {

/// The argument that makes aglow-mux the server of ImageDecoder, with the
/// process id of its client and DecodeWireVersion after it.
inline constexpr std::string_view DecodeServerArgument = "--decode-images";
/// The version of what the client and the server of ImageDecoder say to
/// each other (ImageWire.h, AfterglowProto/DecodeRecord.h): a server of
/// another version ends at once. A window runs the aglow-mux next to it,
/// which an upgrade can replace with another version while the window runs.
inline constexpr std::string_view DecodeWireVersion = "3";

/// The formats that a program may send.
enum class ImageFormats : std::uint8_t {
    Png, ///< kitty graphics f=100: PNG only, and of an APNG its default image, as kitty shows it
    CommonFrames, ///< iTerm2 inline images: PNG, JPEG, GIF, WebP, BMP, TIFF and netPBM, with each frame of a GIF, WebP or APNG animation
};

/// One frame of decodeFrames(): RGBA8 with straight alpha, and the
/// milliseconds that it shows.
struct DecodedFrame
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    std::uint32_t delayMs = 0;
};

/// Decodes the images that programs send into RGBA8 with straight alpha:
/// those of the panes of sessions in the daemon, and those of a window's own
/// panes (ProgramImages in the application). AfterglowImage decodes, with
/// libpng, libjpeg-turbo and libwebp built in, and readers of its own for
/// GIF, BMP, TIFF and netPBM.
///
/// Each image decodes in a process of its own, so that a corrupt file ends
/// that process, not the daemon and its sessions, nor the window. Those
/// processes fork from a server that the client starts with an executable
/// of aglow-mux: a fork costs less than an exec. The daemon passes its own
/// (/proc/self/exe), whose code and libraries are those of the daemon's
/// start, also after an upgrade replaced them on disk; a window passes the
/// aglow-mux next to it, which DecodeWireVersion checks. The client has
/// threads, and a process with threads cannot fork without an exec safely:
/// the server has none. A decoding process ends with the server, the server
/// with its client.
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
    /// default quota.
    static constexpr std::size_t MaxBytes = AfterglowProto::GraphicsDefaultQuota;

    /// A line for the log of the client: what went wrong with a decode, at
    /// most once a minute for each kind.
    using Reporter = std::function<void(const std::string& line)>;
    /// The reports of the daemon: lines on standard error, which start with
    /// "aglow-mux: ".
    static void reportOnStandardError(const std::string& line);

    /// The server is @p executable with DecodeServerArgument: the daemon
    /// passes /proc/self/exe. It starts now, and again with a decode after
    /// it ended. A decode waits @p waitMs for the server's answer and the
    /// pixels. @p report takes the client's reports; the server writes its
    /// own on standard error, after "afterglow-image: ".
    explicit ImageDecoder(std::string executable, int waitMs = WaitMs, Reporter report = reportOnStandardError);
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
    /// decode() of ImageFormats::CommonFrames: each frame of an animation,
    /// a whole canvas, and the milliseconds that it shows, at most
    /// AfterglowProto::GraphicsMaxFrames of them and @p maxBytes of pixels
    /// together. An animation that the process could not finish (corrupt
    /// data, its CPU limit) keeps the frames that came.
    bool decodeFrames(std::string_view data, std::size_t maxBytes, std::vector<DecodedFrame>& frames, int cancel);
    /// decodeFrames() as ITerminalHost::decodeImageFrames gives them: the
    /// frames into AfterglowVt::ImageFrame, their milliseconds into
    /// @p delays. A template, so that this header needs no AfterglowVt: the
    /// server includes it too.
    template <typename Frame>
    bool decodeFrames(std::string_view data, std::size_t maxBytes, std::vector<Frame>& frames, std::vector<std::uint32_t>& delays,
                      int cancel)
    {
        std::vector<DecodedFrame> decoded;
        if (!decodeFrames(data, maxBytes, decoded, cancel)) return false;
        for (DecodedFrame& d : decoded) {
            Frame frame;
            frame.width = d.width;
            frame.height = d.height;
            frame.rgba = std::move(d.rgba);
            frames.push_back(std::move(frame));
            delays.push_back(d.delayMs);
        }
        return true;
    }

private:
    /// Decodes @p data in a process: the frames that it writes, up to
    /// @p maxBytes of pixels.
    bool run(std::string_view data, ImageFormats formats, std::size_t maxBytes, std::vector<DecodedFrame>& frames, int cancel);
    /// Starts the server. m_mutex held.
    bool startLocked();
    /// Ends the server and reaps it. m_mutex held.
    void stopLocked();
    /// Asks the server for a process that decodes @p input into @p output,
    /// and returns a pidfd of that process. None, with the server running,
    /// when it could not fork, when @p deadline passed before the answer
    /// (the answer is owed then), and when @p cancel is readable
    /// (@p cancelled). A server that does not take the request, or that
    /// does not answer m_waitMs after a request, ends: m_socket is then
    /// closed. m_mutex held.
    AfterglowProto::UniqueFd requestLocked(ImageFormats formats, int input, int output,
                                           std::chrono::steady_clock::time_point deadline, int cancel, bool& cancelled);
    /// The server's answer to a request: the pidfd of its process, or none.
    /// m_socket closes when the server ended. m_mutex held.
    AfterglowProto::UniqueFd answerLocked();
    /// Closes the socket of a server that ended, and reaps it. m_mutex held.
    void serverEndedLocked();
    /// Ends a server that did not answer in time, with the back-off of one
    /// that ended before it answered. m_mutex held.
    void stuckLocked();
    /// A request's answer comes to the next request: by @p due, or the
    /// server is stuck. m_mutex held.
    void oweLocked(std::chrono::steady_clock::time_point due);

    const std::string m_executable;
    const Reporter m_report;
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
    /// The executable is of another version (DecodeWireVersion): no server
    /// starts again.
    bool m_otherVersion = false;
    /// Answers that the requests of panes that closed, and of callers whose
    /// deadline came first, left on the socket.
    int m_owedAnswers = 0;
    /// When the oldest owed answer must have come.
    std::chrono::steady_clock::time_point m_answerDue;
    /// A line for m_report, at most once a minute for each @p last. Its own
    /// lock: a pane whose decode is late does not wait for another pane's.
    void reportOnce(std::chrono::steady_clock::time_point& last, const std::string& text);
    std::mutex m_reportMutex;
    /// Of reports of failed starts, and of the others; m_reportMutex.
    std::chrono::steady_clock::time_point m_lastStartReport;
    std::chrono::steady_clock::time_point m_lastReport;
};

} // namespace AfterglowMux
