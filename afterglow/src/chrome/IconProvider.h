// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QQuickImageProvider>

namespace Afterglow {

/// Icons from the desktop's icon theme for the chrome, tinted to the
/// chrome's text colour so they follow the terminal's theme:
/// image://icon/<icon name>/<#rrggbb>.
class IconProvider : public QQuickImageProvider
{
public:
    IconProvider();
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};

} // namespace Afterglow
