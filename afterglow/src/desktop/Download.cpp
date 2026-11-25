// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "desktop/Download.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringView>
#include <QTextBoundaryFinder>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Afterglow {

namespace {
/// The longest name that a download suggests, in UTF-8 bytes.
constexpr qsizetype MaxNameBytes = 200;
/// The longest extension that a cut name keeps, its dot included, in code points.
constexpr qsizetype MaxExtension = 16;

} // namespace

QString renameToFreeName(const QByteArray& part, const QString& target, DownloadRename calls)
{
    const QFileInfo info(target);
    const QDir directory = info.dir();
    const QString name = info.fileName();
    for (int n = 0; n < DownloadNames; ++n) {
        const QString path = n == 0 ? target : directory.filePath(numberedDownloadName(name, n));
        const QByteArray to = QFile::encodeName(path);
        if (calls.noReplace) {
            if (::renameat2(AT_FDCWD, part.constData(), AT_FDCWD, to.constData(), RENAME_NOREPLACE) == 0)
                return path;
        } else {
            errno = EINVAL;
        }
        // A file system without RENAME_NOREPLACE (NFS, FUSE), or a seccomp
        // filter without renameat2 (ENOSYS): link(2) also fails on a name
        // that is taken.
        if (errno == EINVAL || errno == ENOSYS) {
            if (calls.link && ::link(part.constData(), to.constData()) == 0) {
                ::unlink(part.constData());
                return path;
            }
            if (!calls.link)
                errno = EPERM;
            // Nor hard links (some FUSE file systems): a look, then a rename.
            if (errno != EEXIST) {
                struct stat st;
                if (::lstat(to.constData(), &st) == 0)
                    errno = EEXIST;
                else if (errno == ENOENT)
                    return ::rename(part.constData(), to.constData()) == 0 ? path : QString();
            }
        }
        if (errno != EEXIST)
            return {};
    }
    errno = EEXIST;
    return {};
}

QString numberedDownloadName(const QString& name, int n)
{
    if (n == 0)
        return name;
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > 0 ? name.left(dot) : name;
    const QString suffix = dot > 0 ? name.mid(dot) : QString();
    // One arg(): the program chose the stem, which may hold a %2.
    return QStringLiteral("%1 (%2)%3").arg(stem, QString::number(n), suffix);
}

QString cleanDownloadName(std::string_view name)
{
    QString suggested = QString::fromUtf8(name.data(), qsizetype(name.size()));
    suggested.replace(QLatin1Char('\\'), QLatin1Char('/'));
    suggested = suggested.section(QLatin1Char('/'), -1);
    // By code point: a character outside the BMP is two UTF-16 units, and
    // neither half of the pair is printable on its own.
    std::u32string kept;
    for (const char32_t c : suggested.toUcs4())
        if (c && QChar::isPrint(c) && QChar::category(c) != QChar::Other_Format)
            kept += c;
    QString clean = QString::fromStdU32String(kept);
    // A program names no hidden file: with Terminal.downloadDirectory set
    // to ~ and downloads allowed, ".bashrc" would be the shell's own.
    while (clean.startsWith(QLatin1Char('.')))
        clean.remove(0, 1);
    // NAME_MAX is 255 bytes. A long name keeps room for what the user adds,
    // and keeps its last extension when that is short: the stem is cut at
    // the end of a whole character (a grapheme, with its combining marks).
    if (clean.toUtf8().size() > MaxNameBytes) {
        const qsizetype dot = clean.lastIndexOf(QLatin1Char('.'));
        const bool extension = dot > 0 && QStringView(clean).mid(dot).toUcs4().size() <= MaxExtension;
        const QString suffix = extension ? clean.mid(dot) : QString();
        const QString stem = extension ? clean.left(dot) : clean;
        const qsizetype budget = MaxNameBytes - suffix.toUtf8().size();
        QTextBoundaryFinder graphemes(QTextBoundaryFinder::Grapheme, stem);
        qsizetype end = 0, used = 0;
        for (qsizetype start = 0, next = graphemes.toNextBoundary(); next > 0;
             start = next, next = graphemes.toNextBoundary()) {
            used += QStringView(stem).mid(start, next - start).toUtf8().size();
            if (used > budget)
                break;
            end = next;
        }
        // A first character over the budget alone (marks without end) leaves
        // no stem: the name would be a hidden file.
        clean = (end > 0 ? stem.left(end) : QStringLiteral("download")) + suffix;
    }
    if (clean.isEmpty())
        clean = QStringLiteral("download.bin");
    return clean;
}

/// The downloads that release() took from their owners, until their writers
/// return. Its end, at the exit of the process, waits for each that
/// finished: it gets its name before the process ends.
struct DownloadFile::Released
{
    std::mutex mutex;
    std::vector<std::unique_ptr<DownloadFile>> files; ///< guarded by mutex

    ~Released()
    {
        for (std::unique_ptr<DownloadFile>& file : files) {
            bool cancelled = false;
            {
                std::lock_guard lock(file->m_mutex);
                cancelled = file->m_cancelled;
            }
            // A cancelled writer only removes its part file, and can wait on
            // a disk that hangs (a lost NFS server): the exit does not wait
            // for it. It goes with the process, and with its object, which
            // no one else uses once it is released.
            if (cancelled && !file->m_done.load()) {
                file->m_thread.detach();
                // Kept where a leak check sees it, as the process ends.
                static std::vector<DownloadFile*>* const kept = new std::vector<DownloadFile*>;
                kept->push_back(file.release());
            }
        }
    }
};

DownloadFile::Released& DownloadFile::released()
{
    static Released instance;
    return instance;
}

std::unique_ptr<DownloadFile> DownloadFile::create(const QString& path, bool replace, std::function<void()> room,
                                                   Ended ended, QString& error)
{
    const QByteArray target = QFile::encodeName(path);
    for (int n = 0; n < 100; ++n) {
        const QByteArray part = n == 0 ? target + ".part" : target + '.' + QByteArray::number(n) + ".part";
        const int fd = ::open(part.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd >= 0)
            return std::unique_ptr<DownloadFile>(
                new DownloadFile(fd, part, path, replace, std::move(room), std::move(ended)));
        if (errno != EEXIST)
            break;
    }
    error = QString::fromLocal8Bit(std::strerror(errno));
    return nullptr;
}

DownloadFile::DownloadFile(int fd, QByteArray part, QString target, bool replace, std::function<void()> room,
                           Ended ended)
    : m_fd(fd)
    , m_part(std::move(part))
    , m_target(std::move(target))
    , m_replace(replace)
    , m_room(std::move(room))
    , m_ended(std::move(ended))
    , m_thread([this] {
        run();
    })
{
}

DownloadFile::~DownloadFile()
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_finishing)
            m_cancelled = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

void DownloadFile::release(std::unique_ptr<DownloadFile> file)
{
    if (!file)
        return;
    {
        std::lock_guard calls(file->m_callMutex);
        file->m_released = true;
    }
    {
        std::lock_guard lock(file->m_mutex);
        if (!file->m_finishing)
            file->m_cancelled = true;
    }
    file->m_wake.notify_all();
    // Those whose writers returned go after the lock: their joins take no time.
    std::vector<std::unique_ptr<DownloadFile>> done;
    Released& all = released();
    std::lock_guard lock(all.mutex);
    for (auto it = all.files.begin(); it != all.files.end();) {
        if ((*it)->m_done.load()) {
            done.push_back(std::move(*it));
            it = all.files.erase(it);
        } else {
            ++it;
        }
    }
    all.files.push_back(std::move(file));
}

bool DownloadFile::write(std::string_view bytes)
{
    if (bytes.empty())
        return true;
    {
        std::lock_guard lock(m_mutex);
        if (m_failed)
            return false;
        m_queue.emplace_back(bytes);
        m_queued += bytes.size();
    }
    m_wake.notify_all();
    return true;
}

bool DownloadFile::pauseIfFull(const std::function<void()>& pause)
{
    std::lock_guard lock(m_mutex);
    if (m_queued <= MaxQueued || m_failed)
        return false;
    pause();
    m_paused = true;
    return true;
}

bool DownloadFile::hasRoomLocked() const
{
    // Half empty before the producer goes on: it does not pause again at
    // each chunk.
    return m_paused && (m_queued <= MaxQueued / 2 || m_failed);
}

void DownloadFile::finish(bool complete, const QString& reason)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_finishing)
            return;
        m_finishing = true;
        m_complete = complete;
        m_reason = reason;
    }
    m_wake.notify_all();
}

void DownloadFile::run()
{
    struct Done
    {
        std::atomic_bool& done;
        ~Done()
        {
            done.store(true);
        }
    } done{m_done};
    for (;;) {
        std::string chunk;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] {
                return m_cancelled || !m_queue.empty() || m_finishing;
            });
            if (m_cancelled || m_queue.empty())
                break;
            chunk = std::move(m_queue.front());
            m_queue.pop_front();
        }
        bool failed = false;
        QString error;
        {
            std::lock_guard lock(m_mutex);
            failed = m_failed;
        }
        // After a failure the rest goes nowhere: the producer hears of it at
        // its next write.
        for (std::string_view rest = chunk; !failed && !rest.empty();) {
            const ssize_t n = ::write(m_fd, rest.data(), rest.size());
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                failed = true;
                error = QString::fromLocal8Bit(std::strerror(n < 0 ? errno : ENOSPC));
                break;
            }
            rest.remove_prefix(std::size_t(n));
        }
        bool room = false;
        {
            std::lock_guard lock(m_mutex);
            m_queued -= chunk.size();
            if (failed && !m_failed) {
                m_failed = true;
                m_error = error;
            }
            room = hasRoomLocked();
        }
        if (room) {
            // The look again, and room() with it, under the queue's lock,
            // which pauseIfFull() holds for its look and its pause: a
            // producer that paused again since keeps its pause.
            std::lock_guard calls(m_callMutex);
            std::lock_guard lock(m_mutex);
            if (hasRoomLocked()) {
                m_paused = false;
                if (!m_released && m_room)
                    m_room();
            }
        }
    }
    bool cancelled = false;
    bool ok = false;
    QString error;
    {
        std::lock_guard lock(m_mutex);
        cancelled = m_cancelled;
        ok = m_complete && !m_failed;
        error = m_failed ? m_error : m_reason;
        m_queue.clear();
        m_queued = 0;
    }
    QString saved = m_target;
    if (cancelled || !ok) {
        ::close(m_fd);
        ::unlink(m_part.constData());
        // A cancel comes from the destructor, whose owner has stopped its
        // producer: no one waits for the end.
        if (cancelled)
            return;
    } else if (::close(m_fd) != 0) {
        // A close that fails may have lost the end of the file.
        ok = false;
        error = QString::fromLocal8Bit(std::strerror(errno));
        ::unlink(m_part.constData());
    } else {
        const QString renamed = !m_replace ? renameToFreeName(m_part, m_target)
            : ::rename(m_part.constData(), QFile::encodeName(m_target).constData()) == 0 ? m_target
                                                                                         : QString();
        if (!renamed.isEmpty()) {
            saved = renamed;
        } else {
            // The whole download is in the part file: it stays, and the
            // error names it.
            const QString reason = QString::fromLocal8Bit(std::strerror(errno));
            ok = false;
            error = QStringLiteral("%1. The download is kept in %2").arg(reason, QFile::decodeName(m_part));
        }
    }
    std::lock_guard calls(m_callMutex);
    if (!m_released && m_ended)
        m_ended(saved, ok, error);
}

} // namespace Afterglow
