// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QCursor>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPixmapCache>

namespace Afterglow {
inline QCursor protocolPointer(const QString& name, qreal scale)
{
    static const QHash<QString, Qt::CursorShape> shapes = {
        {QStringLiteral("default"), Qt::ArrowCursor}, {QStringLiteral("text"), Qt::IBeamCursor},
        {QStringLiteral("pointer"), Qt::PointingHandCursor}, {QStringLiteral("wait"), Qt::WaitCursor},
        {QStringLiteral("progress"), Qt::BusyCursor}, {QStringLiteral("crosshair"), Qt::CrossCursor},
        {QStringLiteral("move"), Qt::SizeAllCursor}, {QStringLiteral("not-allowed"), Qt::ForbiddenCursor},
        {QStringLiteral("no-drop"), Qt::ForbiddenCursor}, {QStringLiteral("alias"), Qt::DragLinkCursor},
        {QStringLiteral("copy"), Qt::DragCopyCursor}, {QStringLiteral("grab"), Qt::OpenHandCursor},
        {QStringLiteral("grabbing"), Qt::ClosedHandCursor}, {QStringLiteral("help"), Qt::WhatsThisCursor},
        {QStringLiteral("ew-resize"), Qt::SizeHorCursor}, {QStringLiteral("e-resize"), Qt::SizeHorCursor},
        {QStringLiteral("w-resize"), Qt::SizeHorCursor}, {QStringLiteral("ns-resize"), Qt::SizeVerCursor},
        {QStringLiteral("n-resize"), Qt::SizeVerCursor}, {QStringLiteral("s-resize"), Qt::SizeVerCursor},
        {QStringLiteral("col-resize"), Qt::SplitHCursor}, {QStringLiteral("row-resize"), Qt::SplitVCursor},
        {QStringLiteral("nesw-resize"), Qt::SizeBDiagCursor}, {QStringLiteral("ne-resize"), Qt::SizeBDiagCursor},
        {QStringLiteral("sw-resize"), Qt::SizeBDiagCursor}, {QStringLiteral("nwse-resize"), Qt::SizeFDiagCursor},
        {QStringLiteral("nw-resize"), Qt::SizeFDiagCursor}, {QStringLiteral("se-resize"), Qt::SizeFDiagCursor},
    };
    if (auto it = shapes.constFind(name); it != shapes.constEnd()) return QCursor(*it);
    if (name != QLatin1String("cell") && name != QLatin1String("vertical-text")
        && name != QLatin1String("zoom-in") && name != QLatin1String("zoom-out")) return QCursor(Qt::IBeamCursor);
    // Qt has no built-in shapes for these CSS cursors. Draw outlined symbols
    // that remain visible over both light and dark terminal content.
    const QString key = QStringLiteral("afterglow-pointer-%1-%2").arg(name).arg(scale);
    QPixmap pixmap;
    if (!QPixmapCache::find(key, &pixmap)) {
        pixmap = QPixmap(qCeil(28 * scale), qCeil(28 * scale));
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        for (int pass = 0; pass < 2; ++pass) {
            painter.setPen(QPen(pass ? Qt::black : Qt::white, pass ? 1.5 : 3.5, Qt::SolidLine, Qt::RoundCap));
            if (name == QLatin1String("cell")) {
                painter.drawLine(QPointF(4, 14), QPointF(24, 14));
                painter.drawLine(QPointF(14, 4), QPointF(14, 24));
            } else if (name == QLatin1String("vertical-text")) {
                painter.drawLine(QPointF(5, 14), QPointF(23, 14));
                painter.drawLine(QPointF(5, 10), QPointF(5, 18));
                painter.drawLine(QPointF(23, 10), QPointF(23, 18));
            } else {
                painter.drawEllipse(QRectF(3, 3, 15, 15));
                painter.drawLine(QPointF(16, 16), QPointF(24, 24));
                painter.drawLine(QPointF(7, 10.5), QPointF(14, 10.5));
                if (name == QLatin1String("zoom-in")) painter.drawLine(QPointF(10.5, 7), QPointF(10.5, 14));
            }
        }
        painter.end();
        QPixmapCache::insert(key, pixmap);
    }
    return QCursor(pixmap, 14, 14);
}
} // namespace Afterglow
