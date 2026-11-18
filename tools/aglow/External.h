// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// The programs that aglow icat runs when they are installed, as kitten icat
// does: ImageMagick for the formats that aglow does not read itself, and curl
// for images at http, https and ftp URLs. aglow is one static program with
// no TLS library and no such decoders. Each runs in a child with a limit on
// its CPU time and its run time, magick also on its memory. What they give
// goes through aglow's checks as any image file does: a plain PNG goes to
// the terminal as it is, and anything else through aglow's own decoder.
namespace Aglow {

/// The ImageMagick coder that reads @p bytes, from their signature, among
/// the formats that aglow sends to ImageMagick (AVIF, HEIC, JPEG XL, QOI,
/// SVG, TGA, ICO, CUR, and a BMP with JPEG or PNG data or with a run-length
/// encoding at a depth that aglow does not read), or empty. @p name, the
/// file's name, says only whether a file without a signature is a TGA.
std::string magickCoder(std::string_view bytes, std::string_view name);

/// The name of the format of a coder, for messages ("AVIF").
std::string_view magickFormat(std::string_view coder);

/// Whether magick is on PATH.
bool magickInstalled();

/// Converts @p bytes, which @p coder reads, to a PNG of their first frame
/// with magick: the data goes on standard input with the coder named, never
/// a path or a name, under a policy that allows that coder alone and no
/// delegate program, turned upright. False with @p error set when it cannot.
/// @p stop says when to give up (Ctrl+C).
bool magickToPng(std::string_view bytes, const std::string& coder, std::string& png, std::string& error,
                 const std::function<bool()>& stop);

/// Whether @p text is an http, https or ftp URL.
bool isRemoteUrl(std::string_view text);

/// The last part of the path of @p url, without its query and fragment and
/// with its %XX escapes decoded ("a.tga" of "https://host/a%2Etga?x=1"), for
/// magickCoder(). Empty when the path ends with a slash.
std::string urlFileName(std::string_view url);

/// Downloads @p url with curl into @p out, up to @p maxBytes. False with
/// @p error set when it cannot: no curl, an HTTP error, a redirect to another
/// protocol, a file over the limit, or a timeout.
bool download(const std::string& url, std::uint64_t maxBytes, std::string& out, std::string& error,
              const std::function<bool()>& stop);

} // namespace Aglow
