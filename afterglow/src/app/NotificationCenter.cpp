// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "NotificationCenter.h"

#include "../Pane.h"
#include "../TerminalWindow.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QIcon>
#include <QTimer>

namespace {

const QString Service = QStringLiteral("org.freedesktop.Notifications");
const QString Path = QStringLiteral("/org/freedesktop/Notifications");
const QString Interface = QStringLiteral("org.freedesktop.Notifications");
const QString DesktopEntry = QStringLiteral("io.github.fuddlesworth.Afterglow");

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

/// Notification servers may read body markup: program text is plain.
QString escapeMarkup(const QString& s)
{
    return s.toHtmlEscaped();
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
    qDBusRegisterMetaType<NotificationImage>();
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(Service, Path, Interface, QStringLiteral("ActionInvoked"), this, SLOT(onActionInvoked(uint,QString)));
    bus.connect(Service, Path, Interface, QStringLiteral("NotificationClosed"), this, SLOT(onClosed(uint,uint)));
    bus.connect(Service, Path, Interface, QStringLiteral("ActivationToken"), this, SLOT(onActivationToken(uint,QString)));
}

std::string NotificationCenter::capabilities()
{
    return "a=focus,report:c=1:o=always,unfocused,invisible:p=title,body,?,close,alive,buttons,icon"
           ":s=system,silent,error,warn,warning,info,question:u=0,1,2:w=1";
}

bool NotificationCenter::rateLimited(const Pane* pane)
{
    // At most 20 notifications in 10 seconds from one pane.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<qint64>& recent = m_recent[pane];
    recent.removeIf([now](qint64 t) { return now - t > 10000; });
    if (recent.size() >= 20) {
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
        if (it->pane == pane && it->clientId == clientId) {
            return it.key();
        }
    }
    return 0;
}

void NotificationCenter::handle(Pane* pane, TerminalWindow* window, const AfterglowVt::NotificationRequest& r)
{
    using Kind = AfterglowVt::NotificationRequest::Kind;
    const QString clientId = fromUtf8(r.id);
    if (r.kind == Kind::Close) {
        if (const uint id = idFor(pane, clientId)) {
            m_entries[id].closeRequested = true;
            close(id);
        }
        return;
    }
    if (r.kind == Kind::Alive) {
        QStringList open;
        for (const Entry& e : std::as_const(m_entries)) {
            if (e.pane == pane && !e.clientId.isEmpty()) {
                open.append(e.clientId);
            }
        }
        const QString reply = QStringLiteral("\x1b]99;i=%1:p=alive;%2\x1b\\")
                                  .arg(clientId.isEmpty() ? QStringLiteral("0") : clientId, open.join(QLatin1Char(',')));
        pane->sendInput(reply.toStdString());
        return;
    }
    if (rateLimited(pane)) {
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
    QImage image;
    if (!r.iconData.empty()) {
        image = QImage::fromData(reinterpret_cast<const uchar*>(r.iconData.data()), int(r.iconData.size()));
        if (!image.isNull() && image.width() > 512) {
            image = image.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        if (!image.isNull() && !r.iconCacheId.empty()) {
            const QString key = fromUtf8(r.iconCacheId);
            m_iconCache.insert(key, image);
            m_iconCacheOrder.removeAll(key);
            m_iconCacheOrder.append(key);
            while (m_iconCacheOrder.size() > 32) {
                m_iconCache.remove(m_iconCacheOrder.takeFirst());
            }
        }
    } else if (!r.iconCacheId.empty()) {
        image = m_iconCache.value(fromUtf8(r.iconCacheId));
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

    Entry entry;
    entry.pane = pane;
    entry.window = window;
    entry.clientId = clientId;
    entry.focusOnActivate = r.focusOnActivate;
    entry.reportActivation = r.reportActivation;
    entry.reportClose = r.reportClose;
    const int timeout = r.expireMs < 0 ? -1 : r.expireMs;
    notify(entry, idFor(pane, clientId), fromUtf8(r.title), escapeMarkup(fromUtf8(r.body)), iconName, actions, hints,
           timeout, r.expireMs);
}

void NotificationCenter::showSimple(Pane* pane, TerminalWindow* window, const QString& title, const QString& body,
                                    int urgency)
{
    if (pane && rateLimited(pane)) {
        return;
    }
    Entry entry;
    entry.pane = pane;
    entry.window = window;
    notify(entry, 0, title, escapeMarkup(body), QString(), {QStringLiteral("default"), QString()},
           {{QStringLiteral("desktop-entry"), DesktopEntry}, {QStringLiteral("urgency"), QVariant::fromValue(uchar(urgency))}},
           -1, -1);
}

void NotificationCenter::notify(const Entry& entry, uint replaces, const QString& title, const QString& body,
                                const QString& icon, const QStringList& actions, const QVariantMap& hints, int timeout,
                                int expireMs)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(Service, Path, Interface, QStringLiteral("Notify"));
    msg << QStringLiteral("afterglow") << replaces << icon << title << body << actions << hints << timeout;
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, entry, replaces, expireMs](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        const QDBusPendingReply<uint> reply = *w;
        if (reply.isError()) {
            return;
        }
        const uint id = reply.value();
        if (replaces != 0 && replaces != id) {
            m_entries.remove(replaces);
        }
        m_entries.insert(id, entry);
        if (expireMs > 0) {
            // The protocol promises closing after the time, whatever the server does.
            QTimer::singleShot(expireMs, this, [this, id] {
                if (m_entries.contains(id)) {
                    close(id);
                }
            });
        }
    });
}

void NotificationCenter::close(uint id)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(Service, Path, Interface, QStringLiteral("CloseNotification"));
    msg << id;
    QDBusConnection::sessionBus().asyncCall(msg);
}

void NotificationCenter::onActivationToken(uint id, const QString& token)
{
    // Lets the window take focus on Wayland when the notification is clicked.
    if (m_entries.contains(id)) {
        qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
    }
}

void NotificationCenter::onActionInvoked(uint id, const QString& action)
{
    const auto it = m_entries.constFind(id);
    if (it == m_entries.cend()) {
        return;
    }
    const Entry& e = *it;
    const QString client = e.clientId.isEmpty() ? QStringLiteral("0") : e.clientId;
    if (action == QLatin1String("default")) {
        if (e.focusOnActivate && e.window && e.pane) {
            e.window->revealPane(e.pane);
        }
        if (e.reportActivation && e.pane) {
            e.pane->sendInput(QStringLiteral("\x1b]99;i=%1;\x1b\\").arg(client).toStdString());
        }
    } else if (bool ok = false; action.toInt(&ok) > 0 && ok && e.reportActivation && e.pane) {
        e.pane->sendInput(QStringLiteral("\x1b]99;i=%1;%2\x1b\\").arg(client, action).toStdString());
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
        it->pane->sendInput(QStringLiteral("\x1b]99;i=%1:p=close;\x1b\\").arg(client).toStdString());
    }
    m_entries.erase(it);
}

} // namespace Afterglow
