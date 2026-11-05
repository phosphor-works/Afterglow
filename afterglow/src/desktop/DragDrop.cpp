// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "DragDrop.h"
#include "FileDrop.h"
#include "ProtoBytes.h"
#include <AfterglowProto/Base64.h>
#include <AfterglowProto/DragDrop.h>
#include <QBuffer>
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
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QSet>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace Afterglow {
namespace {
    using Meta = QMap<QByteArray, QByteArray>;
    // Afterglow's limits. Only the packet size comes from the protocol, and a client cannot change any of them.
    constexpr qsizetype MaxData = 64 * 1024 * 1024; // one MIME type or one remote entry
    constexpr qsizetype MaxTotal = 256 * 1024 * 1024; // cached source data, queued drop reads
    constexpr qsizetype MaxRemoteBytes = 1024LL * 1024 * 1024;
    constexpr int MaxRemoteEntries = 16384;
    constexpr int MaxRemoteUris = 1024;
    constexpr qsizetype MaxPathLength = 4096;
    constexpr qsizetype MaxLinkTarget = 4096;
    constexpr int MaxMimeTypes = 256;
    constexpr int MaxDirectoryEntries = 16384; // one listing
    constexpr int MaxListedEntries = 65536; // every listing of one drop
    constexpr std::size_t MaxDirectoryHandles = 256;
    constexpr std::size_t MaxQueuedReads = 64;
    constexpr std::size_t MaxPictures = 16;
    constexpr int MaxPictureSide = 4096;
    constexpr qsizetype MaxPacket = qsizetype(AfterglowProto::DndMaxPacket);
    constexpr qsizetype MaxCommand = qsizetype(AfterglowProto::DndMaxCommand);
    constexpr qsizetype MaxControlPayload = 128 * 1024; // reassembled MIME lists and ids
    constexpr qsizetype MaxMachineId = 128;
    constexpr qsizetype ReadChunk = MaxPacket / 4 * 3; // raw bytes whose base64 fills one packet
    constexpr std::size_t MaxInputBacklog = 1024 * 1024;
    constexpr int BacklogRetryMs = 5;
    constexpr int RequestTimeoutMs = 30000;
    constexpr int RemoteTimeoutMs = 120000;
    // Drop targets such as file managers copy dropped files after the drag ends.
    constexpr int FinishedTreeLifetimeMs = 10 * 60 * 1000;
    using ProtoBytes::bytesOf;
    using ProtoBytes::view;
    Meta metaOf(const AfterglowProto::DndMeta& meta)
    {
        Meta m;
        for (const auto& [key, value] : meta)
            m.insert(bytesOf(key), bytesOf(value));
        return m;
    }
    QByteArray machineId() { return bytesOf(AfterglowProto::dndMachineId()); }
    bool validMime(const QByteArray& mime) { return AfterglowProto::dndValidMime(view(mime)); }
    QStringList mimeList(const QByteArray& data)
    {
        QStringList result;
        for (const auto& mime : data.split(' ')) {
            if (mime.isEmpty())
                continue;
            if (!validMime(mime))
                return { };
            if (!result.contains(QString::fromLatin1(mime)))
                result.append(QString::fromLatin1(mime));
            if (result.size() > MaxMimeTypes)
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
    // Types besides text/uri-list that desktop targets resolve as local files.
    bool carriesUris(const QString& mime)
    {
        static const QStringList types { QStringLiteral("application/x-kde4-urilist"),
            QStringLiteral("x-special/gnome-copied-files"), QStringLiteral("x-special/nautilus-clipboard"),
            QStringLiteral("text/x-moz-url"), QStringLiteral("application/vnd.portal.filetransfer"),
            QStringLiteral("application/vnd.portal.files") };
        return types.contains(mime);
    }
    // A remote program can name only network URLs. Other schemes resolve on this computer.
    bool remoteScheme(const QUrl& url)
    {
        const auto scheme = url.scheme().toLower();
        return scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("ftp")
            || scheme == QLatin1String("mailto");
    }
    bool receiverType(const QByteArray& type) { return AfterglowProto::dndReceiverType(view(type)); }
    // Writes one packet without keeping a descriptor open between packets.
    bool appendFile(const QString& path, const QByteArray& bytes, bool create)
    {
        const auto raw = QFile::encodeName(path);
        const int fd = ::open(raw.constData(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT | O_EXCL : O_APPEND),
            0666);
        if (fd < 0)
            return false;
        qsizetype done = 0;
        while (done < bytes.size()) {
            const auto n = ::write(fd, bytes.constData() + done, std::size_t(bytes.size() - done));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                ::close(fd);
                return false;
            }
            done += n;
        }
        return ::close(fd) == 0;
    }
    class LazyMime : public QMimeData {
    public:
        QStringList names;
        QList<int> indices;
        std::function<QByteArray(int)> fetch;
        QStringList formats() const override { return names; }
        bool hasFormat(const QString& type) const override { return names.contains(type); }

    protected:
        QVariant retrieveData(const QString& type, QMetaType preferred) const override
        {
            const auto at = names.indexOf(type);
            if (at < 0)
                return { };
            const auto data = fetch(indices[at]);
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
struct DragDrop::State : std::enable_shared_from_this<DragDrop::State> {
    QPointer<DragDrop> owner;
    bool accepting = false, offering = false, pressed = false, gesture = false, offered = false, started = false;
    bool starting = false, dropAnnounced = false;
    int sourceAction = 0;
    bool hovering = false, dropped = false, finished = false, localSource = false, uriRead = false;
    QPoint origin;
    QByteArray receiverId, sourceId, receiverMachine, sourceMachine;
    QStringList dropMimes, sourceMimes;
    // The platform owns the drop data and can free it during the nested drop loop.
    QPointer<const QMimeData> incoming;
    int allowed = 0, chosen = 0, finalAction = 0, sourceActions = 0;
    // The program's answer to the drag over its pane: the action and the
    // types it asked for. Each drag event decides from it against what that
    // event offers, which can change while the drag moves.
    int requested = 0;
    QStringList requestedMimes;
    // Counts the program's answers, so a drag event can wait for the next one.
    std::uint64_t answers = 0;
    // A desktop learns the answer to a drag only when a drag event returns. A
    // pointer that stays still sends no other event, so the event that gives
    // the program new types waits this long for its answer.
    int answerWait = DragDrop::DefaultAnswerWaitMs;
    // The last drag event of this hover accepted the drag: the desktop has that answer.
    bool answered = false;
    void choose()
    {
        chosen = (requested == 1 || requested == 2) && (requested & allowed)
                && std::all_of(requestedMimes.begin(), requestedMimes.end(), [&](const auto& mime) { return dropMimes.contains(mime); })
            ? requested : 0;
    }
    QPointer<QWindow> window;
    QPointer<QDrag> drag;
    // Chunked messages: payloads of data types are base64 of up to MaxData bytes.
    AfterglowProto::DndAssembler chunks { std::size_t(4 * MaxData / 3 + 4), std::size_t(MaxControlPayload) };
    // Counts protocol resets, so that a loop that emits replies sees one that a reply caused.
    quint64 resets = 0;
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
    // Repeated requests for one directory share a listing.
    std::map<QString, QStringList> listings;
    int listedEntries = 0;
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
    QElapsedTimer dropActivity, sourceActivity;
    std::function<std::size_t()> backlog;
    std::function<void(int)> deliver;
    std::unique_ptr<QTemporaryDir> temporary, finishedTree;
    QTimer treeExpiry;
    struct Remote {
        QString path;
        QByteArray bytes;
        qint64 kind = -1;
        bool done = false;
    };
    std::map<QByteArray, Remote> remote;
    std::map<qint64, Directory> remoteDirs;
    std::set<int> requestedRoots;
    QList<QUrl> remoteUrls;
    int remotePending = 0;
    qsizetype remoteBytes = 0;
    explicit State(DragDrop* o)
        : owner(o)
    {
        QObject::connect(&timer, &QTimer::timeout, owner, [this] { pump(); });
        treeExpiry.setSingleShot(true);
        QObject::connect(&treeExpiry, &QTimer::timeout, owner, [this] { finishedTree.reset(); });
    }
    static qint64 integer(const Meta& m, const char* key, qint64 fallback = 0)
    {
        const auto it = m.constFind(key);
        return it == m.cend() ? fallback : AfterglowProto::parseDndInteger(view(*it)).value_or(fallback);
    }
    static int number(const Meta& m, const char* key, int fallback = 0)
    {
        return int(std::min<qint64>(integer(m, key, fallback), INT32_MAX));
    }
    bool leftButton() const { return QGuiApplication::mouseButtons() & Qt::LeftButton; }
    // The program has started a role or is sending a message.
    bool engaged() const { return accepting || offering || started || starting || temporary || chunks.active(); }
    void send(Meta m, const QByteArray& payload = { }, bool source = false) { sendRaw(std::move(m), view(payload), source); }
    void sendRaw(Meta m, std::string_view payload, bool source = false)
    {
        if (!m.contains("i")) {
            const auto id = source ? sourceId : receiverId;
            if (!id.isEmpty())
                m["i"] = id;
        }
        AfterglowProto::DndMeta meta;
        for (auto it = m.begin(); it != m.end(); ++it)
            meta.emplace(it.key().toStdString(), it.value().toStdString());
        // A refused packet resets the protocol in the reply handler. The rest
        // of the message must not follow it to the program.
        const auto generation = resets;
        for (const auto& packet : AfterglowProto::writeDnd(std::move(meta), payload)) {
            if (!owner || resets != generation)
                return;
            Q_EMIT owner->reply(bytesOf(packet));
        }
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
    // Reports an error on the channel of the role that sent the message.
    void reject(const QByteArray& type, const QByteArray& error)
    {
        if (receiverType(type))
            dropError({ }, error);
        else
            sourceError(error);
    }
    void clearDrop()
    {
        incoming = nullptr;
        hovering = dropped = finished = uriRead = false;
        dropUris.clear();
        dropMimes.clear();
        directories.clear();
        listings.clear();
        listedEntries = 0;
        reads.clear();
        readBytes = 0;
        timer.stop();
        chosen = finalAction = requested = 0;
        requestedMimes.clear();
        answered = false;
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
        remoteUrls.clear();
        remotePending = 0;
        remoteBytes = 0;
        temporary.reset();
    }
    void reset()
    {
        ++resets;
        if (started)
            QDrag::cancel();
        accepting = offering = pressed = false;
        clearDrop();
        clearSource();
        // The rest of a message that was being put together belongs to the
        // protocol that ended: its chunks must not start new messages.
        chunks.abandon();
        receiverId.clear();
        sourceId.clear();
        receiverMachine.clear();
        sourceMachine.clear();
    }
    bool spin(const std::function<bool()>& done, int timeout = RequestTimeoutMs)
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
        // A slow reader (SSH) must not fill the PTY input queue.
        if (backlog && backlog() > MaxInputBacklog) {
            timer.start(BacklogRetryMs);
            return;
        }
        if (timer.interval() != 0)
            timer.start(0);
        auto& read = reads.front();
        QByteArray fromFile;
        std::string_view bytes;
        if (read.file) {
            fromFile = read.file->read(ReadChunk);
            if (read.file->error() != QFileDevice::NoError) {
                const auto m = read.meta;
                dropError(m, "EIO");
                return;
            }
            bytes = view(fromFile);
        } else {
            bytes = view(read.bytes).substr(std::size_t(read.offset), std::size_t(ReadChunk));
            read.offset += qsizetype(bytes.size());
        }
        if (!bytes.empty()) {
            // The meta is copied before the reply handlers run: they can clear the queue.
            sendRaw(read.meta, AfterglowProto::base64Encode(bytes));
            dropActivity.restart();
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
        if (localSource) {
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
            char buf[MaxLinkTarget];
            const auto n = ::readlink(raw.constData(), buf, sizeof buf);
            if (n < 0 || n == sizeof buf) {
                dropError(m, "EIO");
                return;
            }
            m["X"] = "1";
            queue(m, QByteArray(buf, n));
        } else if (S_ISDIR(st.st_mode)) {
            if (directories.size() >= MaxDirectoryHandles) {
                dropError(m, "ENOMEM");
                return;
            }
            auto listing = listings.find(path);
            if (listing == listings.end()) {
                if (!QDir(path).isReadable()) {
                    dropError(m, "EPERM");
                    return;
                }
                QDirIterator iterator(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
                QStringList entries;
                while (iterator.hasNext()) {
                    iterator.next();
                    const auto info = iterator.fileInfo();
                    if (!info.isFile() && !info.isDir() && !info.isSymLink())
                        continue;
                    if (entries.size() >= MaxDirectoryEntries || listedEntries + entries.size() >= MaxListedEntries) {
                        dropError(m, "ENOMEM");
                        return;
                    }
                    entries.append(info.fileName());
                }
                listedEntries += int(entries.size());
                listing = listings.emplace(path, entries).first;
            }
            QByteArray names;
            for (const auto& entry : listing->second) {
                if (!names.isEmpty())
                    names += '\0';
                names += entry.toUtf8();
            }
            const int handle = nextHandle++;
            directories.emplace(handle, Directory { path, listing->second });
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
        if (reads.size() >= MaxQueuedReads) {
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
            const auto path = localFilePath(dropUris[y - 1]);
            if (path.isEmpty()) {
                dropError(m, "EUNKNOWN");
                return;
            }
            file(m, path);
            return;
        }
        auto bytes = incoming->data(mime);
        if (bytes.size() > MaxData) {
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
        if (index != int(pictures.size()) || pictures.size() >= MaxPictures) {
            sourceError("EINVAL");
            return;
        }
        QImage image;
        if (kind == 24 || kind == 32) {
            const int stride = kind / 8;
            if (w <= 0 || h <= 0 || w > MaxPictureSide || h > MaxPictureSide || qint64(w) * h * stride != bytes.size()) {
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
            if (reader.size().width() > MaxPictureSide || reader.size().height() > MaxPictureSide) {
                sourceError("EFBIG");
                return;
            }
            image = reader.read();
        } else if (kind == 0) {
            if (bytes.size() > MaxPacket || w < 0 || h < 0) {
                sourceError("EFBIG");
                return;
            }
            QFont font = QGuiApplication::font();
            const double scale = h > 0 && w > 0 ? double(w) / h : 1.0;
            // A font sized in pixels reports no point size.
            if (font.pointSizeF() > 0)
                font.setPointSizeF(std::clamp(font.pointSizeF() * scale, 1.0, 256.0));
            else
                font.setPixelSize(int(std::lround(std::clamp(font.pixelSize() * scale, 1.0, 340.0))));
            const QString text = QString::fromUtf8(bytes).replace(QLatin1Char('\n'), QLatin1Char(' '));
            QFontMetrics metrics(font);
            const QSize size(std::min(MaxPictureSide, metrics.horizontalAdvance(text) + 8), metrics.height() + 8);
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
        // A reset during the drag ended the protocol: nothing may reach the
        // PTY, where the shell would read it as typed input.
        if (!started)
            return { };
        if (sourceAction && !leftButton())
            announceDrop();
        send({ { "t", "e" }, { "x", "1" }, { "y", QByteArray::number(index) } }, { }, true);
        if (!complete.contains(index) && !failed.contains(index)) {
            send({ { "t", "e" }, { "x", "5" }, { "y", QByteArray::number(index) } }, { }, true);
            sourceActivity.restart();
            spin(
                [&] {
                    return complete.contains(index) || failed.contains(index) || failed.contains(-1) || !started
                        || sourceActivity.elapsed() > RequestTimeoutMs;
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
        // Set when the start was queued; a second start is rejected until this one returns.
        struct Done {
            bool& flag;
            ~Done() { flag = false; }
        } done { starting };
        if (!offered || !gesture || !pressed || !window || !leftButton()) {
            sourceError("EPERM");
            return;
        }
        // Only a positive match proves a local source. A missing id is remote.
        const auto local = machineId();
        const bool remoteSource = sourceMachine.isEmpty() || local.isEmpty() || sourceMachine != local;
        if (remoteSource && !prepareRemote())
            return;
        if (!offered || !pressed || !window || !leftButton()) {
            sourceError("EPERM");
            return;
        }
        auto* operation = new QDrag(window);
        drag = operation;
        auto* mime = new LazyMime;
        for (int n = 0; n < sourceMimes.size(); ++n)
            if (!remoteSource || !carriesUris(sourceMimes[n])) {
                mime->names.append(sourceMimes[n]);
                mime->indices.append(n);
            }
        // The platform can ask for data after the pane, and this state, are gone.
        mime->fetch = [weak = weak_from_this()](int index) {
            const auto state = weak.lock();
            return state ? state->fetch(index) : QByteArray();
        };
        operation->setMimeData(mime);
        if (!pictures.empty())
            operation->setPixmap(pictures.front());
        QObject::connect(operation, &QDrag::actionChanged, owner, [this](Qt::DropAction action) {
            if (!started)
                return;
            sourceAction = int(action) & 3;
            send({ { "t", "e" }, { "x", "2" }, { "o", QByteArray::number(int(action) & 3) } }, { }, true);
        });
        started = true;
        send({ { "t", "E" } }, "OK", true);
        // A refused reply resets the protocol synchronously.
        if (!started) {
            operation->deleteLater();
            drag.clear();
            return;
        }
        QTimer dropMonitor;
        QObject::connect(&dropMonitor, &QTimer::timeout, owner, [this] {
            if (sourceAction && !leftButton())
                announceDrop();
        });
        dropMonitor.start(10);
        const auto action = operation->exec(Qt::DropActions(sourceActions), Qt::CopyAction);
        if (action != Qt::IgnoreAction)
            announceDrop();
        // A reset during exec() cancelled the drag and ended the protocol.
        if (started)
            send({ { "t", "e" }, { "x", "4" }, { "y", action == Qt::IgnoreAction ? "1" : "0" } }, { }, true);
        if (drag)
            drag->deleteLater();
        drag.clear();
        pressed = false;
        if (temporary) {
            finishedTree = std::move(temporary);
            treeExpiry.start(FinishedTreeLifetimeMs);
        }
        clearSource();
    }
    // For the data types (p, e and k) the payload is decoded already. It is
    // taken by value, so that a large one is freed when dispatch ends.
    void dispatch(Meta m, QByteArray payload, bool end);
};
void DragDrop::State::remoteData(const Meta& m, const QByteArray& bytes, bool end)
{
    const int root = number(m, "x"), child = number(m, "y");
    const qint64 parent = integer(m, "Y"), kind = integer(m, "X");
    // A root entry has exactly one key, so every entry is announced and counted.
    if (!temporary || started || !requestedRoots.contains(root) || (parent == 0 && child != 0)) {
        sourceError("EINVAL");
        return;
    }
    const auto key = QByteArray::number(root) + ':' + QByteArray::number(parent) + ':' + QByteArray::number(child);
    auto found = remote.find(key);
    if (found == remote.end()) {
        if (remote.size() >= std::size_t(MaxRemoteEntries)) {
            sourceError("EMFILE");
            return;
        }
        QString path;
        if (parent) {
            const auto dir = remoteDirs.find(parent);
            if (dir == remoteDirs.end() || child < 1 || child > dir->second.entries.size()) {
                sourceError("EINVAL");
                return;
            }
            path = QDir(dir->second.path).filePath(dir->second.entries[child - 1]);
        } else {
            if (root < 1 || root > remoteUrls.size()) {
                sourceError("EINVAL");
                return;
            }
            QString name = QFileInfo(QDir::cleanPath(remoteUrls[root - 1].toLocalFile())).fileName();
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
        if (path.size() > MaxPathLength) {
            sourceError("ENAMETOOLONG");
            return;
        }
        Remote item;
        item.path = path;
        item.kind = kind;
        if (kind == 0 && !appendFile(path, { }, true)) {
            sourceError("EIO");
            return;
        }
        found = remote.emplace(key, std::move(item)).first;
    }
    auto& entry = found->second;
    if (entry.done) {
        sourceError("EINVAL");
        return;
    }
    remoteBytes += bytes.size();
    if (remoteBytes > MaxRemoteBytes) {
        sourceError("EFBIG");
        return;
    }
    if (entry.kind == 0) {
        if (!bytes.isEmpty() && !appendFile(entry.path, bytes, false)) {
            sourceError("EIO");
            return;
        }
    } else {
        // A link holds one target. A listing holds names of at most 255 bytes.
        const qsizetype limit = entry.kind == 1 ? MaxLinkTarget : qsizetype(MaxDirectoryEntries) * 256;
        if (entry.bytes.size() + bytes.size() > limit) {
            sourceError("EFBIG");
            return;
        }
        entry.bytes += bytes;
    }
    if (!end)
        return;
    if (remotePending <= 0) {
        sourceError("EINVAL");
        return;
    }
    if (entry.kind == 1) {
        const QString target = QString::fromUtf8(entry.bytes);
        if (target.isEmpty() || target.toUtf8() != entry.bytes || target.contains(QChar(0))
            || QDir::isAbsolutePath(target) || !linkStaysInside(entry.path, entry.bytes, temporary->path())) {
            sourceError("EPERM");
            return;
        }
        if (::symlink(entry.bytes.constData(), QFile::encodeName(entry.path).constData())) {
            sourceError("EIO");
            return;
        }
    } else if (entry.kind != 0) {
        if (remoteDirs.contains(entry.kind) || remoteDirs.size() >= MaxDirectoryHandles || !QDir().mkdir(entry.path)) {
            sourceError("EINVAL");
            return;
        }
        QStringList names;
        QSet<QString> seen;
        if (!entry.bytes.isEmpty())
            for (const auto& raw : entry.bytes.split('\0')) {
                const QString name = QString::fromUtf8(raw);
                if (name.isEmpty() || raw.size() > 255 || name.toUtf8() != raw || name.contains(QLatin1Char('/'))
                    || name == QLatin1String(".") || name == QLatin1String("..") || seen.contains(name)) {
                    sourceError("EINVAL");
                    return;
                }
                seen.insert(name);
                names.append(name);
                if (names.size() + remotePending > MaxRemoteEntries) {
                    sourceError("EMFILE");
                    return;
                }
            }
        remoteDirs.emplace(entry.kind, Directory { entry.path, names });
        remotePending += int(names.size());
    }
    entry.bytes = QByteArray();
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
    if (urls.size() > MaxRemoteUris) {
        sourceError("EMFILE");
        return false;
    }
    for (const auto& url : urls)
        if (!url.isLocalFile() && !remoteScheme(url)) {
            sourceError("EPERM");
            return false;
        }
    finishedTree.reset();
    treeExpiry.stop();
    temporary = std::make_unique<QTemporaryDir>();
    if (!temporary->isValid()) {
        sourceError("EIO");
        return false;
    }
    remoteUrls = urls;
    for (int n = 0; n < urls.size(); ++n)
        if (urls[n].isLocalFile()) {
            requestedRoots.insert(n + 1);
            ++remotePending;
            send({ { "t", "k" }, { "x", QByteArray::number(n + 1) } }, { }, true);
        }
    sourceActivity.restart();
    spin(
        [&] {
            return remotePending == 0 || failed.contains(-1) || !offered || !pressed
                || sourceActivity.elapsed() > RemoteTimeoutMs;
        },
        0);
    if (failed.contains(-1) || !offered)
        return false;
    if (!pressed) {
        sourceError("EPERM");
        return false;
    }
    if (remotePending) {
        sourceError("ETIMEDOUT");
        return false;
    }
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
void DragDrop::State::dispatch(Meta m, QByteArray payload, bool end)
{
    const auto type = m.value("t", "a");
    if (type == "a") {
        if (m.contains("i"))
            receiverId = m.value("i");
        else if (number(m, "x") != 1)
            receiverId.clear();
        if (number(m, "x") == 1) {
            if (payload.size() > MaxMachineId) {
                dropError(m, "EINVAL");
                return;
            }
            receiverMachine = payload;
        } else
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
        ++answers;
        // An answer without types takes any of them.
        requested = number(m, "o");
        requestedMimes = payload.isEmpty() ? QStringList() : mimeList(payload);
        // Types that are not valid names: the program accepts nothing.
        if (!payload.isEmpty() && requestedMimes.isEmpty())
            requested = 0;
        choose();
        return;
    }
    if (type == "r") {
        dropActivity.restart();
        request(m);
        return;
    }
    if (type == "o") {
        if (m.contains("i"))
            sourceId = m.value("i");
        else if (number(m, "x") == 1)
            sourceId.clear();
        if (number(m, "x") == 1) {
            if (payload.size() > MaxMachineId) {
                sourceError("EINVAL");
                return;
            }
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
        // The offer that a start or a remote copy uses must not change under it.
        if (!gesture || !pressed || started || starting || temporary || !leftButton()) {
            sourceError("EPERM");
            return;
        }
        // A new offer replaces the state of an earlier one, including its errors.
        clearSource();
        gesture = true;
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
            if (started || starting || temporary) {
                sourceError("EINVAL");
                return;
            }
            // Leave the parser callback before entering the platform drag loop.
            starting = true;
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
            return;
        }
        const int index = number(m, "y");
        if (index < 0 || index >= sourceMimes.size()) {
            sourceError("EINVAL");
            return;
        }
        failed.insert(index);
        return;
    }
    if (type == "p" || type == "e" || type == "k") {
        if ((type == "p" && (!offered || started || temporary)) || (type == "e" && !started)
            || (type == "k" && !temporary)) {
            sourceError("EINVAL");
            return;
        }
        // handle() decoded the payload.
        const auto& bytes = payload;
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
void DragDrop::reset()
{
    d->reset();
    // A terminal reset also ends a message that was being dropped: the next packet starts a new one.
    d->chunks.reset();
}
void DragDrop::end() { d->reset(); }
void DragDrop::abort(Lost lost, bool reset)
{
    // No OSC 72 packet and no reset was lost: the protocol is whole.
    if (lost == Lost::Nothing)
        return;
    if (lost == Lost::Reset) {
        this->reset();
        return;
    }
    const auto state = d;
    // A lost reset ended the drop and the drag before the lost packets came:
    // the program waits for no answer about them.
    if (!reset && state->dropped)
        state->dropError({ }, "ENOMEM");
    if (!reset && (state->offered || state->started))
        state->sourceError("ENOMEM");
    state->reset();
    // The pane drops commands when the program writes faster than they are
    // handled, nearly always in the middle of a large chunked message, and
    // the gap can hold its first packet. So the packets after the gap are
    // taken as the rest of that message, up to the one with m=0. Read as new
    // messages, chunks without a t would be t=a: they would enable drops, or
    // with x=1 make base64 data the receiver's machine id. A gap that ends
    // on a message boundary costs one message.
    state->chunks.discard(true);
}
void DragDrop::setInputBacklog(std::function<std::size_t()> backlog) { d->backlog = std::move(backlog); }
void DragDrop::setCommandDelivery(std::function<void(int)> deliver) { d->deliver = std::move(deliver); }
void DragDrop::setAnswerWait(int ms) { d->answerWait = std::max(0, ms); }
bool DragDrop::acceptsDrops() const { return d->accepting; }
bool DragDrop::offersDrags() const { return d->offering; }
bool DragDrop::acceptsDrag() const { return d->hovering && !d->dropped && d->answered; }
QByteArray DragDrop::queryReply(const QByteArray& command) { return bytesOf(AfterglowProto::dndQueryReply(view(command))); }
void DragDrop::handle(const QByteArray& command)
{
    const auto state = d;
    const auto [raw, payload] = AfterglowProto::splitDnd(view(command));
    const auto parsed = AfterglowProto::parseDndMeta(raw);
    const bool overlong = command.size() > MaxCommand;
    if (overlong || qsizetype(payload.size()) > MaxPacket) {
        // A chunk of a message that failed earlier gets no second error. A
        // program that uses no role and sends no message gets none either:
        // the command can be part of a binary file that it prints, and kitty
        // also drops an overlong escape code without a reply.
        if ((overlong || !state->chunks.discarding()) && state->engaged()) {
            auto type = bytesOf(AfterglowProto::dndType(raw));
            if (type.isEmpty())
                type = bytesOf(state->chunks.type());
            state->reject(type, "EFBIG");
        }
        state->chunks.discard(AfterglowProto::dndMore(raw));
        if (overlong)
            state->reset();
        return;
    }
    if (!parsed) {
        state->reset();
        // The packet's own m tells whether chunks follow it, also when the
        // message it belongs to was not being put together.
        state->chunks.discard(AfterglowProto::dndMore(raw));
        return;
    }
    const auto found = parsed->find("t");
    const auto type = found == parsed->end() ? std::string() : found->second;
    if (type == "q" || type == "Q") {
        // The PTY thread answers queries first. This path serves direct callers.
        const auto response = queryReply(command);
        if (!response.isEmpty())
            Q_EMIT reply(response);
        return;
    }
    const bool continued = state->chunks.active();
    // A continuation has the type of its first chunk.
    const auto messageType = continued ? bytesOf(state->chunks.type()) : type.empty() ? QByteArray("a") : bytesOf(type);
    if (type == "e" || type == "k" || (continued && !receiverType(messageType)))
        state->sourceActivity.restart();
    if (continued && receiverType(messageType))
        state->dropActivity.restart();
    AfterglowProto::DndAssembler::Message message;
    switch (state->chunks.feed(*parsed, payload, message)) {
    case AfterglowProto::DndAssembler::Result::Pending:
    case AfterglowProto::DndAssembler::Result::Dropped:
        return;
    case AfterglowProto::DndAssembler::Result::Mismatch:
        state->reset();
        return;
    case AfterglowProto::DndAssembler::Result::TooLarge:
        state->reject(messageType, "EFBIG");
        return;
    case AfterglowProto::DndAssembler::Result::Ready: {
        const auto kind = message.meta.find("t");
        if (kind == message.meta.end() || !AfterglowProto::dndDataType(kind->second)) {
            state->dispatch(metaOf(message.meta), bytesOf(message.payload), message.emptyEnd);
            return;
        }
        // A data message holds up to 64 MiB, as about 85 MiB of base64. The
        // base64 is decoded where the assembler put it, and freed before
        // the decoded bytes are copied for dispatch.
        auto decoded = AfterglowProto::base64Decode(message.payload, AfterglowProto::Base64Check::StrictOrUnpadded);
        std::string().swap(message.payload);
        if (!decoded) {
            state->sourceError("EINVAL");
            return;
        }
        auto bytes = bytesOf(*decoded);
        decoded.reset();
        state->dispatch(metaOf(message.meta), std::move(bytes), message.emptyEnd);
        return;
    }
    }
}
void DragDrop::drop(QDropEvent* event, bool final, QPoint cell, QPoint pixel)
{
    const auto state = d;
    // An enter or move event runs no event loop: a drag event handled inside it would
    // answer before it, and Qt takes the older answer last, so the compositor
    // would refuse a drop that the program accepted. The answers that the
    // program has sent already are taken first.
    if (state->deliver)
        state->deliver(0);
    if (!state->accepting || state->dropped) {
        event->ignore();
        return;
    }
    // Other desktop clients name these types. They reach the program inside an OSC.
    QStringList names;
    for (const auto& format : event->mimeData()->formats())
        if (names.size() < MaxMimeTypes && validMime(format.toUtf8()) && !names.contains(format))
            names.append(format);
    const bool changed = !state->hovering || names != state->dropMimes;
    if (!state->hovering) {
        state->clearDrop();
        state->hovering = true;
    }
    state->incoming = event->mimeData();
    state->allowed = int(event->possibleActions()) & 3;
    state->dropMimes = names;
    state->choose();
    Meta m { { "t", final ? "M" : "m" }, { "x", QByteArray::number(std::max(0, cell.x())) },
        { "y", QByteArray::number(std::max(0, cell.y())) }, { "X", QByteArray::number(std::max(0, pixel.x())) },
        { "Y", QByteArray::number(std::max(0, pixel.y())) }, { "o", QByteArray::number(state->allowed) } };
    if (final) {
        if (!state->chosen) {
            event->ignore();
            // Qt sends no leave event after a drop.
            state->send({ { "t", "m" }, { "x", "-1" }, { "y", "-1" } });
            state->clearDrop();
            return;
        }
        state->dropped = true;
        // Any drag from this process, from any window, names files that its program chose.
        state->localSource = event->source() != nullptr;
        state->dropActivity.start();
    }
    state->send(m, final || changed ? names.join(QLatin1Char(' ')).toUtf8() : QByteArray());
    if (!final && changed && state->deliver) {
        // Only the program's commands arrive during the wait, so no other
        // drag event is handled before this one returns.
        const auto answers = state->answers;
        QElapsedTimer waited;
        waited.start();
        while (state->answers == answers && state->accepting && state->hovering && waited.elapsed() < state->answerWait)
            state->deliver(int(state->answerWait - waited.elapsed()));
        if (!state->accepting || !state->hovering) {
            event->ignore();
            return;
        }
    }
    if (final) {
        state->spin(
            [&] {
                return state->finished || !state->accepting || !state->dropped
                    || state->dropActivity.elapsed() > RequestTimeoutMs;
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
        state->answered = true;
    } else {
        event->ignore();
        state->answered = false;
    }
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
    if (d->offering && !d->started && !d->starting) {
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
