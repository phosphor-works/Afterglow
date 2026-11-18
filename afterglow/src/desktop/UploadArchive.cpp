// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UploadArchive.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Zlib.h>

#include <QCoreApplication>
#include <QFile>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Afterglow::UploadArchive {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t Block = 512;
/// The largest value of a 12-byte ustar field: eleven octal digits. Larger
/// sizes and times, and times before 1970, go in a pax header.
constexpr std::uint64_t MaxOctal11 = (std::uint64_t(1) << 33) - 1;

QString tr(const char* text)
{
    return QCoreApplication::translate("UploadArchive", text);
}

QString shown(const fs::path& path)
{
    return QFile::decodeName(path.c_str());
}

/// An octal field of @p width bytes, the last a NUL, as ustar writes it.
/// The caller keeps @p value below 8^(width - 1).
void octal(char* field, std::size_t width, std::uint64_t value)
{
    field[width - 1] = '\0';
    for (std::size_t i = width - 1; i-- > 0;) {
        field[i] = char('0' + (value & 7));
        value >>= 3;
    }
}

/// The CRC-32 of gzip, updated in steps: AfterglowProto::crc32Of takes a
/// whole buffer, and the tar data is never whole in memory.
std::uint32_t crc32Update(std::uint32_t crc, std::string_view data)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const char b : data)
        crc = table[(crc ^ static_cast<unsigned char>(b)) & 0xff] ^ (crc >> 8);
    return ~crc;
}

/// The gzip data of a tar stream, deflated as the tar data comes. It stops
/// once the gzip data passes the limit: only what may be sent is in memory,
/// and data that compresses well is not refused for its size before deflate.
class Gzip
{
public:
    explicit Gzip(std::size_t maxBytes)
        : m_max(maxBytes)
        , m_out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10)
    {
    }

    /// False when zlib failed, or the gzip data is past the limit.
    bool write(std::string_view bytes)
    {
        m_crc = crc32Update(m_crc, bytes);
        m_size += bytes.size();
        m_stream.setInput(bytes);
        // Without finish, zlib keeps what it cannot write yet for the next step.
        while (m_stream.inputLeft() > 0) {
            if (!step(false))
                return false;
        }
        return true;
    }

    /// Zeros up to the next block.
    bool pad()
    {
        static constexpr char zeros[Block] = {};
        return write(std::string_view(zeros, (Block - m_size % Block) % Block));
    }

    /// The gzip data: nothing when it is past the limit or zlib failed.
    std::optional<std::string> finish()
    {
        for (;;) {
            const auto result = stepResult(true);
            if (!result)
                return std::nullopt;
            if (*result == AfterglowProto::ZlibStream::Result::End)
                break;
        }
        // The zlib stream ends with its Adler-32: gzip has the CRC-32 and the
        // size instead.
        if (m_out.size() < 10 + 4)
            return std::nullopt;
        m_out.resize(m_out.size() - 4);
        const auto le32 = [this](std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                m_out += char((v >> (8 * i)) & 0xff);
        };
        le32(m_crc);
        le32(std::uint32_t(m_size));
        if (m_out.size() > m_max) {
            m_tooLarge = true;
            return std::nullopt;
        }
        return std::move(m_out);
    }

    /// Whether the last failure was the limit, not zlib.
    bool tooLarge() const
    {
        return m_tooLarge;
    }
    std::uint64_t size() const
    {
        return m_size;
    }

private:
    bool step(bool finish)
    {
        return stepResult(finish).has_value();
    }

    std::optional<AfterglowProto::ZlibStream::Result> stepResult(bool finish)
    {
        std::size_t produced = 0;
        const auto result = m_stream.step(m_buffer.data(), m_buffer.size(), produced, finish);
        if (result == AfterglowProto::ZlibStream::Result::Error)
            return std::nullopt;
        // The two bytes of the zlib header are not part of gzip data.
        const std::size_t skip = std::min(m_headerLeft, produced);
        m_headerLeft -= skip;
        m_out.append(m_buffer.data() + skip, produced - skip);
        // The Adler-32 that finish() takes off is still counted here.
        if (m_out.size() > m_max + 4) {
            m_tooLarge = true;
            return std::nullopt;
        }
        return result;
    }

    AfterglowProto::ZlibStream m_stream{AfterglowProto::ZlibStream::Mode::Deflate};
    std::array<char, 65536> m_buffer{};
    std::size_t m_max;
    std::string m_out;
    std::size_t m_headerLeft = 2;
    std::uint32_t m_crc = 0;
    std::uint64_t m_size = 0;
    bool m_tooLarge = false;
};

struct Entry
{
    std::string name; ///< "./relative", with '/' at the end for a directory
    char type = '0'; ///< '0' a file, '2' a link, '5' a directory
    std::uint32_t mode = 0;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::string link;
    fs::path path;
    dev_t device = 0;
    ino_t inode = 0;
};

/// A pax record "LEN key=value\n", whose length counts its own digits.
std::string paxRecord(std::string_view key, std::string_view value)
{
    const std::size_t body = key.size() + value.size() + 3; // ' ', '=', '\n'
    std::size_t length = body + 1;
    while (std::to_string(length).size() + body != length)
        ++length;
    return std::to_string(length) + " " + std::string(key) + "=" + std::string(value) + "\n";
}

bool header(Gzip& out, std::string_view name, char type, std::uint32_t mode, std::uint64_t size, std::int64_t mtime,
            std::string_view link)
{
    char block[Block] = {};
    std::memcpy(block, name.data(), std::min<std::size_t>(name.size(), 100));
    octal(block + 100, 8, mode);
    octal(block + 108, 8, 0); // uid: the user who extracts owns the files
    octal(block + 116, 8, 0);
    // Past these fields, a pax header holds the value (paxRecords).
    octal(block + 124, 12, std::min(size, MaxOctal11));
    octal(block + 136, 12, std::uint64_t(std::clamp<std::int64_t>(mtime, 0, std::int64_t(MaxOctal11))));
    block[156] = type;
    std::memcpy(block + 157, link.data(), std::min<std::size_t>(link.size(), 100));
    std::memcpy(block + 257, "ustar", 6);
    std::memcpy(block + 263, "00", 2);
    // The checksum counts its own field as spaces.
    std::memset(block + 148, ' ', 8);
    unsigned sum = 0;
    for (const char c : block)
        sum += static_cast<unsigned char>(c);
    // Six digits, a NUL and a space.
    octal(block + 148, 7, sum);
    block[155] = ' ';
    return out.write(std::string_view(block, Block));
}

/// The pax records of what does not fit the ustar fields of @p entry.
std::string paxRecords(const Entry& entry)
{
    std::string records;
    if (entry.name.size() > 100)
        records += paxRecord("path", entry.name);
    if (entry.link.size() > 100)
        records += paxRecord("linkpath", entry.link);
    if (entry.size > MaxOctal11)
        records += paxRecord("size", std::to_string(entry.size));
    if (entry.mtime < 0 || std::uint64_t(entry.mtime) > MaxOctal11)
        records += paxRecord("mtime", std::to_string(entry.mtime));
    return records;
}

/// The entry of @p path, named @p name. Nothing, with @p error set, when it
/// cannot be read. Nothing with @p error empty for a device, a socket or a
/// pipe: tar of iTerm2 keeps them, it2ul users want files.
std::optional<Entry> entryOf(const fs::path& path, std::string name, QString& error)
{
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        error = tr("Cannot read %1: %2.").arg(shown(path), QString::fromLocal8Bit(std::strerror(errno)));
        return std::nullopt;
    }
    Entry entry;
    entry.path = path;
    // Without the setuid and setgid bits: tar run as root restores them.
    entry.mode = std::uint32_t(st.st_mode & 01777);
    entry.mtime = st.st_mtime;
    entry.device = st.st_dev;
    entry.inode = st.st_ino;
    if (S_ISDIR(st.st_mode)) {
        entry.type = '5';
        name += '/';
    } else if (S_ISLNK(st.st_mode)) {
        entry.type = '2';
        std::error_code ec;
        entry.link = fs::read_symlink(path, ec).string();
        if (ec) {
            error = tr("Cannot read the link %1.").arg(shown(path));
            return std::nullopt;
        }
    } else if (S_ISREG(st.st_mode)) {
        entry.size = std::uint64_t(st.st_size);
    } else {
        error.clear();
        return std::nullopt;
    }
    entry.name = std::move(name);
    return entry;
}

/// The data of the file of @p entry, padded to a block. The file is opened
/// without following a link and without waiting, and must be the one that
/// entryOf saw: a pipe or a link swapped in after it would block the upload
/// or send another file.
bool writeData(Gzip& out, const Entry& entry, QString& error)
{
    const auto cannotRead = [&] {
        error = tr("Cannot read %1.").arg(shown(entry.path));
        return false;
    };
    const int fd = ::open(entry.path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return cannotRead();
    struct Closer
    {
        int fd;
        ~Closer()
        {
            ::close(fd);
        }
    } closer{fd};
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_dev != entry.device || st.st_ino != entry.inode) {
        error = tr("%1 changed while it was read.").arg(shown(entry.path));
        return false;
    }
    std::array<char, 65536> buffer;
    std::uint64_t left = entry.size;
    while (left > 0) {
        const ssize_t got = ::read(fd, buffer.data(), std::size_t(std::min<std::uint64_t>(left, buffer.size())));
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0)
            return cannotRead();
        // The header holds the size: a file that became shorter cannot be sent.
        if (got == 0) {
            error = tr("%1 changed while it was read.").arg(shown(entry.path));
            return false;
        }
        if (!out.write(std::string_view(buffer.data(), std::size_t(got))))
            return false;
        left -= std::uint64_t(got);
    }
    return out.pad();
}

/// Writes @p entry, and what a directory holds, sorted by name. Devices,
/// sockets and pipes in a directory are left out.
bool writeTree(Gzip& out, const Entry& entry, QString& error)
{
    if (const std::string records = paxRecords(entry); !records.empty()) {
        if (!header(out, "./PaxHeaders/entry", 'x', 0644, records.size(), entry.mtime, {}) || !out.write(records)
            || !out.pad()) {
            return false;
        }
    }
    if (!header(out, entry.name, entry.type, entry.mode, entry.size, entry.mtime, entry.link))
        return false;
    if (entry.type == '0')
        return writeData(out, entry, error);
    if (entry.type != '5')
        return true;
    std::error_code ec;
    std::vector<fs::path> children;
    for (fs::directory_iterator it(entry.path, ec), end; !ec && it != end; it.increment(ec))
        children.push_back(it->path());
    if (ec) {
        error = tr("Cannot read %1: %2.").arg(shown(entry.path), QString::fromStdString(ec.message()));
        return false;
    }
    std::sort(children.begin(), children.end());
    for (const fs::path& child : children) {
        auto childEntry = entryOf(child, entry.name + child.filename().string(), error);
        if (!childEntry) {
            if (!error.isEmpty())
                return false;
            continue;
        }
        if (!writeTree(out, *childEntry, error))
            return false;
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
        for (; a != common.end() && b != dir.end() && *a == *b; ++a, ++b)
            shared /= *a;
        common = shared;
    }
    return common;
}

/// Whether @p path is in the directory @p dir, at any depth.
bool inside(const fs::path& path, const fs::path& dir)
{
    const auto [d, p] = std::mismatch(dir.begin(), dir.end(), path.begin(), path.end());
    return d == dir.end() && p != path.end();
}

struct Chosen
{
    fs::path path; ///< as chosen: its name goes in the archive
    fs::path real; ///< with links followed: what is read
    bool directory = false;
};

} // namespace

std::optional<std::string> build(const QStringList& paths, std::size_t maxBytes, QString& error)
{
    if (paths.isEmpty()) {
        error = tr("No file was chosen.");
        return std::nullopt;
    }
    error.clear();
    // As iTerm2: the directory of each file, and each directory itself. A
    // chosen link is followed: the program gets what the user chose, where
    // a link alone would point at nothing there.
    std::vector<Chosen> chosen;
    for (const QString& p : paths) {
        fs::path path = fs::absolute(fs::path(QFile::encodeName(p).toStdString())).lexically_normal();
        // "dir/" is "dir": its name is in the archive, not ".".
        while (!path.has_filename() && path.has_relative_path())
            path = path.parent_path();
        std::error_code ec;
        const fs::path real = fs::canonical(path, ec);
        if (ec) {
            error = tr("%1 does not exist.").arg(p);
            return std::nullopt;
        }
        if (std::ranges::any_of(chosen, [&](const Chosen& c) {
                return c.path == path;
            }))
            continue;
        chosen.push_back({path, real, fs::is_directory(real, ec)});
    }
    // A path in a chosen directory comes with it. The directories first:
    // erase_if moves the elements that the predicate would read.
    std::vector<fs::path> chosenDirs;
    for (const Chosen& c : chosen) {
        if (c.directory)
            chosenDirs.push_back(c.path);
    }
    std::erase_if(chosen, [&](const Chosen& c) {
        return std::ranges::any_of(chosenDirs, [&](const fs::path& d) {
            return inside(c.path, d);
        });
    });
    std::vector<fs::path> dirs;
    for (const Chosen& c : chosen)
        dirs.push_back(c.directory ? c.path : c.path.parent_path());
    fs::path base = commonAncestor(dirs);
    // A chosen directory goes below its parent, with its name.
    if (std::ranges::any_of(chosen, [&](const Chosen& c) {
            return c.path == base;
        }))
        base = base.parent_path();

    Gzip out(maxBytes);
    const auto failed = [&] {
        if (out.tooLarge()) {
            error = tr("The files are larger than Terminal.uploadLimit lets through. Send them with aglow transfer.");
        } else if (error.isEmpty()) {
            error = tr("Could not compress the files.");
        }
        return std::nullopt;
    };
    for (const Chosen& c : chosen) {
        auto entry = entryOf(c.real, "./" + c.path.lexically_relative(base).string(), error);
        if (!entry) {
            if (error.isEmpty())
                error = tr("%1 is not a file, a directory or a link.").arg(shown(c.path));
            return std::nullopt;
        }
        if (!writeTree(out, *entry, error))
            return failed();
    }
    // The end: two blocks of zeros.
    static constexpr char end[2 * Block] = {};
    if (!out.write(std::string_view(end, sizeof end)))
        return failed();
    std::optional<std::string> archive = out.finish();
    if (!archive)
        return failed();
    return archive;
}

std::string answer(std::string_view archive)
{
    const std::string encoded = AfterglowProto::base64Encode(archive);
    std::string out = "ok\n";
    out.reserve(answerSize(archive.size()));
    for (std::size_t at = 0; at < encoded.size(); at += 76) {
        if (at)
            out += '\r';
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
