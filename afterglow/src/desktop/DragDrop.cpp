// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "DragDrop.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QDrag>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMessageAuthenticationCode>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <algorithm>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace Afterglow {
namespace {
    using Meta = QMap<QByteArray, QByteArray>;
    constexpr qsizetype MaxData = 64 * 1024 * 1024;
    constexpr qsizetype MaxTotal = 256 * 1024 * 1024;
    QByteArray machineId()
    {
        QFile file(QStringLiteral("/etc/machine-id"));
        if (!file.open(QIODevice::ReadOnly))
            return { };
        return "1:"
            + QMessageAuthenticationCode::hash(
                file.readAll().trimmed(), "tty-dnd-protocol-machine-id", QCryptographicHash::Sha256)
                  .toHex();
    }
    QStringList mimeList(const QByteArray& data)
    {
        QStringList result;
        for (const auto& mime : data.split(' ')) {
            if (mime.isEmpty())
                continue;
            if (!mime.contains('/') || mime.size() > 255)
                return { };
            for (unsigned char c : mime)
                if (c <= 32 || c >= 127)
                    return { };
            if (!result.contains(QString::fromLatin1(mime)))
                result.append(QString::fromLatin1(mime));
            if (result.size() > 256)
                return { };
        }
        return result;
    }
    QList<QUrl> uris(const QByteArray& data)
    {
        QList<QUrl> result;
        for (auto line : data.split('\n')) {
            line = line.trimmed();
            if (line.isEmpty() || line.startsWith('#'))
                continue;
            result.push_back(QUrl::fromEncoded(line, QUrl::StrictMode));
        }
        return result;
    }
    class LazyMime : public QMimeData {
    public:
        QStringList names;
        std::function<QByteArray(int)> fetch;
        QStringList formats() const override { return names; }
        bool hasFormat(const QString& type) const override { return names.contains(type); }

    protected:
        QVariant retrieveData(const QString& type, QMetaType preferred) const override
        {
            const auto index = names.indexOf(type);
            if (index < 0)
                return { };
            const auto data = fetch(int(index));
            if (type == QLatin1String("text/uri-list") && preferred.id() == QMetaType::QVariantList) {
                QVariantList list;
                for (const auto& url : uris(data))
                    list.append(url);
                return list;
            }
            if (preferred.id() == QMetaType::QString)
                return QString::fromUtf8(data);
            return data;
        }
    };
} // namespace
struct DragDrop::State {
    QPointer<DragDrop> owner;
    bool accepting = false, offering = false, pressed = false, gesture = false, offered = false, started = false;
    bool dropAnnounced = false;
    int sourceAction = 0;
    bool hovering = false, dropped = false, finished = false, sameWindow = false, uriRead = false;
    QPoint origin;
    QByteArray receiverId, sourceId, receiverMachine, sourceMachine;
    QStringList dropMimes, sourceMimes, preferred;
    const QMimeData* incoming = nullptr;
    int allowed = 0, chosen = 0, finalAction = 0, sourceActions = 0;
    QPointer<QWindow> window;
    QPointer<QDrag> drag;
    Meta chunks;
    QByteArray chunkData;
    std::map<int, QByteArray> data;
    std::set<int> complete, failed;
    std::vector<QPixmap> pictures;
    qsizetype total = 0, pictureBytes = 0;
    Meta imageMeta;
    int imageIndex = 0;
    struct Directory {
        QString path;
        QStringList entries;
    };
    std::map<int, Directory> directories;
    int nextHandle = 2;
    QList<QUrl> dropUris;
    struct Read {
        Meta meta;
        std::unique_ptr<QFile> file;
        QByteArray bytes;
        qsizetype offset = 0;
    };
    std::deque<Read> reads;
    qsizetype readBytes = 0;
    QTimer timer;
    QElapsedTimer activity;
    std::unique_ptr<QTemporaryDir> temporary;
    struct Remote {
        QString path;
        std::unique_ptr<QFile> file;
        QByteArray bytes;
        qint64 kind = -1;
        bool done = false;
    };
    std::map<QByteArray, Remote> remote;
    std::map<qint64, Directory> remoteDirs;
    std::set<int> requestedRoots;
    int remotePending = 0;
    qsizetype remoteBytes = 0;
    explicit State(DragDrop* o)
        : owner(o)
    {
        QObject::connect(&timer, &QTimer::timeout, owner, [this] { pump(); });
    }
    static int number(const Meta& m, const char* key, int fallback = 0)
    {
        return m.contains(key) ? int(std::min<qint64>(m.value(key).toLongLong(), INT32_MAX)) : fallback;
    }
    void send(Meta m, const QByteArray& payload = { }, bool source = false)
    {
        if (!m.contains("i") && m.value("t") != "q" && m.value("t") != "Q") {
            const auto id = source ? sourceId : receiverId;
            if (!id.isEmpty())
                m["i"] = id;
        }
        qsizetype offset = 0;
        do {
            const auto part = payload.mid(offset, 4096);
            offset += part.size();
            m["m"] = offset < payload.size() ? "1" : "0";
            QByteArray bytes("\x1b]72;");
            bool first = true;
            for (auto it = m.begin(); it != m.end(); ++it) {
                if (!first)
                    bytes += ':';
                first = false;
                bytes += it.key() + '=' + it.value();
            }
            bytes += ';';
            bytes += part;
            bytes += "\x1b\\";
            if (owner)
                Q_EMIT owner->reply(bytes);
        } while (offset < payload.size());
    }
    void sourceError(const QByteArray& error)
    {
        send({ { "t", "E" } }, error, true);
        if (started)
            QDrag::cancel();
        else
            clearSource();
        offered = false;
        gesture = false;
        failed.insert(-1);
    }
    void dropError(Meta m, const QByteArray& error)
    {
        m["t"] = "R";
        send(m, error);
        if (dropped) {
            finished = true;
            finalAction = 0;
            reads.clear();
            timer.stop();
        }
    }
    void clearDrop()
    {
        incoming = nullptr;
        hovering = dropped = finished = uriRead = false;
        dropUris.clear();
        dropMimes.clear();
        preferred.clear();
        directories.clear();
        reads.clear();
        readBytes = 0;
        timer.stop();
        chosen = finalAction = 0;
        nextHandle = 2;
    }
    void clearSource()
    {
        offered = started = gesture = false;
        dropAnnounced = false;
        sourceAction = 0;
        data.clear();
        complete.clear();
        failed.clear();
        pictures.clear();
        total = pictureBytes = 0;
        imageIndex = 0;
        imageMeta.clear();
        remote.clear();
        remoteDirs.clear();
        requestedRoots.clear();
        remotePending = 0;
        remoteBytes = 0;
        temporary.reset();
    }
    void reset()
    {
        if (started)
            QDrag::cancel();
        accepting = offering = pressed = false;
        clearDrop();
        clearSource();
        chunks.clear();
        chunkData.clear();
        receiverId.clear();
        sourceId.clear();
        receiverMachine.clear();
        sourceMachine.clear();
    }
    bool spin(const std::function<bool()>& done, int timeout = 30000)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!done() && (timeout <= 0 || elapsed.elapsed() < timeout)) {
            QEventLoop loop;
            QTimer::singleShot(5, &loop, &QEventLoop::quit);
            loop.exec();
        }
        return done();
    }
    void queue(Meta m, QByteArray bytes)
    {
        if (readBytes + bytes.size() > MaxTotal) {
            dropError(m, "ENOMEM");
            return;
        }
        readBytes += bytes.size();
        reads.push_back({ std::move(m), { }, std::move(bytes), 0 });
        timer.start(0);
    }
    void pump()
    {
        if (reads.empty() || !dropped || finished) {
            timer.stop();
            return;
        }
        auto& read = reads.front();
        QByteArray bytes;
        if (read.file) {
            bytes = read.file->read(3072);
            if (read.file->error() != QFileDevice::NoError) {
                const auto m = read.meta;
                dropError(m, "EIO");
                return;
            }
        } else {
            bytes = read.bytes.mid(read.offset, 3072);
            read.offset += bytes.size();
        }
        if (!bytes.isEmpty()) {
            send(read.meta, bytes.toBase64());
            activity.restart();
        } else {
            const auto meta = read.meta;
            readBytes -= read.bytes.size();
            reads.pop_front();
            // Reply handlers may reset the protocol synchronously.
            send(meta);
        }
    }
    void file(Meta m, const QString& path)
    {
        if (sameWindow) {
            dropError(m, "EPERM");
            return;
        }
        struct stat st { };
        const auto raw = QFile::encodeName(path);
        if (::lstat(raw.constData(), &st)) {
            dropError(m, errno == ENOENT ? "ENOENT" : "EPERM");
            return;
        }
        m["X"] = "0";
        if (S_ISLNK(st.st_mode)) {
            char buf[4096];
            const auto n = ::readlink(raw.constData(), buf, sizeof buf);
            if (n < 0 || n == sizeof buf) {
                dropError(m, "EIO");
                return;
            }
            m["X"] = "1";
            queue(m, QByteArray(buf, n));
        } else if (S_ISDIR(st.st_mode)) {
            if (directories.size() >= 256) {
                dropError(m, "ENOMEM");
                return;
            }
            QDir dir(path);
            if (!dir.isReadable()) {
                dropError(m, "EPERM");
                return;
            }
            QDirIterator iterator(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
            QStringList entries;
            QByteArray names;
            while (iterator.hasNext()) {
                iterator.next();
                const auto info = iterator.fileInfo();
                if (!info.isFile() && !info.isDir() && !info.isSymLink())
                    continue;
                if (entries.size() >= 16384) {
                    dropError(m, "ENOMEM");
                    return;
                }
                if (!entries.isEmpty())
                    names += '\0';
                names += info.fileName().toUtf8();
                entries.append(info.fileName());
            }
            const int handle = nextHandle++;
            directories.emplace(handle, Directory { path, entries });
            m["X"] = QByteArray::number(handle);
            queue(m, names);
        } else if (S_ISREG(st.st_mode)) {
            const int fd = ::open(raw.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                dropError(m, "EPERM");
                return;
            }
            struct stat actual { };
            if (::fstat(fd, &actual) || !S_ISREG(actual.st_mode)) {
                ::close(fd);
                dropError(m, "EINVAL");
                return;
            }
            auto stream = std::make_unique<QFile>();
            if (!stream->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
                ::close(fd);
                dropError(m, "EIO");
                return;
            }
            reads.push_back({ m, std::move(stream), { }, 0 });
            timer.start(0);
        } else
            dropError(m, "EINVAL");
    }
    void request(const Meta& m)
    {
        if (!dropped || finished || !incoming) {
            dropError(m, "EPERM");
            return;
        }
        if (reads.size() >= 64) {
            dropError(m, "EMFILE");
            return;
        }
        const int x = number(m, "x"), y = number(m, "y"), parent = number(m, "Y");
        if (parent) {
            auto it = directories.find(parent);
            if (it == directories.end()) {
                dropError(m, "EINVAL");
                return;
            }
            if (!x) {
                directories.erase(it);
                return;
            }
            if (x < 1 || x > it->second.entries.size()) {
                dropError(m, "ENOENT");
                return;
            }
            file(m, QDir(it->second.path).filePath(it->second.entries[x - 1]));
            return;
        }
        if (!x) {
            finalAction = number(m, "o");
            if ((finalAction != 1 && finalAction != 2) || !(allowed & finalAction))
                finalAction = 0;
            finished = true;
            reads.clear();
            return;
        }
        if (x < 1 || x > dropMimes.size()) {
            dropError(m, "EINVAL");
            return;
        }
        const auto mime = dropMimes[x - 1];
        if (y) {
            if (mime != QLatin1String("text/uri-list") || !uriRead) {
                dropError(m, "EINVAL");
                return;
            }
            if (y < 1 || y > dropUris.size()) {
                dropError(m, "ENOENT");
                return;
            }
            if (!dropUris[y - 1].isLocalFile()) {
                dropError(m, "EUNKNOWN");
                return;
            }
            file(m, dropUris[y - 1].toLocalFile());
            return;
        }
        auto bytes = incoming->data(mime);
        if (bytes.size() > MaxTotal) {
            dropError(m, "ENOMEM");
            return;
        }
        Meta response = m;
        if (mime == QLatin1String("text/uri-list")) {
            uriRead = true;
            dropUris = uris(bytes);
            if (!receiverMachine.isEmpty() && receiverMachine != machineId())
                response["X"] = "1";
        }
        queue(response, bytes);
    }
    void picture(const Meta& m, const QByteArray& bytes)
    {
        const qint64 index = -qint64(number(m, "x")) - 1;
        const int kind = number(m, "y"), w = number(m, "X"), h = number(m, "Y");
        if (index != int(pictures.size()) || pictures.size() >= 16) {
            sourceError("EINVAL");
            return;
        }
        QImage image;
        if (kind == 24 || kind == 32) {
            const int stride = kind / 8;
            if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || qint64(w) * h * stride != bytes.size()) {
                sourceError("EINVAL");
                return;
            }
            image = QImage(reinterpret_cast<const uchar*>(bytes.constData()), w, h, w * stride,
                kind == 24 ? QImage::Format_RGB888 : QImage::Format_RGBA8888)
                        .copy();
        } else if (kind == 100) {
            QBuffer buffer;
            buffer.setData(bytes);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "PNG");
            if (reader.size().width() > 4096 || reader.size().height() > 4096) {
                sourceError("EFBIG");
                return;
            }
            image = reader.read();
        } else if (kind == 0) {
            if (bytes.size() > 4096 || w < 0 || h < 0) {
                sourceError("EFBIG");
                return;
            }
            QFont font = QGuiApplication::font();
            const double scale = h > 0 && w > 0 ? double(w) / h : 1.0;
            font.setPointSizeF(std::clamp(font.pointSizeF() * scale, 1.0, 256.0));
            const QString text = QString::fromUtf8(bytes).replace(QLatin1Char('\n'), QLatin1Char(' '));
            QFontMetrics metrics(font);
            const QSize size(std::min(4096, metrics.horizontalAdvance(text) + 8), metrics.height() + 8);
            image = QImage(size, QImage::Format_ARGB32_Premultiplied);
            image.fill(QColor(0, 0, 0, std::clamp(number(m, "o"), 0, 1024) * 255 / 1024));
            QPainter painter(&image);
            painter.setFont(font);
            painter.setPen(Qt::white);
            painter.drawText(image.rect().adjusted(4, 4, -4, -4), Qt::AlignCenter, text);
        }
        if (image.isNull()) {
            sourceError("EINVAL");
            return;
        }
        if (pictureBytes + image.sizeInBytes() > MaxData) {
            sourceError("EFBIG");
            return;
        }
        pictureBytes += image.sizeInBytes();
        pictures.push_back(QPixmap::fromImage(image));
    }
    void announceDrop()
    {
        if (started && !dropAnnounced) {
            dropAnnounced = true;
            send({ { "t", "e" }, { "x", "3" } }, { }, true);
        }
    }
    QByteArray fetch(int index)
    {
        if (sourceAction && !(QGuiApplication::mouseButtons() & Qt::LeftButton))
            announceDrop();
        send({ { "t", "e" }, { "x", "1" }, { "y", QByteArray::number(index) } }, { }, true);
        if (!complete.contains(index) && !failed.contains(index)) {
            send({ { "t", "e" }, { "x", "5" }, { "y", QByteArray::number(index) } }, { }, true);
            activity.restart();
            spin(
                [&] {
                    return complete.contains(index) || failed.contains(index) || failed.contains(-1) || !started
                        || activity.elapsed() > 30000;
                },
                0);
            if (started && !complete.contains(index) && !failed.contains(index) && !failed.contains(-1))
                sourceError("ETIMEDOUT");
        }
        return complete.contains(index) ? data[index] : QByteArray();
    }
    void remoteData(const Meta& m, const QByteArray& bytes, bool end);
    bool prepareRemote();
    void start()
    {
        if (!offered || !gesture || !pressed || !window || !(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
            sourceError("EPERM");
            return;
        }
        if (!sourceMachine.isEmpty() && sourceMachine != machineId() && !prepareRemote())
            return;
        if (!pressed || !(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
            sourceError("EPERM");
            return;
        }
        auto* operation = new QDrag(window);
        drag = operation;
        auto* mime = new LazyMime;
        mime->names = sourceMimes;
        mime->fetch = [this](int index) { return fetch(index); };
        operation->setMimeData(mime);
        if (!pictures.empty())
            operation->setPixmap(pictures.front());
        QObject::connect(operation, &QDrag::actionChanged, owner, [this](Qt::DropAction action) {
            sourceAction = int(action) & 3;
            send({ { "t", "e" }, { "x", "2" }, { "o", QByteArray::number(int(action) & 3) } }, { }, true);
        });
        started = true;
        send({ { "t", "E" } }, "OK", true);
        QTimer dropMonitor;
        QObject::connect(&dropMonitor, &QTimer::timeout, owner, [this] {
            if (sourceAction && !(QGuiApplication::mouseButtons() & Qt::LeftButton))
                announceDrop();
        });
        dropMonitor.start(10);
        const auto action = operation->exec(Qt::DropActions(sourceActions), Qt::CopyAction);
        if (action != Qt::IgnoreAction)
            announceDrop();
        send({ { "t", "e" }, { "x", "4" }, { "y", action == Qt::IgnoreAction ? "1" : "0" } }, { }, true);
        if (drag)
            drag->deleteLater();
        drag.clear();
        pressed = false;
        clearSource();
    }
    void dispatch(Meta m, const QByteArray& payload);
};
void DragDrop::State::remoteData(const Meta& m, const QByteArray& bytes, bool end)
{
    const int root = number(m, "x"), child = number(m, "y");
    const qint64 parent = m.value("Y").toLongLong(), kind = m.value("X").toLongLong();
    if (!temporary || !requestedRoots.contains(root)) {
        sourceError("EINVAL");
        return;
    }
    const auto key = QByteArray::number(root) + ':' + QByteArray::number(parent) + ':' + QByteArray::number(child);
    auto found = remote.find(key);
    if (found == remote.end()) {
        QString path;
        if (parent) {
            const auto dir = remoteDirs.find(parent);
            if (dir == remoteDirs.end() || child < 1 || child > dir->second.entries.size()) {
                sourceError("EINVAL");
                return;
            }
            path = QDir(dir->second.path).filePath(dir->second.entries[child - 1]);
        } else {
            const int uriIndex = int(sourceMimes.indexOf(QStringLiteral("text/uri-list")));
            const auto urls = uris(data[uriIndex]);
            if (root < 1 || root > urls.size()) {
                sourceError("EINVAL");
                return;
            }
            QString name = QFileInfo(QDir::cleanPath(urls[root - 1].toLocalFile())).fileName();
            if (name.isEmpty())
                name = QStringLiteral("directory");
            if (name == QLatin1String(".") || name == QLatin1String("..") || name.contains(QLatin1Char('/'))
                || name.contains(QChar(0))) {
                sourceError("EINVAL");
                return;
            }
            const auto base = temporary->filePath(QString::number(root));
            QDir().mkpath(base);
            path = QDir(base).filePath(name);
        }
        if (remote.size() >= 16384 || path.size() > 4096) {
            sourceError("EMFILE");
            return;
        }
        Remote item;
        item.path = path;
        item.kind = kind;
        if (kind == 0) {
            item.file = std::make_unique<QFile>(path);
            if (!item.file->open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                sourceError("EIO");
                return;
            }
        }
        found = remote.emplace(key, std::move(item)).first;
    }
    auto& entry = found->second;
    if (entry.done) {
        sourceError("EINVAL");
        return;
    }
    remoteBytes += bytes.size();
    if (remoteBytes > 1024LL * 1024 * 1024) {
        sourceError("EFBIG");
        return;
    }
    if (entry.file) {
        if (entry.file->write(bytes) != bytes.size()) {
            sourceError("EIO");
            return;
        }
    } else {
        if (entry.bytes.size() + bytes.size() > MaxData) {
            sourceError("EFBIG");
            return;
        }
        entry.bytes += bytes;
    }
    if (!end)
        return;
    if (entry.file) {
        if (!entry.file->flush()) {
            sourceError("EIO");
            return;
        }
        entry.file->close();
    } else if (entry.kind == 1) {
        const QString target = QString::fromUtf8(entry.bytes);
        const QString resolved = QDir::cleanPath(QDir(QFileInfo(entry.path).absolutePath()).filePath(target));
        if (target.isEmpty() || target.toUtf8() != entry.bytes || target.contains(QChar(0))
            || QDir::isAbsolutePath(target) || !resolved.startsWith(temporary->path() + QLatin1Char('/'))) {
            sourceError("EPERM");
            return;
        }
        if (::symlink(entry.bytes.constData(), QFile::encodeName(entry.path).constData())) {
            sourceError("EIO");
            return;
        }
    } else {
        if (remoteDirs.contains(entry.kind) || remoteDirs.size() >= 256 || !QDir().mkdir(entry.path)) {
            sourceError("EINVAL");
            return;
        }
        QStringList names;
        if (!entry.bytes.isEmpty())
            for (const auto& raw : entry.bytes.split('\0')) {
                const QString name = QString::fromUtf8(raw);
                if (name.isEmpty() || raw.size() > 255 || name.toUtf8() != raw || name.contains(QLatin1Char('/'))
                    || name == QLatin1String(".") || name == QLatin1String("..") || names.contains(name)) {
                    sourceError("EINVAL");
                    return;
                }
                names.append(name);
                if (names.size() + remotePending > 16384) {
                    sourceError("EMFILE");
                    return;
                }
            }
        remoteDirs.emplace(entry.kind, Directory { entry.path, names });
        remotePending += int(names.size());
    }
    entry.done = true;
    --remotePending;
}
bool DragDrop::State::prepareRemote()
{
    const int index = int(sourceMimes.indexOf(QStringLiteral("text/uri-list")));
    if (index < 0)
        return true;
    if (!complete.contains(index)) {
        sourceError("EINVAL");
        return false;
    }
    const auto urls = uris(data[index]);
    if (urls.size() > 1024) {
        sourceError("EMFILE");
        return false;
    }
    temporary = std::make_unique<QTemporaryDir>();
    if (!temporary->isValid()) {
        sourceError("EIO");
        return false;
    }
    for (int n = 0; n < urls.size(); ++n)
        if (urls[n].isLocalFile()) {
            requestedRoots.insert(n + 1);
            ++remotePending;
            send({ { "t", "k" }, { "x", QByteArray::number(n + 1) } }, { }, true);
        }
    activity.restart();
    spin([&] { return remotePending == 0 || failed.contains(-1) || !offered || activity.elapsed() > 120000; }, 0);
    if (remotePending && offered && !failed.contains(-1)) {
        sourceError("ETIMEDOUT");
        return false;
    }
    if (failed.contains(-1) || !offered)
        return false;
    QByteArray replacement;
    for (int n = 0; n < urls.size(); ++n) {
        QUrl url = urls[n];
        if (requestedRoots.contains(n + 1)) {
            const auto it = remote.find(QByteArray::number(n + 1) + ":0:0");
            if (it == remote.end() || !it->second.done) {
                sourceError("EIO");
                return false;
            }
            url = QUrl::fromLocalFile(
                it->second.path + ((it->second.kind != 0 && it->second.kind != 1) ? QStringLiteral("/") : QString()));
        }
        replacement += url.toEncoded() + "\r\n";
    }
    data[index] = replacement;
    return true;
}
void DragDrop::State::dispatch(Meta m, const QByteArray& payload)
{
    const auto type = m.value("t", "a");
    if (type == "q" || type == "Q") {
        Meta response { { "t", type } };
        if (m.contains("i"))
            response["i"] = m.value("i");
        send(response);
        return;
    }
    if (type == "a") {
        if (m.contains("i"))
            receiverId = m.value("i");
        else if (number(m, "x") != 1)
            receiverId.clear();
        if (number(m, "x") == 1)
            receiverMachine = payload;
        else
            accepting = true;
        return;
    }
    if (type == "A") {
        accepting = false;
        clearDrop();
        return;
    }
    if (type == "m") {
        if (!hovering || dropped)
            return;
        const int action = number(m, "o");
        chosen = (action == 1 || action == 2) && (action & allowed) ? action : 0;
        if (!payload.isEmpty()) {
            preferred = mimeList(payload);
            if (preferred.isEmpty() || std::any_of(preferred.begin(), preferred.end(), [&](const auto& mime) {
                    return !dropMimes.contains(mime);
                }))
                chosen = 0;
        }
        return;
    }
    if (type == "r") {
        activity.restart();
        request(m);
        return;
    }
    if (type == "o") {
        if (m.contains("i"))
            sourceId = m.value("i");
        else if (number(m, "x") == 1)
            sourceId.clear();
        if (number(m, "x") == 1) {
            offering = true;
            sourceMachine = payload;
            return;
        }
        if (number(m, "x") == 2) {
            offering = false;
            if (started)
                QDrag::cancel();
            else
                clearSource();
            return;
        }
        if (!gesture || !pressed || started || !(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
            sourceError("EPERM");
            return;
        }
        sourceMimes = mimeList(payload);
        sourceActions = number(m, "o");
        if (sourceMimes.isEmpty() || sourceActions < 1 || sourceActions > 3) {
            sourceError("EINVAL");
            return;
        }
        offered = true;
        return;
    }
    if (type == "P") {
        if (number(m, "x") == -1) {
            // Leave the parser callback before entering the platform drag loop.
            if (started) {
                sourceError("EINVAL");
                return;
            }
            const auto state = owner->d;
            QTimer::singleShot(0, owner, [state] { state->start(); });
        } else if (drag) {
            const int index = number(m, "x");
            drag->setPixmap(index >= 0 && index < int(pictures.size()) ? pictures[std::size_t(index)] : QPixmap());
        }
        return;
    }
    if (type == "E") {
        if (!started && !temporary) {
            sourceError("EINVAL");
            return;
        }
        if (!m.contains("y") || number(m, "y") == -1) {
            failed.insert(-1);
            offered = false;
            if (started)
                QDrag::cancel();
        } else
            failed.insert(number(m, "y"));
        return;
    }
    if (type == "p" || type == "e" || type == "k") {
        if ((type == "p" && (!offered || started)) || (type == "e" && !started) || (type == "k" && !temporary)) {
            sourceError("EINVAL");
            return;
        }
        const auto decoded = QByteArray::fromBase64Encoding(payload, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            sourceError("EINVAL");
            return;
        }
        const auto bytes = decoded.decoded;
        const bool end = payload.isEmpty() || m.value("_end") == "1";
        if (type == "k") {
            remoteData(m, bytes, end);
            return;
        }
        const int index = number(m, type == "p" ? "x" : "y");
        if ((index >= sourceMimes.size()) || (index < 0 && type != "p") || complete.contains(index)) {
            sourceError("EINVAL");
            return;
        }
        if (index < 0 && imageIndex != index) {
            if (imageIndex != 0 || -qint64(index) != int(pictures.size()) + 1) {
                sourceError("EINVAL");
                return;
            }
            imageIndex = index;
            imageMeta = m;
        }
        total += bytes.size();
        if (total > MaxTotal || data[index].size() + bytes.size() > MaxData) {
            sourceError("EFBIG");
            return;
        }
        data[index] += bytes;
        if (end) {
            complete.insert(index);
            if (index < 0) {
                picture(imageMeta, data[index]);
                data.erase(index);
                imageIndex = 0;
            }
        }
    }
}
DragDrop::DragDrop(QObject* parent)
    : QObject(parent)
    , d(std::make_shared<State>(this))
{
}
DragDrop::~DragDrop() { d->reset(); }
void DragDrop::reset() { d->reset(); }
void DragDrop::abort()
{
    const auto state = d;
    if (state->dropped)
        state->dropError({ }, "ENOMEM");
    if (state->offered || state->started)
        state->sourceError("ENOMEM");
    state->reset();
}
bool DragDrop::acceptsDrops() const { return d->accepting; }
bool DragDrop::offersDrags() const { return d->offering; }
QByteArray DragDrop::queryReply(const QByteArray& command)
{
    const auto meta = command.first(command.indexOf(';') < 0 ? command.size() : command.indexOf(';'));
    QByteArray type, id;
    for (const auto& field : meta.split(':')) {
        if (field.startsWith("t="))
            type = field.mid(2);
        if (field.startsWith("i=")) {
            bool ok;
            const auto n = field.mid(2).toLongLong(&ok);
            if (!ok || n < INT32_MIN || n > UINT32_MAX)
                return { };
            id = field.mid(2);
        }
    }
    if (type != "q" && type != "Q")
        return { };
    return QByteArray("\x1b]72;t=") + type + (id.isEmpty() ? QByteArray() : ":i=" + id) + ";\x1b\\";
}
void DragDrop::handle(const QByteArray& command)
{
    const auto state = d;
    if (command.size() > 8192) {
        state->reset();
        return;
    }
    const auto separator = command.indexOf(';');
    const auto raw = separator < 0 ? command : command.left(separator);
    const auto payload = separator < 0 ? QByteArray() : command.mid(separator + 1);
    if (payload.size() > 4096) {
        state->sourceError("EFBIG");
        return;
    }
    Meta m;
    for (const auto& field : raw.split(':')) {
        const auto pos = field.indexOf('=');
        if (pos != 1)
            continue;
        const auto key = field.left(pos), value = field.mid(pos + 1);
        if (!QByteArray("tixyXYom").contains(key))
            continue;
        if (m.contains(key)) {
            state->reset();
            return;
        }
        if (key == "t") {
            if (value.size() != 1) {
                state->reset();
                return;
            }
        } else {
            bool ok;
            const auto n = value.toLongLong(&ok);
            if (!ok || n < INT32_MIN || n > UINT32_MAX) {
                state->reset();
                return;
            }
        }
        m[key] = value;
    }
    if (m.value("t") == "q" || m.value("t") == "Q") {
        state->dispatch(m, payload);
        return;
    }
    if (m.value("t") == "e" || m.value("t") == "k" || !state->chunks.isEmpty())
        state->activity.restart();
    if (!state->chunks.isEmpty()) {
        if (m.contains("i") && m.value("i") != state->chunks.value("i")) {
            state->reset();
            return;
        }
        if (state->chunkData.size() + payload.size() > 4 * MaxData / 3 + 4) {
            state->sourceError("EFBIG");
            state->chunks.clear();
            state->chunkData.clear();
            return;
        }
        state->chunkData += payload;
        if (m.value("m") != "1") {
            auto meta = std::move(state->chunks);
            auto bytes = std::move(state->chunkData);
            state->chunks.clear();
            state->chunkData.clear();
            if (payload.isEmpty())
                meta["_end"] = "1";
            state->dispatch(meta, bytes);
        }
    } else if (m.value("m") == "1") {
        state->chunks = m;
        state->chunkData = payload;
    } else
        state->dispatch(m, payload);
}
void DragDrop::drop(QDropEvent* event, bool final, QPoint cell, QPoint pixel, QWindow* targetWindow)
{
    const auto state = d;
    if (!state->accepting || state->dropped) {
        event->ignore();
        return;
    }
    const auto names = event->mimeData()->formats();
    const bool changed = !state->hovering || names != state->dropMimes;
    if (!state->hovering) {
        state->clearDrop();
        state->hovering = true;
    }
    state->incoming = event->mimeData();
    state->allowed = int(event->possibleActions()) & 3;
    state->dropMimes = names;
    Meta m { { "t", final ? "M" : "m" }, { "x", QByteArray::number(std::max(0, cell.x())) },
        { "y", QByteArray::number(std::max(0, cell.y())) }, { "X", QByteArray::number(std::max(0, pixel.x())) },
        { "Y", QByteArray::number(std::max(0, pixel.y())) }, { "o", QByteArray::number(state->allowed) } };
    if (final) {
        if (!state->chosen) {
            event->ignore();
            state->clearDrop();
            return;
        }
        state->dropped = true;
        state->sameWindow = false;
        auto* source = event->source();
        if (source) {
            auto* sourceWindow = qobject_cast<QWindow*>(source);
            state->sameWindow = sourceWindow && sourceWindow == targetWindow;
        }
        state->activity.start();
    }
    state->send(m, final || changed ? names.join(QLatin1Char(' ')).toUtf8() : QByteArray());
    if (!final && changed)
        state->spin([&] { return state->chosen || !state->accepting; }, 100);
    if (final) {
        state->spin(
            [&] {
                return state->finished || !state->accepting || !state->dropped || state->activity.elapsed() > 30000;
            },
            0);
        if (state->finished && state->finalAction) {
            event->setDropAction(Qt::DropAction(state->finalAction));
            event->accept();
        } else
            event->ignore();
        state->clearDrop();
    } else if (state->chosen) {
        event->setDropAction(Qt::DropAction(state->chosen));
        event->accept();
    } else
        event->ignore();
}
void DragDrop::leave()
{
    if (d->hovering && !d->dropped) {
        d->send({ { "t", "m" }, { "x", "-1" }, { "y", "-1" } });
        d->clearDrop();
    }
}
void DragDrop::press(QPoint position)
{
    if (d->offering && !d->started) {
        d->clearSource();
        d->pressed = true;
        d->origin = position;
    }
}
void DragDrop::release()
{
    d->pressed = false;
    if (!d->started)
        d->gesture = false;
}
bool DragDrop::move(QWindow* window, QPoint position, QPoint cell, QPoint pixel)
{
    if (!d->offering || !d->pressed)
        return false;
    if (!d->gesture && (position - d->origin).manhattanLength() >= QGuiApplication::styleHints()->startDragDistance()) {
        d->window = window;
        d->gesture = true;
        d->send(
            { { "t", "o" }, { "x", QByteArray::number(std::max(0, cell.x())) },
                { "y", QByteArray::number(std::max(0, cell.y())) }, { "X", QByteArray::number(std::max(0, pixel.x())) },
                { "Y", QByteArray::number(std::max(0, pixel.y())) } },
            { }, true);
    }
    return d->gesture;
}
} // namespace Afterglow
