// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// The files of an iTerm2 upload (OSC 1337 RequestUpload, it2ul): a tar
// archive compressed with gzip, which goes to the program's input in base64,
// as iTerm2 sends it.
namespace Afterglow::UploadArchive {

/// The archive of @p paths, files and directories with what is in them, as
/// iTerm2 makes it: each path relative to the deepest directory that holds
/// all of them (the parent of a directory chosen alone), and written as
/// "./path" so that no name starts with '-'. A chosen link is followed, a
/// link in a chosen directory stays a link. Devices, sockets and pipes in a
/// chosen directory are left out. The setuid and setgid bits are dropped.
/// Its gzip data is at most @p maxBytes. Nothing, with @p error set to a
/// sentence for the user, past that, for an empty @p paths, for a path that
/// does not exist, for a chosen device, socket or pipe, or for a file that
/// cannot be read or that changes while it is read.
std::optional<std::string> build(const QStringList& paths, std::size_t maxBytes, QString& error);

/// What the program reads: "ok", then the archive in base64 lines of 76
/// characters, each but the last ended with a carriage return, then an
/// empty line (iTerm2's requestUploadWithFormat). it2ul reads it with `read`.
std::string answer(std::string_view archive);

/// The bytes of answer() for an archive of @p bytes.
std::size_t answerSize(std::size_t bytes);

/// The answer to a refused or cancelled request: Ctrl+C, which ends it2ul,
/// as iTerm2 sends it.
inline constexpr std::string_view Cancel = "\x03";

} // namespace Afterglow::UploadArchive
