// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FileTransfer.h"
#include "ProtoBytes.h"
#include <AfterglowProto/Delta.h>
#include <AfterglowProto/Fd.h>
#include <AfterglowProto/Transfer.h>
#include <AfterglowProto/Zlib.h>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMutex>
#include <QScopeGuard>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <deque>
#include <fcntl.h>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace Afterglow {
namespace {
    Q_LOGGING_CATEGORY(lcFileTransfer, "afterglow.desktop", QtInfoMsg)
    using Fields = QMap<QByteArray, QByteArray>;
    using ProtoBytes::bytesOf;
    using ProtoBytes::view;
    namespace Status = AfterglowProto::TransferStatus;
    // Afterglow's limits on what the transfers of one pane can make it hold.
    // The protocol sets none, and a client has no way to change them.
    constexpr qsizetype MaxQueuedBytes = 16 * 1024 * 1024; // each of the input and output queues
    constexpr qsizetype MaxCommand = qsizetype(AfterglowProto::TransferMaxCommand);
    constexpr qsizetype MaxPacketData = qsizetype(AfterglowProto::TransferMaxData);
    constexpr int MaxFiles = 16384; // per session
    constexpr std::size_t MaxSessions = 8;
    constexpr std::size_t MaxActiveFiles = 128; // regular files and rsync reads in progress
    constexpr std::size_t MaxStreams = 128; // outgoing file streams
    constexpr int MaxDepth = 128;
    // The requested paths of all receive sessions, and the commands that
    // send sessions hold for files that wait for the user. A sender names
    // all of its files at the start: 16,384 of them fit with paths of 700 bytes.
    constexpr qsizetype MaxRequestBytes = 16 * 1024 * 1024;
    constexpr qsizetype MaxRsyncBuffer = 16 * 1024 * 1024; // one signature or one literal
    // Bytes after which one pump() stops scanning a delta. A block hashed for
    // a weak match counts as its length, so this bounds the hashing too. A
    // refill before that check reads up to two blocks (2 MiB at most) more.
    constexpr qsizetype DeltaStep = 1024 * 1024;
    constexpr std::size_t MaxPendingBytes = 64 * 1024 * 1024; // every rsync buffer of the pane
    constexpr qsizetype DeliveryWindow = 65536;
    constexpr std::size_t MaxInputBacklog = 1024 * 1024;
    constexpr qsizetype ListingHeadroom = 64 * 1024; // statuses queued behind a listing
    // A No, or a dialog that the program took away, keeps the next dialog
    // away this long, so that a program cannot ask again and again. The
    // dialog lists at most MaxPromptPaths paths, which a user can read: a
    // Yes approves those. Both bound what a program can do to the user, as
    // the limits above bound what it can make the pane hold.
    constexpr int DenialCooldownMs = 10000;
    constexpr qsizetype MaxPromptPaths = 100;
    // A delta upload tells the program the hashes of the blocks of the file
    // that it replaces. A file this small gains nothing from a delta, and
    // its hash could give away a short secret: it is replaced whole.
    constexpr qint64 MinDeltaBasis = 4096;
    // A link's data: "path:" and a target of PATH_MAX - 1 bytes, which is
    // more than one packet holds.
    constexpr qsizetype MaxLinkData = 5 + 4095;
    // The PTY gives no signal when its input queue drains, so output that a
    // slow reader holds back is retried at this interval.
    constexpr int BacklogPollMs = 10;
    // Descriptors that the transfers of every pane hold. The limits of one
    // pane do not bound a window of many panes, and the process shares
    // RLIMIT_NOFILE with its sockets, fonts and PTYs.
    std::atomic<std::size_t> g_descriptors { 0 };
    // Until when no pane checks a transfer password, after a wrong one, on the monotonic clock.
    std::atomic<qint64> g_passwordsBlockedUntil { 0 };
    qint64 monotonicMs()
    {
        QElapsedTimer now;
        now.start();
        return now.msecsSinceReference();
    }
    std::size_t descriptorLimit()
    {
        // Transfers take at most half of the limit.
        static const std::size_t limit = [] {
            rlimit nofile { };
            if (::getrlimit(RLIMIT_NOFILE, &nofile) != 0 || nofile.rlim_cur == RLIM_INFINITY)
                return std::size_t(4096);
            return std::size_t(std::max<rlim_t>(nofile.rlim_cur / 2, 16));
        }();
        return limit;
    }
    // Regular files and rsync reads in progress: one pane's count, and every pane's.
    struct Descriptors {
        std::size_t pane = 0;
        // @p streams: descriptors of outgoing streams, which count against every
        // pane's limit and not against this pane's.
        bool full(std::size_t more, std::size_t streams = 0) const
        {
            return pane + more > MaxActiveFiles || g_descriptors.load() + more + streams > descriptorLimit();
        }
        void add(std::size_t count)
        {
            pane += count;
            g_descriptors.fetch_add(count);
        }
        void remove(std::size_t count)
        {
            pane -= count;
            g_descriptors.fetch_sub(count);
        }
    };
    bool safe(const QByteArray& s) { return AfterglowProto::transferSafeId(view(s)); }
    std::optional<Fields> decode(const QByteArray& raw)
    {
        const auto decoded = AfterglowProto::decodeTransfer(view(raw));
        if (!decoded)
            return std::nullopt;
        Fields f;
        for (const auto& [key, value] : *decoded)
            f.insert(bytesOf(key), bytesOf(value));
        return f;
    }
    // The session id of a cancel request, read without decoding the base64 fields.
    QByteArray cancelId(const QByteArray& raw) { return bytesOf(AfterglowProto::transferCancelId(view(raw))); }
    // Later code reads only these fields. The data, password and status fields can be large.
    Fields metadata(const Fields& f)
    {
        Fields kept;
        for (const char* key : { "ft", "sz", "prm", "mod", "zip", "tt" })
            if (f.contains(key))
                kept[key] = f.value(key);
        return kept;
    }
    QByteArray encode(const Fields& fields)
    {
        // Views of the map's bytes, in its (sorted) order: the values are not copied before encoding.
        std::vector<std::pair<std::string_view, std::string_view>> f;
        f.reserve(std::size_t(fields.size()));
        for (auto it = fields.begin(); it != fields.end(); ++it)
            f.emplace_back(view(it.key()), view(it.value()));
        return bytesOf(AfterglowProto::encodeTransfer(f));
    }
    QString pathOf(const QByteArray& value)
    {
        if (value.isEmpty() || value.size() > 4096 || value.contains('\0'))
            return { };
        QString path = QString::fromUtf8(value);
        if (path.toUtf8() != value)
            return { };
        if (path == QLatin1String("~"))
            path = QDir::homePath();
        else if (path.startsWith(QLatin1String("~/")))
            path = QDir::homePath() + path.mid(1);
        // Not QDir::isAbsolutePath, nor QDir::filePath: both take ":name"
        // for a Qt resource.
        if (!path.startsWith(QLatin1Char('/')))
            path = QDir::homePath() + QLatin1Char('/') + path;
        for (const auto& part : path.split(QLatin1Char('/')))
            if (part.toUtf8().size() > 255)
                return { };
        return QDir::cleanPath(path);
    }
    std::string_view errorName() { return AfterglowProto::transferErrorName(errno); }
    // A path as the permission dialog shows it: a control character, a line
    // separator, a bidirectional control or another invisible format
    // character (a zero-width space, a tag) in a name could hide what follows.
    QString shown(const QString& path)
    {
        QString out;
        out.reserve(path.size());
        for (const char32_t u : path.toUcs4()) {
            const auto category = QChar::category(u);
            const bool hidden = category == QChar::Other_Control || category == QChar::Other_Format
                || category == QChar::Separator_Line || category == QChar::Separator_Paragraph;
            if (u > 0xffff && hidden)
                out += QStringLiteral("\\U%1").arg(uint(u), 8, 16, QLatin1Char('0'));
            else if (hidden || u == U'\\')
                out += QStringLiteral("\\u%1").arg(uint(u), 4, 16, QLatin1Char('0'));
            else
                out += QString::fromUcs4(&u, 1);
        }
        return out;
    }
    // A delta's file, read as the generator asks for it.
    class FileSource : public AfterglowProto::ByteSource {
    public:
        explicit FileSource(QFile& file)
            : m_file(file)
        {
        }
        bool read(std::string& out, std::size_t max) override
        {
            const std::size_t old = out.size();
            out.resize(old + max);
            const qint64 n = m_file.read(out.data() + old, qint64(max));
            out.resize(old + std::size_t(std::max<qint64>(n, 0)));
            // Sources are regular files, where an empty read is the end.
            // QFile::atEnd() compares the position with st_size, which is 0
            // for a file in /proc that holds data: the delta would rebuild an
            // empty file.
            if (n == 0)
                m_end = true;
            return n >= 0;
        }
        bool atEnd() const override { return m_end; }

    private:
        QFile& m_file;
        bool m_end = false;
    };
    // The umask, read without changing it. The process shares one umask, and
    // code that sets one for a moment (a socket that binds with mode 0600)
    // runs on the GUI thread: read it there, not on a worker.
    mode_t currentUmask()
    {
        QFile status(QStringLiteral("/proc/self/status"));
        if (status.open(QIODevice::ReadOnly))
            for (const auto& line : status.readAll().split('\n'))
                if (line.startsWith("Umask:")) {
                    bool ok = false;
                    const auto value = line.mid(6).trimmed().toUInt(&ok, 8);
                    if (ok)
                        return mode_t(value & 0777);
                }
        return mode_t(022);
    }
    QString joined(const QString& directory, const QString& name)
    {
        return directory == QLatin1String("/") ? directory + name : directory + QLatin1Char('/') + name;
    }
    // Where the user approved a file to go or to come from: the real path,
    // when the dialog showed, of the deepest directory above it that
    // existed, and the levels below that one that did not exist yet. A link
    // of the user's own (a ~/Downloads on another disk) is part of the real
    // path, and the dialog shows where it leads. A missing level must become
    // a directory that the transfer makes: a link there, which the same
    // program could make after the dialog, could lead anywhere.
    struct Place {
        QString real;
        QStringList missing;
        QString path() const
        {
            QString out = real;
            for (const auto& level : missing)
                out = joined(out, level);
            return out;
        }
    };
    Place placeOf(const QString& directory)
    {
        Place place;
        for (QString at = directory;;) {
            place.real = QFileInfo(at).canonicalFilePath();
            const QFileInfo info(at);
            if (!place.real.isEmpty() || info.absolutePath() == at)
                return place;
            place.missing.prepend(info.fileName());
            at = info.absolutePath();
        }
    }
    // The path that /proc gives for an open descriptor. Empty when it gives none.
    QByteArray descriptorPath(int fd)
    {
        char buffer[4096 + 64];
        const auto n = ::readlink(("/proc/self/fd/" + QByteArray::number(fd)).constData(), buffer, sizeof buffer);
        return n > 0 && std::size_t(n) < sizeof buffer ? QByteArray(buffer, n) : QByteArray();
    }
    // Opens @p real with @p flags, and checks that no link led elsewhere on
    // the way: the descriptor is at that path. Sets errno and returns -1 when
    // it fails, with EPERM for a path that leads elsewhere now.
    int openChecked(const QString& real, int flags)
    {
        const auto path = QFile::encodeName(real);
        const int fd = ::open(path.constData(), flags | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 || descriptorPath(fd) == path)
            return fd;
        ::close(fd);
        errno = EPERM;
        return -1;
    }
    // The directory of @p place, checked. Each missing level is opened
    // without following a link, and made first when @p made is given: the
    // paths of the directories made here go there, with the mode that a new
    // directory gets under @p mask. Sets errno and returns -1 when it fails.
    int openPlace(const Place& place, mode_t mask, std::vector<QString>* made)
    {
        if (place.real.isEmpty()) {
            errno = ENOENT;
            return -1;
        }
        AfterglowProto::UniqueFd fd(openChecked(place.real, O_RDONLY | O_DIRECTORY));
        QString path = place.real;
        for (const auto& level : place.missing) {
            if (!fd.valid())
                return -1;
            const auto name = QFile::encodeName(level);
            path = joined(path, level);
            // 0700 until the mode below: the umask of the moment does not decide it.
            const bool make = made && ::mkdirat(fd.get(), name.constData(), 0700) == 0;
            if (made && !make && errno != EEXIST)
                return -1;
            fd.reset(::openat(fd.get(), name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (make) {
                made->push_back(path);
                if (fd.valid() && ::fchmod(fd.get(), 0777 & ~mask))
                    return -1;
            }
        }
        return fd.release();
    }
    // The modification time of a client's fields. Times before 1970 are
    // negative. Only -1 means "not given".
    std::optional<timespec> modificationOf(const Fields& f)
    {
        bool ok = false;
        const auto ns = f.value("mod").toLongLong(&ok);
        if (!ok || ns == -1)
            return std::nullopt;
        timespec time { time_t(ns / 1000000000), long(ns % 1000000000) };
        if (time.tv_nsec < 0) {
            time.tv_nsec += 1000000000;
            --time.tv_sec;
        }
        return time;
    }
    // Sets the mode and time of a file that this session made, on its
    // descriptor: nothing goes through a link. The permission bits only: a
    // program on another computer cannot make a set-user-ID, set-group-ID or
    // sticky file here. kitty keeps those bits. Without a mode from the
    // client, a file gets the mode that a new file would get under @p umask.
    bool setMetadata(int fd, const Fields& f, std::optional<mode_t> umask)
    {
        bool ok = false;
        const auto mode = f.value("prm").toLongLong(&ok);
        if (ok && mode >= 0) {
            if (::fchmod(fd, mode_t(mode & 0777)))
                return false;
        } else if (umask && ::fchmod(fd, 0666 & ~*umask))
            return false;
        if (const auto time = modificationOf(f)) {
            const timespec times[2] { { 0, UTIME_OMIT }, *time };
            if (::futimens(fd, times))
                return false;
        }
        return true;
    }
    // A directory that the session made, at the real path where it made it:
    // no link leads to another one.
    bool setDirectoryMetadata(const QString& real, const Fields& f)
    {
        const AfterglowProto::UniqueFd fd(openChecked(real, O_RDONLY | O_DIRECTORY));
        return fd.valid() && setMetadata(fd.get(), f, std::nullopt);
    }
    // A symbolic link has no mode of its own on Linux: its time only.
    bool setLinkTime(int directory, const QByteArray& name, const Fields& f)
    {
        const auto time = modificationOf(f);
        if (!time)
            return true;
        const timespec times[2] { { 0, UTIME_OMIT }, *time };
        return ::utimensat(directory, name.constData(), times, AT_SYMLINK_NOFOLLOW) == 0;
    }
    bool isLink(const Fields& meta) { return meta.value("ft") == "symlink" || meta.value("ft") == "link"; }
} // namespace

struct FileTransfer::Worker {
    // A regular file's temporary file, in the checked directory of its
    // destination. The descriptor of that directory stays open: the file
    // replaces its destination there, wherever a link leads meanwhile.
    struct Output {
        AfterglowProto::UniqueFd directory;
        QByteArray name;
        QFile file;
        bool keep = false;
        ~Output()
        {
            file.close();
            if (!keep && directory.valid())
                ::unlinkat(directory.get(), name.constData(), 0);
        }
    };
    struct File {
        Fields meta;
        QString path;
        // The destination's directory, and its name there.
        Place place;
        QByteArray name;
        std::unique_ptr<Output> output;
        std::unique_ptr<QFile> basis;
        QByteArray linkData;
        // A delta upload's operations that are not complete yet, or a delta read's signature.
        std::string pending;
        qint64 written = 0;
        bool committed = false, complete = false, failed = false, rsync = false, ended = false;
        bool overBudget = false, tooLarge = false;
        std::unique_ptr<AfterglowProto::ZlibStream> inflater;
        // Applies the operations of a delta upload, and checks the file hash.
        std::unique_ptr<AfterglowProto::DeltaApplier> applier;
        // Descriptors this file holds against the limits: its output and its
        // directory, and a delta basis.
        Descriptors* activeCount = nullptr;
        std::size_t held = 0;
        std::size_t* pendingTotal;
        quint32 blockSize = 0;
        // A directory that was there before the session: it keeps its mode and time.
        bool existed = false;
        File(Descriptors* count, std::size_t* pendingBytes)
            : pendingTotal(pendingBytes)
        {
            if (count)
                charge(count, 1);
        }
        ~File() { releaseIo(); }
        void charge(Descriptors* count, std::size_t descriptors)
        {
            activeCount = count;
            count->add(descriptors);
            held += descriptors;
        }
        bool addPending(std::string_view bytes)
        {
            if (*pendingTotal + bytes.size() > MaxPendingBytes) {
                overBudget = true;
                return false;
            }
            pending += bytes;
            *pendingTotal += bytes.size();
            return true;
        }
        // What stays is less than one operation, so the move is short.
        void dropPending(std::size_t count)
        {
            pending.erase(0, count);
            *pendingTotal -= count;
        }
        void clearPending()
        {
            *pendingTotal -= pending.size();
            std::string().swap(pending);
        }
        void releaseIo()
        {
            output.reset();
            basis.reset();
            clearPending();
            inflater.reset();
            // A finished file stays in the session until it ends. Free the hash state now.
            applier.reset();
            if (activeCount) {
                activeCount->remove(held);
                activeCount = nullptr;
                held = 0;
            }
        }
    };
    struct Request {
        QByteArray fid;
        QString path;
        // The directory of the path, from the dialog that showed it.
        std::optional<Place> place;
    };
    struct Session {
        QByteArray id;
        QString token;
        bool sending = false, approved = false, asking = false;
        // The client sent ac=finish and reads no more: what goes wrong now is
        // logged in one line for the session. Each error counts its files
        // and keeps the first one; the errors are a short fixed set.
        bool finishing = false;
        std::map<QByteArray, std::pair<int, QByteArray>> finishFailures;
        int quiet = 0, expected = 0;
        // Requests received so far. list() frees the requests themselves, so
        // their count cannot say whether the session still collects them.
        int requested = 0;
        std::vector<Request> requests;
        qsizetype requestBytes = 0;
        // A send session without the password: the sender names the
        // destinations only after the session starts. Each file waits, with
        // the commands that name it, until the user approves its
        // destination. The commands of other files go on. heldOrder keeps
        // the waiting files in the order they came. The dialog that shows
        // lists the first `asked` of them, and only those: a file that came
        // later waits for a dialog of its own.
        bool holding = false;
        std::deque<QByteArray> heldOrder;
        std::map<QByteArray, std::vector<Fields>> held;
        // A finish that came while files waited. It runs when none waits.
        std::optional<Fields> heldFinish;
        // Where the destinations that a dialog showed are, by file.
        std::map<QByteArray, Place> places;
        // The files of the dialog that shows, or the requests that it lists.
        std::size_t asked = 0;
        // The session waits for a dialog, which opens when no other shows,
        // once the fence askAfter has come back from the GUI thread.
        bool waiting = false;
        quint64 askAfter = 0;
        // The user approved files of this session before: a later No
        // refuses the files of its own dialog, and the others finish.
        bool granted = false;
        // A later No left files that came while its dialog showed. They get
        // a dialog of their own: they did not ask again after the No.
        bool afterNo = false;
        std::map<QByteArray, std::unique_ptr<File>> files;
        // Links whose target file has not reached its final state, keyed by the target.
        std::multimap<QByteArray, QByteArray> waiters;
        // What a receive session lists, by the path that the client names.
        // The file is read at its real path, which the dialog showed.
        struct Listed {
            Fields fields;
            QString real;
        };
        std::map<QString, Listed> listing;
        // The real paths of the directories that the session made.
        std::vector<QString> createdDirs;
        QSet<QString> created; // the same paths, for lookups
    };
    // The delta of a file against the receiver's signature, built as pump()
    // asks for data: a large file needs no copy on disk, and the worker,
    // which serves every session of the pane, handles uploads and messages
    // between steps. Outgoing streams are one queue: the next file waits.
    struct Delta {
        std::unique_ptr<QFile> basis;
        FileSource source;
        std::size_t* pendingTotal;
        std::size_t charged;
        // The generator reads the signature into its tables only when the
        // stream reaches the front, so that the streams queued behind it
        // hold no tables. The signature stays charged to the pane's rsync
        // budget until the stream ends.
        AfterglowProto::DeltaGenerator generator;
        Delta(std::unique_ptr<QFile> file, std::string signature, std::size_t* total)
            : basis(std::move(file))
            , source(*basis)
            , pendingTotal(total)
            , charged(signature.size())
            , generator(std::move(signature))
        {
        }
        ~Delta() { *pendingTotal -= charged; }
        Delta(const Delta&) = delete;
        Delta& operator=(const Delta&) = delete;
    };
    // The signature of a delta upload's old file, made as pump() asks for
    // data: one step hashes a bounded part of the file, so that a large one
    // does not hold the worker. It reads at offsets, on a descriptor of its
    // own, while the upload reads the same file for its copy operations.
    struct Signature {
        AfterglowProto::UniqueFd basis;
        AfterglowProto::SignatureBuilder builder;
        quint64 size, offset = 0;
        bool started = false;
        bool done() const { return started && offset >= size; }
    };
    // Each stream holds one descriptor: its source, its delta's basis, or
    // the old file of its signature.
    struct Stream {
        Stream() { g_descriptors.fetch_add(1); }
        ~Stream() { g_descriptors.fetch_sub(1); }
        Stream(const Stream&) = delete;
        Stream& operator=(const Stream&) = delete;
        Fields fields;
        std::unique_ptr<QFile> source;
        std::unique_ptr<Delta> delta; // in place of the source
        std::unique_ptr<Signature> signature; // in place of the source
        // Delta operations or signature records. The first generatedSent bytes went out already.
        std::string generated;
        std::size_t generatedSent = 0;
        QByteArray input;
        std::unique_ptr<AfterglowProto::ZlibStream> deflater;
        bool eof = false;
    };
    // What long work checks to stop early: a reset, or a cancel of its own session.
    struct Watch {
        QByteArray id;
        quint64 reset, cancel;
    };
    Descriptors activeFiles;
    std::size_t pendingBytes = 0;
    qsizetype requestBytes = 0;
    std::deque<std::unique_ptr<Stream>> streams;
    FileTransfer* owner;
    QThread thread;
    QObject* context = new QObject;
    std::atomic<qsizetype> queuedBytes { 0 };
    // The fences that the worker sent through the GUI thread, and the last
    // one that came back.
    quint64 fencesSent = 0, fencesPassed = 0;
    std::atomic<qsizetype> deliveryBytes { 0 };
    // Incremented on the GUI thread by reset() and abort().
    std::atomic<quint64> resetEpoch { 0 };
    // The reset that the worker has applied. Packets older than a reset are stale.
    quint64 clearedEpoch = 0;
    std::atomic<quint64> cancelEpoch { 0 };
    QMutex cancelMutex;
    std::map<QByteArray, int> cancelRequests;
    std::atomic<bool> inputBlocked { false };
    std::function<std::size_t()> backlog; // GUI thread
    std::atomic<std::size_t> backlogBytes { 0 };
    std::atomic<bool> backlogQueried { false };
    std::map<QByteArray, std::unique_ptr<Session>> sessions;
    QString password;
    qint64 deltaLimit = DefaultDeltaLimit;
    QElapsedTimer denied;
    // Read on the GUI thread, which constructs the worker.
    const mode_t creationMask = currentUmask();
    // Outgoing packets are paced. Keep at most a bounded amount queued.
    // Each keeps the fields that a cancel and a newer progress status look for.
    struct Packet {
        QByteArray bytes, id, fid;
        bool progress = false;
        // The position of the packet in what the worker queued. It rises
        // along the queue: every removal keeps the order of the rest.
        quint64 seq = 0;
    };
    std::deque<Packet> outgoing;
    qsizetype outgoingBytes = 0;
    // The position of the last packet queued. For each handled command that
    // waits for answered(), the position of the last packet that anything
    // queued before the command was handled. A fence waits until every packet
    // up to it has left the queue, delivered or dropped.
    quint64 queuedPackets = 0;
    std::deque<quint64> fences;
    QTimer* timer = nullptr;
    bool overflow = false;
    explicit Worker(FileTransfer* o)
        : owner(o)
    {
        context->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, context, &QObject::deleteLater);
        thread.start();
        QMetaObject::invokeMethod(context, [this] {
            timer = new QTimer(context);
            QObject::connect(timer, &QTimer::timeout, context, [this] { deliver(); });
        });
    }
    ~Worker()
    {
        thread.requestInterruption();
        QMetaObject::invokeMethod(
            context,
            [this] {
                while (!sessions.empty())
                    remove(sessions.begin()->first);
            },
            Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
    }
    void deliver()
    {
        // A reset is on its way. Everything queued before it is stale.
        if (resetEpoch.load() != clearedEpoch)
            return;
        // Fill the delivery window, not one packet for each tick. Each pump()
        // is bounded, and a reset or the pane's close stops the loop between them.
        for (int i = 0; i < 64 && !streams.empty() && outgoingBytes < DeliveryWindow; ++i) {
            if (thread.isInterruptionRequested() || resetEpoch.load() != clearedEpoch)
                return;
            pump();
        }
        while (!outgoing.empty() && deliveryBytes.load() < DeliveryWindow && backlogBytes.load() < MaxInputBacklog) {
            auto bytes = std::move(outgoing.front().bytes);
            outgoing.pop_front();
            outgoingBytes -= bytes.size();
            deliveryBytes.fetch_add(bytes.size());
            QMetaObject::invokeMethod(owner, [this, bytes, epoch = clearedEpoch] {
                deliveryBytes.fetch_sub(bytes.size());
                if (resetEpoch.load() == epoch) {
                    Q_EMIT owner->reply(bytes);
                    if (backlog)
                        backlogBytes.store(backlog());
                }
                wake();
            });
            // Between the packets: a reply of a later command does not
            // come before the answer of an earlier one.
            answer();
        }
        answer();
        // A slow reader (SSH) leaves input waiting in the PTY. Ask again until it drains.
        if (!outgoing.empty() && backlogBytes.load() >= MaxInputBacklog && !backlogQueried.exchange(true))
            QMetaObject::invokeMethod(owner, [this] {
                backlogBytes.store(backlog ? backlog() : 0);
                backlogQueried.store(false);
            });
        // Output that waits for the window waits for a delivery to finish,
        // which wakes the timer. Ticking meanwhile would only spin.
        if (outgoing.empty() ? streams.empty() : deliveryBytes.load() >= DeliveryWindow)
            timer->stop();
        else if (!outgoing.empty() && backlogBytes.load() >= MaxInputBacklog)
            timer->start(BacklogPollMs);
        else if (timer->interval() != 1)
            timer->start(1);
    }
    // answered() for the commands whose packets left the queue: delivered,
    // with their reply() posted to the owner before this, or dropped. Call it
    // after each removal from the queue.
    void answer()
    {
        const quint64 done = outgoing.empty() ? queuedPackets : outgoing.front().seq - 1;
        while (!fences.empty() && fences.front() <= done) {
            fences.pop_front();
            QMetaObject::invokeMethod(owner, [o = owner] { Q_EMIT o->answered(); });
        }
    }
    // Drops what waits for delivery. The commands that wait for it are answered.
    void clearOutgoing()
    {
        outgoing.clear();
        outgoingBytes = 0;
        answer();
    }
    // From any thread: deliver() runs again soon.
    void wake()
    {
        QMetaObject::invokeMethod(context, [this] {
            if (timer && (!timer->isActive() || timer->interval() != 1))
                timer->start(1);
        });
    }
    // From the reset that the worker applied, not the latest one: the work
    // of a packet that was queued before a reset sees that reset.
    Watch watch(const QByteArray& id) const { return { id, clearedEpoch, ~quint64(0) }; }
    bool stopped(Watch& w)
    {
        if (thread.isInterruptionRequested() || resetEpoch.load() != w.reset)
            return true;
        const auto cancel = cancelEpoch.load();
        if (cancel == w.cancel)
            return false;
        w.cancel = cancel;
        QMutexLocker lock(&cancelMutex);
        return cancelRequests.contains(w.id);
    }
    void forgetCancel(const QByteArray& id)
    {
        QMutexLocker lock(&cancelMutex);
        if (auto it = cancelRequests.find(id); it != cancelRequests.end() && --it->second <= 0)
            cancelRequests.erase(it);
    }
    void send(Fields f)
    {
        if (overflow)
            return;
        const auto bytes = encode(f);
        if (outgoingBytes + bytes.size() > MaxQueuedBytes) {
            overflow = true;
            // Queued, never direct: callers still hold the session that this removes.
            // A reset or an abort that runs first ends the same sessions and
            // clears the flag. The sessions after it did not overflow.
            QMetaObject::invokeMethod(
                context,
                [this, epoch = clearedEpoch] {
                    if (clearedEpoch != epoch)
                        return;
                    clearOutgoing();
                    overflow = false;
                    for (auto& [id, session] : sessions)
                        status(*session, { }, "ENOMEM");
                    while (!sessions.empty())
                        remove(sessions.begin()->first);
                },
                Qt::QueuedConnection);
            return;
        }
        Packet packet { bytes, f.value("id"), f.value("fid"), view(f.value("st")) == Status::Progress, 0 };
        // A newer progress status drops the one that waits. It takes a new
        // position: a command whose fence covered the dropped one is not held
        // until the newer status, which can belong to a later command, goes out.
        if (packet.progress && !outgoing.empty() && outgoing.back().progress && outgoing.back().id == packet.id
            && outgoing.back().fid == packet.fid) {
            outgoingBytes -= outgoing.back().bytes.size();
            outgoing.pop_back();
        }
        packet.seq = ++queuedPackets;
        outgoingBytes += bytes.size();
        outgoing.push_back(std::move(packet));
        if (timer && !timer->isActive())
            timer->start(1);
    }
    void status(Session& s, const QByteArray& file, std::string_view status, qint64 size = -1, Fields extra = { })
    {
        const bool success = AfterglowProto::transferSuccess(status);
        if (s.finishing) {
            // Written to the PTY now, it would reach the shell as typed input.
            if (!success) {
                auto& [count, first] = s.finishFailures[bytesOf(status)];
                if (count++ == 0) {
                    const auto known = s.files.find(file);
                    first = known != s.files.end() && !known->second->path.isEmpty()
                        ? shown(known->second->path).toUtf8() : file;
                }
            }
            return;
        }
        if (s.quiet == 2 || (s.quiet == 1 && success))
            return;
        extra["ac"] = "status";
        extra["id"] = s.id;
        extra["st"] = bytesOf(status);
        if (!file.isEmpty())
            extra["fid"] = file;
        if (size >= 0)
            extra["sz"] = QByteArray::number(size);
        send(extra);
    }
    bool prompting() const
    {
        return std::any_of(sessions.begin(), sessions.end(), [](const auto& item) { return item.second->asking; });
    }
    bool coolingDown() const { return denied.isValid() && denied.elapsed() < DenialCooldownMs; }
    // A reset or an overflow is on its way, or the pane closes: it ends the
    // sessions, and nothing that they wait for may reach the disk.
    bool interrupted() const
    {
        return overflow || resetEpoch.load() != clearedEpoch || thread.isInterruptionRequested();
    }
    // What a held command costs against MaxRequestBytes.
    static qsizetype heldCost(const Fields& f)
    {
        return std::accumulate(f.begin(), f.end(), qsizetype(64),
            [](qsizetype sum, const QByteArray& value) { return sum + value.size() + 16; });
    }
    // The session waits for a dialog, which lists what came before now. The
    // pane's reader posts each command to the GUI thread, which hands it to
    // handle(). A fence posted to the GUI thread now runs after every
    // command that the reader had posted by then, and comes back to the
    // worker after the commands that those handed over.
    void wantDialog(Session& s)
    {
        s.waiting = true;
        s.askAfter = ++fencesSent;
        QMetaObject::invokeMethod(owner, [this, fence = s.askAfter] {
            QMetaObject::invokeMethod(context, [this, fence] {
                fencesPassed = std::max(fencesPassed, fence);
                askWaiting();
            });
        });
    }
    // Opens the dialog of a session that waits for one, once its fence has
    // come back: the dialog lists every file that the program had named
    // when the session began to wait. One dialog at a time, so that a
    // program cannot stack them over the window.
    void askWaiting()
    {
        std::vector<QByteArray> ids;
        for (const auto& [id, s] : sessions)
            if (s->waiting && fencesPassed >= s->askAfter)
                ids.push_back(id);
        for (const auto& id : ids) {
            if (prompting() || interrupted())
                return;
            if (const auto it = sessions.find(id); it != sessions.end() && it->second->waiting)
                ask(*it->second);
        }
    }
    // A path as the dialog lists it: a slash after a directory, which is
    // read whole, and a line under it with where links lead it, when that is
    // elsewhere. A name cannot hold a line break (shown() escapes it), so a
    // name cannot make such a line. A home directory that is a link (/home
    // on another disk) is the same place for every path under it: that
    // link alone gets no line.
    static QString described(const QString& path, const QString& real, bool directory)
    {
        const auto slash = [directory](const QString& p) { return directory && !p.endsWith(QLatin1Char('/')) ? p + QLatin1Char('/') : p; };
        const QString home = QDir::homePath(), realHome = QFileInfo(home).canonicalFilePath();
        const bool underHome = path.startsWith(home + QLatin1Char('/')) && !realHome.isEmpty();
        if (real == path || (underHome && real == realHome + path.mid(home.size())))
            return slash(shown(path));
        return slash(shown(path)) + QLatin1Char('\n') + FileTransfer::tr("  resolves to %1").arg(slash(shown(real)));
    }
    // The dialog of @p s. It lists what waits, at most MaxPromptPaths paths:
    // the files that a receive session asks for, or the destinations of the
    // files of a send session. A Yes approves the ones listed.
    void ask(Session& s)
    {
        s.waiting = false;
        const bool exempt = std::exchange(s.afterNo, false);
        if (coolingDown() && !exempt) {
            // A send session that the user approved before goes on with its files.
            if (s.granted)
                refuseHeld(s, s.heldOrder.size(), "EPERM");
            else {
                status(s, { }, "EPERM");
                remove(QByteArray(s.id));
            }
            return;
        }
        QStringList paths;
        if (s.holding) {
            s.asked = std::min(s.heldOrder.size(), std::size_t(MaxPromptPaths));
            for (std::size_t i = 0; i < s.asked; ++i) {
                const auto& fid = s.heldOrder[i];
                const QString path = pathOf(s.held.at(fid).front().value("n"));
                const QFileInfo info(path);
                Place place = placeOf(info.absolutePath());
                paths.push_back(described(path, joined(place.path(), info.fileName()), false));
                s.places[fid] = std::move(place);
            }
        } else {
            s.asked = std::min(s.requests.size(), std::size_t(MaxPromptPaths));
            for (std::size_t i = 0; i < s.asked; ++i) {
                auto& request = s.requests[i];
                const QFileInfo info(request.path);
                request.place = placeOf(info.absolutePath());
                const QString real = joined(request.place->path(), info.fileName());
                struct stat st { };
                const bool directory = ::lstat(QFile::encodeName(real).constData(), &st) == 0 && S_ISDIR(st.st_mode);
                paths.push_back(described(request.path, real, directory));
            }
        }
        const int total = int(s.holding ? s.heldOrder.size() : s.requests.size());
        s.asking = true;
        QMetaObject::invokeMethod(owner, [o = owner, token = s.token, writing = s.sending, paths, total] {
            Q_EMIT o->permissionRequested(token, writing, paths, total);
        });
    }
    // Why a file command of a send session cannot be written, or nothing.
    // A held file is checked as it comes: the dialog lists only files that
    // the session can write.
    static std::string_view refusal(const Session& s, const Fields& f)
    {
        const auto fid = f.value("fid");
        if (!safe(fid) || pathOf(f.value("n")).isEmpty() || s.files.contains(fid) || s.held.contains(fid)
            || s.files.size() + s.held.size() >= std::size_t(MaxFiles))
            return "EINVAL";
        const auto type = f.value("ft", "regular");
        if (type != "regular" && type != "directory" && type != "symlink" && type != "link")
            return "EINVAL";
        const auto zip = f.value("zip", "none");
        if (zip != "none" && zip != "zlib")
            return "EINVAL";
        return { };
    }
    // A command of a send session that names a file that waits for the user.
    void hold(Session& s, const Fields& f)
    {
        const auto fid = f.value("fid");
        const bool file = f.value("ac") == "file";
        if (file) {
            if (const auto error = refusal(s, f); !error.empty()) {
                status(s, fid, error);
                return;
            }
        }
        const qsizetype cost = heldCost(f);
        if (requestBytes + cost > MaxRequestBytes) {
            // A file that does not fit is refused, and the others go on.
            // Data for a file that waits ends the session: a client sends
            // none before the file has STARTED.
            if (file)
                status(s, fid, "ENOMEM");
            else {
                status(s, { }, "ENOMEM");
                remove(QByteArray(s.id));
            }
            return;
        }
        requestBytes += cost;
        s.requestBytes += cost;
        s.held[fid].push_back(f);
        if (!file)
            return;
        s.heldOrder.push_back(fid);
        if (!s.asking && !s.waiting)
            wantDialog(s);
    }
    // Forgets the commands that wait with file @p fid.
    void dropHeld(Session& s, const QByteArray& fid)
    {
        const auto it = s.held.find(fid);
        if (it == s.held.end())
            return;
        for (const Fields& f : it->second) {
            const qsizetype cost = heldCost(f);
            requestBytes -= cost;
            s.requestBytes -= cost;
        }
        s.held.erase(it);
        s.places.erase(fid);
    }
    // After a dialog of a send session: the files that came while it showed
    // get one of their own, and a finish that waited runs once none waits.
    void afterHeld(Session& s)
    {
        if (!s.heldOrder.empty())
            wantDialog(s);
        else if (s.heldFinish) {
            const Fields f = *s.heldFinish;
            s.heldFinish.reset();
            dispatch(s, f);
        }
    }
    // Refuses the first @p count waiting files of @p s with @p error. The
    // other files of the session go on.
    void refuseHeld(Session& s, std::size_t count, std::string_view error)
    {
        for (std::size_t i = 0; i < count && !s.heldOrder.empty(); ++i) {
            const QByteArray fid = s.heldOrder.front();
            s.heldOrder.pop_front();
            dropHeld(s, fid);
            status(s, fid, error);
        }
        s.asked = 0;
        afterHeld(s);
    }
    // A Yes for a send session: the files that the dialog listed run, each
    // with the commands that waited for it.
    void grant(const QByteArray& id)
    {
        Session& s = *sessions.at(id);
        s.granted = true;
        const std::vector<QByteArray> approved(s.heldOrder.begin(), s.heldOrder.begin() + qsizetype(s.asked));
        s.heldOrder.erase(s.heldOrder.begin(), s.heldOrder.begin() + qsizetype(s.asked));
        s.asked = 0;
        for (const auto& fid : approved) {
            auto found = sessions.find(id);
            if (found == sessions.end())
                return;
            auto commands = std::move(found->second->held.at(fid));
            for (const Fields& f : commands) {
                const qsizetype cost = heldCost(f);
                requestBytes -= cost;
                found->second->requestBytes -= cost;
            }
            found->second->held.erase(fid);
            for (const Fields& f : commands) {
                // A command can end the session.
                found = sessions.find(id);
                if (found == sessions.end() || interrupted())
                    return;
                dispatch(*found->second, f);
            }
        }
        if (const auto found = sessions.find(id); found != sessions.end())
            afterHeld(*found->second);
    }
    // Takes the id by value: callers pass references to the key that this erases.
    void remove(const QByteArray id)
    {
        auto it = sessions.find(id);
        if (it == sessions.end())
            return;
        // A program that ends its session while the dialog shows, and asks
        // again, would keep a dialog in front of the user: as after a No.
        if (it->second->asking)
            denied.start();
        const auto createdDirs = std::move(it->second->createdDirs);
        std::erase_if(streams, [&](const auto& stream) { return stream->fields.value("id") == id; });
        const auto token = it->second->token;
        requestBytes -= it->second->requestBytes;
        sessions.erase(it);
        for (auto p = createdDirs.rbegin(); p != createdDirs.rend(); ++p)
            QDir().rmdir(*p);
        QMetaObject::invokeMethod(owner, [o = owner, token] { Q_EMIT o->sessionClosed(token); });
    }
    void authorize(const QString& token, bool allowed)
    {
        if (interrupted())
            return;
        const auto it = std::find_if(
            sessions.begin(), sessions.end(), [&](const auto& item) { return item.second->token == token; });
        if (it == sessions.end() || !it->second->asking)
            return;
        const QByteArray id = it->first;
        Session& s = *it->second;
        s.asking = false;
        if (!allowed) {
            denied.start();
            // A later No refuses the files of its own dialog. Those that the
            // user approved before finish, and those that came while it
            // showed get a dialog of their own.
            if (s.granted) {
                s.afterNo = s.heldOrder.size() > s.asked;
                refuseHeld(s, s.asked, "EPERM");
            }
            else {
                status(s, { }, "EPERM");
                remove(id);
            }
        } else if (s.holding)
            grant(id);
        else {
            s.approved = true;
            // The dialog listed the first requests. The program reads none of
            // the others. A client takes nothing but the permission before
            // it, so the refusals come after the OK, as the statuses of the
            // listing do.
            status(s, { }, Status::Ok);
            for (std::size_t i = s.asked; i < s.requests.size(); ++i)
                status(s, s.requests[i].fid, "EPERM");
            s.requests.resize(s.asked);
            list(s);
        }
        askWaiting();
    }
    void fail(Session& s, const QByteArray& fid, std::string_view error)
    {
        s.files.at(fid)->failed = true;
        status(s, fid, error);
    }
    void handle(const QByteArray& raw, const QByteArray& cancelled);
    /// A command of an approved session.
    void dispatch(Session& s, const Fields& f);
    void cancel(const QByteArray& id);
    void list(Session& s);
    void receiveFile(Session& s, const Fields& f);
    std::string_view createOutput(File& file);
    bool openOutput(Session& s, const QByteArray& fid, File& file);
    void data(Session& s, const Fields& f);
    void finish(Session& s);
    bool commitLink(Session& s, const QByteArray& fid);
    void settle(Session& s, const QByteArray& fid);
    void finishLinks(Session& s);
    void sendFile(Session& s, const Fields& f);
    bool stream(Session& s, const QByteArray& fid, std::unique_ptr<QFile> file, bool compress,
        std::unique_ptr<Delta> delta = { }, std::unique_ptr<Signature> signature = { });
    QByteArray read(Stream& item, bool& error, bool& atEnd);
    void pump();
    bool write(File& file, std::string_view bytes, Watch& stop);
    struct DeltaTarget;
    bool rawWrite(File& file, std::string_view bytes);
    void delta(Session& s, const QByteArray& fid, File& file);
};

FileTransfer::FileTransfer(QObject* parent)
    : QObject(parent)
    , m_worker(std::make_unique<Worker>(this))
{
}
FileTransfer::~FileTransfer() = default;
void FileTransfer::handle(const QByteArray& command)
{
    Q_EMIT answering();
    // Posted after the work of the command, on every path. The worker answers
    // once every packet queued before then has left its queue, delivered or
    // dropped: those of the command, and those of anything before it.
    const auto fence = qScopeGuard([w = m_worker.get()] {
        QMetaObject::invokeMethod(w->context, [w] {
            w->fences.push_back(w->queuedPackets);
            w->answer();
        });
    });
    if (command.size() > MaxCommand) {
        abort();
        return;
    }
    if (m_worker->inputBlocked.load())
        return;
    if (m_worker->queuedBytes.fetch_add(command.size()) + command.size() > MaxQueuedBytes) {
        m_worker->queuedBytes.fetch_sub(command.size());
        if (!m_worker->inputBlocked.exchange(true))
            abort();
        return;
    }
    // Work that already runs for the session must see a cancel before its packet arrives.
    const auto cancelled = cancelId(command);
    if (!cancelled.isEmpty()) {
        QMutexLocker lock(&m_worker->cancelMutex);
        ++m_worker->cancelRequests[cancelled];
        m_worker->cancelEpoch.fetch_add(1);
    }
    QMetaObject::invokeMethod(m_worker->context, [w = m_worker.get(), command, cancelled] {
        w->queuedBytes.fetch_sub(command.size());
        w->handle(command, cancelled);
        if (!cancelled.isEmpty())
            w->forgetCancel(cancelled);
        // A dialog that ended with the command lets the next one open.
        w->askWaiting();
    });
}
void FileTransfer::approve(const QString& token, bool allowed)
{
    QMetaObject::invokeMethod(m_worker->context, [w = m_worker.get(), token, allowed] { w->authorize(token, allowed); });
}
void FileTransfer::reset()
{
    const auto epoch = m_worker->resetEpoch.fetch_add(1) + 1;
    QMetaObject::invokeMethod(m_worker->context, [w = m_worker.get(), epoch] {
        w->clearOutgoing();
        w->overflow = false;
        w->clearedEpoch = epoch;
        while (!w->sessions.empty())
            w->remove(w->sessions.begin()->first);
    });
}
void FileTransfer::abort()
{
    const auto epoch = m_worker->resetEpoch.fetch_add(1) + 1;
    QMetaObject::invokeMethod(m_worker->context, [w = m_worker.get(), epoch] {
        w->clearOutgoing();
        w->overflow = false;
        w->clearedEpoch = epoch;
        for (auto& [id, session] : w->sessions)
            w->status(*session, { }, "ENOMEM");
        while (!w->sessions.empty())
            w->remove(w->sessions.begin()->first);
        w->inputBlocked.store(false);
    });
}
void FileTransfer::setPassword(const QString& password)
{
    QMetaObject::invokeMethod(m_worker->context, [w = m_worker.get(), password] { w->password = password; });
}
void FileTransfer::setDeltaLimit(qint64 bytes)
{
    QMetaObject::invokeMethod(m_worker->context, [w = m_worker.get(), bytes] { w->deltaLimit = bytes; });
}
void FileTransfer::setInputBacklog(std::function<std::size_t()> backlog) { m_worker->backlog = std::move(backlog); }

void FileTransfer::Worker::cancel(const QByteArray& id)
{
    const auto it = sessions.find(id);
    if (it == sessions.end()) {
        Session unknown;
        unknown.id = id;
        status(unknown, { }, Status::Canceled);
        return;
    }
    std::erase_if(outgoing, [&](const Packet& packet) {
        if (packet.id != id)
            return false;
        outgoingBytes -= packet.bytes.size();
        return true;
    });
    // A quiet session queues no status: the commands that waited only for
    // the packets erased here are answered now.
    answer();
    status(*it->second, { }, Status::Canceled);
    remove(id);
}

void FileTransfer::Worker::handle(const QByteArray& raw, const QByteArray& cancelled)
{
    // A reset is queued behind this packet: the packet came before it, and the
    // reset ends its session. Nothing it asks for may reach the disk.
    if (overflow || resetEpoch.load() != clearedEpoch)
        return;
    const auto decoded = decode(raw);
    if (!decoded) {
        // handle() on the GUI thread read the id of this cancel and stopped
        // the session's work already. Finish the cancel, so that the client
        // gets an answer and the session does not wait with its work cut short.
        if (!cancelled.isEmpty())
            cancel(cancelled);
        return;
    }
    const auto& f = *decoded;
    // Before the other checks: the GUI thread applied this cancel already.
    if (f.value("ac") == "cancel") {
        cancel(f.value("id"));
        return;
    }
    for (const auto& key : { QByteArray("sz"), QByteArray("prm"), QByteArray("mod"), QByteArray("q") }) {
        if (!f.contains(key))
            continue;
        bool valid = false;
        const auto value = f.value(key).toLongLong(&valid);
        // A modification time before 1970 is negative.
        if (!valid || (key != "mod" && value < -1) || (key == "q" && (value < 0 || value > 2))
            || (key == "prm" && value > 07777)) {
            // The answer of a known session follows its q.
            Session invalid;
            invalid.id = f.value("id");
            const auto known = sessions.find(invalid.id);
            status(known != sessions.end() ? *known->second : invalid, f.value("fid"), "EINVAL");
            return;
        }
    }
    const auto id = f.value("id"), action = f.value("ac");
    auto it = sessions.find(id);
    if (action == "send" || action == "receive") {
        if (it != sessions.end()) {
            status(*it->second, { }, "EINVAL");
            remove(id);
            return;
        }
        auto s = std::make_unique<Session>();
        s->id = id;
        s->sending = action == "send";
        s->quiet = std::clamp(f.value("q").toInt(), 0, 2);
        s->token = QUuid::createUuid().toString();
        if (sessions.size() >= MaxSessions) {
            status(*s, { }, "EBUSY");
            return;
        }
        bool valid = false;
        s->expected = f.value("sz").toInt(&valid);
        if (!s->sending && (!valid || s->expected <= 0 || s->expected > MaxFiles)) {
            status(*s, { }, "EINVAL");
            return;
        }
        // A session that gives a password asks for no dialog: the password
        // is right, or the session is refused. A wrong one stops every pane
        // from checking a password for a while, so that a program cannot
        // guess it, and the log says so.
        const auto supplied = f.value("pw");
        if (!supplied.isEmpty()) {
            if (monotonicMs() < g_passwordsBlockedUntil.load()) {
                status(*s, { }, "EPERM");
                return;
            }
            const QByteArray expected = bytesOf(AfterglowProto::transferPasswordHash(view(id), password.toStdString()));
            unsigned difference = unsigned(supplied.size() != expected.size());
            if (supplied.size() == expected.size())
                for (qsizetype n = 0; n < expected.size(); ++n)
                    difference |= unsigned(quint8(supplied[n]) ^ quint8(expected[n]));
            if (password.isEmpty() || difference != 0) {
                g_passwordsBlockedUntil.store(monotonicMs() + DenialCooldownMs);
                qCWarning(lcFileTransfer) << "File transfer: a program gave a wrong password. No password is checked for"
                                          << DenialCooldownMs / 1000 << "seconds.";
                status(*s, { }, "EPERM");
                return;
            }
        }
        const bool bypass = !supplied.isEmpty();
        if (!bypass) {
            // After a denial, and while a dialog is open, a program cannot ask again.
            if (coolingDown()) {
                status(*s, { }, "EPERM");
                return;
            }
            if (prompting()) {
                status(*s, { }, "EBUSY");
                return;
            }
        }
        auto* added = s.get();
        sessions.emplace(id, std::move(s));
        if (added->sending) {
            // The prompt waits for the destinations, which come only after
            // this answer: the files wait in the session until it is approved.
            added->holding = !bypass;
            added->approved = true;
            status(*added, { }, Status::Ok);
        } else if (bypass)
            added->approved = true;
        return;
    }
    if (it == sessions.end()) {
        Session unknown;
        unknown.id = id;
        status(unknown, { }, "ENOENT");
        return;
    }
    Session& s = *it->second;
    if (!s.sending && s.requested < s.expected) {
        const auto path = pathOf(f.value("n"));
        if (action != "file" || path.isEmpty() || !safe(f.value("fid"))) {
            status(s, { }, "EINVAL");
            remove(id);
            return;
        }
        // Requests wait for a dialog with no time limit. Bound what they hold.
        const qsizetype cost = f.value("n").size() + f.value("fid").size();
        if (requestBytes + cost > MaxRequestBytes) {
            status(s, { }, "ENOMEM");
            remove(id);
            return;
        }
        requestBytes += cost;
        s.requestBytes += cost;
        s.requests.push_back({ f.value("fid"), path, std::nullopt });
        if (++s.requested == s.expected) {
            if (s.approved) {
                s.asked = s.requests.size();
                status(s, { }, Status::Ok);
                list(s);
            } else
                wantDialog(s);
        }
        return;
    }
    if (!s.approved) {
        status(s, { }, "EPERM");
        remove(id);
        return;
    }
    if (s.holding) {
        // Nothing comes after a finish.
        if (s.heldFinish)
            return;
        // A file waits for its dialog, with the data that names it. The
        // commands of the other files go on.
        if (action == "file" || ((action == "data" || action == "end_data") && s.held.contains(f.value("fid")))) {
            hold(s, f);
            return;
        }
        if (action == "finish" && !s.held.empty()) {
            s.heldFinish = f;
            return;
        }
    }
    dispatch(s, f);
}

void FileTransfer::Worker::dispatch(Session& s, const Fields& f)
{
    const auto id = s.id, action = f.value("ac");
    if (action == "finish") {
        if (s.sending)
            finish(s);
        remove(id);
        return;
    }
    if (action == "file") {
        if (s.sending)
            receiveFile(s, f);
        else
            sendFile(s, f);
    } else if (action == "data" || action == "end_data")
        data(s, f);
    else {
        status(s, { }, "EINVAL");
        remove(id);
    }
}

void FileTransfer::Worker::receiveFile(Session& s, const Fields& f)
{
    const auto fid = f.value("fid");
    const QString path = pathOf(f.value("n"));
    if (const auto error = refusal(s, f); !error.empty()) {
        status(s, fid, error);
        return;
    }
    const auto type = f.value("ft", "regular");
    // Where the dialog showed the destination. A session with the password showed none.
    const QFileInfo info(path);
    Place place;
    if (const auto shownPlace = s.places.find(fid); shownPlace != s.places.end()) {
        place = std::move(shownPlace->second);
        s.places.erase(shownPlace);
    } else
        place = placeOf(info.absolutePath());
    const QByteArray name = QFile::encodeName(info.fileName());
    // Validate before any directory is created: a rejected file leaves
    // nothing behind. A directory that does not exist yet holds nothing.
    AfterglowProto::UniqueFd directory(openPlace(place, creationMask, nullptr));
    if (!directory.valid() && errno != ENOENT) {
        status(s, fid, errorName());
        return;
    }
    struct stat existing { };
    const bool exists
        = directory.valid() && ::fstatat(directory.get(), name.constData(), &existing, AT_SYMLINK_NOFOLLOW) == 0;
    if (type == "regular" && exists && !S_ISREG(existing.st_mode)) {
        status(s, fid, "EINVAL");
        return;
    }
    const bool rsync = f.value("tt") == "rsync" && type == "regular" && exists && existing.st_size > MinDeltaBasis;
    // A delta upload holds its old file and its output open, and its
    // signature goes out as a stream. Refuse before STARTED when any has no room.
    if (rsync && (activeFiles.full(3, 1) || streams.size() >= MaxStreams)) {
        status(s, fid, "EBUSY");
        return;
    }
    if (!directory.valid()) {
        // From the top down, so that removal in reverse goes deepest first.
        std::vector<QString> made;
        directory.reset(openPlace(place, creationMask, &made));
        s.createdDirs.insert(s.createdDirs.end(), made.begin(), made.end());
        for (const auto& real : made)
            s.created.insert(real);
        if (!directory.valid()) {
            status(s, fid, errorName());
            return;
        }
    }
    auto file = std::make_unique<File>(nullptr, &pendingBytes);
    file->meta = metadata(f);
    file->path = path;
    file->place = std::move(place);
    file->name = name;
    if (type == "directory") {
        const QString real = joined(file->place.path(), info.fileName());
        const bool made = ::mkdirat(directory.get(), name.constData(), 0700) == 0;
        if (!made && errno != EEXIST) {
            status(s, fid, errorName());
            return;
        }
        // A link to a directory stays: nothing goes through it that the
        // dialog did not show, and it is not this session's to change.
        struct stat target { };
        if (!made && (::fstatat(directory.get(), name.constData(), &target, 0) || !S_ISDIR(target.st_mode))) {
            status(s, fid, "EEXIST");
            return;
        }
        if (made) {
            s.createdDirs.push_back(real);
            s.created.insert(real);
            const AfterglowProto::UniqueFd own(
                ::openat(directory.get(), name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!own.valid() || ::fchmod(own.get(), 0777 & ~creationMask)) {
                status(s, fid, errorName());
                return;
            }
        }
        // A directory that this session made as the parent of an earlier file is its own.
        file->existed = !made && !s.created.contains(real);
        file->complete = true;
        s.files.emplace(fid, std::move(file));
        status(s, fid, Status::Ok);
        settle(s, fid);
        return;
    }
    std::unique_ptr<Signature> signature;
    if (rsync) {
        file->basis = std::make_unique<QFile>();
        const int fd = ::openat(directory.get(), name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        struct stat basisStat { };
        const bool regular = fd >= 0 && ::fstat(fd, &basisStat) == 0 && S_ISREG(basisStat.st_mode);
        const bool opened = regular && file->basis->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle);
        if (!opened && fd >= 0)
            ::close(fd);
        if (opened) {
            file->charge(&activeFiles, 1);
            // One size for the signature, the applier and the read below. A
            // basis that grows while it is read must not add records past the
            // block count that the applier accepts, or keep the stream going.
            const auto basisSize = quint64(basisStat.st_size);
            signature = std::make_unique<Signature>(Signature {
                AfterglowProto::UniqueFd(::fcntl(fd, F_DUPFD_CLOEXEC, 0)),
                AfterglowProto::SignatureBuilder(basisSize), basisSize });
            file->blockSize = signature->builder.blockSize();
            // Before the basis is read: a file that cannot be applied costs no signature.
            file->applier = std::make_unique<AfterglowProto::DeltaApplier>(
                file->blockSize, basisSize, std::size_t(MaxRsyncBuffer));
            if (!file->applier->valid()) {
                status(s, fid, "ENOMEM");
                return;
            }
            if (!signature->basis.valid()) {
                status(s, fid, errorName());
                return;
            }
            // Its output too, before STARTED: a delta upload, which the
            // client sends in groups, then fails at its data for nothing
            // that another file took.
            if (const auto error = createOutput(*file); !error.empty()) {
                status(s, fid, error);
                return;
            }
            file->rsync = true;
        } else
            file->basis.reset();
    }
    const bool delta = file->rsync;
    s.files.emplace(fid, std::move(file));
    // The stream before STARTED: another pane can take the last descriptor
    // meanwhile, and a refusal must be the file's only status. Its data goes
    // out from the timer, after STARTED.
    if (signature && !stream(s, fid, nullptr, false, { }, std::move(signature))) {
        s.files.erase(fid);
        return;
    }
    status(s, fid, Status::Started, -1, { { "tt", delta ? "rsync" : "simple" } });
}

// A regular file's temporary file in its checked directory, and the
// inflater of its compressed data. A file of a sender that names all of its
// files at the start holds neither until its data comes: both count against
// the descriptors of the pane. The error, or nothing.
std::string_view FileTransfer::Worker::createOutput(File& file)
{
    if (activeFiles.full(2))
        return "EBUSY";
    auto output = std::make_unique<Output>();
    output->name = ".afterglow-transfer-" + QUuid::createUuid().toString(QUuid::Id128).toLatin1();
    output->directory.reset(openPlace(file.place, creationMask, nullptr));
    if (!output->directory.valid())
        return errorName();
    const int fd = ::openat(output->directory.get(), output->name.constData(),
        O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return errorName();
    if (!output->file.open(fd, QIODevice::ReadWrite, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return "EIO";
    }
    // kitten marks the links of a compressed transfer with zip=zlib too, and
    // sends their targets plain, as kitty reads them: only a file's data is inflated.
    if (file.meta.value("zip") == "zlib") {
        file.inflater = std::make_unique<AfterglowProto::ZlibStream>(AfterglowProto::ZlibStream::Mode::Inflate);
        if (!file.inflater->valid()) {
            file.inflater.reset();
            return "ENOMEM";
        }
    }
    file.output = std::move(output);
    file.charge(&activeFiles, 2);
    return { };
}
bool FileTransfer::Worker::openOutput(Session& s, const QByteArray& fid, File& file)
{
    const auto error = createOutput(file);
    if (error.empty())
        return true;
    fail(s, fid, error);
    file.releaseIo();
    settle(s, fid);
    return false;
}

bool FileTransfer::Worker::rawWrite(File& file, std::string_view bytes)
{
    if (thread.isInterruptionRequested())
        return false;
    const auto size = qint64(bytes.size());
    if (file.output) {
        // Copy operations can multiply a small packet. Never write past the
        // declared size. kitten transfer declares none, so a delta upload
        // without a size stops at the configured limit.
        bool known = false;
        const auto declared = file.meta.value("sz").toLongLong(&known);
        if (known && declared >= 0) {
            if (file.written + size > declared) {
                file.tooLarge = true;
                return false;
            }
        } else if ((file.rsync || file.inflater) && deltaLimit > 0 && file.written + size > deltaLimit) {
            // A packet of compressed data can inflate to a thousand times its size.
            file.tooLarge = true;
            return false;
        }
        if (file.output->file.write(bytes.data(), size) != size)
            return false;
    } else {
        if (file.linkData.size() + size > MaxLinkData)
            return false;
        file.linkData.append(bytes.data(), qsizetype(size));
    }
    file.written += size;
    return true;
}

void FileTransfer::Worker::data(Session& s, const Fields& f)
{
    const auto fid = f.value("fid");
    const auto it = s.files.find(fid);
    if (it == s.files.end() || it->second->failed || it->second->complete)
        return;
    auto& file = *it->second;
    const auto bytes = f.value("d");
    if (!s.sending && file.rsync) {
        if (bytes.size() > MaxPacketData
            || file.pending.size() + std::size_t(bytes.size()) > std::size_t(MaxRsyncBuffer)) {
            file.failed = true;
            file.releaseIo();
            status(s, fid, "EFBIG");
            return;
        }
        if (!file.addPending(view(bytes))) {
            file.failed = true;
            file.releaseIo();
            status(s, fid, "ENOMEM");
            return;
        }
        if (f.value("ac") == "end_data") {
            file.complete = true;
            delta(s, fid, file);
            file.releaseIo();
        }
        return;
    }
    if (s.sending && !file.output && !isLink(file.meta) && !openOutput(s, fid, file))
        return;
    // One watch for the whole packet: a cancel or reset stops it between any two writes.
    auto stop = watch(s.id);
    bool ok = bytes.size() <= MaxPacketData;
    if (ok && file.inflater) {
        file.inflater->setInput(view(bytes));
        if (file.ended && !bytes.isEmpty())
            ok = false;
        while (ok && file.inflater->inputLeft() > 0) {
            char buffer[65536];
            std::size_t produced = 0;
            const auto result = file.inflater->step(buffer, sizeof buffer, produced);
            ok = result != AfterglowProto::ZlibStream::Result::Error
                && write(file, std::string_view(buffer, produced), stop);
            if (result == AfterglowProto::ZlibStream::Result::End) {
                file.ended = true;
                ok &= file.inflater->inputLeft() == 0;
                break;
            }
            // A packet of 4 KiB can inflate to megabytes.
            if (stopped(stop)) {
                ok = false;
                break;
            }
        }
    } else if (ok)
        ok = write(file, view(bytes), stop);
    const bool end = f.value("ac") == "end_data";
    if (end && file.inflater && !file.ended)
        ok = false;
    if (end && file.rsync && (!file.applier || !file.applier->finished() || !file.pending.empty()))
        ok = false;
    if (end && file.meta.contains("sz") && file.meta.value("sz").toLongLong() >= 0 && file.output)
        ok &= file.written == file.meta.value("sz").toLongLong();
    if (!ok) {
        file.failed = true;
        const std::string_view error = file.overBudget ? "ENOMEM" : file.tooLarge ? "EFBIG" : "EIO";
        file.releaseIo();
        status(s, fid, error);
        settle(s, fid);
        return;
    }
    file.complete = end;
    if (end && file.output) {
        // The temporary file has mode 0600. Without a mode from the client
        // (none, or prm=-1), follow the umask. The metadata goes on the
        // descriptor, and the rename stays in the checked directory.
        auto& output = *file.output;
        if (!output.file.flush() || !setMetadata(output.file.handle(), file.meta, creationMask)
            || ::renameat(output.directory.get(), output.name.constData(), output.directory.get(), file.name.constData())) {
            file.failed = true;
            status(s, fid, errorName());
            file.releaseIo();
            settle(s, fid);
            return;
        }
        output.keep = true;
        file.committed = true;
    }
    if (!end || file.output)
        status(s, fid, end ? Status::Ok : Status::Progress, file.written);
    if (end) {
        file.releaseIo();
        if (!isLink(file.meta) || commitLink(s, fid))
            settle(s, fid);
    }
}

void FileTransfer::Worker::finish(Session& s)
{
    s.finishing = true;
    finishLinks(s);
    // Children change directory times; restore directory metadata last. A
    // directory that was there before keeps its own: it can be a link to a
    // directory, and it is not this session's to change.
    std::vector<std::pair<QByteArray, File*>> directories;
    for (auto& [fid, file] : s.files) {
        if (!file->complete && !file->failed)
            status(s, fid, "EINVAL");
        if (file->meta.value("ft") == "directory" && !file->existed)
            directories.emplace_back(fid, file.get());
    }
    std::sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) {
        return a.second->path.count(QLatin1Char('/')) > b.second->path.count(QLatin1Char('/'));
    });
    for (const auto& [fid, file] : directories)
        if (!setDirectoryMetadata(joined(file->place.path(), QFile::decodeName(file->name)), file->meta))
            status(s, fid, errorName());
    s.createdDirs.clear();
    if (!s.finishFailures.empty()) {
        QByteArray failures;
        for (const auto& [error, files] : s.finishFailures)
            failures += (failures.isEmpty() ? "" : ", ") + error + " for " + QByteArray::number(files.first)
                + (files.first == 1 ? " file (" : " files (the first ") + files.second + ")";
        qCWarning(lcFileTransfer).noquote() << "File transfer" << s.id << "failed at the finish:" << failures;
    }
}

bool FileTransfer::Worker::commitLink(Session& s, const QByteArray& fid)
{
    auto& file = *s.files.at(fid);
    const bool hard = file.meta.value("ft") == "link";
    const QByteArray targetId = hard ? (file.linkData.startsWith("fid:") ? file.linkData.mid(4) : file.linkData)
        : file.linkData.startsWith("fid_abs:") ? file.linkData.mid(8)
        : file.linkData.startsWith("fid:")     ? file.linkData.mid(4)
                                               : QByteArray();
    QString target;
    const File* source = nullptr;
    if (!targetId.isEmpty()) {
        // Link targets can name files that arrive later in the session.
        const auto found = s.files.find(targetId);
        if (found != s.files.end() && found->second->failed) {
            fail(s, fid, "ENOENT");
            return true;
        }
        if (found == s.files.end() || !found->second->complete
            || (isLink(found->second->meta) && !found->second->committed)) {
            s.waiters.emplace(targetId, fid);
            return false;
        }
        // A hard link to a symbolic link would be a second name of the link:
        // a later change through it would reach the link's target.
        if (hard && found->second->meta.value("ft", "regular") != "regular" && found->second->meta.value("ft") != "link") {
            fail(s, fid, "EINVAL");
            return true;
        }
        source = found->second.get();
        target = source->path;
        if (!hard && file.linkData.startsWith("fid:"))
            target = QDir(QFileInfo(file.path).absolutePath()).relativeFilePath(target);
    } else if (!hard && file.linkData.startsWith("path:"))
        target = QString::fromUtf8(file.linkData.mid(5));
    if (target.isEmpty() || target.contains(QChar(0)) || QString::fromUtf8(file.linkData).toUtf8() != file.linkData) {
        fail(s, fid, "EINVAL");
        return true;
    }
    // In the checked directories of the link and of a hard link's file.
    const QByteArray temporary = ".afterglow-link-" + QUuid::createUuid().toString(QUuid::Id128).toLatin1();
    const AfterglowProto::UniqueFd directory(openPlace(file.place, creationMask, nullptr));
    int result = -1;
    if (directory.valid() && hard) {
        const AfterglowProto::UniqueFd from(openPlace(source->place, creationMask, nullptr));
        if (from.valid())
            result = ::linkat(from.get(), source->name.constData(), directory.get(), temporary.constData(), 0);
    } else if (directory.valid())
        result = ::symlinkat(QFile::encodeName(target).constData(), directory.get(), temporary.constData());
    if (result)
        fail(s, fid, errorName());
    else if (::renameat(directory.get(), temporary.constData(), directory.get(), file.name.constData())) {
        fail(s, fid, errorName());
        ::unlinkat(directory.get(), temporary.constData(), 0);
    } else if (!hard && !setLinkTime(directory.get(), file.name, file.meta))
        fail(s, fid, errorName());
    else {
        file.committed = true;
        status(s, fid, Status::Ok, file.written);
    }
    return true;
}

void FileTransfer::Worker::settle(Session& s, const QByteArray& fid)
{
    // Each link is committed once, when its target is final, so a chain costs O(n log n).
    std::vector<QByteArray> work { fid };
    while (!work.empty() && !thread.isInterruptionRequested()) {
        const auto target = work.back();
        work.pop_back();
        const auto [first, last] = s.waiters.equal_range(target);
        std::vector<QByteArray> ready;
        for (auto it = first; it != last; ++it)
            ready.push_back(it->second);
        s.waiters.erase(first, last);
        for (const auto& link : ready)
            if (commitLink(s, link))
                work.push_back(link);
    }
}

void FileTransfer::Worker::finishLinks(Session& s)
{
    std::set<QByteArray> waiting;
    for (const auto& [target, link] : s.waiters)
        waiting.insert(link);
    // A target that is not itself a waiting link never arrived or never completed.
    std::vector<QByteArray> work;
    for (auto it = s.waiters.begin(); it != s.waiters.end();) {
        if (waiting.contains(it->first)) {
            ++it;
            continue;
        }
        fail(s, it->second, "ENOENT");
        work.push_back(it->second);
        it = s.waiters.erase(it);
    }
    while (!work.empty()) {
        const auto target = work.back();
        work.pop_back();
        const auto [first, last] = s.waiters.equal_range(target);
        for (auto it = first; it != last; ++it) {
            fail(s, it->second, "ENOENT");
            work.push_back(it->second);
        }
        s.waiters.erase(first, last);
    }
    // The links that remain wait on each other.
    for (const auto& [target, link] : s.waiters)
        fail(s, link, "ELOOP");
    s.waiters.clear();
}

void FileTransfer::Worker::list(Session& s)
{
    auto stop = watch(s.id);
    // An entry has the path that the client names, and the real path where
    // it is read: links above a request lead where the dialog showed. A
    // request's own status comes from the directory that was checked.
    struct Entry {
        QString path, real;
        QByteArray request, parent;
        int depth;
        struct stat status;
    };
    std::deque<Entry> pending;
    for (const auto& request : s.requests) {
        const QFileInfo info(request.path);
        const Place place = request.place ? *request.place : placeOf(info.absolutePath());
        const AfterglowProto::UniqueFd directory(openPlace(place, creationMask, nullptr));
        const QByteArray name = QFile::encodeName(info.fileName());
        struct stat st { };
        // "/" has no name in its directory: it is the directory itself.
        if (!directory.valid()
            || ::fstatat(directory.get(), name.constData(), &st, name.isEmpty() ? AT_EMPTY_PATH : AT_SYMLINK_NOFOLLOW)) {
            status(s, request.fid, errorName());
            continue;
        }
        pending.push_back({ request.path, joined(place.path(), info.fileName()), request.fid, { }, 0, st });
    }
    // Other sessions share the output queue. A listing that does not fit is
    // refused whole, as soon as it is known not to fit.
    const qsizetype budget = std::min(MaxQueuedBytes / 2, MaxQueuedBytes - outgoingBytes - ListingHeadroom);
    qsizetype listingBytes = 0;
    std::map<QString, QByteArray> byReal;
    requestBytes -= s.requestBytes;
    s.requestBytes = 0;
    std::vector<Request>().swap(s.requests);
    std::map<std::pair<quint64, quint64>, QByteArray> inodes;
    // One log line for the listing, however many children it leaves out.
    struct LeftOut {
        int count = 0;
        QString first;
        QByteArray error;
        ~LeftOut()
        {
            if (count)
                qCInfo(lcFileTransfer) << "File transfer: left out" << count << "entries from a listing, the first"
                                       << first << error;
        }
    } leftOut;
    while (!pending.empty()) {
        // A cancel or reset ends the session. A partial listing is never reported as complete.
        if (stopped(stop))
            return;
        const auto entry = pending.front();
        pending.pop_front();
        struct stat st = entry.status;
        const auto path = QFile::encodeName(entry.real);
        // A child that went away since its directory was read is left out:
        // a status would name the request, and a client fails the whole
        // source on it. A child whose name is not UTF-8 never gets here:
        // QDirIterator skips it without a word, so it is not counted either.
        if (entry.depth > 0 && ::lstat(path.constData(), &st)) {
            if (leftOut.count++ == 0) {
                leftOut.first = entry.path;
                leftOut.error = bytesOf(errorName());
            }
            continue;
        }
        // Devices, sockets and pipes in a directory are left out, as kitten leaves them out when it sends.
        if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode) && !S_ISDIR(st.st_mode)) {
            if (entry.depth == 0)
                status(s, entry.request, "EINVAL");
            continue;
        }
        if (entry.depth > MaxDepth) {
            status(s, entry.request, "EFBIG");
            continue;
        }
        if (s.listing.size() >= MaxFiles) {
            status(s, { }, "EFBIG");
            return;
        }
        Fields f { { "ac", "file" }, { "id", s.id }, { "fid", entry.request }, { "n", entry.path.toUtf8() },
            { "st", QByteArray::number(s.listing.size() + 1) }, { "prm", QByteArray::number(st.st_mode & 07777) },
            { "sz", QByteArray::number(st.st_size) }, { "pr", entry.parent } };
        // Nanoseconds overflow 64 bits after the year 2262. Leave such a time out.
        qint64 modified = 0;
        if (!__builtin_mul_overflow(qint64(st.st_mtim.tv_sec), qint64(1000000000), &modified)
            && !__builtin_add_overflow(modified, qint64(st.st_mtim.tv_nsec), &modified))
            f["mod"] = QByteArray::number(modified);
        f["ft"] = S_ISDIR(st.st_mode) ? "directory" : S_ISLNK(st.st_mode) ? "symlink" : "regular";
        if (S_ISREG(st.st_mode)) {
            const auto inode = std::make_pair(quint64(st.st_dev), quint64(st.st_ino));
            if (auto found = inodes.find(inode); found != inodes.end()) {
                f["ft"] = "link";
                f["d"] = found->second;
            } else
                inodes[inode] = f["st"];
        }
        // A child that another request listed already (the sources overlap) is listed once.
        if (s.listing.contains(entry.path)) {
            if (entry.depth == 0)
                status(s, entry.request, "EEXIST");
            continue;
        }
        // A symbolic link can get the id of its target below: room for it.
        listingBytes += encode(f).size() + (S_ISLNK(st.st_mode) ? 16 : 0);
        if (listingBytes > budget) {
            status(s, { }, "EFBIG");
            return;
        }
        s.listing[entry.path] = { f, entry.real };
        byReal[entry.real] = f["st"];
        if (S_ISDIR(st.st_mode)) {
            QDirIterator children(entry.real, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            while (children.hasNext()) {
                if (pending.size() + s.listing.size() >= std::size_t(MaxFiles)) {
                    status(s, { }, "EFBIG");
                    return;
                }
                const QString child = children.next();
                pending.push_back(
                    { joined(entry.path, children.fileName()), child, entry.request, f["st"], entry.depth + 1, { } });
            }
        }
    }
    for (auto& [path, listed] : s.listing)
        if (listed.fields.value("ft") == "symlink")
            if (const auto it = byReal.find(QFileInfo(listed.real).symLinkTarget()); it != byReal.end())
                listed.fields["d"] = it->second;
    for (const auto& [path, listed] : s.listing)
        send(listed.fields);
    status(s, { }, Status::Ok, -1, { { "n", QDir::homePath().toUtf8() } });
}

bool FileTransfer::Worker::stream(Session& s, const QByteArray& fid, std::unique_ptr<QFile> source, bool compress,
    std::unique_ptr<Delta> delta, std::unique_ptr<Signature> signature)
{
    if (streams.size() >= MaxStreams || g_descriptors.load() + 1 > descriptorLimit()) {
        status(s, fid, "EBUSY");
        return false;
    }
    auto item = std::make_unique<Stream>();
    item->fields = { { "id", s.id }, { "fid", fid } };
    item->source = std::move(source);
    item->delta = std::move(delta);
    item->signature = std::move(signature);
    if (compress) {
        item->deflater = std::make_unique<AfterglowProto::ZlibStream>(AfterglowProto::ZlibStream::Mode::Deflate);
        if (!item->deflater->valid()) {
            status(s, fid, "ENOMEM");
            return false;
        }
    }
    streams.push_back(std::move(item));
    if (timer && !timer->isActive())
        timer->start(1);
    return true;
}

QByteArray FileTransfer::Worker::read(Stream& item, bool& error, bool& atEnd)
{
    if (item.signature) {
        auto& signature = *item.signature;
        if (item.generated.size() - item.generatedSent < std::size_t(MaxPacketData) && !signature.done()) {
            item.generated.erase(0, item.generatedSent);
            item.generatedSent = 0;
            if (!signature.started) {
                item.generated += signature.builder.header();
                signature.started = true;
            }
            std::string block;
            for (qsizetype step = 0; signature.offset < signature.size && step < DeltaStep;) {
                const auto want = std::min<quint64>(signature.builder.blockSize(), signature.size - signature.offset);
                block.resize(std::size_t(want));
                // A short read means the file shrank, or it states more than
                // it holds (sysfs): it no longer matches the signature.
                if (::pread(signature.basis.get(), block.data(), block.size(), off_t(signature.offset)) != ssize_t(want)) {
                    error = true;
                    return { };
                }
                item.generated += signature.builder.record(block);
                signature.offset += want;
                step += qsizetype(want);
            }
        }
        QByteArray data = bytesOf(std::string_view(item.generated).substr(item.generatedSent, std::size_t(MaxPacketData)));
        item.generatedSent += std::size_t(data.size());
        atEnd = signature.done() && item.generatedSent == item.generated.size();
        return data;
    }
    if (!item.delta) {
        QByteArray data = item.source->read(MaxPacketData);
        error = item.source->error() != QFileDevice::NoError;
        // Sources are regular files, where only an empty read is the end.
        // QFile::atEnd() compares the position with st_size once its 16 KiB
        // read buffer is empty. A file in /proc states 0 and holds more, so
        // the stream would stop after the first buffer. A file in /sys can
        // state more than it holds.
        atEnd = data.isEmpty();
        return data;
    }
    auto& generator = item.delta->generator;
    if (item.generated.size() - item.generatedSent < std::size_t(MaxPacketData) && !generator.done()) {
        // Drop what went out only before more is made: this moves less than one packet.
        item.generated.erase(0, item.generatedSent);
        item.generatedSent = 0;
        if (!generator.produce(item.delta->source, item.generated, std::size_t(MaxPacketData), std::size_t(DeltaStep))) {
            error = true;
            return { };
        }
    }
    QByteArray data = bytesOf(std::string_view(item.generated).substr(item.generatedSent, std::size_t(MaxPacketData)));
    item.generatedSent += std::size_t(data.size());
    atEnd = generator.done() && item.generatedSent == item.generated.size();
    return data;
}

void FileTransfer::Worker::pump()
{
    if (streams.empty())
        return;
    auto& item = *streams.front();
    QByteArray data;
    bool end = false, error = false;
    if (!item.deflater) {
        data = read(item, error, end);
    } else {
        if (item.deflater->inputLeft() == 0 && !item.eof) {
            item.input = read(item, error, item.eof);
            item.deflater->setInput(view(item.input));
        }
        // A delta step can end with nothing to send yet. Without input,
        // deflate would report no progress.
        if (!error && item.deflater->inputLeft() == 0 && !item.eof)
            return;
        data.resize(MaxPacketData);
        std::size_t produced = 0;
        const auto result = item.deflater->step(data.data(), std::size_t(MaxPacketData), produced, item.eof);
        data.resize(qsizetype(produced));
        end = result == AfterglowProto::ZlibStream::Result::End;
        error |= result == AfterglowProto::ZlibStream::Result::Error;
    }
    auto f = item.fields;
    if (error) {
        streams.pop_front();
        // Through status(), so that a quiet session stays quiet. The upload
        // whose signature failed can take no data.
        if (const auto session = sessions.find(f.value("id")); session != sessions.end()) {
            auto& s = *session->second;
            const auto fid = f.value("fid");
            const auto file = s.files.find(fid);
            if (s.sending && file != s.files.end()) {
                file->second->failed = true;
                file->second->releaseIo();
            }
            status(s, fid, "EIO");
            if (s.sending && file != s.files.end())
                settle(s, fid);
        }
        return;
    }
    if (end || !data.isEmpty()) {
        f["ac"] = end ? "end_data" : "data";
        f["d"] = data;
        send(f);
    }
    if (end)
        streams.pop_front();
}

void FileTransfer::Worker::sendFile(Session& s, const Fields& f)
{
    const auto fid = f.value("fid");
    const auto path = pathOf(f.value("n"));
    if (!safe(fid) || !s.listing.contains(path)) {
        status(s, fid, "EPERM");
        return;
    }
    const auto& listed = s.listing.at(path);
    const auto type = listed.fields.value("ft");
    const auto zip = f.value("zip", "none");
    if (zip != "none" && zip != "zlib") {
        status(s, fid, "EINVAL");
        return;
    }
    if (type == "directory" || type == "link") {
        status(s, fid, "EINVAL");
        return;
    }
    // In the directory where the listing found the file, if links still lead there.
    const QFileInfo real(listed.real);
    const AfterglowProto::UniqueFd directory(openChecked(real.absolutePath(), O_RDONLY | O_DIRECTORY));
    const QByteArray name = QFile::encodeName(real.fileName());
    if (!directory.valid()) {
        status(s, fid, errorName());
        return;
    }
    if (type == "symlink") {
        char target[4097];
        const auto length = ::readlinkat(directory.get(), name.constData(), target, sizeof target);
        if (length < 0) {
            status(s, fid, errorName());
            return;
        }
        if (length > 4096) {
            status(s, fid, "EFBIG");
            return;
        }
        send({ { "ac", "end_data" }, { "id", s.id }, { "fid", fid }, { "d", QByteArray(target, length) } });
        return;
    }
    if (f.value("tt") == "rsync") {
        if (s.files.contains(fid) || s.files.size() >= MaxFiles) {
            status(s, fid, "EINVAL");
            return;
        }
        if (activeFiles.full(1)) {
            status(s, fid, "EBUSY");
            return;
        }
    }
    const int fd = ::openat(directory.get(), name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        status(s, fid, errorName());
        return;
    }
    struct stat st { };
    if (::fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        ::close(fd);
        status(s, fid, "EINVAL");
        return;
    }
    auto source = std::make_unique<QFile>();
    if (!source->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        status(s, fid, "EIO");
        return;
    }
    if (f.value("tt") == "rsync") {
        auto file = std::make_unique<File>(&activeFiles, &pendingBytes);
        file->meta = metadata(f);
        file->basis = std::move(source);
        file->rsync = true;
        s.files.emplace(fid, std::move(file));
        return;
    }
    stream(s, fid, std::move(source), zip == "zlib");
}

// Copies blocks of the basis for a delta upload, and stops on a cancel or reset.
struct FileTransfer::Worker::DeltaTarget : AfterglowProto::DeltaTarget {
    Worker& worker;
    File& file;
    Watch& stop;
    DeltaTarget(Worker& w, File& f, Watch& watch)
        : worker(w)
        , file(f)
        , stop(watch)
    {
    }
    // Read straight into the applier's buffer: a block is copied once on its way to the file.
    bool readBlock(std::uint64_t index, std::string& out) override
    {
        if (worker.stopped(stop))
            return false;
        const auto at = qint64(index * file.blockSize);
        if (file.basis->pos() != at && !file.basis->seek(at))
            return false;
        const std::size_t old = out.size();
        out.resize(old + file.blockSize);
        const qint64 n = file.basis->read(out.data() + old, qint64(file.blockSize));
        out.resize(old + std::size_t(std::max<qint64>(n, 0)));
        return n >= 0;
    }
    // Copy operations can multiply a small packet. rawWrite() stops at the
    // declared size, or at the delta limit when the client declared none.
    bool write(std::string_view bytes) override { return worker.rawWrite(file, bytes); }
};

bool FileTransfer::Worker::write(File& file, std::string_view bytes, Watch& stop)
{
    if (!file.rsync)
        return rawWrite(file, bytes);
    if (!file.applier || !file.addPending(bytes))
        return false;
    DeltaTarget target(*this, file, stop);
    const auto used = file.applier->apply(file.pending, target);
    if (!used)
        return false;
    file.dropPending(*used);
    return true;
}

void FileTransfer::Worker::delta(Session& s, const QByteArray& fid, File& file)
{
    // kitty refuses a signature block over DeltaMaxBlockSize, so no client sends one, and the bound
    // keeps a refill and the hash of one block small.
    if (!AfterglowProto::signatureBlockSize(file.pending, AfterglowProto::DeltaMaxBlockSize)) {
        status(s, fid, "EINVAL");
        return;
    }
    // The signature's bytes stay in the pane's budget: they move, with no
    // copy, from the file to the stream.
    std::string signature;
    signature.swap(file.pending);
    auto generator = std::make_unique<Delta>(std::move(file.basis), std::move(signature), &pendingBytes);
    // The basis now counts as the stream's descriptor, not the file's.
    file.releaseIo();
    // After the move: dropping the generator then releases the signature's budget once.
    if (!generator->generator.valid()) {
        status(s, fid, "ENOMEM");
        return;
    }
    stream(s, fid, nullptr, file.meta.value("zip") == "zlib", std::move(generator));
}

void FileTransfer::endPasswordPause() { g_passwordsBlockedUntil.store(0); }

FileTransfer::Question FileTransfer::question(bool writing, const QStringList& paths, int total, const QString& asker)
{
    Question q;
    // A remote program can name any file, and chooses where files go: the
    // paths are part of the question.
    const QString who = asker.isEmpty() ? tr("A terminal program") : asker;
    q.text = writing ? tr("%1 wants to write these files on this computer. Existing files can be replaced. Allow it?").arg(who)
                     : tr("%1 wants to read these files on this computer. Allow it?").arg(who);
    // The question stays short however long a path is: each line of it, so
    // that the line of where a path resolves stays a line of its own. The
    // detail has each path whole.
    const auto line = [](const QString& path) {
        QStringList lines = path.split(QLatin1Char('\n'));
        for (QString& part : lines)
            if (part.size() > QuestionLength)
                part = part.left(QuestionLength / 2) + QChar(0x2026) + part.right(QuestionLength / 2 - 1);
        return lines.join(QLatin1Char('\n'));
    };
    for (qsizetype i = 0; i < std::min<qsizetype>(paths.size(), QuestionPaths); ++i)
        q.text += QLatin1Char('\n') + line(paths[i]);
    if (paths.size() > QuestionPaths)
        q.text += QLatin1Char('\n') + tr("and %n more", nullptr, int(paths.size() - QuestionPaths));
    // A Yes covers the listed paths only.
    if (const int rest = total - int(paths.size()); rest > 0)
        q.text += QLatin1Char('\n')
            + (writing ? tr("Files not in this list: %1. They get a question of their own.").arg(rest)
                       : tr("Requested files not in this list: %1. They are refused.").arg(rest));
    if (!writing && std::any_of(paths.begin(), paths.end(), [](const QString& p) { return p.endsWith(QLatin1Char('/')); }))
        q.text += QLatin1Char('\n') + tr("A path that ends with a slash is a directory. The program can read all the files in it.");
    q.detail = paths.join(QLatin1Char('\n'));
    return q;
}

} // namespace Afterglow
