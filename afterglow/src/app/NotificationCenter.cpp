// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "NotificationCenter.h"

#include "../Pane.h"
#include "../TerminalWindow.h"

#include <PhosphorDBus/Client.h>

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QBuffer>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QIcon>
#include <QImageReader>
#include <QLoggingCategory>
#include <QTimer>

Q_LOGGING_CATEGORY(lcNotifications, "afterglow.notifications", QtInfoMsg)

namespace {

const QString Service = QStringLiteral("org.freedesktop.Notifications");
const QString Path = QStringLiteral("/org/freedesktop/Notifications");
const QString Interface = QStringLiteral("org.freedesktop.Notifications");
const QString DesktopEntry = QStringLiteral(AFTERGLOW_APP_ID);

PhosphorDBus::Client notificationServer()
{
    return PhosphorDBus::Client(QDBusConnection::sessionBus(), Service, Path, &lcNotifications());
}

/// The image-data hint: (iiibiiay).
struct NotificationImage
{
    QImage image;
};

QDBusArgument& operator<<(QDBusArgument& arg, const NotificationImage& n)
{
    const QImage img = n.image.convertToFormat(QImage::Format_RGBA8888);
    arg.beginStructure();
    arg << img.width() << img.height() << int(img.bytesPerLine()) << true << 8 << 4
        << QByteArray(reinterpret_cast<const char*>(img.constBits()), qsizetype(img.sizeInBytes()));
    arg.endStructure();
    return arg;
}

const QDBusArgument& operator>>(const QDBusArgument& arg, NotificationImage&)
{
    return arg; // never received
}

QString fromUtf8(const std::string& s)
{
    return QString::fromUtf8(s.data(), qsizetype(s.size()));
}

// Icons from programs are decoded on the GUI thread: a small file can
// declare a huge image. Larger sides are refused before decoding.
constexpr int MaxIconSide = 2048;
constexpr int IconSide = 512; ///< what the notification gets
constexpr int IconAllocationMiB = 16;

// The formats OSC 99 icons may use, by their signature. Any other bytes would
// reach whatever image plugins are installed (SVG, TIFF) on the GUI thread.
QByteArray iconFormat(const QByteArray& bytes)
{
    if (bytes.startsWith("\x89PNG\r\n\x1a\n")) return QByteArrayLiteral("png");
    if (bytes.startsWith("\xff\xd8\xff")) return QByteArrayLiteral("jpeg");
    if (bytes.startsWith("GIF87a") || bytes.startsWith("GIF89a")) return QByteArrayLiteral("gif");
    return {};
}

QImage decodeIcon(const std::string& data)
{
    QByteArray bytes = QByteArray::fromRawData(data.data(), qsizetype(data.size()));
    const QByteArray format = iconFormat(bytes);
    if (format.isEmpty()) return {};
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, format);
    reader.setAllocationLimit(IconAllocationMiB);
    // Reads only the header.
    if (const QSize size = reader.size(); size.isValid()) {
        if (size.width() > MaxIconSide || size.height() > MaxIconSide) return {};
        if (size.width() > IconSide || size.height() > IconSide)
            reader.setScaledSize(size.scaled(IconSide, IconSide, Qt::KeepAspectRatio).expandedTo(QSize(1, 1)));
    }
    QImage image = reader.read();
    if (image.width() > IconSide || image.height() > IconSide)
        image = image.scaled(IconSide, IconSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

} // namespace

Q_DECLARE_METATYPE(NotificationImage)

namespace Afterglow {

NotificationCenter& NotificationCenter::instance()
{
    static NotificationCenter center;
    return center;
}

NotificationCenter::NotificationCenter()
{
    m_clock.start();
    qDBusRegisterMetaType<NotificationImage>();
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(Service, Path, Interface, QStringLiteral("ActionInvoked"), this, SLOT(onActionInvoked(uint,QString)));
    bus.connect(Service, Path, Interface, QStringLiteral("NotificationClosed"), this, SLOT(onClosed(uint,uint)));
    bus.connect(Service, Path, Interface, QStringLiteral("ActivationToken"), this, SLOT(onActivationToken(uint,QString)));
    // A restarted server numbers its notifications again: ids held for the
    // old one would replace or match another application's notification.
    // Only a server that goes away takes its ids with it: one that takes a
    // free name (D-Bus activation by our own Notify) answers that call with
    // a valid id, which must not be dropped.
    auto* watcher = new QDBusServiceWatcher(Service, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString&, const QString& oldOwner, const QString& newOwner) {
        if (!oldOwner.isEmpty()) forgetServer();
        // A query with no owner would start the server that just left
        // (D-Bus activation), perhaps before the one that replaces it.
        if (newOwner.isEmpty()) m_bodyMarkup = true;
        else queryCapabilities();
    });
    queryCapabilities();
}

void NotificationCenter::forgetServer()
{
    m_entries.clear();
    ++m_server;
}

void NotificationCenter::queryCapabilities()
{
    // Markup in bodies is the server's choice: one without it shows tags and
    // entities as text, so program text goes escaped only where it is read.
    m_bodyMarkup = true;
    const QDBusPendingCall call = notificationServer().asyncCall(Interface, QStringLiteral("GetCapabilities"), {});
    auto* watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        const QDBusPendingReply<QStringList> reply = *w;
        if (!reply.isError()) m_bodyMarkup = reply.value().contains(QLatin1String("body-markup"));
    });
}

std::string NotificationCenter::capabilities()
{
    return "a=focus,report:c=1:o=always,unfocused,invisible:p=title,body,?,close,alive,buttons,icon"
           ":s=system,silent,error,warn,warning,info,question:u=0,1,2:w=1";
}

void NotificationCenter::track(const Pane* pane)
{
    if (!pane || m_tracked.contains(pane)) return;
    m_tracked.insert(pane);
    connect(pane, &QObject::destroyed, this, [this, pane, prefix = QString::number(pane->id()) + QLatin1Char(':')] {
        m_tracked.remove(pane);
        m_recent.remove(pane);
        m_entries.removeIf([pane](const auto& it) { return it.value().owner == pane; });
        m_inFlight.removeIf([pane](const auto& it) { return it.key().first == pane; });
        for (auto it = m_iconCache.begin(); it != m_iconCache.end();) {
            if (it.key().startsWith(prefix)) { m_iconCacheOrder.removeAll(it.key()); it = m_iconCache.erase(it); }
            else ++it;
        }
    });
}

bool NotificationCenter::rateLimited(const Pane* pane)
{
    if (!pane || m_rateLimit <= 0) return false;
    track(pane);
    const qint64 now = m_clock.elapsed();
    QList<qint64>& recent = m_recent[pane];
    recent.removeIf([now](qint64 t) { return now - t > RateWindowMs; });
    if (recent.size() >= m_rateLimit) {
        return true;
    }
    recent.append(now);
    return false;
}

uint NotificationCenter::idFor(const Pane* pane, const QString& clientId) const
{
    if (clientId.isEmpty()) {
        return 0; // without an id nothing is ever replaced
    }
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (it->owner == pane && it->clientId == clientId) {
            return it.key();
        }
    }
    return 0;
}

void NotificationCenter::handle(Pane* pane, const AfterglowVt::NotificationRequest& r)
{
    using Kind = AfterglowVt::NotificationRequest::Kind;
    track(pane);
    const QString clientId = fromUtf8(r.id);
    const Key key{pane, clientId};
    const auto pending = clientId.isEmpty() ? m_inFlight.end() : m_inFlight.find(key);
    if (r.kind == Kind::Close) {
        if (pending != m_inFlight.end()) {
            // Applied once the server says which notification it is.
            pending->close = true;
            pending->next.reset();
        } else if (const uint id = idFor(pane, clientId)) {
            close(id);
        }
        return;
    }
    if (r.kind == Kind::Alive) {
        QStringList open;
        for (const Entry& e : std::as_const(m_entries)) {
            if (e.owner == pane && !e.clientId.isEmpty() && !open.contains(e.clientId)) {
                open.append(e.clientId);
            }
        }
        for (auto it = m_inFlight.cbegin(); it != m_inFlight.cend(); ++it) {
            if (it.key().first == pane && !it->close && !open.contains(it.key().second)) open.append(it.key().second);
        }
        const QString reply = QStringLiteral("\x1b]99;i=%1:p=alive;%2\x1b\\")
                                  .arg(clientId.isEmpty() ? QStringLiteral("0") : clientId, open.join(QLatin1Char(',')));
        pane->sendReply(reply.toStdString());
        return;
    }
    // An update of a notification already on screen adds none.
    const uint replaces = idFor(pane, clientId);
    if (pending == m_inFlight.end() && !replaces && rateLimited(pane)) {
        return;
    }

    // Icon: a theme icon by name, the app name as one, or transmitted data.
    QString iconName;
    for (const std::string& n : r.iconNames) {
        const QString name = fromUtf8(n);
        static const QHash<QString, QString> Standard = {
            {QStringLiteral("error"), QStringLiteral("dialog-error")},
            {QStringLiteral("warn"), QStringLiteral("dialog-warning")},
            {QStringLiteral("warning"), QStringLiteral("dialog-warning")},
            {QStringLiteral("info"), QStringLiteral("dialog-information")},
            {QStringLiteral("question"), QStringLiteral("dialog-question")},
            {QStringLiteral("help"), QStringLiteral("help-contents")},
            {QStringLiteral("file-manager"), QStringLiteral("system-file-manager")},
            {QStringLiteral("system-monitor"), QStringLiteral("utilities-system-monitor")},
            {QStringLiteral("text-editor"), QStringLiteral("accessories-text-editor")},
        };
        const QString themed = Standard.value(name, name);
        if (QIcon::hasThemeIcon(themed)) {
            iconName = themed;
            break;
        }
    }
    QVariantMap hints{{QStringLiteral("desktop-entry"), DesktopEntry}, {QStringLiteral("urgency"), QVariant::fromValue(uchar(r.urgency))}};
    // OSC 99 types use the freedesktop category names ("im.received", "transfer.complete").
    if (!r.types.empty()) hints.insert(QStringLiteral("category"), fromUtf8(r.types.front()));
    QImage image;
    if (!r.iconData.empty()) {
        image = decodeIcon(r.iconData);
        if (!image.isNull() && !r.iconCacheId.empty()) {
            track(pane);
            const QString key = QString::number(pane->id()) + QLatin1Char(':') + fromUtf8(r.iconCacheId);
            m_iconCache.insert(key, image);
            m_iconCacheOrder.removeAll(key);
            m_iconCacheOrder.append(key);
            while (m_iconCacheOrder.size() > 32) {
                m_iconCache.remove(m_iconCacheOrder.takeFirst());
            }
        }
    } else if (!r.iconCacheId.empty()) {
        image = m_iconCache.value(QString::number(pane->id()) + QLatin1Char(':') + fromUtf8(r.iconCacheId));
    }
    if (iconName.isEmpty() && !r.appName.empty() && QIcon::hasThemeIcon(fromUtf8(r.appName))) {
        iconName = fromUtf8(r.appName);
    }
    if (iconName.isEmpty() && !image.isNull()) {
        hints.insert(QStringLiteral("image-data"), QVariant::fromValue(NotificationImage{image}));
    }

    const QString sound = fromUtf8(r.sound);
    if (sound == QLatin1String("silent")) {
        hints.insert(QStringLiteral("suppress-sound"), true);
    } else if (sound != QLatin1String("system")) {
        static const QHash<QString, QString> Sounds = {
            {QStringLiteral("error"), QStringLiteral("dialog-error")},
            {QStringLiteral("warn"), QStringLiteral("dialog-warning")},
            {QStringLiteral("warning"), QStringLiteral("dialog-warning")},
            {QStringLiteral("info"), QStringLiteral("dialog-information")},
            {QStringLiteral("question"), QStringLiteral("dialog-question")},
        };
        hints.insert(QStringLiteral("sound-name"), Sounds.value(sound, sound));
    }

    QStringList actions{QStringLiteral("default"), QString()};
    for (std::size_t i = 0; i < r.buttons.size() && i < 8; ++i) {
        actions << QString::number(i + 1) << fromUtf8(r.buttons[i]);
    }

    Request request;
    request.entry.pane = pane;
    request.entry.owner = pane;
    request.entry.clientId = clientId;
    request.entry.focusOnActivate = r.focusOnActivate;
    request.entry.reportActivation = r.reportActivation;
    request.entry.reportClose = r.reportClose;
    request.title = fromUtf8(r.title);
    request.body = fromUtf8(r.body); // may hold newlines, which the body shows as line breaks
    request.icon = iconName;
    request.actions = actions;
    request.hints = hints;
    request.timeout = r.expireMs < 0 ? -1 : r.expireMs;
    request.expireMs = r.expireMs;
    if (pending != m_inFlight.end()) {
        // The newest update replaces any that waits, and undoes a close.
        pending->next = request;
        pending->close = false;
        return;
    }
    send(request, replaces);
}

void NotificationCenter::showSimple(Pane* pane, const QString& title, const QString& body, int urgency)
{
    if (rateLimited(pane)) {
        return;
    }
    showNotice(pane, title, body, urgency);
}

void NotificationCenter::showNotice(Pane* pane, const QString& title, const QString& body, int urgency)
{
    track(pane);
    Request request;
    request.entry.pane = pane;
    request.entry.owner = pane;
    request.title = title;
    request.body = body;
    request.actions = {QStringLiteral("default"), QString()};
    request.hints = {{QStringLiteral("desktop-entry"), DesktopEntry}, {QStringLiteral("urgency"), QVariant::fromValue(uchar(urgency))}};
    send(request, 0);
}

void NotificationCenter::send(const Request& request, uint replaces)
{
    const Key key{request.entry.owner, request.entry.clientId};
    if (!request.entry.clientId.isEmpty()) m_inFlight.insert(key, {});
    const QString body = m_bodyMarkup ? request.body.toHtmlEscaped() : request.body;
    const QDBusPendingCall call = notificationServer().asyncCall(Interface, QStringLiteral("Notify"),
        {QStringLiteral("Afterglow"), replaces, request.icon.isEmpty() ? DesktopEntry : request.icon, request.title, body,
         request.actions, request.hints, request.timeout});
    auto* watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, key, entry = request.entry, replaces, expireMs = request.expireMs, server = m_server](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        const QDBusPendingReply<uint> reply = *w;
        // The pane's state went with it. Its address may already be another
        // pane's, so the key may name that pane's entry: leave it alone.
        if (entry.owner && !entry.pane) return;
        // What the program asked for while this call was out.
        const InFlight waiting = entry.clientId.isEmpty() ? InFlight{} : m_inFlight.take(key);
        // The server changed while the call was out: its answer and REPLACES
        // name nothing now, and the new server may give them to another
        // application's notification.
        if (server != m_server) {
            if (waiting.next) send(*waiting.next, 0);
            return;
        }
        if (reply.isError()) {
            const QDBusError::ErrorType error = reply.error().type();
            if (error == QDBusError::NoReply && (waiting.next || waiting.close)) {
                // NoReply has two causes. The bus sends it for a server that left
                // without answering, whose ids then name nothing for the server
                // that follows. libdbus sends it for its own reply timeout, while
                // a slow server lives on and may have applied REPLACES. The bus
                // sends NameOwnerChanged for a server that left before it answers
                // a later call, so one round trip to the bus tells them apart.
                // Requests for the id wait for that answer.
                m_inFlight.insert(key, waiting);
                QDBusMessage ask = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                                  QStringLiteral("/org/freedesktop/DBus"),
                                                                  QStringLiteral("org.freedesktop.DBus"),
                                                                  QStringLiteral("GetNameOwner"));
                ask << Service;
                auto* owner = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(ask), this);
                connect(owner, &QDBusPendingCallWatcher::finished, this,
                        [this, key, entry, replaces, server](QDBusPendingCallWatcher* w) {
                    w->deleteLater();
                    if (entry.owner && !entry.pane) return;
                    const InFlight settled = m_inFlight.take(key);
                    const bool gone = server != m_server || w->isError();
                    // forgetServer() dropped the old ids of a server that left. When the
                    // bus could not answer, the old id goes here, or idFor() keeps two.
                    if (gone && server == m_server && replaces != 0) m_entries.remove(replaces);
                    if (settled.close) {
                        if (!gone && replaces != 0) close(replaces);
                    } else if (settled.next) {
                        send(*settled.next, gone ? 0 : replaces);
                    }
                });
                return;
            }
            // A server that is not there: its ids name nothing for the server that follows.
            const bool gone = error == QDBusError::ServiceUnknown || error == QDBusError::Disconnected;
            if (waiting.next) send(*waiting.next, gone ? 0 : replaces);
            return;
        }
        const uint id = reply.value();
        if (replaces != 0 && replaces != id) {
            m_entries.remove(replaces);
        }
        Entry current = entry;
        current.generation = ++m_generation;
        m_entries.insert(id, current);
        if (waiting.close) {
            close(id);
        } else if (waiting.next) {
            send(*waiting.next, id);
        } else if (expireMs > 0) {
            // The protocol promises closing after the time, whatever the server does.
            QTimer::singleShot(expireMs, this, [this, id, generation = current.generation] {
                if (m_entries.contains(id) && m_entries.value(id).generation == generation) {
                    close(id);
                }
            });
        }
    });
}

void NotificationCenter::close(uint id)
{
    notificationServer().fireAndForget(this, Interface, QStringLiteral("CloseNotification"), {id},
                                       QStringLiteral("close notification"));
}

void NotificationCenter::onActivationToken(uint id, const QString& token)
{
    // Sent just before ActionInvoked. Kept for that click alone: in the
    // environment it would reach every shell started later.
    if (const auto it = m_entries.find(id); it != m_entries.end()) {
        it->activationToken = token;
    }
}

void NotificationCenter::onActionInvoked(uint id, const QString& action)
{
    const auto it = m_entries.find(id);
    if (it == m_entries.end()) {
        return;
    }
    const Entry e = *it;
    it->activationToken.clear();
    const QString client = e.clientId.isEmpty() ? QStringLiteral("0") : e.clientId;
    if (action == QLatin1String("default")) {
        if (e.focusOnActivate && e.pane) {
            if (auto* window = qobject_cast<TerminalWindow*>(e.pane->parent())) {
                // Lets the window take focus on Wayland: Qt reads the token
                // from the environment when the window asks to be activated.
                if (!e.activationToken.isEmpty()) qputenv("XDG_ACTIVATION_TOKEN", e.activationToken.toUtf8());
                window->revealPane(e.pane);
                qunsetenv("XDG_ACTIVATION_TOKEN");
            }
        }
        if (e.reportActivation && e.pane) {
            e.pane->sendReply(QStringLiteral("\x1b]99;i=%1;\x1b\\").arg(client).toStdString());
        }
    } else if (bool ok = false; action.toInt(&ok) > 0 && ok && e.reportActivation && e.pane) {
        e.pane->sendReply(QStringLiteral("\x1b]99;i=%1;%2\x1b\\").arg(client, action).toStdString());
    }
}

void NotificationCenter::onClosed(uint id, uint)
{
    const auto it = m_entries.find(id);
    if (it == m_entries.end()) {
        return;
    }
    if (it->reportClose && it->pane) {
        const QString client = it->clientId.isEmpty() ? QStringLiteral("0") : it->clientId;
        it->pane->sendReply(QStringLiteral("\x1b]99;i=%1:p=close;\x1b\\").arg(client).toStdString());
    }
    m_entries.erase(it);
}

} // namespace Afterglow
