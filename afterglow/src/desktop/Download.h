// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace Afterglow {

/// The file name that an iTerm2 download suggests, as the user sees and
/// keeps it: its last path part, printable, short enough for NAME_MAX, and
/// with no leading dot, so that it never names a hidden file (a
/// ".bashrc" in a download directory of ~). "download.bin" when nothing is
/// left.
QString cleanDownloadName(std::string_view name);

/// The names that a download tries in its directory: @p name, then
/// "name (1).ext" up to "name (N).ext" with N one less.
inline constexpr int DownloadNames = 1000;
/// @p name for try @p n of DownloadNames: @p name itself for 0, else
/// "stem (n).ext", which keeps a last extension.
QString numberedDownloadName(const QString& name, int n);

/// What a file system offers for a rename that replaces no file. Both
/// unless a test leaves one out, as a file system can.
struct DownloadRename
{
    bool noReplace = true; ///< renameat2 with RENAME_NOREPLACE (not NFS, FUSE)
    bool link = true; ///< link(2), which also fails on a name that is taken
};
/// Renames @p part to @p target, or to the first free numbered name beside
/// it (numberedDownloadName): the file that it became, else empty with
/// errno. Never over a file that is there: a program that names its
/// download for a file that the user made while it streamed loses nothing.
/// A file system with neither of @p calls looks at the name first, and a
/// file that comes to it between the look and the rename is lost.
QString renameToFreeName(const QByteArray& part, const QString& target, DownloadRename calls = {});

/// A download that streams to a file of its own next to the file that it
/// becomes: "name.part", or "name.N.part" when that is taken, so that the
/// rename at the end stays on one file system. A thread of its own writes
/// it: a slow file system holds neither the pane's lock nor the GUI thread.
/// The rename replaces no file that came to the name while it streamed:
/// the download takes the next free "name (N).ext" then, unless the user
/// chose to replace it.
class DownloadFile
{
public:
    /// The bytes that may wait for the disk: past them the producer
    /// pauses (pauseIfFull).
    static constexpr std::size_t MaxQueued = std::size_t(8) << 20;
    /// How a download ended: the file that it became, whether it was saved,
    /// and why not.
    using Ended = std::function<void(const QString& path, bool ok, const QString& error)>;

    /// Creates the part file of @p path. Null, with @p error, when it cannot.
    /// With @p replace (the user confirmed it in Save As) the download
    /// replaces a file at @p path, else it takes another name beside it.
    /// @p room runs on the writer thread when a queue that made a producer
    /// pause (pauseIfFull) has room again, with the queue's lock held: it
    /// takes no lock that is held around pauseIfFull(). @p ended runs on the
    /// writer thread once a finish() is done, and never for a download that
    /// is destroyed before its finish().
    static std::unique_ptr<DownloadFile> create(const QString& path, bool replace, std::function<void()> room,
                                                Ended ended, QString& error);
    /// A download that did not finish loses its part file. One that did
    /// waits for its writer to rename or remove it.
    ~DownloadFile();
    /// Its owner goes, and does not wait for the disk: @p file goes on
    /// without it, as ~DownloadFile would (one that did not finish loses
    /// its part file, one that did gets its name), but neither room() nor
    /// ended() runs any more, once this returns. The process joins the
    /// writer later: at the next release, or at its exit. The exit waits
    /// only for a download that finished: one that did not only loses its
    /// part file, and its writer can wait on a disk that hangs.
    static void release(std::unique_ptr<DownloadFile> file);
    DownloadFile(const DownloadFile&) = delete;
    DownloadFile& operator=(const DownloadFile&) = delete;

    /// Queues @p bytes for the writer. False once a write failed: the
    /// download is lost, and finish() says why.
    bool write(std::string_view bytes);
    /// When the queue is past MaxQueued, runs @p pause (the producer
    /// stops) and returns true: room() comes once the writer has emptied
    /// half of it. Under the queue's lock, so that room() cannot come
    /// between the look and the pause.
    bool pauseIfFull(const std::function<void()>& pause);
    /// The download ends: with @p complete, the file becomes its target
    /// once all of it is written, else the part file goes, and the error
    /// is @p reason unless a write failed first. Then ended().
    void finish(bool complete, const QString& reason);
    /// The file that the download asks for. It becomes another when that
    /// name is taken at the end and the download does not replace it: Ended
    /// names the file.
    const QString& target() const
    {
        return m_target;
    }

private:
    struct Released;
    static Released& released();

    DownloadFile(int fd, QByteArray part, QString target, bool replace, std::function<void()> room, Ended ended);
    void run();
    /// m_mutex held: a producer paused, and the queue is half empty.
    bool hasRoomLocked() const;

    const int m_fd;
    const QByteArray m_part;
    const QString m_target;
    const bool m_replace;
    const std::function<void()> m_room;
    const Ended m_ended;
    std::mutex m_mutex;
    std::condition_variable m_wake; ///< the writer waits for bytes and the end
    std::deque<std::string> m_queue; ///< guarded by m_mutex
    std::size_t m_queued = 0; ///< bytes in m_queue, guarded by m_mutex
    bool m_paused = false; ///< a producer paused: room() is due, guarded by m_mutex
    bool m_failed = false; ///< guarded by m_mutex
    QString m_error; ///< why a write failed, guarded by m_mutex
    bool m_finishing = false; ///< guarded by m_mutex
    bool m_complete = false; ///< guarded by m_mutex
    QString m_reason; ///< guarded by m_mutex
    bool m_cancelled = false; ///< guarded by m_mutex
    /// Held while room() or ended() runs: release() waits for one that runs.
    /// Taken before m_mutex, which room() runs under too: ended() takes the
    /// producer's own lock, which is held around pauseIfFull().
    std::mutex m_callMutex;
    bool m_released = false; ///< release(): no more calls, guarded by m_callMutex
    std::atomic_bool m_done{false}; ///< the writer has returned: a join takes no time
    std::thread m_thread;
};

} // namespace Afterglow
