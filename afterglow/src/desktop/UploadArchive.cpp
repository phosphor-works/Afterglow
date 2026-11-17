// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UploadArchive.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Zlib.h>

#include <QFile>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include <sys/stat.h>

namespace Afterglow::UploadArchive {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t Block = 512;
/// The tar data that is read in memory at most, for each byte of gzip data
/// that may come out: text compresses to about a quarter.
constexpr std::size_t TarPerGzipByte = 4;

/// An octal field of @p width bytes, the last a NUL, as ustar writes it. The
/// value fits: sizes stay far below 8 GiB (Terminal.uploadLimit).
void octal(char* field, std::size_t width, std::uint64_t value)
{
    field[width - 1] = '\0';
    for (std::size_t i = width - 1; i-- > 0;) {
        field[i] = char('0' + (value & 7));
        value >>= 3;
    }
}

struct Entry
{
    std::string name; ///< "./relative", with '/' at the end for a directory
    char type = '0'; ///< '0' a file, '2' a link, '5' a directory
    std::uint32_t mode = 0;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::string link;
    fs::path path;
};

/// A pax record "LEN key=value\n", whose length counts its own digits.
std::string paxRecord(std::string_view key, std::string_view value)
{
    const std::size_t body = key.size() + value.size() + 3; // ' ', '=', '\n'
    std::size_t length = body + 1;
    while (std::to_string(length).size() + body != length) ++length;
    return std::to_string(length) + " " + std::string(key) + "=" + std::string(value) + "\n";
}

void header(std::string& out, std::string_view name, char type, std::uint32_t mode, std::uint64_t size, std::int64_t mtime,
            std::string_view link)
{
    char block[Block] = {};
    std::memcpy(block, name.data(), std::min<std::size_t>(name.size(), 100));
    octal(block + 100, 8, mode);
    octal(block + 108, 8, 0); // uid: the user who extracts owns the files
    octal(block + 116, 8, 0);
    octal(block + 124, 12, size);
    octal(block + 136, 12, std::uint64_t(std::max<std::int64_t>(0, mtime)));
    block[156] = type;
    std::memcpy(block + 157, link.data(), std::min<std::size_t>(link.size(), 100));
    std::memcpy(block + 257, "ustar", 6);
    std::memcpy(block + 263, "00", 2);
    // The checksum counts its own field as spaces.
    std::memset(block + 148, ' ', 8);
    unsigned sum = 0;
    for (const char c : block) sum += static_cast<unsigned char>(c);
    // Six digits, a NUL and a space.
    octal(block + 148, 7, sum);
    block[155] = ' ';
    out.append(block, Block);
}

void pad(std::string& out)
{
    out.append((Block - out.size() % Block) % Block, '\0');
}

/// The entry of @p path, named @p name: nothing, with @p error set, when it cannot be read.
std::optional<Entry> entryOf(const fs::path& path, std::string name, QString& error)
{
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        error = QStringLiteral("cannot read %1: %2").arg(QFile::decodeName(path.c_str()), QString::fromLocal8Bit(std::strerror(errno)));
        return std::nullopt;
    }
    Entry entry;
    entry.path = path;
    entry.mode = std::uint32_t(st.st_mode & 07777);
    entry.mtime = st.st_mtime;
    if (S_ISDIR(st.st_mode)) {
        entry.type = '5';
        name += '/';
    } else if (S_ISLNK(st.st_mode)) {
        entry.type = '2';
        std::error_code ec;
        entry.link = fs::read_symlink(path, ec).string();
        if (ec) {
            error = QStringLiteral("cannot read the link %1").arg(QFile::decodeName(path.c_str()));
            return std::nullopt;
        }
    } else if (S_ISREG(st.st_mode)) {
        entry.size = std::uint64_t(st.st_size);
    } else {
        // A device, a socket or a pipe: tar of iTerm2 keeps them, it2ul
        // users want files.
        error = QStringLiteral("%1 is no file, directory or link").arg(QFile::decodeName(path.c_str()));
        return std::nullopt;
    }
    entry.name = std::move(name);
    return entry;
}

/// @p entry, and what a directory holds, sorted by name.
bool collect(Entry entry, std::vector<Entry>& out, QString& error)
{
    const bool directory = entry.type == '5';
    const fs::path path = entry.path;
    const std::string name = entry.name;
    out.push_back(std::move(entry));
    if (!directory) return true;
    std::error_code ec;
    std::vector<fs::path> children;
    for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) children.push_back(it->path());
    if (ec) {
        error = QStringLiteral("cannot read %1: %2").arg(QFile::decodeName(path.c_str()), QString::fromStdString(ec.message()));
        return false;
    }
    std::sort(children.begin(), children.end());
    for (const fs::path& child : children) {
        auto childEntry = entryOf(child, name + child.filename().string(), error);
        if (!childEntry || !collect(std::move(*childEntry), out, error)) return false;
    }
    return true;
}

/// The deepest directory that holds each of @p dirs.
fs::path commonAncestor(const std::vector<fs::path>& dirs)
{
    fs::path common = dirs.front();
    for (const fs::path& dir : dirs) {
        fs::path shared;
        auto a = common.begin(), b = dir.begin();
        for (; a != common.end() && b != dir.end() && *a == *b; ++a, ++b) shared /= *a;
        common = shared;
    }
    return common;
}

/// gzip data of @p data: the deflate stream of zlibCompress in a gzip frame.
std::string gzip(std::string_view data)
{
    const std::string zlib = AfterglowProto::zlibCompress(data);
    // A zlib stream: two bytes of header, the deflate data, an Adler-32.
    std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
    out.append(zlib, 2, zlib.size() - 6);
    const auto le32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out += char((v >> (8 * i)) & 0xff);
    };
    le32(AfterglowProto::crc32Of(data));
    le32(std::uint32_t(data.size()));
    return out;
}

} // namespace

std::optional<std::string> build(const QStringList& paths, std::size_t maxBytes, QString& error)
{
    if (paths.isEmpty()) {
        error = QStringLiteral("no file was chosen");
        return std::nullopt;
    }
    // As iTerm2: the directory of each file, and each directory itself.
    std::vector<fs::path> chosen, dirs;
    bool anyFile = false;
    for (const QString& p : paths) {
        const fs::path path = fs::path(QFile::encodeName(p).toStdString()).lexically_normal();
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(path, ec);
        if (ec || !fs::exists(status)) {
            error = QStringLiteral("%1 does not exist").arg(p);
            return std::nullopt;
        }
        chosen.push_back(path);
        if (fs::is_directory(status)) {
            dirs.push_back(path);
        } else {
            anyFile = true;
            dirs.push_back(path.parent_path());
        }
    }
    fs::path base = commonAncestor(dirs);
    if (!anyFile && dirs.size() == 1) base = base.parent_path();

    std::vector<Entry> entries;
    for (const fs::path& path : chosen) {
        const fs::path relative = path.lexically_relative(base);
        auto entry = entryOf(path, "./" + relative.string(), error);
        if (!entry || !collect(std::move(*entry), entries, error)) return std::nullopt;
    }

    const std::size_t maxTar = maxBytes * TarPerGzipByte;
    const QString tooLarge = QStringLiteral("the files are larger than Terminal.uploadLimit lets through: send them with aglow transfer");
    std::string tar;
    for (const Entry& entry : entries) {
        // A name or a link past 100 bytes goes in a pax header before it.
        if (entry.name.size() > 100 || entry.link.size() > 100) {
            std::string records;
            if (entry.name.size() > 100) records += paxRecord("path", entry.name);
            if (entry.link.size() > 100) records += paxRecord("linkpath", entry.link);
            header(tar, "./PaxHeaders/entry", 'x', 0644, records.size(), entry.mtime, {});
            tar += records;
            pad(tar);
        }
        header(tar, entry.name, entry.type, entry.mode, entry.size, entry.mtime, entry.link);
        if (entry.type == '0') {
            if (tar.size() + entry.size > maxTar) {
                error = tooLarge;
                return std::nullopt;
            }
            std::ifstream file(entry.path, std::ios::binary);
            const std::size_t at = tar.size();
            tar.resize(at + entry.size);
            if (!file.read(tar.data() + at, std::streamsize(entry.size)) || std::size_t(file.gcount()) != entry.size) {
                error = QStringLiteral("cannot read %1").arg(QFile::decodeName(entry.path.c_str()));
                return std::nullopt;
            }
            pad(tar);
        }
        if (tar.size() > maxTar) {
            error = tooLarge;
            return std::nullopt;
        }
    }
    // The end: two blocks of zeros.
    tar.append(2 * Block, '\0');
    std::string archive = gzip(tar);
    if (archive.size() > maxBytes) {
        error = tooLarge;
        return std::nullopt;
    }
    return archive;
}

std::string answer(std::string_view archive)
{
    const std::string encoded = AfterglowProto::base64Encode(archive);
    std::string out = "ok\n";
    out.reserve(answerSize(archive.size()));
    for (std::size_t at = 0; at < encoded.size(); at += 76) {
        if (at) out += '\r';
        out.append(encoded, at, 76);
    }
    out += "\n\n";
    return out;
}

std::size_t answerSize(std::size_t bytes)
{
    const std::size_t encoded = (bytes + 2) / 3 * 4;
    const std::size_t lines = encoded ? (encoded + 75) / 76 : 0;
    return 3 + encoded + (lines ? lines - 1 : 0) + 2;
}

} // namespace Afterglow::UploadArchive
