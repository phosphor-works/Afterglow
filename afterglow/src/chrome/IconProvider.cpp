// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "IconProvider.h"

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QStandardPaths>

namespace Afterglow {

IconProvider::IconProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
    // Outside a desktop session no theme or search path may be set; look in
    // the standard places, for Breeze and hicolor, the likeliest installed.
    QStringList paths = QIcon::themeSearchPaths();
    for (const QString& dir : QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, QStringLiteral("icons"),
                                                        QStandardPaths::LocateDirectory)) {
        if (!paths.contains(dir)) {
            paths.append(dir);
        }
    }
    QIcon::setThemeSearchPaths(paths);
    if (QIcon::themeName().isEmpty()) {
        QIcon::setThemeName(QStringLiteral("breeze"));
    }
    if (QIcon::fallbackThemeName().isEmpty()) {
        QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
    }
}

QImage IconProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize)
{
    const auto slash = id.lastIndexOf(QLatin1Char('/'));
    const QString name = slash > 0 ? id.left(slash) : id;
    const QColor tint(slash > 0 ? id.mid(slash + 1) : QString());
    const int side = requestedSize.isValid() ? std::max(requestedSize.width(), requestedSize.height()) : 16;
    const QIcon icon = QIcon::fromTheme(name);
    if (icon.isNull()) {
        *size = QSize();
        return {};
    }
    QImage img = icon.pixmap(QSize(side, side)).toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (tint.isValid()) {
        // Action icons are single-colour shapes: keep the shape, take the tint.
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), tint);
    }
    *size = img.size();
    return img;
}

} // namespace Afterglow
