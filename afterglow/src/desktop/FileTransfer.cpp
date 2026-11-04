// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FileTransfer.h"
#include <AfterglowProto/Delta.h>
#include <AfterglowProto/Transfer.h>
#include <AfterglowProto/Zlib.h>
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
#include <algorithm>
#include <atomic>
#include <deque>
#include <fcntl.h>
#include <map>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace Afterglow {
namespace {
    using Fields = QMap<QByteArray, QByteArray>;
    // Protocol limits. They are fixed: a client cannot negotiate them.
    constexpr qsizetype MaxQueuedBytes = 16 * 1024 * 1024; // each of the input and output queues
    constexpr qsizetype MaxCommand = qsizetype(AfterglowProto::TransferMaxCommand);
    constexpr qsizetype MaxPacketData = qsizetype(AfterglowProto::TransferMaxData);
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
    std::string_view view(const QByteArray& bytes) { return { bytes.constData(), std::size_t(bytes.size()) }; }
    QByteArray bytesOf(std::string_view bytes) { return { bytes.data(), qsizetype(bytes.size()) }; }
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
        AfterglowProto::TransferFields f;
        for (auto it = fields.begin(); it != fields.end(); ++it)
            f.emplace(it.key().toStdString(), it.value().toStdString());
        return bytesOf(AfterglowProto::encodeTransfer(f));
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
    QByteArray errorName() { return bytesOf(AfterglowProto::transferErrorName(errno)); }
    // A delta's file, read as the generator asks for it.
    class FileSource : public AfterglowProto::ByteSource {
    public:
        explicit FileSource(QFile& file)
            : m_file(file)
        {
        }
        bool read(std::string& out, std::size_t max) override
        {
            const auto data = m_file.read(qint64(max));
            out.append(data.constData(), std::size_t(data.size()));
            return m_file.error() == QFileDevice::NoError;
        }
        bool atEnd() const override { return m_file.atEnd(); }

    private:
        QFile& m_file;
    };
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
        bool committed = false, complete = false, failed = false, rsync = false, ended = false;
        bool overBudget = false;
        std::unique_ptr<AfterglowProto::ZlibStream> inflater;
        // Applies the operations of a delta upload, and checks the file hash.
        std::unique_ptr<AfterglowProto::DeltaApplier> applier;
        // Descriptors this file holds against the pane limit: its output, and a delta basis.
        std::size_t* activeCount = nullptr;
        std::size_t held = 1;
        std::size_t* pendingTotal;
        quint32 blockSize = 0;
        File(std::size_t* count, std::size_t* pendingBytes)
            : activeCount(count)
            , pendingTotal(pendingBytes)
        {
            if (activeCount)
                ++*activeCount;
        }
        ~File() { releaseIo(); }
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
            inflater.reset();
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
        FileSource source;
        // The generator reads the signature into its tables only when the
        // stream reaches the front, so that the streams queued behind it
        // hold no tables. The signature stays charged to the pane's rsync
        // budget until the stream ends.
        AfterglowProto::DeltaGenerator generator;
        std::size_t* pendingTotal;
        std::size_t charged;
        Delta(std::unique_ptr<QFile> file, const QByteArray& signature, std::size_t* total)
            : basis(std::move(file))
            , source(*basis)
            , generator(signature.toStdString())
            , pendingTotal(total)
            , charged(std::size_t(signature.size()))
        {
        }
        ~Delta() { *pendingTotal -= charged; }
        Delta(const Delta&) = delete;
        Delta& operator=(const Delta&) = delete;
    };
    struct Stream {
        Fields fields;
        std::unique_ptr<QFile> source;
        std::unique_ptr<Delta> delta; // in place of the source
        std::string generated;        // delta operations not sent yet
        QByteArray input;
        std::unique_ptr<AfterglowProto::ZlibStream> deflater;
        bool eof = false;
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
        const bool success = AfterglowProto::transferSuccess(view(status));
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
    struct DeltaTarget;
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
        const QByteArray expected = bytesOf(AfterglowProto::transferPasswordHash(view(id), password.toStdString()));
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
        file->inflater = std::make_unique<AfterglowProto::ZlibStream>(AfterglowProto::ZlibStream::Mode::Inflate);
        if (!file->inflater->valid()) {
            status(s, fid, "ENOMEM");
            return;
        }
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
            AfterglowProto::SignatureBuilder builder(quint64(file->basis->size()));
            file->blockSize = builder.blockSize();
            signature->write(bytesOf(builder.header()));
            while (!file->basis->atEnd()) {
                const auto block = file->basis->read(file->blockSize);
                if (block.isEmpty() || stopped(stop)) {
                    status(s, fid, "EIO");
                    return;
                }
                const auto record = bytesOf(builder.record(view(block)));
                if (signature->write(record) != record.size()) {
                    status(s, fid, "EIO");
                    return;
                }
            }
            signature->seek(0);
            file->rsync = true;
            file->applier = std::make_unique<AfterglowProto::DeltaApplier>(
                file->blockSize, quint64(file->basis->size()), std::size_t(MaxRsyncBuffer));
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
    if (ok && file.inflater) {
        file.inflater->setInput(view(bytes));
        if (file.ended && !bytes.isEmpty())
            ok = false;
        while (ok && file.inflater->inputLeft() > 0) {
            char buffer[65536];
            std::size_t produced = 0;
            const auto result = file.inflater->step(buffer, sizeof buffer, produced);
            ok = result != AfterglowProto::ZlibStream::Result::Error
                && write(s, file, QByteArray(buffer, qsizetype(produced)));
            if (result == AfterglowProto::ZlibStream::Result::End) {
                file.ended = true;
                ok &= file.inflater->inputLeft() == 0;
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
    if (end && file.inflater && !file.ended)
        ok = false;
    if (end && file.rsync && (!file.applier->finished() || !file.pending.isEmpty()))
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
    if (!item.delta) {
        QByteArray data = item.source->read(MaxPacketData);
        error = item.source->error() != QFileDevice::NoError;
        // Sources are regular files, where an empty read is the end. atEnd()
        // alone compares with st_size, which a file in /sys or /proc states
        // larger than what it holds: the stream would never end.
        atEnd = item.source->atEnd() || data.isEmpty();
        return data;
    }
    if (!item.delta->generator.produce(item.delta->source, item.generated, std::size_t(MaxPacketData), std::size_t(DeltaStep))) {
        error = true;
        return { };
    }
    QByteArray data = bytesOf(std::string_view(item.generated).substr(0, std::size_t(MaxPacketData)));
    item.generated.erase(0, std::size_t(data.size()));
    atEnd = item.delta->generator.done() && item.generated.empty();
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
    Watch stop;
    DeltaTarget(Worker& w, File& f, Watch watch)
        : worker(w)
        , file(f)
        , stop(std::move(watch))
    {
    }
    bool readBlock(std::uint64_t index, std::string& out) override
    {
        if (worker.stopped(stop))
            return false;
        const auto at = qint64(index * file.blockSize);
        if (file.basis->pos() != at && !file.basis->seek(at))
            return false;
        const auto block = file.basis->read(file.blockSize);
        out.append(block.constData(), std::size_t(block.size()));
        return true;
    }
    // Copy operations can multiply a small packet: rawWrite() keeps to the declared size.
    bool write(std::string_view bytes) override { return worker.rawWrite(file, bytesOf(bytes)); }
};

bool FileTransfer::Worker::write(Session& s, File& file, const QByteArray& bytes)
{
    if (!file.rsync)
        return rawWrite(file, bytes);
    if (!file.addPending(bytes))
        return false;
    DeltaTarget target(*this, file, watch(s.id));
    const auto used = file.applier->apply(view(file.pending), target);
    if (!used)
        return false;
    file.dropPending(qsizetype(*used));
    return true;
}

void FileTransfer::Worker::delta(Session& s, const QByteArray& fid, File& file)
{
    if (!AfterglowProto::signatureBlockSize(view(file.pending), quint32(MaxRsyncBuffer))) {
        status(s, fid, "EINVAL");
        return;
    }
    // The signature's bytes stay in the pane's budget: they move from the file to the stream.
    auto generator = std::make_unique<Delta>(std::move(file.basis), file.pending, &pendingBytes);
    file.pending = QByteArray();
    stream(s, fid, nullptr, file.meta.value("zip") == "zlib", std::move(generator));
}

} // namespace Afterglow
