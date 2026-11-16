// SPDX-FileCopyrightText: 2026 fuddlesworth
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
/// session's panes. The window holds none of the decoders (CLAUDE.md), and
/// both kinds of pane read the same formats and convert colour alike. One
/// for the application, started at the first image; thread-safe. Null when
/// aglow-mux is not there: images then do not show, and a warning says so
/// once.
AfterglowMux::ImageDecoder* decoder();

/// For tests whose program is not next to aglow-mux: the server to start.
/// Before the first decoder().
void setServerExecutable(std::string path);

} // namespace Afterglow::ProgramImages
