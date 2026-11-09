// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>

class QImage;
class QString;

namespace Afterglow {

/// What a screenshot shows: one pane, the panes of its tab (the window
/// without the tab bar and the window's docked areas), or the whole window.
enum class CaptureScope { Pane, Tab, Window };

/// The answer to a screenshot: the image in device pixels, or an error and
/// its code (not_shown or failed). Called once. It can run inside a frame of
/// the window, or while the window is destroyed: it must not change the
/// window.
using CaptureReply = std::function<void(const QString& error, const QString& code, const QImage& image)>;

} // namespace Afterglow
