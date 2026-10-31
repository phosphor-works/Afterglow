// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FileTransfer.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
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
#include <xxhash.h>

namespace Afterglow {
namespace {
    using Fields = QMap<QByteArray, QByteArray>;
    constexpr qsizetype MaxQueuedBytes = 16 * 1024 * 1024;
    constexpr int MaxFiles = 16384;
    bool safe(const QByteArray& s)
    {
        return !s.isEmpty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || QByteArrayLiteral("_:./@-").contains(char(c));
        });
    }
    std::optional<Fields> decode(const QByteArray& raw)
    {
        if (raw.size() > 16384)
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
    bool setMetadata(const QString& path, const Fields& f, bool symlink)
    {
        const auto raw = QFile::encodeName(path);
        bool ok = false;
        const auto mode = f.value("prm").toLongLong(&ok);
        if (ok && mode >= 0 && !symlink && ::chmod(raw.constData(), mode_t(mode & 07777)))
            return false;
        const auto ns = f.value("mod").toLongLong(&ok);
        if (ok && ns >= 0) {
            timespec times[2] { { 0, UTIME_OMIT }, { time_t(ns / 1000000000), long(ns % 1000000000) } };
            if (::utimensat(AT_FDCWD, raw.constData(), times, symlink ? AT_SYMLINK_NOFOLLOW : 0))
                return false;
        }
        return true;
    }
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
        z_stream stream { };
        XXH3_state_t* hash = XXH3_createState();
        quint32 blockSize = 0;
        bool hashSeen = false;
        File() { XXH3_128bits_reset(hash); }
        ~File()
        {
            if (compressed)
                inflateEnd(&stream);
            XXH3_freeState(hash);
        }
    };
    struct Session {
        QByteArray id;
        QString token;
        bool sending = false, approved = false;
        int quiet = 0, expected = 0;
        std::vector<Fields> requests;
        std::map<QByteArray, std::unique_ptr<File>> files;
        std::map<QString, Fields> listing;
        std::vector<QString> createdDirs;
    };
    struct Stream {
        Fields fields;
        std::unique_ptr<QFile> source;
        QByteArray input;
        z_stream z { };
        bool compressed = false, eof = false;
        ~Stream()
        {
            if (compressed)
                deflateEnd(&z);
        }
    };
    std::deque<std::unique_ptr<Stream>> streams;
    FileTransfer* owner;
    QThread thread;
    QObject* context = new QObject;
    std::atomic<qsizetype> queuedBytes { 0 };
    std::atomic<qsizetype> deliveryBytes { 0 };
    std::atomic<quint64> cancellationEpoch { 0 };
    std::atomic<bool> inputBlocked { false };
    std::map<QByteArray, std::unique_ptr<Session>> sessions;
    QString password;
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
            QObject::connect(timer, &QTimer::timeout, context, [this] {
                if (outgoing.empty())
                    pump();
                for (int i = 0; i < 4 && !outgoing.empty() && deliveryBytes.load() < 65536; ++i) {
                    auto bytes = std::move(outgoing.front());
                    outgoing.pop_front();
                    outgoingBytes -= bytes.size();
                    deliveryBytes.fetch_add(bytes.size());
                    QMetaObject::invokeMethod(owner, [this, bytes] {
                        deliveryBytes.fetch_sub(bytes.size());
                        Q_EMIT owner->reply(bytes);
                    });
                }
                if (outgoing.empty() && streams.empty())
                    timer->stop();
            });
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
    void send(Fields f)
    {
        if (overflow)
            return;
        const auto bytes = encode(f);
        if (outgoingBytes + bytes.size() > MaxQueuedBytes) {
            overflow = true;
            QMetaObject::invokeMethod(context, [this] {
                outgoing.clear();
                outgoingBytes = 0;
                overflow = false;
                for (auto& [id, session] : sessions)
                    status(*session, { }, "ENOMEM");
                while (!sessions.empty())
                    remove(sessions.begin()->first);
            });
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
    void ask(Session& s)
    {
        QStringList paths;
        for (const auto& request : s.requests)
            paths.push_back(pathOf(request.value("n")));
        const auto token = s.token;
        const bool writing = s.sending;
        QMetaObject::invokeMethod(
            owner, [o = owner, token, writing, paths] { Q_EMIT o->permissionRequested(token, writing, paths); });
    }
    void remove(const QByteArray& id)
    {
        auto it = sessions.find(id);
        if (it == sessions.end())
            return;
        const auto createdDirs = std::move(it->second->createdDirs);
        std::erase_if(streams, [&](const auto& stream) { return stream->fields.value("id") == id; });
        const auto token = it->second->token;
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
                if (!allowed) {
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
    void handle(const QByteArray& raw);
    void list(Session& s);
    void receiveFile(Session& s, const Fields& f);
    void data(Session& s, const Fields& f);
    void finish(Session& s);
    void commitLinks(Session& s, bool final);
    void sendFile(Session& s, const Fields& f);
    void stream(Session& s, const QByteArray& fid, std::unique_ptr<QFile> file, bool compress);
    void pump();
    bool write(File& file, const QByteArray& bytes);
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
    const auto decoded = decode(command);
    if (decoded && decoded->value("ac") == "cancel")
        m_worker->cancellationEpoch.fetch_add(1);
    if (command.size() > 16384) {
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
    QMetaObject::invokeMethod(m_worker->context, [w = m_worker.get(), command] {
        w->queuedBytes.fetch_sub(command.size());
        w->handle(command);
    });
}
void FileTransfer::approve(const QString& token, bool allowed)
{
    QMetaObject::invokeMethod(
        m_worker->context, [w = m_worker.get(), token, allowed] { w->authorize(token, allowed); });
}
void FileTransfer::reset()
{
    m_worker->cancellationEpoch.fetch_add(1);
    QMetaObject::invokeMethod(m_worker->context, [w = m_worker.get()] {
        while (!w->sessions.empty())
            w->remove(w->sessions.begin()->first);
        w->outgoing.clear();
        w->outgoingBytes = 0;
    });
}
void FileTransfer::abort()
{
    m_worker->cancellationEpoch.fetch_add(1);
    QMetaObject::invokeMethod(m_worker->context, [w = m_worker.get()] {
        w->outgoing.clear();
        w->outgoingBytes = 0;
        w->overflow = false;
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
        if (!valid || value < -1 || (key == "q" && (value < 0 || value > 2)) || (key == "prm" && value > 07777)) {
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
        if (sessions.size() >= 8) {
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
        auto* added = s.get();
        sessions.emplace(id, std::move(s));
        if (added->sending) {
            if (bypass)
                authorize(added->token, true);
            else
                ask(*added);
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
    if (!s.sending && s.requests.size() < std::size_t(s.expected)) {
        if (action != "file" || pathOf(f.value("n")).isEmpty() || !safe(f.value("fid"))) {
            status(s, { }, "EINVAL");
            remove(id);
            return;
        }
        s.requests.push_back(f);
        if (s.requests.size() == std::size_t(s.expected)) {
            if (s.approved) {
                status(s, { }, "OK");
                list(s);
            } else
                ask(s);
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
    const auto epoch = cancellationEpoch.load();
    const auto fid = f.value("fid");
    const QString path = pathOf(f.value("n"));
    if (!safe(fid) || path.isEmpty() || s.files.contains(fid) || s.files.size() >= MaxFiles) {
        status(s, fid, "EINVAL");
        return;
    }
    auto file = std::make_unique<File>();
    file->meta = f;
    file->path = path;
    const auto type = f.value("ft", "regular");
    if (type != "regular" && type != "directory" && type != "symlink" && type != "link") {
        status(s, fid, "EINVAL");
        return;
    }
    for (QString parent = QFileInfo(path).absolutePath(); !QFileInfo::exists(parent);
        parent = QFileInfo(parent).absolutePath()) {
        s.createdDirs.insert(s.createdDirs.begin(), parent);
        if (parent == QFileInfo(parent).absolutePath())
            break;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        status(s, fid, "EPERM");
        return;
    }
    if (type == "directory") {
        const bool existed = QFileInfo::exists(path);
        if (!QDir().mkpath(path)) {
            status(s, fid, "EPERM");
            return;
        }
        if (!existed)
            s.createdDirs.push_back(path);
        file->complete = true;
        s.files.emplace(fid, std::move(file));
        status(s, fid, "OK");
        return;
    }
    if (type == "regular") {
        struct stat st { };
        if (::lstat(QFile::encodeName(path).constData(), &st) == 0 && !S_ISREG(st.st_mode)) {
            status(s, fid, "EINVAL");
            return;
        }
        file->output = std::make_unique<QTemporaryFile>(
            QFileInfo(path).absolutePath() + QStringLiteral("/.afterglow-transfer-XXXXXX"));
        if (!file->output->open()) {
            status(s, fid, "EPERM");
            return;
        }
    }
    const auto zip = f.value("zip", "none");
    if (zip != "none" && zip != "zlib") {
        status(s, fid, "EINVAL");
        return;
    }
    if (zip == "zlib" && type == "regular") {
        if (inflateInit(&file->stream) != Z_OK) {
            status(s, fid, "ENOMEM");
            return;
        }
        file->compressed = true;
    }
    std::unique_ptr<QTemporaryFile> signature;
    if (f.value("tt") == "rsync" && type == "regular" && QFileInfo::exists(path)) {
        file->basis = std::make_unique<QFile>();
        const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        struct stat basisStat { };
        const bool regular = fd >= 0 && ::fstat(fd, &basisStat) == 0 && S_ISREG(basisStat.st_mode);
        const bool opened = regular && file->basis->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle);
        if (!opened && fd >= 0)
            ::close(fd);
        if (opened) {
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
                if (block.isEmpty() || thread.isInterruptionRequested() || cancellationEpoch.load() != epoch) {
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
    const bool rsync = file->rsync;
    s.files.emplace(fid, std::move(file));
    status(s, fid, "STARTED", -1, { { "tt", rsync ? "rsync" : "simple" } });
    if (signature)
        stream(s, fid, std::move(signature), false);
}

bool FileTransfer::Worker::rawWrite(File& file, const QByteArray& bytes)
{
    if (thread.isInterruptionRequested())
        return false;
    if (file.output) {
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
        if (bytes.size() > 4096 || file.pending.size() + bytes.size() > MaxQueuedBytes) {
            file.failed = true;
            status(s, fid, "EFBIG");
            return;
        }
        file.pending += bytes;
        if (f.value("ac") == "end_data") {
            file.complete = true;
            delta(s, fid, file);
        }
        return;
    }
    bool ok = bytes.size() <= 4096;
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
                && write(file, QByteArray(buffer, sizeof buffer - file.stream.avail_out));
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
        ok = write(file, bytes);
    const bool end = f.value("ac") == "end_data";
    if (end && file.compressed && !file.ended)
        ok = false;
    if (end && file.rsync && (!file.hashSeen || !file.pending.isEmpty()))
        ok = false;
    if (end && file.meta.contains("sz") && file.meta.value("sz").toLongLong() >= 0 && file.output)
        ok &= file.written == file.meta.value("sz").toLongLong();
    if (!ok) {
        file.failed = true;
        file.output.reset();
        status(s, fid, "EIO");
        return;
    }
    file.complete = end;
    if (end && file.output) {
        if (!file.output->flush() || !setMetadata(file.output->fileName(), file.meta, false)
            || ::rename(
                QFile::encodeName(file.output->fileName()).constData(), QFile::encodeName(file.path).constData())) {
            file.failed = true;
            status(s, fid, errorName());
            file.output.reset();
            return;
        }
        file.output->setAutoRemove(false);
        file.committed = true;
    }
    if (!end || file.output)
        status(s, fid, end ? "OK" : "PROGRESS", file.written);
    if (end)
        commitLinks(s, false);
}

void FileTransfer::Worker::finish(Session& s)
{
    commitLinks(s, true);
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

void FileTransfer::Worker::commitLinks(Session& s, bool final)
{
    // Link targets can name files that arrive later in the session.
    std::set<QByteArray> pending;
    for (const auto& [fid, file] : s.files)
        if (file->complete && !file->failed && !file->committed
            && (file->meta.value("ft") == "symlink" || file->meta.value("ft") == "link"))
            pending.insert(fid);
    for (std::size_t pass = 0; !pending.empty() && pass <= s.files.size(); ++pass) {
        bool progress = false;
        for (auto it = pending.begin(); it != pending.end();) {
            auto& file = *s.files.at(*it);
            const bool hard = file.meta.value("ft") == "link";
            QByteArray targetId = hard ? (file.linkData.startsWith("fid:") ? file.linkData.mid(4) : file.linkData)
                : file.linkData.startsWith("fid_abs:") ? file.linkData.mid(8)
                : file.linkData.startsWith("fid:")     ? file.linkData.mid(4)
                                                       : QByteArray();
            QString target;
            if (!targetId.isEmpty()) {
                auto targetFile = s.files.find(targetId);
                if (targetFile == s.files.end() || !targetFile->second->complete || targetFile->second->failed) {
                    if (!final && (targetFile == s.files.end() || !targetFile->second->complete)) {
                        ++it;
                        continue;
                    }
                    file.failed = true;
                    status(s, *it, "ENOENT");
                    it = pending.erase(it);
                    continue;
                }
                if (pending.contains(targetId)) {
                    ++it;
                    continue;
                }
                target = targetFile->second->path;
                if (!hard && file.linkData.startsWith("fid:"))
                    target = QDir(QFileInfo(file.path).absolutePath()).relativeFilePath(target);
            } else if (!hard && file.linkData.startsWith("path:"))
                target = QString::fromUtf8(file.linkData.mid(5));
            if (target.isEmpty() || target.contains(QChar(0))) {
                file.failed = true;
                status(s, *it, "EINVAL");
                it = pending.erase(it);
                continue;
            }
            if (QString::fromUtf8(file.linkData).toUtf8() != file.linkData) {
                file.failed = true;
                status(s, *it, "EINVAL");
                it = pending.erase(it);
                continue;
            }
            const QString temporary = QFileInfo(file.path).absolutePath() + QStringLiteral("/.afterglow-link-")
                + QUuid::createUuid().toString(QUuid::Id128);
            const auto source = QFile::encodeName(target), dest = QFile::encodeName(temporary);
            const int result
                = hard ? ::link(source.constData(), dest.constData()) : ::symlink(source.constData(), dest.constData());
            if (result) {
                file.failed = true;
                status(s, *it, errorName());
            } else if (::rename(dest.constData(), QFile::encodeName(file.path).constData())) {
                file.failed = true;
                status(s, *it, errorName());
                ::unlink(dest.constData());
            } else if (!setMetadata(file.path, file.meta, !hard)) {
                file.failed = true;
                status(s, *it, errorName());
            } else {
                file.committed = true;
                status(s, *it, "OK", file.written);
            }
            it = pending.erase(it);
            progress = true;
        }
        if (!progress)
            break;
    }
    if (final)
        for (const auto& id : pending) {
            s.files.at(id)->failed = true;
            status(s, id, "ELOOP");
        }
}

void FileTransfer::Worker::list(Session& s)
{
    const auto epoch = cancellationEpoch.load();
    struct Entry {
        QString path;
        QByteArray request, parent;
        int depth;
    };
    std::deque<Entry> pending;
    for (const auto& f : s.requests)
        pending.push_back({ pathOf(f.value("n")), f.value("fid"), { }, 0 });
    std::map<std::pair<quint64, quint64>, QByteArray> inodes;
    while (!pending.empty() && !thread.isInterruptionRequested() && cancellationEpoch.load() == epoch) {
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
        if (s.listing.size() >= MaxFiles || entry.depth > 128) {
            status(s, entry.request, "EFBIG");
            break;
        }
        Fields f { { "ac", "file" }, { "id", s.id }, { "fid", entry.request }, { "n", entry.path.toUtf8() },
            { "st", QByteArray::number(s.listing.size() + 1) }, { "prm", QByteArray::number(st.st_mode & 07777) },
            { "mod", QByteArray::number(qint64(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec) },
            { "sz", QByteArray::number(st.st_size) }, { "pr", entry.parent } };
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
                if (pending.size() + s.listing.size() >= MaxFiles) {
                    status(s, entry.request, "EFBIG");
                    break;
                }
                pending.push_back({ children.next(), entry.request, f["st"], entry.depth + 1 });
            }
        }
    }
    qsizetype listingBytes = 0;
    for (auto& [path, f] : s.listing) {
        listingBytes += encode(f).size();
        if (listingBytes > MaxQueuedBytes / 2) {
            status(s, { }, "EFBIG");
            break;
        }
        if (f.value("ft") == "symlink") {
            const QString target = QFileInfo(path).symLinkTarget();
            if (auto it = s.listing.find(target); it != s.listing.end())
                f["d"] = it->second.value("st");
        }
        send(f);
    }
    status(s, { }, "OK", -1, { { "n", QDir::homePath().toUtf8() } });
}

void FileTransfer::Worker::stream(Session& s, const QByteArray& fid, std::unique_ptr<QFile> source, bool compress)
{
    if (streams.size() >= 128) {
        status(s, fid, "EBUSY");
        return;
    }
    auto item = std::make_unique<Stream>();
    item->fields = { { "id", s.id }, { "fid", fid } };
    item->source = std::move(source);
    if (compress) {
        if (deflateInit(&item->z, Z_DEFAULT_COMPRESSION) != Z_OK) {
            status(s, fid, "ENOMEM");
            return;
        }
        item->compressed = true;
    }
    streams.push_back(std::move(item));
    if (timer && !timer->isActive())
        timer->start(1);
}

void FileTransfer::Worker::pump()
{
    if (streams.empty())
        return;
    auto& item = *streams.front();
    QByteArray data;
    bool end = false, error = false;
    if (!item.compressed) {
        data = item.source->read(4096);
        error = item.source->error() != QFileDevice::NoError;
        end = item.source->atEnd();
    } else {
        if (item.z.avail_in == 0 && !item.eof) {
            item.input = item.source->read(4096);
            item.eof = item.source->atEnd();
            error = item.source->error() != QFileDevice::NoError;
            item.z.next_in = reinterpret_cast<Bytef*>(item.input.data());
            item.z.avail_in = uInt(item.input.size());
        }
        data.resize(4096);
        item.z.next_out = reinterpret_cast<Bytef*>(data.data());
        item.z.avail_out = 4096;
        const auto result = deflate(&item.z, item.eof ? Z_FINISH : Z_NO_FLUSH);
        data.resize(4096 - item.z.avail_out);
        end = result == Z_STREAM_END;
        error |= result != Z_OK && result != Z_STREAM_END;
    }
    auto f = item.fields;
    if (error) {
        f["ac"] = "status";
        f["st"] = "EIO";
        send(f);
        streams.pop_front();
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
        if (s.files.contains(fid)) {
            status(s, fid, "EINVAL");
            return;
        }
        auto file = std::make_unique<File>();
        file->meta = f;
        file->basis = std::move(source);
        file->rsync = true;
        s.files.emplace(fid, std::move(file));
        return;
    }
    stream(s, fid, std::move(source), zip == "zlib");
}

bool FileTransfer::Worker::write(File& file, const QByteArray& bytes)
{
    if (!file.rsync)
        return rawWrite(file, bytes);
    file.pending += bytes;
    while (!file.pending.isEmpty()) {
        const auto type = quint8(file.pending[0]);
        if (file.hashSeen)
            return false;
        qsizetype used = 0;
        if (type == 1) {
            if (file.pending.size() < 5)
                break;
            const auto size = qFromLittleEndian<quint32>(file.pending.constData() + 1);
            if (size > 16 * 1024 * 1024)
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
            for (quint64 i = 0; i < count; ++i) {
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
        file.pending.remove(0, used);
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
    if (blockSize == 0 || blockSize > 16 * 1024 * 1024) {
        status(s, fid, "EINVAL");
        return;
    }
    struct Block {
        quint64 strong, index;
    };
    std::map<quint32, std::vector<Block>> blocks;
    for (qsizetype at = 12; at <= signature.size() - 20; at += 20) {
        quint64 index = 0, strong = 0;
        quint32 weak = 0;
        for (int byte = 0; byte < 8; ++byte) {
            index |= quint64(quint8(signature[at + byte])) << (byte * 8);
            strong |= quint64(quint8(signature[at + 12 + byte])) << (byte * 8);
        }
        for (int byte = 0; byte < 4; ++byte)
            weak |= quint32(quint8(signature[at + 8 + byte])) << (byte * 8);
        blocks[weak].push_back({ strong, index });
    }
    auto output = std::make_unique<QTemporaryFile>();
    if (!output->open()) {
        status(s, fid, "EIO");
        return;
    }
    const auto epoch = cancellationEpoch.load();
    QByteArray buffer, literal;
    qsizetype position = 0;
    quint32 a = 0, b = 0;
    bool fresh = true;
    auto flushLiteral = [&] {
        if (literal.isEmpty())
            return true;
        QByteArray op(5, '\0');
        op[0] = 1;
        qToLittleEndian(quint32(literal.size()), op.data() + 1);
        XXH3_128bits_update(file.hash, literal.constData(), std::size_t(literal.size()));
        const bool ok = output->write(op) == op.size() && output->write(literal) == literal.size();
        literal.clear();
        return ok;
    };
    for (;;) {
        if (thread.isInterruptionRequested() || cancellationEpoch.load() != epoch) {
            status(s, fid, "EINTR");
            return;
        }
        if (buffer.size() - position <= blockSize && !file.basis->atEnd()) {
            buffer.remove(0, position);
            position = 0;
            buffer += file.basis->read(std::max<qint64>(65536, 2 * qint64(blockSize)));
            if (file.basis->error() != QFileDevice::NoError) {
                status(s, fid, "EIO");
                return;
            }
            fresh = true;
        }
        const auto length = std::min<qsizetype>(blockSize, buffer.size() - position);
        if (!length)
            break;
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
        const auto candidates = blocks.find(a | (b << 16));
        if (candidates != blocks.end()) {
            const auto strong = XXH3_64bits(buffer.constData() + position, std::size_t(length));
            for (const auto& block : candidates->second)
                if (block.strong == strong) {
                    match = block.index;
                    break;
                }
        }
        if (match) {
            if (!flushLiteral()) {
                status(s, fid, "EIO");
                return;
            }
            QByteArray op(9, '\0');
            qToLittleEndian(*match, op.data() + 1);
            if (output->write(op) != op.size()) {
                status(s, fid, "EIO");
                return;
            }
            XXH3_128bits_update(file.hash, buffer.constData() + position, std::size_t(length));
            position += length;
            fresh = true;
        } else {
            literal += buffer[position];
            fresh = position + length >= buffer.size();
            if (!fresh) {
                const quint32 old = quint8(buffer[position]), next = quint8(buffer[position + length]);
                a = (a - old + next) & 0xffffu;
                b = (b - quint32(length) * old + a) & 0xffffu;
            }
            ++position;
            if (literal.size() >= 65536 && !flushLiteral()) {
                status(s, fid, "EIO");
                return;
            }
        }
    }
    if (!flushLiteral()) {
        status(s, fid, "EIO");
        return;
    }
    XXH128_canonical_t hash { };
    XXH128_canonicalFromHash(&hash, XXH3_128bits_digest(file.hash));
    QByteArray final(3, '\0');
    final[0] = 2;
    qToLittleEndian(quint16(16), final.data() + 1);
    final.append(reinterpret_cast<const char*>(hash.digest), 16);
    if (output->write(final) != final.size() || !output->seek(0)) {
        status(s, fid, "EIO");
        return;
    }
    stream(s, fid, std::move(output), file.meta.value("zip") == "zlib");
    file.pending.clear();
}
} // namespace Afterglow
