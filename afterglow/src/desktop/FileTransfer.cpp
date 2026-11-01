// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FileTransfer.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QTemporaryFile>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtEndian>
#include <zlib.h>
#define XXH_INLINE_ALL
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <xxhash.h>

namespace Afterglow {
namespace {
    using Fields = QMap<QByteArray, QByteArray>;
    // Protocol limits. They are fixed: a client cannot negotiate them.
    constexpr qsizetype MaxQueuedBytes = 16 * 1024 * 1024; // each of the input and output queues
    constexpr qsizetype MaxCommand = 16384;
    constexpr qsizetype MaxPacketData = 4096;
    constexpr int MaxFiles = 16384; // per session
    constexpr std::size_t MaxSessions = 8;
    constexpr std::size_t MaxActiveFiles = 128; // regular files and rsync reads in progress
    constexpr std::size_t MaxStreams = 128; // outgoing file streams
    constexpr int MaxDepth = 128;
    constexpr qsizetype MaxRequestBytes = 4 * 1024 * 1024; // requested paths of all receive sessions
    constexpr qsizetype MaxRsyncBuffer = 16 * 1024 * 1024; // one signature or one literal
    // File bytes after which one pump() stops scanning a delta. A refill
    // before that check reads up to two blocks (32 MiB at most) more.
    constexpr qsizetype DeltaStep = 1024 * 1024;
    constexpr std::size_t MaxPendingBytes = 64 * 1024 * 1024; // every rsync buffer of the pane
    constexpr qsizetype DeliveryWindow = 65536;
    constexpr std::size_t MaxInputBacklog = 1024 * 1024;
    constexpr qsizetype ListingHeadroom = 64 * 1024; // statuses queued behind a listing
    constexpr int DenialCooldownMs = 10000;
    constexpr qsizetype MaxPromptPaths = 100;
    bool safe(const QByteArray& s)
    {
        return !s.isEmpty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || QByteArrayLiteral("_:./@-").contains(char(c));
        });
    }
    std::optional<Fields> decode(const QByteArray& raw)
    {
        if (raw.size() > MaxCommand)
            return std::nullopt;
        Fields f;
        for (const auto& field : raw.split(';')) {
            const auto at = field.indexOf('=');
            if (at <= 0)
                continue;
            const auto key = field.left(at), value = field.mid(at + 1);
            static const QList<QByteArray> keys { "ac", "id", "fid", "n", "st", "d", "pw", "sz", "q", "ft", "zip", "tt",
                "prm", "mod", "pr" };
            if (!keys.contains(key))
                continue;
            if (f.contains(key))
                return std::nullopt;
            if (key == "n" || key == "st" || key == "d" || key == "pw") {
                const auto decoded = QByteArray::fromBase64Encoding(value, QByteArray::AbortOnBase64DecodingErrors);
                if (!decoded)
                    return std::nullopt;
                f[key] = decoded.decoded;
            } else
                f[key] = value;
        }
        if (!safe(f.value("id")) || (f.contains("fid") && !safe(f.value("fid"))))
            return std::nullopt;
        return f;
    }
    // The session id of a cancel request, read without decoding the base64 fields.
    QByteArray cancelId(const QByteArray& raw)
    {
        QByteArray action, id;
        for (const auto& field : raw.split(';')) {
            if (action.isEmpty() && field.startsWith("ac="))
                action = field.mid(3);
            else if (id.isEmpty() && field.startsWith("id="))
                id = field.mid(3);
        }
        return action == "cancel" && safe(id) ? id : QByteArray();
    }
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
        QByteArray out = QByteArrayLiteral("\x1b]5113;");
        bool first = true;
        for (auto it = fields.begin(); it != fields.end(); ++it) {
            if (!first)
                out += ';';
            first = false;
            out += it.key();
            out += '=';
            out += (it.key() == "n" || it.key() == "st" || it.key() == "d" || it.key() == "pw")
                ? it.value().toBase64(QByteArray::OmitTrailingEquals)
                : it.value();
        }
        return out + QByteArrayLiteral("\x1b\\");
    }
    QString pathOf(const QByteArray& value)
    {
        if (value.isEmpty() || value.size() > 4096 || value.contains('\0'))
            return { };
        QString path = QString::fromUtf8(value);
        if (path.toUtf8() != value)
            return { };
        if (path.startsWith(QLatin1String("~/")))
            path = QDir::homePath() + path.mid(1);
        if (!QDir::isAbsolutePath(path))
            path = QDir::home().filePath(path);
        for (const auto& part : path.split(QLatin1Char('/')))
            if (part.toUtf8().size() > 255)
                return { };
        return QDir::cleanPath(path);
    }
    QByteArray errorName()
    {
        switch (errno) {
        case EACCES:
        case EPERM:
            return "EPERM";
        case ENOENT:
            return "ENOENT";
        case ENOSPC:
            return "ENOSPC";
        case EEXIST:
            return "EEXIST";
        case ENOTDIR:
            return "ENOTDIR";
        case ELOOP:
            return "ELOOP";
        default:
            return "EIO";
        }
    }
    // A file without a mode from the client gets the mode a new file would get.
    mode_t creationMask()
    {
        static const mode_t mask = [] {
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
        }();
        return mask;
    }
    bool setMetadata(const QString& path, const Fields& f, bool symlink)
    {
        const auto raw = QFile::encodeName(path);
        bool ok = false;
        const auto mode = f.value("prm").toLongLong(&ok);
        if (ok && mode >= 0 && !symlink && ::chmod(raw.constData(), mode_t(mode & 07777)))
            return false;
        // Times before 1970 are negative. Only -1 means "not given".
        const auto ns = f.value("mod").toLongLong(&ok);
        if (ok && ns != -1) {
            time_t seconds = time_t(ns / 1000000000);
            long nanoseconds = long(ns % 1000000000);
            if (nanoseconds < 0) {
                nanoseconds += 1000000000;
                --seconds;
            }
            timespec times[2] { { 0, UTIME_OMIT }, { seconds, nanoseconds } };
            if (::utimensat(AT_FDCWD, raw.constData(), times, symlink ? AT_SYMLINK_NOFOLLOW : 0))
                return false;
        }
        return true;
    }
    bool isLink(const Fields& meta) { return meta.value("ft") == "symlink" || meta.value("ft") == "link"; }
} // namespace

struct FileTransfer::Worker {
    struct File {
        Fields meta;
        QString path;
        std::unique_ptr<QTemporaryFile> output;
        std::unique_ptr<QFile> basis;
        QByteArray linkData, pending;
        qint64 written = 0;
        bool committed = false, complete = false, failed = false, compressed = false, rsync = false, ended = false;
        bool overBudget = false;
        z_stream stream { };
        XXH3_state_t* hash = XXH3_createState();
        // Descriptors this file holds against the pane limit: its output, and a delta basis.
        std::size_t* activeCount = nullptr;
        std::size_t held = 1;
        std::size_t* pendingTotal;
        quint32 blockSize = 0;
        bool hashSeen = false;
        File(std::size_t* count, std::size_t* pendingBytes)
            : activeCount(count)
            , pendingTotal(pendingBytes)
        {
            if (activeCount)
                ++*activeCount;
            if (hash)
                XXH3_128bits_reset(hash);
        }
        ~File()
        {
            releaseIo();
            XXH3_freeState(hash);
        }
        bool addPending(const QByteArray& bytes)
        {
            if (*pendingTotal + std::size_t(bytes.size()) > MaxPendingBytes) {
                overBudget = true;
                return false;
            }
            pending += bytes;
            *pendingTotal += std::size_t(bytes.size());
            return true;
        }
        void dropPending(qsizetype count)
        {
            pending.remove(0, count);
            *pendingTotal -= std::size_t(count);
        }
        void clearPending()
        {
            *pendingTotal -= std::size_t(pending.size());
            pending.clear();
        }
        void releaseIo()
        {
            output.reset();
            basis.reset();
            clearPending();
            if (compressed) {
                inflateEnd(&stream);
                compressed = false;
            }
            if (activeCount) {
                *activeCount -= held;
                activeCount = nullptr;
            }
        }
    };
    struct Request {
        QByteArray fid;
        QString path;
    };
    struct Session {
        QByteArray id;
        QString token;
        bool sending = false, approved = false, asking = false;
        int quiet = 0, expected = 0;
        // Requests received so far. list() frees the requests themselves, so
        // their count cannot say whether the session still collects them.
        int requested = 0;
        std::vector<Request> requests;
        qsizetype requestBytes = 0;
        std::map<QByteArray, std::unique_ptr<File>> files;
        // Links whose target file has not reached its final state, keyed by the target.
        std::multimap<QByteArray, QByteArray> waiters;
        std::map<QString, Fields> listing;
        std::vector<QString> createdDirs;
    };
    // The delta of a file against the receiver's signature, built as pump()
    // asks for data: a large file needs no copy on disk, and the worker,
    // which serves every session of the pane, handles uploads and messages
    // between steps. Outgoing streams are one queue: the next file waits.
    struct Delta {
        std::unique_ptr<QFile> basis;
        // Stays charged to the pane's rsync budget until the stream ends. It
        // is read into the tables only when the stream reaches the front, so
        // that the streams queued behind it hold no tables.
        QByteArray signature;
        std::size_t* pendingTotal = nullptr;
        std::size_t charged = 0;
        quint32 blockSize = 0;
        // Keyed by both hashes: many blocks with one weak hash cost one lookup, not a scan.
        std::unordered_set<quint32> weakHashes;
        std::map<std::pair<quint32, quint64>, quint64> blocks;
        XXH3_state_t* hash = XXH3_createState();
        QByteArray buffer, literal;
        qsizetype position = 0;
        quint32 a = 0, b = 0;
        bool fresh = true, parsed = false, done = false;
        Delta()
        {
            if (hash)
                XXH3_128bits_reset(hash);
        }
        ~Delta()
        {
            XXH3_freeState(hash);
            if (pendingTotal)
                *pendingTotal -= charged;
        }
        Delta(const Delta&) = delete;
        Delta& operator=(const Delta&) = delete;
        void parse()
        {
            for (qsizetype at = 12; at <= signature.size() - 20; at += 20) {
                quint64 index = 0, strong = 0;
                quint32 weak = 0;
                for (int byte = 0; byte < 8; ++byte) {
                    index |= quint64(quint8(signature[at + byte])) << (byte * 8);
                    strong |= quint64(quint8(signature[at + 12 + byte])) << (byte * 8);
                }
                for (int byte = 0; byte < 4; ++byte)
                    weak |= quint32(quint8(signature[at + 8 + byte])) << (byte * 8);
                weakHashes.insert(weak);
                blocks.try_emplace({ weak, strong }, index);
            }
            signature = QByteArray();
            parsed = true;
        }
        void flushLiteral(QByteArray& out)
        {
            if (literal.isEmpty())
                return;
            QByteArray op(5, '\0');
            op[0] = 1;
            qToLittleEndian(quint32(literal.size()), op.data() + 1);
            XXH3_128bits_update(hash, literal.constData(), std::size_t(literal.size()));
            out += op;
            out += literal;
            literal.clear();
        }
        // Appends operations to OUT until it holds WANT bytes, DeltaStep bytes
        // of the file were read, or the delta ends. False on a read error.
        bool produce(QByteArray& out, qsizetype want);
    };
    struct Stream {
        Fields fields;
        std::unique_ptr<QFile> source;
        std::unique_ptr<Delta> delta; // in place of the source
        QByteArray generated;         // delta operations not sent yet
        QByteArray input;
        z_stream z { };
        bool compressed = false, eof = false;
        ~Stream()
        {
            if (compressed)
                deflateEnd(&z);
        }
    };
    // What long work checks to stop early: a reset, or a cancel of its own session.
    struct Watch {
        QByteArray id;
        quint64 reset, cancel;
    };
    std::size_t activeFiles = 0;
    std::size_t pendingBytes = 0;
    qsizetype requestBytes = 0;
    std::deque<std::unique_ptr<Stream>> streams;
    FileTransfer* owner;
    QThread thread;
    QObject* context = new QObject;
    std::atomic<qsizetype> queuedBytes { 0 };
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
    QElapsedTimer denied;
    // Outgoing packets are paced. Keep at most a bounded amount queued.
    std::deque<QByteArray> outgoing;
    qsizetype outgoingBytes = 0;
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
        // Fill the delivery window, not one packet for each tick.
        for (int i = 0; i < 64 && !streams.empty() && outgoingBytes < DeliveryWindow; ++i)
            pump();
        while (!outgoing.empty() && deliveryBytes.load() < DeliveryWindow && backlogBytes.load() < MaxInputBacklog) {
            auto bytes = std::move(outgoing.front());
            outgoing.pop_front();
            outgoingBytes -= bytes.size();
            deliveryBytes.fetch_add(bytes.size());
            QMetaObject::invokeMethod(owner, [this, bytes, epoch = clearedEpoch] {
                deliveryBytes.fetch_sub(bytes.size());
                if (resetEpoch.load() != epoch)
                    return;
                Q_EMIT owner->reply(bytes);
                if (backlog)
                    backlogBytes.store(backlog());
            });
        }
        // A slow reader (SSH) leaves input waiting in the PTY. Ask again until it drains.
        if (!outgoing.empty() && backlogBytes.load() >= MaxInputBacklog && !backlogQueried.exchange(true))
            QMetaObject::invokeMethod(owner, [this] {
                backlogBytes.store(backlog ? backlog() : 0);
                backlogQueried.store(false);
            });
        if (outgoing.empty() && streams.empty())
            timer->stop();
    }
    Watch watch(const QByteArray& id) const { return { id, resetEpoch.load(), ~quint64(0) }; }
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
            QMetaObject::invokeMethod(
                context,
                [this] {
                    outgoing.clear();
                    outgoingBytes = 0;
                    overflow = false;
                    for (auto& [id, session] : sessions)
                        status(*session, { }, "ENOMEM");
                    while (!sessions.empty())
                        remove(sessions.begin()->first);
                },
                Qt::QueuedConnection);
            return;
        }
        if (f.value("st") == "PROGRESS" && !outgoing.empty()) {
            const auto old = decode(outgoing.back().mid(7, outgoing.back().size() - 9));
            if (old && old->value("st") == "PROGRESS" && old->value("id") == f.value("id")
                && old->value("fid") == f.value("fid")) {
                outgoingBytes -= outgoing.back().size();
                outgoing.pop_back();
            }
        }
        outgoingBytes += bytes.size();
        outgoing.push_back(bytes);
        if (timer && !timer->isActive())
            timer->start(1);
    }
    void status(Session& s, const QByteArray& file, const QByteArray& status, qint64 size = -1, Fields extra = { })
    {
        const bool success = status == "OK" || status == "STARTED" || status == "PROGRESS" || status == "CANCELED";
        if (s.quiet == 2 || (s.quiet == 1 && success))
            return;
        extra["ac"] = "status";
        extra["id"] = s.id;
        extra["st"] = status;
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
    // One permission dialog at a time, so a program cannot stack them over the window.
    bool ask(Session& s)
    {
        if (prompting()) {
            status(s, { }, "EBUSY");
            return false;
        }
        s.asking = true;
        QStringList paths;
        for (const auto& request : s.requests) {
            if (paths.size() == MaxPromptPaths)
                break;
            paths.push_back(request.path);
        }
        const auto token = s.token;
        const bool writing = s.sending;
        const int total = int(s.requests.size());
        QMetaObject::invokeMethod(owner,
            [o = owner, token, writing, paths, total] { Q_EMIT o->permissionRequested(token, writing, paths, total); });
        return true;
    }
    // Takes the id by value: callers pass references to the key that this erases.
    void remove(const QByteArray id)
    {
        auto it = sessions.find(id);
        if (it == sessions.end())
            return;
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
        for (auto& [id, ptr] : sessions)
            if (ptr->token == token) {
                auto& s = *ptr;
                s.asking = false;
                if (!allowed) {
                    denied.start();
                    status(s, { }, "EPERM");
                    remove(id);
                    return;
                }
                s.approved = true;
                status(s, { }, "OK");
                if (!s.sending)
                    list(s);
                return;
            }
    }
    void fail(Session& s, const QByteArray& fid, const QByteArray& error)
    {
        s.files.at(fid)->failed = true;
        status(s, fid, error);
    }
    void handle(const QByteArray& raw);
    void list(Session& s);
    void receiveFile(Session& s, const Fields& f);
    void data(Session& s, const Fields& f);
    void finish(Session& s);
    bool commitLink(Session& s, const QByteArray& fid);
    void settle(Session& s, const QByteArray& fid);
    void finishLinks(Session& s);
    void sendFile(Session& s, const Fields& f);
    bool stream(Session& s, const QByteArray& fid, std::unique_ptr<QFile> file, bool compress,
        std::unique_ptr<Delta> delta = { });
    QByteArray read(Stream& item, bool& error, bool& atEnd);
    void pump();
    bool write(Session& s, File& file, const QByteArray& bytes);
    bool rawWrite(File& file, const QByteArray& bytes);
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
        w->handle(command);
        if (!cancelled.isEmpty())
            w->forgetCancel(cancelled);
    });
}
void FileTransfer::approve(const QString& token, bool allowed)
{
    QMetaObject::invokeMethod(
        m_worker->context, [w = m_worker.get(), token, allowed] { w->authorize(token, allowed); });
}
void FileTransfer::reset()
{
    const auto epoch = m_worker->resetEpoch.fetch_add(1) + 1;
    QMetaObject::invokeMethod(m_worker->context, [w = m_worker.get(), epoch] {
        w->outgoing.clear();
        w->outgoingBytes = 0;
        w->clearedEpoch = epoch;
        while (!w->sessions.empty())
            w->remove(w->sessions.begin()->first);
    });
}
void FileTransfer::abort()
{
    const auto epoch = m_worker->resetEpoch.fetch_add(1) + 1;
    QMetaObject::invokeMethod(m_worker->context, [w = m_worker.get(), epoch] {
        w->outgoing.clear();
        w->outgoingBytes = 0;
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
void FileTransfer::setInputBacklog(std::function<std::size_t()> backlog) { m_worker->backlog = std::move(backlog); }

void FileTransfer::Worker::handle(const QByteArray& raw)
{
    if (overflow)
        return;
    const auto decoded = decode(raw);
    if (!decoded)
        return;
    const auto& f = *decoded;
    for (const auto& key : { QByteArray("sz"), QByteArray("prm"), QByteArray("mod"), QByteArray("q") }) {
        if (!f.contains(key))
            continue;
        bool valid = false;
        const auto value = f.value(key).toLongLong(&valid);
        // A modification time before 1970 is negative.
        if (!valid || (key != "mod" && value < -1) || (key == "q" && (value < 0 || value > 2))
            || (key == "prm" && value > 07777)) {
            Session invalid;
            invalid.id = f.value("id");
            status(invalid, f.value("fid"), "EINVAL");
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
        const QByteArray expected = QByteArrayLiteral("sha256:")
            + QCryptographicHash::hash(id + ';' + password.toUtf8(), QCryptographicHash::Sha256).toHex();
        const auto supplied = f.value("pw");
        unsigned difference = unsigned(supplied.size() != expected.size());
        if (supplied.size() == expected.size())
            for (qsizetype n = 0; n < expected.size(); ++n)
                difference |= unsigned(quint8(supplied[n]) ^ quint8(expected[n]));
        const bool bypass = !password.isEmpty() && difference == 0;
        if (!bypass) {
            // After a denial, and while a dialog is open, a program cannot ask again.
            if (denied.isValid() && denied.elapsed() < DenialCooldownMs) {
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
            if (bypass)
                authorize(added->token, true);
            else if (!ask(*added))
                remove(id);
        } else if (bypass)
            added->approved = true;
        return;
    }
    if (it == sessions.end()) {
        Session unknown;
        unknown.id = id;
        status(unknown, { }, action == "cancel" ? "CANCELED" : "ENOENT");
        return;
    }
    Session& s = *it->second;
    if (action == "cancel") {
        std::erase_if(outgoing, [&](const auto& packet) {
            const auto fields = decode(packet.mid(7, packet.size() - 9));
            if (fields && fields->value("id") == id) {
                outgoingBytes -= packet.size();
                return true;
            }
            return false;
        });
        status(s, { }, "CANCELED");
        remove(id);
        return;
    }
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
        s.requests.push_back({ f.value("fid"), path });
        if (++s.requested == s.expected) {
            if (s.approved) {
                status(s, { }, "OK");
                list(s);
            } else if (!ask(s))
                remove(id);
        }
        return;
    }
    if (!s.approved) {
        status(s, { }, "EPERM");
        remove(id);
        return;
    }
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
    auto stop = watch(s.id);
    const auto fid = f.value("fid");
    const QString path = pathOf(f.value("n"));
    if (!safe(fid) || path.isEmpty() || s.files.contains(fid) || s.files.size() >= MaxFiles) {
        status(s, fid, "EINVAL");
        return;
    }
    const auto type = f.value("ft", "regular");
    if (type != "regular" && type != "directory" && type != "symlink" && type != "link") {
        status(s, fid, "EINVAL");
        return;
    }
    if (type == "regular" && activeFiles >= MaxActiveFiles) {
        status(s, fid, "EBUSY");
        return;
    }
    const auto zip = f.value("zip", "none");
    if (zip != "none" && zip != "zlib") {
        status(s, fid, "EINVAL");
        return;
    }
    // Validate before any directory is created: a rejected file leaves nothing behind.
    struct stat existing { };
    const bool exists = ::lstat(QFile::encodeName(path).constData(), &existing) == 0;
    if (type == "regular" && exists && !S_ISREG(existing.st_mode)) {
        status(s, fid, "EINVAL");
        return;
    }
    const bool rsync = f.value("tt") == "rsync" && type == "regular" && exists;
    // A delta upload also holds its basis open.
    if (rsync && activeFiles + 2 > MaxActiveFiles) {
        status(s, fid, "EBUSY");
        return;
    }
    // The signature goes out as a stream. Refuse before STARTED when none is free.
    if (rsync && streams.size() >= MaxStreams) {
        status(s, fid, "EBUSY");
        return;
    }
    auto file = std::make_unique<File>(type == "regular" ? &activeFiles : nullptr, &pendingBytes);
    if (!file->hash) {
        status(s, fid, "ENOMEM");
        return;
    }
    file->meta = metadata(f);
    file->path = path;
    // Record new parents from the top down, so that removal in reverse goes deepest first.
    std::vector<QString> parents;
    for (QString parent = QFileInfo(path).absolutePath(); !QFileInfo::exists(parent);
        parent = QFileInfo(parent).absolutePath()) {
        parents.push_back(parent);
        if (parent == QFileInfo(parent).absolutePath())
            break;
    }
    s.createdDirs.insert(s.createdDirs.end(), parents.rbegin(), parents.rend());
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        status(s, fid, "EPERM");
        return;
    }
    if (type == "directory") {
        if (!QDir().mkpath(path)) {
            status(s, fid, "EPERM");
            return;
        }
        if (!exists)
            s.createdDirs.push_back(path);
        file->complete = true;
        s.files.emplace(fid, std::move(file));
        status(s, fid, "OK");
        settle(s, fid);
        return;
    }
    if (type == "regular") {
        file->output = std::make_unique<QTemporaryFile>(
            QFileInfo(path).absolutePath() + QStringLiteral("/.afterglow-transfer-XXXXXX"));
        if (!file->output->open()) {
            status(s, fid, "EPERM");
            return;
        }
    }
    if (zip == "zlib" && type == "regular") {
        if (inflateInit(&file->stream) != Z_OK) {
            status(s, fid, "ENOMEM");
            return;
        }
        file->compressed = true;
    }
    std::unique_ptr<QTemporaryFile> signature;
    if (rsync) {
        file->basis = std::make_unique<QFile>();
        const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        struct stat basisStat { };
        const bool regular = fd >= 0 && ::fstat(fd, &basisStat) == 0 && S_ISREG(basisStat.st_mode);
        const bool opened = regular && file->basis->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle);
        if (!opened && fd >= 0)
            ::close(fd);
        if (opened) {
            ++activeFiles;
            ++file->held;
            signature = std::make_unique<QTemporaryFile>();
            if (!signature->open()) {
                status(s, fid, "EIO");
                return;
            }
            file->blockSize = quint32(std::max(64.0, std::ceil(std::sqrt(double(file->basis->size())))));
            QByteArray header(12, '\0');
            qToLittleEndian(file->blockSize, header.data() + 8);
            signature->write(header);
            quint64 index = 0;
            while (!file->basis->atEnd()) {
                const auto block = file->basis->read(file->blockSize);
                if (block.isEmpty() || stopped(stop)) {
                    status(s, fid, "EIO");
                    return;
                }
                quint32 a = 0, b = 0;
                for (unsigned char c : block) {
                    a += c;
                    b += a;
                }
                QByteArray record(20, '\0');
                qToLittleEndian(index++, record.data());
                qToLittleEndian((a & 0xffffu) | (b << 16), record.data() + 8);
                qToLittleEndian(quint64(XXH3_64bits(block.constData(), std::size_t(block.size()))), record.data() + 12);
                if (signature->write(record) != record.size()) {
                    status(s, fid, "EIO");
                    return;
                }
            }
            signature->seek(0);
            file->rsync = true;
        } else
            file->basis.reset();
    }
    const bool delta = file->rsync;
    s.files.emplace(fid, std::move(file));
    status(s, fid, "STARTED", -1, { { "tt", delta ? "rsync" : "simple" } });
    if (signature && !stream(s, fid, std::move(signature), false))
        s.files.erase(fid);
}

bool FileTransfer::Worker::rawWrite(File& file, const QByteArray& bytes)
{
    if (thread.isInterruptionRequested())
        return false;
    if (file.output) {
        // Copy operations can multiply a small packet. Never write past the declared size.
        bool known = false;
        const auto declared = file.meta.value("sz").toLongLong(&known);
        if (known && declared >= 0 && file.written + bytes.size() > declared)
            return false;
        if (file.output->write(bytes) != bytes.size())
            return false;
        XXH3_128bits_update(file.hash, bytes.constData(), std::size_t(bytes.size()));
    } else {
        if (file.linkData.size() + bytes.size() > 4096)
            return false;
        file.linkData += bytes;
    }
    file.written += bytes.size();
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
        if (bytes.size() > MaxPacketData || file.pending.size() + bytes.size() > MaxRsyncBuffer) {
            file.failed = true;
            file.releaseIo();
            status(s, fid, "EFBIG");
            return;
        }
        if (!file.addPending(bytes)) {
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
    bool ok = bytes.size() <= MaxPacketData;
    if (ok && file.compressed) {
        file.stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.constData()));
        file.stream.avail_in = uInt(bytes.size());
        if (file.ended && !bytes.isEmpty())
            ok = false;
        while (ok && file.stream.avail_in > 0) {
            char buffer[65536];
            file.stream.next_out = reinterpret_cast<Bytef*>(buffer);
            file.stream.avail_out = sizeof buffer;
            const int result = inflate(&file.stream, Z_NO_FLUSH);
            ok = (result == Z_OK || result == Z_STREAM_END)
                && write(s, file, QByteArray(buffer, sizeof buffer - file.stream.avail_out));
            if (result == Z_STREAM_END) {
                file.ended = true;
                ok &= file.stream.avail_in == 0;
                break;
            }
            if (thread.isInterruptionRequested()) {
                ok = false;
                break;
            }
        }
    } else if (ok)
        ok = write(s, file, bytes);
    const bool end = f.value("ac") == "end_data";
    if (end && file.compressed && !file.ended)
        ok = false;
    if (end && file.rsync && (!file.hashSeen || !file.pending.isEmpty()))
        ok = false;
    if (end && file.meta.contains("sz") && file.meta.value("sz").toLongLong() >= 0 && file.output)
        ok &= file.written == file.meta.value("sz").toLongLong();
    if (!ok) {
        file.failed = true;
        const bool overBudget = file.overBudget;
        file.releaseIo();
        status(s, fid, overBudget ? "ENOMEM" : "EIO");
        settle(s, fid);
        return;
    }
    file.complete = end;
    if (end && file.output) {
        // QTemporaryFile creates mode 0600. Without a mode from the client
        // (none, or prm=-1), follow the umask.
        const auto temporary = QFile::encodeName(file.output->fileName());
        bool modeValid = false;
        const bool modeGiven = file.meta.value("prm").toLongLong(&modeValid) >= 0 && modeValid;
        if (!file.output->flush()
            || (!modeGiven && ::chmod(temporary.constData(), 0666 & ~creationMask()))
            || !setMetadata(file.output->fileName(), file.meta, false)
            || ::rename(temporary.constData(), QFile::encodeName(file.path).constData())) {
            file.failed = true;
            status(s, fid, errorName());
            file.releaseIo();
            settle(s, fid);
            return;
        }
        file.output->setAutoRemove(false);
        file.committed = true;
    }
    if (!end || file.output)
        status(s, fid, end ? "OK" : "PROGRESS", file.written);
    if (end) {
        file.releaseIo();
        if (!isLink(file.meta) || commitLink(s, fid))
            settle(s, fid);
    }
}

void FileTransfer::Worker::finish(Session& s)
{
    finishLinks(s);
    // Children change directory times; restore directory metadata last.
    std::vector<std::pair<QByteArray, File*>> directories;
    for (auto& [fid, file] : s.files) {
        if (!file->complete && !file->failed)
            status(s, fid, "EINVAL");
        if (file->meta.value("ft") == "directory")
            directories.emplace_back(fid, file.get());
    }
    std::sort(directories.begin(), directories.end(), [](const auto& a, const auto& b) {
        return a.second->path.count(QLatin1Char('/')) > b.second->path.count(QLatin1Char('/'));
    });
    for (const auto& [fid, file] : directories)
        if (!setMetadata(file->path, file->meta, false))
            status(s, fid, errorName());
    s.createdDirs.clear();
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
        target = found->second->path;
        if (!hard && file.linkData.startsWith("fid:"))
            target = QDir(QFileInfo(file.path).absolutePath()).relativeFilePath(target);
    } else if (!hard && file.linkData.startsWith("path:"))
        target = QString::fromUtf8(file.linkData.mid(5));
    if (target.isEmpty() || target.contains(QChar(0)) || QString::fromUtf8(file.linkData).toUtf8() != file.linkData) {
        fail(s, fid, "EINVAL");
        return true;
    }
    const QString temporary = QFileInfo(file.path).absolutePath() + QStringLiteral("/.afterglow-link-")
        + QUuid::createUuid().toString(QUuid::Id128);
    const auto source = QFile::encodeName(target), dest = QFile::encodeName(temporary);
    const int result = hard ? ::link(source.constData(), dest.constData()) : ::symlink(source.constData(), dest.constData());
    if (result)
        fail(s, fid, errorName());
    else if (::rename(dest.constData(), QFile::encodeName(file.path).constData())) {
        fail(s, fid, errorName());
        ::unlink(dest.constData());
    } else if (!setMetadata(file.path, file.meta, !hard))
        fail(s, fid, errorName());
    else {
        file.committed = true;
        status(s, fid, "OK", file.written);
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
    struct Entry {
        QString path;
        QByteArray request, parent;
        int depth;
    };
    std::deque<Entry> pending;
    for (const auto& request : s.requests)
        pending.push_back({ request.path, request.fid, { }, 0 });
    requestBytes -= s.requestBytes;
    s.requestBytes = 0;
    std::vector<Request>().swap(s.requests);
    std::map<std::pair<quint64, quint64>, QByteArray> inodes;
    while (!pending.empty()) {
        // A cancel or reset ends the session. A partial listing is never reported as complete.
        if (stopped(stop))
            return;
        const auto entry = pending.front();
        pending.pop_front();
        struct stat st { };
        const auto path = QFile::encodeName(entry.path);
        if (::lstat(path.constData(), &st)) {
            status(s, entry.request, errorName());
            continue;
        }
        if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode) && !S_ISDIR(st.st_mode)) {
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
        if (s.listing.contains(entry.path)) {
            status(s, entry.request, "EEXIST");
            continue;
        }
        s.listing[entry.path] = f;
        if (S_ISDIR(st.st_mode)) {
            QDirIterator children(entry.path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            while (children.hasNext()) {
                if (pending.size() + s.listing.size() >= std::size_t(MaxFiles)) {
                    status(s, { }, "EFBIG");
                    return;
                }
                pending.push_back({ children.next(), entry.request, f["st"], entry.depth + 1 });
            }
        }
    }
    // Other sessions share the output queue. A listing that does not fit is refused whole.
    const qsizetype budget = std::min(MaxQueuedBytes / 2, MaxQueuedBytes - outgoingBytes - ListingHeadroom);
    qsizetype listingBytes = 0;
    for (auto& [path, f] : s.listing) {
        if (f.value("ft") == "symlink") {
            const QString target = QFileInfo(path).symLinkTarget();
            if (auto it = s.listing.find(target); it != s.listing.end())
                f["d"] = it->second.value("st");
        }
        listingBytes += encode(f).size();
        if (listingBytes > budget) {
            status(s, { }, "EFBIG");
            return;
        }
    }
    for (const auto& [path, f] : s.listing)
        send(f);
    status(s, { }, "OK", -1, { { "n", QDir::homePath().toUtf8() } });
}

bool FileTransfer::Worker::stream(Session& s, const QByteArray& fid, std::unique_ptr<QFile> source, bool compress,
    std::unique_ptr<Delta> delta)
{
    if (streams.size() >= MaxStreams) {
        status(s, fid, "EBUSY");
        return false;
    }
    auto item = std::make_unique<Stream>();
    item->fields = { { "id", s.id }, { "fid", fid } };
    item->source = std::move(source);
    item->delta = std::move(delta);
    if (compress) {
        if (deflateInit(&item->z, Z_DEFAULT_COMPRESSION) != Z_OK) {
            status(s, fid, "ENOMEM");
            return false;
        }
        item->compressed = true;
    }
    streams.push_back(std::move(item));
    if (timer && !timer->isActive())
        timer->start(1);
    return true;
}

QByteArray FileTransfer::Worker::read(Stream& item, bool& error, bool& atEnd)
{
    if (!item.delta) {
        QByteArray data = item.source->read(MaxPacketData);
        error = item.source->error() != QFileDevice::NoError;
        // Sources are regular files, where an empty read is the end. atEnd()
        // alone compares with st_size, which a file in /sys or /proc states
        // larger than what it holds: the stream would never end.
        atEnd = item.source->atEnd() || data.isEmpty();
        return data;
    }
    if (!item.delta->produce(item.generated, MaxPacketData)) {
        error = true;
        return { };
    }
    QByteArray data = item.generated.left(MaxPacketData);
    item.generated.remove(0, data.size());
    atEnd = item.delta->done && item.generated.isEmpty();
    return data;
}

void FileTransfer::Worker::pump()
{
    if (streams.empty())
        return;
    auto& item = *streams.front();
    QByteArray data;
    bool end = false, error = false;
    if (!item.compressed) {
        data = read(item, error, end);
    } else {
        if (item.z.avail_in == 0 && !item.eof) {
            item.input = read(item, error, item.eof);
            item.z.next_in = reinterpret_cast<Bytef*>(item.input.data());
            item.z.avail_in = uInt(item.input.size());
        }
        // A delta step can end with nothing to send yet. Without input,
        // deflate would report no progress.
        if (!error && item.z.avail_in == 0 && !item.eof)
            return;
        data.resize(MaxPacketData);
        item.z.next_out = reinterpret_cast<Bytef*>(data.data());
        item.z.avail_out = MaxPacketData;
        const auto result = deflate(&item.z, item.eof ? Z_FINISH : Z_NO_FLUSH);
        data.resize(MaxPacketData - item.z.avail_out);
        end = result == Z_STREAM_END;
        error |= result != Z_OK && result != Z_STREAM_END;
    }
    auto f = item.fields;
    if (error) {
        streams.pop_front();
        // Through status(), so that a quiet session stays quiet.
        if (const auto session = sessions.find(f.value("id")); session != sessions.end())
            status(*session->second, f.value("fid"), "EIO");
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
    const auto type = s.listing.at(path).value("ft");
    const auto zip = f.value("zip", "none");
    if (zip != "none" && zip != "zlib") {
        status(s, fid, "EINVAL");
        return;
    }
    if (type == "directory" || type == "link") {
        status(s, fid, "EINVAL");
        return;
    }
    if (type == "symlink") {
        char target[4097];
        const auto length = ::readlink(QFile::encodeName(path).constData(), target, sizeof target);
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
        if (activeFiles >= MaxActiveFiles) {
            status(s, fid, "EBUSY");
            return;
        }
    }
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
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
        if (!file->hash) {
            status(s, fid, "ENOMEM");
            return;
        }
        file->meta = metadata(f);
        file->basis = std::move(source);
        file->rsync = true;
        s.files.emplace(fid, std::move(file));
        return;
    }
    stream(s, fid, std::move(source), zip == "zlib");
}

bool FileTransfer::Worker::write(Session& s, File& file, const QByteArray& bytes)
{
    if (!file.rsync)
        return rawWrite(file, bytes);
    if (!file.addPending(bytes))
        return false;
    while (!file.pending.isEmpty()) {
        const auto type = quint8(file.pending[0]);
        if (file.hashSeen)
            return false;
        qsizetype used = 0;
        if (type == 1) {
            if (file.pending.size() < 5)
                break;
            const auto size = qFromLittleEndian<quint32>(file.pending.constData() + 1);
            if (size > MaxRsyncBuffer)
                return false;
            if (file.pending.size() < 5 + qsizetype(size))
                break;
            if (!rawWrite(file, file.pending.mid(5, size)))
                return false;
            used = 5 + size;
        } else if (type == 0 || type == 3) {
            const qsizetype length = type == 0 ? 9 : 13;
            if (file.pending.size() < length)
                break;
            const auto first = qFromLittleEndian<quint64>(file.pending.constData() + 1);
            const quint64 count = type == 0 ? 1 : quint64(qFromLittleEndian<quint32>(file.pending.constData() + 9)) + 1;
            const quint64 total = (quint64(file.basis->size()) + file.blockSize - 1) / file.blockSize;
            if (first >= total || count > total - first)
                return false;
            if (!file.basis->seek(qint64(first * file.blockSize)))
                return false;
            auto stop = watch(s.id);
            for (quint64 i = 0; i < count; ++i) {
                if (stopped(stop))
                    return false;
                const auto block = file.basis->read(file.blockSize);
                if (block.isEmpty() || !rawWrite(file, block))
                    return false;
            }
            used = length;
        } else if (type == 2) {
            if (file.pending.size() < 3)
                break;
            const auto length = qFromLittleEndian<quint16>(file.pending.constData() + 1);
            if (length != 16)
                return false;
            if (file.pending.size() < 19)
                break;
            XXH128_canonical_t expected { };
            XXH128_canonicalFromHash(&expected, XXH3_128bits_digest(file.hash));
            if (std::memcmp(expected.digest, file.pending.constData() + 3, 16) != 0)
                return false;
            file.hashSeen = true;
            used = 19;
        } else
            return false;
        file.dropPending(used);
    }
    return true;
}

void FileTransfer::Worker::delta(Session& s, const QByteArray& fid, File& file)
{
    const auto& signature = file.pending;
    if (signature.size() < 12 || (signature.size() - 12) % 20 != 0 || signature.left(8) != QByteArray(8, '\0')) {
        status(s, fid, "EINVAL");
        return;
    }
    const auto blockSize = qFromLittleEndian<quint32>(signature.constData() + 8);
    if (blockSize == 0 || blockSize > MaxRsyncBuffer) {
        status(s, fid, "EINVAL");
        return;
    }
    auto generator = std::make_unique<Delta>();
    if (!generator->hash) {
        status(s, fid, "ENOMEM");
        return;
    }
    generator->blockSize = blockSize;
    generator->basis = std::move(file.basis);
    // The signature's bytes stay in the pane's budget: they move from the file to the stream.
    generator->pendingTotal = &pendingBytes;
    generator->charged = std::size_t(file.pending.size());
    generator->signature = std::exchange(file.pending, QByteArray());
    stream(s, fid, nullptr, file.meta.value("zip") == "zlib", std::move(generator));
}

bool FileTransfer::Worker::Delta::produce(QByteArray& out, qsizetype want)
{
    if (!parsed)
        parse();
    qsizetype scanned = 0;
    while (!done && out.size() < want && scanned < DeltaStep) {
        if (buffer.size() - position <= blockSize && !basis->atEnd()) {
            buffer.remove(0, position);
            position = 0;
            buffer += basis->read(std::max<qint64>(65536, 2 * qint64(blockSize)));
            if (basis->error() != QFileDevice::NoError)
                return false;
            fresh = true;
        }
        const auto length = std::min<qsizetype>(blockSize, buffer.size() - position);
        if (!length) {
            flushLiteral(out);
            XXH128_canonical_t digest { };
            XXH128_canonicalFromHash(&digest, XXH3_128bits_digest(hash));
            QByteArray final(3, '\0');
            final[0] = 2;
            qToLittleEndian(quint16(16), final.data() + 1);
            final.append(reinterpret_cast<const char*>(digest.digest), 16);
            out += final;
            done = true;
            break;
        }
        if (fresh) {
            a = b = 0;
            for (qsizetype n = 0; n < length; ++n) {
                a += quint8(buffer[position + n]);
                b += a;
            }
            a &= 0xffffu;
            b &= 0xffffu;
        }
        std::optional<quint64> match;
        const quint32 weak = a | (b << 16);
        if (weakHashes.contains(weak)) {
            const auto strong = XXH3_64bits(buffer.constData() + position, std::size_t(length));
            if (const auto found = blocks.find({ weak, strong }); found != blocks.end())
                match = found->second;
        }
        if (match) {
            flushLiteral(out);
            QByteArray op(9, '\0');
            qToLittleEndian(*match, op.data() + 1);
            out += op;
            XXH3_128bits_update(hash, buffer.constData() + position, std::size_t(length));
            position += length;
            scanned += length;
            fresh = true;
        } else {
            literal += buffer[position];
            const quint32 old = quint8(buffer[position]);
            if (position + length < buffer.size()) {
                const quint32 next = quint8(buffer[position + length]);
                a = (a - old + next) & 0xffffu;
                b = (b - quint32(length) * old + a) & 0xffffu;
                fresh = false;
            } else if (basis->atEnd()) {
                // At the end of the file the window shrinks: roll the old byte out only.
                a = (a - old) & 0xffffu;
                b = (b - quint32(length) * old) & 0xffffu;
                fresh = false;
            } else
                fresh = true; // the next pass refills the buffer
            ++position;
            ++scanned;
            if (literal.size() >= 65536)
                flushLiteral(out);
        }
    }
    return true;
}
} // namespace Afterglow
