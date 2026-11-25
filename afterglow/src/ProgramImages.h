// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace AfterglowMux {
class ImageDecoder;
}

namespace Afterglow::ProgramImages {

/// The decoder of the images that programs send to panes (a Kitty PNG, an
/// iTerm2 image): aglow-mux --decode-images next to afterglow, which decodes
/// each image in a process of its own with limits, as the daemon does for a
/// session's panes. No image file of a pane's program decodes in the
/// window's process, and both kinds of pane read the same formats and
/// convert colour alike. (The icon of an OSC 99 notification is another
/// thing: NotificationCenter reads it with Qt.) One for the application,
/// started at the first image; thread-safe. Null when aglow-mux cannot run:
/// images then do not show, and a warning says so once.
AfterglowMux::ImageDecoder* decoder();

/// For tests whose program is not next to aglow-mux: the server to start.
/// Before the first decoder().
void setServerExecutable(std::string path);

} // namespace Afterglow::ProgramImages
