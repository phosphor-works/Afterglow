// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// aglow dnd: kitten dnd's command line and screen. Drags offer files and data
// from this computer; drops copy files and data here, also from another
// computer over SSH, through the terminal (OSC 72).

#include "Cli.h"
#include "Commands.h"
#include "Image.h"
#include "Tty.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/DndTree.h>
#include <AfterglowProto/DragDrop.h>
#include <AfterglowProto/FdDelta.h>
#include <AfterglowProto/TransferClient.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace Aglow {

namespace {

using AfterglowProto::DndMeta;
namespace Path = AfterglowProto::TransferPath;

constexpr int CopyOnDrop = 1;
constexpr int MoveOnDrop = 2;
// Reads that wait for the terminal at one time. Afterglow ends a drop with
// EMFILE past 64 queued reads.
constexpr std::size_t MaxInFlight = 32;
constexpr std::size_t MaxPreSent = 64 * 1024 * 1024;
constexpr std::size_t WriteAhead = 256 * 1024;
// What the terminal can make this side hold: a list of MIME types, a list of
// URIs (Afterglow sends 64 MiB of one type at most), a link target, a
// directory listing (16,384 names of 255 bytes), and the entries of a tree.
constexpr std::size_t MaxControl = 1024 * 1024;
constexpr std::size_t MaxInMemory = 64 * 1024 * 1024;
constexpr std::size_t MaxLinkTarget = 4096;
constexpr std::size_t MaxListing = 16384 * 256;
constexpr std::size_t MaxRemoteEntries = 1 << 20;
// The decoded drag image, before it is scaled down: Afterglow takes 64 MiB of them for a drag.
constexpr std::size_t ThumbnailBudget = 64 * 1024 * 1024;

const Cli::Command command {
    "dnd",
    "[files to drag]",
    "Drag and drop files and data between the shell and other programs, even over SSH.",
    "The files and directories on the command line are offered as text/uri-list. Start dragging anywhere in the "
    "window to drag them to a file manager, or to another window that runs aglow dnd. Directories are dragged "
    "with their contents. When a move is dropped on another computer, the files are deleted here.\n"
    "\n"
    "Files dropped on the window are copied, or moved, into the working directory, or into --drop-dest. Drop "
    "them on the Copy or Move box to choose. Data on standard input is offered as text/plain. Dropped text/plain "
    "goes to standard output when it is not a terminal.\n"
    "\n"
    "Press Esc or Ctrl+C to quit, which cancels a drag in progress.",
    {
        { "--drag", Cli::Option::Kind::List, "", {},
          "When a drag starts, offer this file as the data of this MIME type, as MIME:PATH, for example "
          "image/jpeg:picture.jpg. Give it again for more types. The path - or /dev/stdin reads standard input." },
        { "--drop", Cli::Option::Kind::List, "", {},
          "Write the dropped data of this MIME type to this file, as MIME:PATH. Give it again for more types. "
          "Without a path, the type is not accepted: text/plain: and text/uri-list: turn those off." },
        { "--drop-dest", Cli::Option::Kind::String, "", {},
          "The directory where dropped files go. The default is the working directory." },
        { "--copy-mode", Cli::Option::Kind::Choice, "auto", { "auto", "independent" },
          "How a copy drop copies local files. auto makes hard links where it can, and copies the data where it "
          "cannot. independent always copies, so that a change to a dropped file never changes its source. A move "
          "always uses hard links where it can." },
        { "--confirm-drop-overwrite", Cli::Option::Kind::Flag, "", {},
          "Ask before a drop of files replaces existing files. Files that replace nothing are copied without a "
          "question." },
        { "--drop-anywhere", Cli::Option::Kind::Choice, "disallowed", { "disallowed", "copy", "move" },
          "Accept drops anywhere in the window, not only on the Copy and Move boxes, with this action." },
        { "--drag-thumbnail", Cli::Option::Kind::String, "", {},
          "An image to show under the pointer while dragging. Without it, an image that is dragged is shown, or "
          "an icon." },
        { "--drag-thumbnail-size", Cli::Option::Kind::Int, "512", {},
          "The largest side of the drag image in pixels. A larger image is scaled down. A terminal can refuse a "
          "drag with an image that is too large." },
        { "--exit-on", Cli::Option::Kind::String, "esc-key", {},
          "The events to exit on, separated by commas: drag-finish, drop-finish and esc-key. The first two are a "
          "completed drag and a completed drop." },
    },
};

struct UriItem
{
    std::string path;
    std::string uri;
    struct stat st { };
    bool sent = false;
};

struct DragSource
{
    std::string mime;
    std::string human;
    std::string path; // empty: the data is in memory
    std::string data;
    std::vector<UriItem> uris;
};

struct DropDest
{
    std::string mime;
    std::string human;
    std::string path; // empty: kept in memory (or standard output)
    bool toStdout = false;
    bool completed = false;
    int fd = -1;
    std::string buffer;
    AfterglowProto::DndBase64Stream decoder;

    bool write(std::string_view bytes)
    {
        if (toStdout) {
            return AfterglowProto::writeAll(STDOUT_FILENO, bytes);
        }
        if (path.empty()) {
            if (buffer.size() + bytes.size() > MaxInMemory) {
                errno = EFBIG;
                return false;
            }
            buffer += bytes;
            return true;
        }
        if (fd < 0) {
            ::mkdir(Path::dirName(path).c_str(), 0755);
            fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0) {
                return false;
            }
        }
        return AfterglowProto::writeAll(fd, bytes);
    }
    void close()
    {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
    void reset()
    {
        close();
        buffer.clear();
        decoder.reset();
        completed = false;
    }
};

struct Region
{
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    bool has(int x, int y) const { return left <= x && x < left + width && top <= y && y < top + height; }
};

// Data that goes to the terminal in pieces: a file, or bytes in memory.
struct Job
{
    DndMeta meta;
    int fd = -1;
    std::string data;
    std::size_t offset = 0;
    bool remote = false; ///< a t=k entry: the next one follows when it ends
};

// An entry of a remote drag tree, sent breadth first.
struct RemoteItem
{
    std::string path;
    std::int64_t root = 0;
    std::int64_t parent = 0;
    std::int64_t child = 0;
};

// An entry of a remote drop tree.
struct RemoteEntry
{
    std::string base; // the directory it goes in
    std::string name;
    std::int64_t kind = -1;
    std::vector<std::unique_ptr<RemoteEntry>> children;
    std::size_t finished = 0;
    bool skipped = false;
    bool started = false;
    bool complete = false; ///< its last packet arrived
    int fd = -1;
    std::string buffer;
    AfterglowProto::DndBase64Stream decoder;
    ~RemoteEntry()
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

std::string percentDecode(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit((unsigned char)text[i + 1]) && std::isxdigit((unsigned char)text[i + 2])) {
            out += char(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

std::string percentEncode(std::string_view path)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : path) {
        if (std::isalnum(c) || std::strchr("/-._~!$&'()*+,;=:@", c)) {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

struct ParsedUri
{
    enum class Kind { File, Data, Other } kind = Kind::Other;
    std::string path;
    std::string mime;
    std::string data;
};

std::vector<ParsedUri> parseUriList(std::string_view text)
{
    std::vector<ParsedUri> out;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string line(text.substr(start, end - start));
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        line.erase(0, std::min(line.find_first_not_of(" \t"), line.size()));
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        ParsedUri uri;
        if (line.starts_with("file://")) {
            // file://host/path: the host is this computer or localhost.
            const auto slash = line.find('/', 7);
            uri.kind = ParsedUri::Kind::File;
            if (slash != std::string::npos) {
                uri.path = Path::clean(percentDecode(line.substr(slash)));
            }
        } else if (line.starts_with("data:")) {
            const auto comma = line.find(',');
            if (comma != std::string::npos) {
                std::string header = line.substr(5, comma - 5);
                const bool base64 = header.ends_with(";base64");
                if (base64) {
                    header.resize(header.size() - 7);
                }
                uri.mime = header.substr(0, header.find(';'));
                if (uri.mime.empty()) {
                    uri.mime = "text/plain";
                }
                const auto payload = line.substr(comma + 1);
                if (base64) {
                    if (auto decoded = AfterglowProto::base64Decode(payload, AfterglowProto::Base64Check::PaddingOptional)) {
                        uri.kind = ParsedUri::Kind::Data;
                        uri.data = std::move(*decoded);
                    }
                } else {
                    uri.kind = ParsedUri::Kind::Data;
                    uri.data = percentDecode(payload);
                }
            }
        }
        out.push_back(std::move(uri));
    }
    return out;
}

std::string extensionFor(std::string_view mime)
{
    static const std::map<std::string, std::string, std::less<>> extensions {
        { "text/plain", ".txt" }, { "text/html", ".html" }, { "image/png", ".png" }, { "image/jpeg", ".jpg" },
        { "image/gif", ".gif" }, { "image/webp", ".webp" }, { "image/svg+xml", ".svg" }, { "application/pdf", ".pdf" },
        { "application/json", ".json" },
    };
    const auto it = extensions.find(mime);
    return it == extensions.end() ? std::string() : it->second;
}

bool copyFile(const std::string& from, const std::string& to, mode_t mode)
{
    const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0) {
        return false;
    }
    const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777);
    if (out < 0) {
        ::close(in);
        return false;
    }
    bool ok = true;
    char buffer[65536];
    for (;;) {
        const ssize_t n = ::read(in, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        if (!AfterglowProto::writeAll(out, std::string_view(buffer, std::size_t(n)))) {
            ok = false;
            break;
        }
    }
    ::close(in);
    ok = ::close(out) == 0 && ok;
    return ok;
}

std::vector<std::string> children(const std::string& path)
{
    std::vector<std::string> names;
    if (DIR* dir = ::opendir(path.c_str())) {
        while (const dirent* entry = ::readdir(dir)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") {
                names.push_back(name);
            }
        }
        ::closedir(dir);
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Copies @p from into the directory @p to, as kitten's local drop does:
// hard links where allowed, else copies. Only files, directories and links.
bool copyTree(const std::string& from, const std::string& to, bool links, std::string& error)
{
    struct stat st { };
    if (::lstat(from.c_str(), &st) != 0) {
        error = from + ": " + std::strerror(errno);
        return false;
    }
    const auto dest = to + "/" + Path::baseName(from);
    if (S_ISDIR(st.st_mode)) {
        if (::mkdir(dest.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
            error = dest + ": " + std::strerror(errno);
            return false;
        }
        for (const auto& name : children(from)) {
            if (!copyTree(from + "/" + name, dest, links, error)) {
                return false;
            }
        }
        return true;
    }
    if (S_ISLNK(st.st_mode)) {
        std::string target(4096, '\0');
        const auto n = ::readlink(from.c_str(), target.data(), target.size());
        if (n < 0 || ::symlink(target.substr(0, std::size_t(n)).c_str(), dest.c_str()) != 0) {
            error = dest + ": " + std::strerror(errno);
            return false;
        }
        return true;
    }
    if (!S_ISREG(st.st_mode)) {
        return true;
    }
    if (links && ::link(from.c_str(), dest.c_str()) == 0) {
        return true;
    }
    if (!copyFile(from, dest, st.st_mode)) {
        error = dest + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool removeTree(const std::string& path)
{
    struct stat st { };
    if (::lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT;
    }
    if (S_ISDIR(st.st_mode)) {
        for (const auto& name : children(path)) {
            removeTree(path + "/" + name);
        }
        return ::rmdir(path.c_str()) == 0;
    }
    return ::unlink(path.c_str()) == 0;
}

// The paths under @p staging that exist under @p dest, except directories that exist in both.
void overwrites(const std::string& staging, const std::string& dest, const std::string& relative, std::vector<std::string>& out)
{
    for (const auto& name : children(staging + relative)) {
        const auto rel = relative + "/" + name;
        struct stat src { };
        struct stat dst { };
        if (::lstat((dest + rel).c_str(), &dst) != 0 || ::lstat((staging + rel).c_str(), &src) != 0) {
            continue;
        }
        if (S_ISDIR(src.st_mode) && S_ISDIR(dst.st_mode)) {
            overwrites(staging, dest, rel, out);
        } else {
            out.push_back(rel.substr(1));
        }
    }
}

// Moves the contents of @p staging into @p dest, merging directories that exist in both.
bool renameContents(const std::string& staging, const std::string& dest, std::string& error)
{
    for (const auto& name : children(staging)) {
        const auto from = staging + "/" + name;
        const auto to = dest + "/" + name;
        if (::rename(from.c_str(), to.c_str()) == 0) {
            continue;
        }
        const int renameError = errno;
        struct stat st { };
        struct stat there { };
        // Merged only into a directory that is one: a link that an earlier
        // drop left there must not take this drop out of the drop directory.
        if (::lstat(from.c_str(), &st) == 0 && S_ISDIR(st.st_mode)
            && (renameError == EEXIST || renameError == ENOTEMPTY || renameError == EISDIR)) {
            if (::lstat(to.c_str(), &there) != 0 || !S_ISDIR(there.st_mode)) {
                error = to + ": " + (S_ISLNK(there.st_mode) ? "a symbolic link is in the way" : std::strerror(ENOTDIR));
                return false;
            }
            if (!renameContents(from, to, error)) {
                return false;
            }
            continue;
        }
        error = to + ": " + std::strerror(renameError);
        return false;
    }
    return true;
}

class Dnd
{
public:
    Dnd(Tty& tty, const Cli::Parsed& options)
        : m_tty(tty)
        , m_options(options)
    {
    }

    std::vector<DragSource> dragSources;
    std::vector<DropDest> dropDests;
    std::optional<Image> thumbnail;
    std::string dropDir;
    std::string tempBase;

    int run();

private:
    bool exitOn(std::string_view event) const;
    void send(const DndMeta& meta, std::string_view payload = {})
    {
        for (const auto& packet : AfterglowProto::writeDnd(meta, payload)) {
            m_tty.write(packet);
        }
    }
    void sendPackets(const std::vector<std::string>& packets)
    {
        for (const auto& packet : packets) {
            m_tty.write(packet);
        }
    }
    DropDest* dest(std::string_view mime)
    {
        for (auto& d : dropDests) {
            if (d.mime == mime) {
                return &d;
            }
        }
        return nullptr;
    }
    DragSource* source(std::string_view mime)
    {
        for (auto& s : dragSources) {
            if (s.mime == mime) {
                return &s;
            }
        }
        return nullptr;
    }
    void dispatch(const DndMeta& meta, std::string_view payload, bool more);
    void render();
    // Drags
    void potentialDrag(int x, int y);
    void dragImage();
    /// @p operation is the o key, or -1 when the event has none.
    void dragEvent(int x, int y, int operation);
    void dataRequest(int index);
    void remoteRequest(int index);
    void pump();
    void resetDrag();
    void finishDrag(std::string_view error);
    // Drops
    void dropMove(int x, int y, bool more, std::string_view mimes, bool drop, int operation);
    void dropData(const DndMeta& meta, std::string_view payload, bool more);
    void remoteDropData(const DndMeta& meta, std::string_view payload, bool more);
    void allMimeDataDropped();
    void allDropDataReceived();
    void endDrop(bool success);
    void resetDrop();
    void request(DndMeta meta);
    void requestDone();
    void confirmDrop(bool accepted);
    void fail(std::string message)
    {
        m_error = std::move(message);
        m_quit = true;
    }

    Tty& m_tty;
    const Cli::Parsed& m_options;
    const std::string m_machine = AfterglowProto::dndMachineId();
    bool m_allowDrops = false;
    bool m_allowDrags = false;
    bool m_quit = false;
    std::string m_error;
    std::string m_notice;
    Region m_copy;
    Region m_move;
    int m_dropped = 0;
    int m_tempCounter = 0;
    // The drag.
    struct
    {
        bool active = false;
        bool accepted = false;
        std::vector<std::string> mimes;
        int acceptedMime = 0;
        int operation = 0;
        bool dropped = false;
        std::deque<Job> jobs;
        std::deque<RemoteItem> remote;
        std::int64_t handles = AfterglowProto::DndFirstHandle;
    } m_drag;
    // The drop.
    struct
    {
        std::vector<std::string> offered;
        std::vector<std::string> accepted;
        std::string mimeBuffer;
        int x = -1;
        int y = -1;
        int action = 0;
        bool inWindow = false;
        bool reading = false;
        bool remoteClient = false;
        int sourceOps = 0;
        std::vector<ParsedUri> uris;
        std::string staging;
        std::unique_ptr<RemoteEntry> root;
        RemoteEntry* open = nullptr;
        RemoteEntry* current = nullptr;
        std::deque<RemoteEntry*> pendingDirs;
        std::deque<DndMeta> waiting;
        std::size_t inFlight = 0;
        std::size_t entries = 0;
        std::vector<std::string> overwrites;
    } m_drop;
    // Chunked messages from the terminal: the packets after the first carry only m.
    bool m_chunking = false;
    DndMeta m_chunkMeta;
};

bool Dnd::exitOn(std::string_view event) const
{
    const auto list = m_options.value("exit-on");
    std::size_t start = 0;
    while (start <= list.size()) {
        auto end = std::min(list.find(',', start), list.size());
        std::string item = list.substr(start, end - start);
        item.erase(0, std::min(item.find_first_not_of(' '), item.size()));
        while (!item.empty() && item.back() == ' ') {
            item.pop_back();
        }
        if (item == event) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

void Dnd::render()
{
    const auto size = m_tty.size();
    const int width = std::max<int>(20, size.ws_col);
    const int height = std::max<int>(8, size.ws_row);
    std::string out = "\x1b[?2026h\x1b[H\x1b[2J";
    int y = 0;
    auto paragraph = [&](std::string text) {
        text = printable(text);
        while (!text.empty()) {
            std::size_t cut = text.size();
            // Cut at a space before the width, counting code points as cells.
            std::size_t cells = 0;
            std::size_t lastSpace = std::string::npos;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
                    continue;
                }
                if (text[i] == ' ') {
                    lastSpace = i;
                }
                if (++cells > std::size_t(width)) {
                    cut = lastSpace != std::string::npos && lastSpace > 0 ? lastSpace : i;
                    break;
                }
            }
            out += text.substr(0, cut) + "\r\n";
            ++y;
            text.erase(0, cut);
            text.erase(0, std::min(text.find_first_not_of(' '), text.size()));
        }
    };
    auto join = [](const std::vector<std::string>& items) {
        std::string s;
        for (const auto& i : items) {
            s += (s.empty() ? "" : " ") + i;
        }
        return s;
    };
    m_copy = m_move = Region {};
    if (!m_notice.empty()) {
        paragraph(m_notice);
        out += "\r\n";
        ++y;
    }
    if (m_drag.active) {
        paragraph("Dragging data...");
        if (m_drag.dropped) {
            paragraph("The dragged data has been dropped, waiting for data transfer requests...");
        } else if (m_drag.operation > 0) {
            paragraph("Another window is willing to accept the dragged data");
        }
        m_tty.write(out + "\x1b[?2026l");
        return;
    }
    if (!m_drop.overwrites.empty()) {
        paragraph("Some of the dropped files will overwrite existing files, listed below. Press \x1b[32mEnter\x1b[39m to "
                  "drop anyway or \x1b[31mEsc\x1b[39m to cancel the drop.");
        out += "\r\n";
        ++y;
        std::size_t shown = 0;
        for (const auto& path : m_drop.overwrites) {
            if (y >= height - 2) {
                break;
            }
            out += printable(path) + "\r\n";
            ++y;
            ++shown;
        }
        if (shown < m_drop.overwrites.size()) {
            out += "... (" + std::to_string(m_drop.overwrites.size() - shown) + " more)";
        }
        m_tty.write(out + "\x1b[?2026l");
        return;
    }
    if (m_drop.reading) {
        paragraph("Reading dropped data, please wait...");
        m_tty.write(out + "\x1b[?2026l");
        return;
    }
    if (m_drop.inWindow) {
        if (m_drop.action == 0) {
            paragraph("A drag is active. Drop it into one of the boxes below to perform that action on the dragged data. "
                      "Available MIME types in the drag:");
            out += "\r\n";
            paragraph(join(m_drop.offered));
        } else {
            paragraph("The drag can be dropped. Supported MIME types:");
            out += "\r\n";
            paragraph(join(m_drop.accepted));
        }
    } else {
        if (m_allowDrags) {
            paragraph("Start dragging anywhere in this window to initiate a drag and drop. If you start the drag in one of "
                      "the Copy or Move boxes below, only that action will be allowed when dropping, otherwise, the drop "
                      "destination can pick either copy or move.");
            out += "\r\n";
        }
        if (m_allowDrops) {
            if (m_dropped > 0) {
                paragraph(std::to_string(m_dropped) + (m_dropped == 1 ? " file has" : " files have")
                          + " been dropped in the last drop event. You can drop more data or press Esc to quit.");
            } else {
                paragraph("Drag some data from another application into this window to transfer the files here.");
            }
        }
    }
    // The two boxes along the bottom edge.
    const int boxHeight = 5;
    const int boxWidth = 10;
    const int top = 1 + std::max(0, height - boxHeight);
    auto box = [&](int x, std::string_view text, Region& region, bool active) {
        region = { x - 1, top - 1, boxWidth, boxHeight };
        if (active) {
            out += "\x1b[32m";
        }
        for (int row = 0; row < boxHeight; ++row) {
            out += "\x1b[" + std::to_string(top + row) + ";" + std::to_string(x) + "H";
            if (row == 0) {
                out += "╭────────╮";
            } else if (row == boxHeight - 1) {
                out += "╰────────╯";
            } else if (row == 2) {
                out += "│  " + std::string(text) + "  │";
            } else {
                out += "│        │";
            }
        }
        out += "\x1b[39m";
    };
    box(1, "Copy", m_copy, m_drop.action == CopyOnDrop);
    box(1 + width - boxWidth, "Move", m_move, m_drop.action == MoveOnDrop);
    m_tty.write(out + "\x1b[?2026l");
}

// ── Drags ───────────────────────────────────────────────────────────────────

void Dnd::potentialDrag(int x, int y)
{
    if (!m_allowDrags || m_drag.active) {
        return;
    }
    int actions = 3;
    if (m_copy.has(x, y)) {
        actions = CopyOnDrop;
    } else if (m_move.has(x, y)) {
        actions = MoveOnDrop;
    }
    std::vector<std::string> mimes;
    for (const auto& s : dragSources) {
        mimes.push_back(s.mime);
    }
    std::string list;
    for (const auto& m : mimes) {
        list += (list.empty() ? "" : " ") + m;
    }
    send({ { "t", "o" }, { "o", std::to_string(actions) } }, list);
    // Data in memory goes before the drag starts: some desktops need it then.
    std::size_t preSent = 0;
    for (std::size_t i = 0; i < dragSources.size(); ++i) {
        const auto& s = dragSources[i];
        if (s.path.empty() && !s.data.empty() && preSent + s.data.size() < MaxPreSent) {
            preSent += s.data.size();
            sendPackets(AfterglowProto::dndDataPackets({ { "t", "p" }, { "x", std::to_string(i) } }, s.data));
        }
    }
    m_drag.mimes = mimes;
    dragImage();
    send({ { "t", "P" }, { "x", "-1" } });
    m_drag.active = true;
    render();
}

void Dnd::dragImage()
{
    std::optional<Image> image = thumbnail;
    auto tryPath = [&](const std::string& path) {
        if (image) {
            return;
        }
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        struct stat st { };
        if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 64 * 1024 * 1024) {
            if (fd >= 0) {
                ::close(fd);
            }
            return;
        }
        std::string bytes;
        readAll(fd, bytes);
        ::close(fd);
        std::string error;
        int w = 0;
        int h = 0;
        if (imageSize(bytes, w, h)) {
            image = decodeImage(bytes, ThumbnailBudget, error);
        }
    };
    for (const auto& s : dragSources) {
        if (!s.path.empty() && s.mime.starts_with("image/")) {
            tryPath(s.path);
        }
    }
    if (const auto* uris = source("text/uri-list")) {
        for (const auto& item : uris->uris) {
            tryPath(item.path);
        }
    }
    if (!image) {
        // A symbol as the drag image: the terminal draws it.
        bool directory = false;
        if (const auto* uris = source("text/uri-list")) {
            directory = !uris->uris.empty() && S_ISDIR(uris->uris.front().st.st_mode);
        }
        sendPackets(AfterglowProto::dndDataPackets({ { "t", "p" }, { "x", "-1" }, { "X", "6" }, { "Y", "1" } },
                                                   directory ? "📁" : "📄"));
        return;
    }
    const auto limit = std::max<long long>(16, m_options.integer("drag-thumbnail-size"));
    int width = image->width;
    int height = image->height;
    if (std::max(width, height) > limit) {
        const double scale = double(limit) / std::max(width, height);
        width = std::max(1, int(width * scale));
        height = std::max(1, int(height * scale));
    }
    std::string pixels = resizeFrame(image->frame(0), image->width, image->height, width, height);
    const bool opaque = frameOpaque(pixels);
    if (opaque) {
        pixels = dropAlpha(pixels);
    }
    sendPackets(AfterglowProto::dndDataPackets({ { "t", "p" },
                                                 { "x", "-1" },
                                                 { "y", opaque ? "24" : "32" },
                                                 { "X", std::to_string(width) },
                                                 { "Y", std::to_string(height) } },
                                               pixels));
}

void Dnd::resetDrag()
{
    for (auto& job : m_drag.jobs) {
        if (job.fd >= 0) {
            ::close(job.fd);
        }
    }
    m_drag.jobs.clear();
    m_drag.remote.clear();
    m_drag.active = m_drag.accepted = m_drag.dropped = false;
    m_drag.mimes.clear();
    m_drag.acceptedMime = m_drag.operation = 0;
    m_drag.handles = AfterglowProto::DndFirstHandle;
}

void Dnd::finishDrag(std::string_view error)
{
    if (error.empty()) {
        send({ { "t", "E" }, { "y", "-1" } });
    } else {
        send({ { "t", "E" } }, error);
    }
    resetDrag();
}

void Dnd::dragEvent(int x, int y, int operation)
{
    switch (x) {
    case 1:
        m_drag.acceptedMime = y;
        break;
    case 2:
        m_drag.operation = std::max(0, operation);
        break;
    case 3:
        m_drag.dropped = true;
        break;
    case 4: {
        // y=1: the drag was canceled or refused, whatever happened before.
        // The action is the one the drop took: Afterglow names it in o.
        // Without it (kitty), it is the last one that the target chose.
        const bool dropped = m_drag.dropped && y == 0;
        const bool move = (operation >= 0 ? operation : m_drag.operation) == MoveOnDrop;
        resetDrag();
        if (dropped && exitOn("drag-finish")) {
            m_quit = true;
        }
        // A move to another computer took copies: the files here go.
        if (dropped && move) {
            if (auto* uris = source("text/uri-list")) {
                for (const auto& item : uris->uris) {
                    if (item.sent && !removeTree(item.path)) {
                        fail("Failed to remove " + item.path + " after the move: " + std::strerror(errno));
                        return;
                    }
                }
            }
            dragSources.clear();
            m_allowDrags = false;
            send({ { "t", "o" }, { "x", "2" } });
            if (!m_allowDrops) {
                m_quit = true;
            }
        }
        break;
    }
    case 5:
        dataRequest(y);
        break;
    default:
        break;
    }
    render();
}

void Dnd::dataRequest(int index)
{
    if (index < 0 || std::size_t(index) >= m_drag.mimes.size()) {
        finishDrag("EINVAL");
        m_notice = "The terminal asked for drag data with an index out of bounds: " + std::to_string(index);
        return;
    }
    const auto* s = source(m_drag.mimes[std::size_t(index)]);
    if (!s) {
        finishDrag("ENOENT");
        return;
    }
    Job job;
    job.meta = { { "t", "e" }, { "y", std::to_string(index) } };
    if (s->path.empty()) {
        job.data = s->data;
    } else {
        job.fd = ::open(s->path.c_str(), O_RDONLY | O_CLOEXEC);
        if (job.fd < 0) {
            finishDrag("EIO");
            m_notice = "Cannot read " + s->path + ": " + std::strerror(errno);
            return;
        }
    }
    m_drag.jobs.push_back(std::move(job));
}

void Dnd::remoteRequest(int index)
{
    auto* uris = source("text/uri-list");
    if (!uris || index < 0 || std::size_t(index) >= uris->uris.size()) {
        finishDrag("EINVAL");
        m_notice = "The terminal asked for an entry of the URI list that does not exist";
        return;
    }
    uris->uris[std::size_t(index)].sent = true;
    m_drag.remote.push_back({ uris->uris[std::size_t(index)].path, index + 1, 0, 0 });
}

// Sends file data and remote entries while the terminal takes them.
void Dnd::pump()
{
    while (m_tty.pending() < WriteAhead && (!m_drag.jobs.empty() || !m_drag.remote.empty())) {
        if (m_drag.jobs.empty()) {
            // The next entry of a remote tree, breadth first.
            const auto item = m_drag.remote.front();
            m_drag.remote.pop_front();
            struct stat st { };
            if (::lstat(item.path.c_str(), &st) != 0) {
                finishDrag("EIO");
                m_notice = "Cannot read " + item.path + ": " + std::strerror(errno);
                return;
            }
            Job job;
            job.remote = true;
            if (S_ISDIR(st.st_mode)) {
                const auto handle = m_drag.handles++;
                std::vector<std::string> names;
                for (const auto& name : children(item.path)) {
                    struct stat child { };
                    const auto path = item.path + "/" + name;
                    if (::lstat(path.c_str(), &child) == 0 && (S_ISDIR(child.st_mode) || S_ISREG(child.st_mode) || S_ISLNK(child.st_mode))) {
                        names.push_back(name);
                    }
                }
                for (std::size_t i = 0; i < names.size(); ++i) {
                    m_drag.remote.push_back({ item.path + "/" + names[i], item.root, handle, std::int64_t(i + 1) });
                }
                job.meta = AfterglowProto::dndRemoteEntry(item.root, item.parent, item.child, handle);
                job.data = AfterglowProto::dndJoinNames(names);
            } else if (S_ISLNK(st.st_mode)) {
                std::string target(4096, '\0');
                const auto n = ::readlink(item.path.c_str(), target.data(), target.size());
                if (n < 0) {
                    finishDrag("EIO");
                    return;
                }
                job.meta = AfterglowProto::dndRemoteEntry(item.root, item.parent, item.child, AfterglowProto::DndLinkKind);
                job.data = target.substr(0, std::size_t(n));
            } else {
                job.meta = AfterglowProto::dndRemoteEntry(item.root, item.parent, item.child, AfterglowProto::DndFileKind);
                job.fd = ::open(item.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                if (job.fd < 0) {
                    finishDrag("EIO");
                    m_notice = "Cannot read " + item.path + ": " + std::strerror(errno);
                    return;
                }
            }
            m_drag.jobs.push_back(std::move(job));
        }
        auto& job = m_drag.jobs.front();
        std::string chunk;
        if (job.fd >= 0) {
            chunk.resize(AfterglowProto::DndDataChunk * 16);
            ssize_t n;
            do {
                n = ::read(job.fd, chunk.data(), chunk.size());
            } while (n < 0 && errno == EINTR);
            if (n < 0) {
                finishDrag("EIO");
                return;
            }
            chunk.resize(std::size_t(n));
        } else {
            chunk = job.data.substr(job.offset, AfterglowProto::DndDataChunk * 16);
            job.offset += chunk.size();
        }
        if (chunk.empty()) {
            // The end of the item: an empty message.
            send(job.meta);
            if (job.fd >= 0) {
                ::close(job.fd);
            }
            m_drag.jobs.pop_front();
            continue;
        }
        for (std::size_t at = 0; at < chunk.size(); at += AfterglowProto::DndDataChunk) {
            send(job.meta, AfterglowProto::base64Encode(std::string_view(chunk).substr(at, AfterglowProto::DndDataChunk)));
        }
    }
}

// ── Drops ───────────────────────────────────────────────────────────────────

void Dnd::resetDrop()
{
    for (auto& d : dropDests) {
        d.reset();
    }
    if (!m_drop.staging.empty()) {
        removeTree(m_drop.staging);
    }
    m_drop.offered.clear();
    m_drop.accepted.clear();
    m_drop.mimeBuffer.clear();
    m_drop.x = m_drop.y = -1;
    m_drop.action = 0;
    m_drop.inWindow = m_drop.reading = m_drop.remoteClient = false;
    m_drop.sourceOps = 0;
    m_drop.uris.clear();
    m_drop.staging.clear();
    m_drop.root.reset();
    m_drop.open = m_drop.current = nullptr;
    m_drop.pendingDirs.clear();
    m_drop.waiting.clear();
    m_drop.inFlight = 0;
    m_drop.entries = 0;
    m_drop.overwrites.clear();
}

void Dnd::endDrop(bool success)
{
    if (m_drop.reading) {
        send({ { "t", "r" }, { "o", std::to_string(success ? m_drop.action : 0) } });
        if (success && m_drop.action == MoveOnDrop && !m_drop.remoteClient) {
            for (const auto& uri : m_drop.uris) {
                if (uri.kind == ParsedUri::Kind::File && !uri.path.empty()) {
                    removeTree(uri.path);
                }
            }
        }
    }
    resetDrop();
    if (success && exitOn("drop-finish")) {
        m_quit = true;
    }
}

void Dnd::request(DndMeta meta)
{
    if (m_drop.inFlight < MaxInFlight) {
        send(meta);
        ++m_drop.inFlight;
    } else {
        m_drop.waiting.push_back(std::move(meta));
    }
}

void Dnd::requestDone()
{
    if (m_drop.inFlight > 0) {
        --m_drop.inFlight;
    }
    while (!m_drop.waiting.empty() && m_drop.inFlight < MaxInFlight) {
        send(m_drop.waiting.front());
        m_drop.waiting.pop_front();
        ++m_drop.inFlight;
    }
}

void Dnd::dropMove(int x, int y, bool more, std::string_view mimes, bool drop, int operation)
{
    const auto previousAction = m_drop.action;
    const auto previousAccepted = m_drop.accepted;
    const bool previousIn = m_drop.inWindow;
    m_drop.x = x;
    m_drop.y = y;
    if (!mimes.empty()) {
        if (m_drop.mimeBuffer.size() + mimes.size() > MaxControl) {
            fail("The terminal sent a list of MIME types that is too long");
            return;
        }
        m_drop.mimeBuffer += mimes;
        if (more) {
            return;
        }
        m_drop.offered.clear();
        m_drop.accepted.clear();
        std::size_t start = 0;
        const auto& all = m_drop.mimeBuffer;
        while (start < all.size()) {
            auto end = std::min(all.find(' ', start), all.size());
            if (end > start) {
                const auto mime = all.substr(start, end - start);
                m_drop.offered.push_back(mime);
                if (dest(mime) && std::find(m_drop.accepted.begin(), m_drop.accepted.end(), mime) == m_drop.accepted.end()) {
                    m_drop.accepted.push_back(mime);
                }
            }
            start = end + 1;
        }
    }
    m_drop.mimeBuffer.clear();
    if (operation != 0) {
        m_drop.sourceOps = operation;
    }
    const auto anywhere = m_options.value("drop-anywhere");
    if (m_copy.has(x, y)) {
        m_drop.action = CopyOnDrop;
    } else if (m_move.has(x, y)) {
        m_drop.action = MoveOnDrop;
    } else if (anywhere == "copy") {
        m_drop.action = CopyOnDrop;
    } else if (anywhere == "move") {
        m_drop.action = MoveOnDrop;
    } else {
        m_drop.action = 0;
        m_drop.accepted.clear();
    }
    // Only the operations that the source allows.
    if (m_drop.sourceOps != 0 && m_drop.action != 0 && !(m_drop.sourceOps & m_drop.action)) {
        m_drop.action = 0;
        m_drop.accepted.clear();
    }
    m_drop.inWindow = x > -1 && y > -1;
    // A drop on the window that is dragging is refused.
    if (!m_drop.inWindow || m_drag.active) {
        resetDrop();
    }
    const bool changed = previousAction != m_drop.action || previousAccepted != m_drop.accepted;
    if (changed && !drop) {
        std::string list;
        if (m_drop.action != 0) {
            for (const auto& m : m_drop.accepted) {
                list += (list.empty() ? "" : " ") + m;
            }
        }
        send({ { "t", "m" }, { "o", std::to_string(m_drop.action) } }, list);
    }
    if (drop) {
        if (m_drop.action == 0 || m_drop.accepted.empty() || m_drag.active) {
            endDrop(false);
            render();
            return;
        }
        m_drop.reading = true;
        std::vector<std::string> requested;
        for (std::size_t i = 0; i < m_drop.offered.size(); ++i) {
            const auto& mime = m_drop.offered[i];
            if (std::find(m_drop.accepted.begin(), m_drop.accepted.end(), mime) != m_drop.accepted.end()
                && std::find(requested.begin(), requested.end(), mime) == requested.end()) {
                requested.push_back(mime);
                request({ { "t", "r" }, { "x", std::to_string(i + 1) } });
            }
        }
    }
    if (drop || changed || previousIn != m_drop.inWindow) {
        render();
    }
}

void Dnd::dropData(const DndMeta& meta, std::string_view payload, bool more)
{
    if (m_drop.root) {
        remoteDropData(meta, payload, more);
        return;
    }
    const auto x = AfterglowProto::parseDndInteger(meta.contains("x") ? meta.at("x") : "0").value_or(0);
    if (x < 1 || std::size_t(x) > m_drop.offered.size()) {
        fail("The terminal sent drop data for an index outside the list of offered MIME types");
        return;
    }
    const auto& mime = m_drop.offered[std::size_t(x - 1)];
    auto* d = dest(mime);
    if (!d) {
        fail("The terminal sent drop data for a MIME type that was not asked for: " + mime);
        return;
    }
    // Without a machine id of its own, this side cannot tell that the files
    // are here: it reads them through the terminal, as from another computer.
    if (mime == "text/uri-list" && ((meta.contains("X") && meta.at("X") == "1") || m_machine.empty())) {
        m_drop.remoteClient = true;
    }
    if (!more && payload.empty()) {
        std::string tail;
        if (!d->decoder.finish(tail) || !d->write(tail)) {
            fail("Failed to write the dropped " + mime + " data");
            return;
        }
        d->completed = true;
        d->close();
        requestDone();
        if (mime == "text/uri-list") {
            m_drop.uris = parseUriList(d->buffer);
        }
        const bool pending = std::any_of(m_drop.accepted.begin(), m_drop.accepted.end(), [&](const std::string& m) {
            const auto* other = dest(m);
            return other && !other->completed;
        });
        if (!pending) {
            allMimeDataDropped();
        }
        return;
    }
    std::string bytes;
    if (!d->decoder.feed(payload, bytes) || !d->write(bytes)) {
        fail("Failed to write the dropped " + mime + " data");
    }
}

void Dnd::allMimeDataDropped()
{
    if (m_drop.uris.empty()) {
        m_dropped = 0;
        for (const auto& d : dropDests) {
            m_dropped += d.mime != "text/uri-list" && d.completed ? 1 : 0;
        }
        endDrop(true);
        render();
        return;
    }
    m_drop.staging = tempBase + "/" + std::to_string(++m_tempCounter);
    if (::mkdir(m_drop.staging.c_str(), 0700) != 0) {
        fail("Cannot create " + m_drop.staging + ": " + std::strerror(errno));
        return;
    }
    // Data URIs become files.
    int counter = 0;
    for (const auto& uri : m_drop.uris) {
        if (uri.kind == ParsedUri::Kind::Data) {
            const auto path = m_drop.staging + "/data-uri-" + std::to_string(++counter) + extensionFor(uri.mime);
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd < 0 || !AfterglowProto::writeAll(fd, uri.data)) {
                fail("Cannot write " + path + ": " + std::strerror(errno));
                return;
            }
            ::close(fd);
        }
    }
    if (m_drop.remoteClient) {
        // Files of another computer: read them through the terminal, one directory level at a time.
        m_drop.root = std::make_unique<RemoteEntry>();
        const auto uriIndex = std::find(m_drop.offered.begin(), m_drop.offered.end(), "text/uri-list") - m_drop.offered.begin();
        for (std::size_t i = 0; i < m_drop.uris.size(); ++i) {
            auto entry = std::make_unique<RemoteEntry>();
            const auto& uri = m_drop.uris[i];
            if (uri.kind != ParsedUri::Kind::File || uri.path.empty() || uri.path == "/") {
                entry->skipped = true;
                ++m_drop.root->finished;
            } else {
                entry->base = m_drop.staging;
                entry->name = Path::baseName(uri.path);
                request(AfterglowProto::dndReadEntry(uriIndex + 1, 0, std::int64_t(i + 1)));
            }
            m_drop.root->children.push_back(std::move(entry));
        }
        if (m_drop.root->finished >= m_drop.root->children.size()) {
            allDropDataReceived();
            return;
        }
        m_drop.open = m_drop.root.get();
        return;
    }
    // Files of this computer: copied here directly. A directory that holds
    // the drop directory would be copied into its own copy without end.
    const bool links = !(m_drop.action == CopyOnDrop && m_options.value("copy-mode") == "independent");
    std::string error;
    char resolved[PATH_MAX];
    const std::string into = ::realpath(dropDir.c_str(), resolved) ? std::string(resolved) : dropDir;
    for (const auto& uri : m_drop.uris) {
        if (uri.kind != ParsedUri::Kind::File || uri.path.empty()) {
            continue;
        }
        const std::string source = ::realpath(uri.path.c_str(), resolved) ? std::string(resolved) : uri.path;
        if (uri.path == "/" || source == "/" || into == source || into.starts_with(source + "/")) {
            m_notice = "The drop failed: " + printable(uri.path) + " holds the drop directory";
            endDrop(false);
            render();
            return;
        }
    }
    for (const auto& uri : m_drop.uris) {
        if (uri.kind == ParsedUri::Kind::File && !uri.path.empty() && !copyTree(uri.path, m_drop.staging, links, error)) {
            m_notice = "The drop failed: " + error;
            endDrop(false);
            render();
            return;
        }
    }
    allDropDataReceived();
}

void Dnd::allDropDataReceived()
{
    m_dropped = 0;
    for (const auto& uri : m_drop.uris) {
        m_dropped += (uri.kind == ParsedUri::Kind::File && !uri.path.empty()) || uri.kind == ParsedUri::Kind::Data ? 1 : 0;
    }
    for (const auto& d : dropDests) {
        m_dropped += d.mime != "text/uri-list" && d.completed ? 1 : 0;
    }
    if (!m_drop.staging.empty()) {
        if (m_options.flag("confirm-drop-overwrite")) {
            overwrites(m_drop.staging, dropDir, "", m_drop.overwrites);
            if (!m_drop.overwrites.empty()) {
                render();
                return;
            }
        }
        std::string error;
        if (!renameContents(m_drop.staging, dropDir, error)) {
            m_notice = "The drop failed: " + error;
            endDrop(false);
            render();
            return;
        }
    }
    endDrop(true);
    render();
}

void Dnd::confirmDrop(bool accepted)
{
    m_drop.overwrites.clear();
    if (!accepted) {
        m_dropped = 0;
        endDrop(false);
        render();
        return;
    }
    std::string error;
    if (!renameContents(m_drop.staging, dropDir, error)) {
        m_notice = "The drop failed: " + error;
        endDrop(false);
    } else {
        endDrop(true);
    }
    render();
}

void Dnd::remoteDropData(const DndMeta& meta, std::string_view payload, bool more)
{
    auto number = [&](const char* key) {
        const auto it = meta.find(key);
        return it == meta.end() ? 0 : AfterglowProto::parseDndInteger(it->second).value_or(0);
    };
    auto* open = m_drop.open;
    if (!open) {
        fail("The terminal sent remote drop data without an open directory");
        return;
    }
    const auto parent = number("Y");
    const auto x = number("x");
    const auto y = number("y");
    if (x == 0 && y == 0 && parent == 0) {
        if (!m_drop.current) {
            fail("The terminal sent remote drop data without an entry");
            return;
        }
    } else {
        const auto index = (parent != 0 && parent != 1 ? x : y) - 1;
        if (index < 0 || std::size_t(index) >= open->children.size()) {
            fail("The terminal sent remote drop data for an entry that does not exist");
            return;
        }
        m_drop.current = open->children[std::size_t(index)].get();
    }
    auto* entry = m_drop.current;
    if (entry->complete || entry->skipped) {
        fail("The terminal sent data for an entry that is complete");
        return;
    }
    if (!entry->started) {
        entry->started = true;
        entry->kind = number("X");
        if (entry->kind == AfterglowProto::DndFileKind) {
            entry->fd = ::open((entry->base + "/" + entry->name).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
            if (entry->fd < 0) {
                fail("Cannot create " + entry->base + "/" + entry->name + ": " + std::strerror(errno));
                return;
            }
        }
    }
    std::string bytes;
    if (!entry->decoder.feed(payload, bytes)) {
        fail("The terminal sent invalid base64");
        return;
    }
    // A link holds one target, a listing its names: both are bounded.
    const std::size_t limit = entry->kind == AfterglowProto::DndLinkKind ? MaxLinkTarget : MaxListing;
    if (entry->fd < 0 && entry->buffer.size() + bytes.size() > limit) {
        fail("The terminal sent too much data for " + entry->name);
        return;
    }
    if (more || !payload.empty()) {
        if (entry->fd < 0) {
            entry->buffer += bytes;
        } else if (!AfterglowProto::writeAll(entry->fd, bytes)) {
            fail("Cannot write " + entry->name + ": " + std::strerror(errno));
        }
        return;
    }
    // The end of the entry.
    if (!entry->decoder.finish(bytes)) {
        fail("The terminal sent invalid base64");
        return;
    }
    const auto path = entry->base + "/" + entry->name;
    if (entry->fd >= 0) {
        const bool ok = AfterglowProto::writeAll(entry->fd, bytes);
        ::close(entry->fd);
        entry->fd = -1;
        if (!ok) {
            fail("Cannot write " + path + ": " + std::strerror(errno));
            return;
        }
    } else {
        entry->buffer += bytes;
        if (entry->kind == AfterglowProto::DndLinkKind) {
            if (::symlink(entry->buffer.c_str(), path.c_str()) != 0) {
                fail("Cannot create the link " + path + ": " + std::strerror(errno));
                return;
            }
        } else {
            if (::mkdir(path.c_str(), 0755) != 0) {
                fail("Cannot create " + path + ": " + std::strerror(errno));
                return;
            }
            const auto names = AfterglowProto::dndSplitNames(entry->buffer);
            if (!names) {
                fail("The terminal sent an unsafe name in the directory " + entry->name);
                return;
            }
            m_drop.entries += names->size();
            if (m_drop.entries > MaxRemoteEntries) {
                fail("The terminal sent too many entries");
                return;
            }
            for (const auto& name : *names) {
                auto child = std::make_unique<RemoteEntry>();
                child->base = path;
                child->name = name;
                entry->children.push_back(std::move(child));
            }
        }
        entry->buffer.clear();
    }
    entry->complete = true;
    m_drop.current = nullptr;
    ++open->finished;
    requestDone();
    if (open->finished < open->children.size()) {
        return;
    }
    // Every entry of this directory arrived: close its handle and go on with
    // the directories in it, breadth first.
    m_drop.open = nullptr;
    if (open->kind > AfterglowProto::DndLinkKind) {
        send(AfterglowProto::dndReadEntry(0, open->kind, 0));
    }
    for (auto& child : open->children) {
        if (child->kind > AfterglowProto::DndLinkKind) {
            if (child->children.empty()) {
                send(AfterglowProto::dndReadEntry(0, child->kind, 0));
            } else {
                m_drop.pendingDirs.push_back(child.get());
            }
        }
    }
    if (m_drop.pendingDirs.empty()) {
        allDropDataReceived();
        return;
    }
    m_drop.open = m_drop.pendingDirs.front();
    m_drop.pendingDirs.pop_front();
    for (std::size_t i = 0; i < m_drop.open->children.size(); ++i) {
        request(AfterglowProto::dndReadEntry(0, m_drop.open->kind, std::int64_t(i + 1)));
    }
}

void Dnd::dispatch(const DndMeta& meta, std::string_view payload, bool more)
{
    auto number = [&](const char* key) {
        const auto it = meta.find(key);
        return it == meta.end() ? 0 : int(AfterglowProto::parseDndInteger(it->second).value_or(0));
    };
    const auto type = meta.contains("t") ? meta.at("t") : std::string("a");
    if (type == "m" || type == "M") {
        dropMove(number("x"), number("y"), more, payload, type == "M", number("o"));
    } else if (type == "R") {
        fail("Error from the terminal while reading dropped data: " + printable(payload));
    } else if (type == "r") {
        dropData(meta, payload, more);
    } else if (type == "o") {
        potentialDrag(number("x"), number("y"));
    } else if (type == "E") {
        if (payload == "OK") {
            if (m_drag.active && !m_drag.accepted) {
                m_drag.accepted = true;
                render();
            }
        } else {
            m_notice = "The terminal refused the drag: " + printable(payload);
            resetDrag();
            render();
        }
    } else if (type == "e") {
        dragEvent(number("x"), number("y"), meta.contains("o") ? number("o") : -1);
    } else if (type == "k") {
        remoteRequest(number("x") - 1);
    }
}

int Dnd::run()
{
    m_allowDrops = !dropDests.empty();
    m_allowDrags = !dragSources.empty();
    resetDrop();
    // The alternate screen, no wrapping, no cursor, and mouse reports, so
    // that a press starts no selection in the terminal.
    m_tty.enterModes("\x1b[?1049h\x1b[?7l\x1b[?25l\x1b[?1002h\x1b[?1006h\x1b]2;Drag and drop\x1b\\",
                     "\x1b[?1006l\x1b[?1002l\x1b[?25h\x1b[?7h\x1b[?1049l");
    const auto& machine = m_machine;
    if (m_allowDrops) {
        std::string list;
        for (const auto& d : dropDests) {
            list += (list.empty() ? "" : " ") + d.mime;
        }
        send({ { "t", "a" } }, list);
        if (!machine.empty()) {
            send({ { "t", "a" }, { "x", "1" } }, machine);
        }
    }
    if (m_allowDrags) {
        send({ { "t", "o" }, { "x", "1" } }, machine);
    }
    render();
    while (!m_quit) {
        pump();
        std::vector<Input> inputs;
        if (!m_tty.turn(m_drag.jobs.empty() && m_drag.remote.empty() ? 200 : 5, inputs)) {
            break;
        }
        for (const auto& input : inputs) {
            if (m_quit) {
                break;
            }
            if (input.type == Input::Type::Osc && input.data.starts_with("72;")) {
                const auto [raw, payload] = AfterglowProto::splitDnd(std::string_view(input.data).substr(3));
                const auto parsed = AfterglowProto::parseDndMeta(raw);
                if (!parsed) {
                    continue;
                }
                const bool more = AfterglowProto::dndMore(raw);
                // The chunks after the first carry the metadata of the first.
                DndMeta meta = *parsed;
                if (m_chunking) {
                    meta = m_chunkMeta;
                    meta["m"] = more ? "1" : "0";
                }
                if (more && !m_chunking) {
                    m_chunkMeta = *parsed;
                }
                m_chunking = more;
                dispatch(meta, payload, more);
            } else if (input.type == Input::Type::Interrupt) {
                m_quit = true;
            } else if (input.type == Input::Type::Escape) {
                if (!m_drop.overwrites.empty()) {
                    confirmDrop(false);
                } else if (exitOn("esc-key")) {
                    m_quit = true;
                }
            } else if (input.type == Input::Type::Text && !m_drop.overwrites.empty() && input.data.find('\r') != std::string::npos) {
                confirmDrop(true);
            }
        }
        if (m_tty.takeSignal()) {
            m_quit = true;
        }
        if (m_tty.takeResize()) {
            render();
        }
    }
    if (m_drag.active) {
        finishDrag({});
    }
    endDrop(false);
    if (m_allowDrops) {
        send({ { "t", "A" } });
    }
    if (m_allowDrags) {
        send({ { "t", "o" }, { "x", "2" } });
    }
    m_tty.restore();
    if (!m_error.empty()) {
        report("dnd", m_error);
        return 1;
    }
    return 0;
}

} // namespace

int runDnd(int argc, char** argv)
{
    std::string error;
    bool help = false;
    const auto parsed = Cli::parse(command, argc, argv, error, help);
    if (!parsed) {
        if (help) {
            std::fputs(Cli::helpText(command).c_str(), stdout);
            return 0;
        }
        report("dnd", error);
        return 1;
    }
    const auto home = homeDirectory();
    const auto cwd = currentDirectory();
    Tty tty;
    Dnd dnd(tty, *parsed);
    // Drop destinations: standard output for text, the files for a list of URIs, and --drop.
    if (!::isatty(STDOUT_FILENO)) {
        DropDest d;
        d.mime = "text/plain";
        d.human = "STDOUT";
        d.toStdout = true;
        dnd.dropDests.push_back(std::move(d));
    }
    {
        DropDest d;
        d.mime = "text/uri-list";
        d.human = "Files";
        dnd.dropDests.push_back(std::move(d));
    }
    for (const auto& spec : parsed->list("drop")) {
        const auto colon = spec.find(':');
        const auto mime = spec.substr(0, colon);
        const auto path = colon == std::string::npos ? std::string() : spec.substr(colon + 1);
        std::erase_if(dnd.dropDests, [&](const DropDest& d) { return d.mime == mime; });
        if (!path.empty()) {
            DropDest d;
            d.mime = mime;
            d.human = path;
            d.path = Path::absolute(path, home, cwd);
            dnd.dropDests.push_back(std::move(d));
        }
    }
    std::string stdinData;
    bool stdinRead = false;
    for (const auto& spec : parsed->list("drag")) {
        const auto colon = spec.find(':');
        if (colon == std::string::npos) {
            report("dnd", "Invalid drag source " + spec + ": it must be MIME:PATH");
            return 1;
        }
        DragSource s;
        s.mime = spec.substr(0, colon);
        const auto path = spec.substr(colon + 1);
        if (path == "-" || path == "/dev/stdin") {
            if (!stdinRead && !readAll(STDIN_FILENO, stdinData)) {
                report("dnd", "Failed to read standard input: " + std::string(std::strerror(errno)));
                return 1;
            }
            stdinRead = true;
            s.human = "STDIN";
            s.data = stdinData;
        } else {
            s.human = path;
            s.path = Path::absolute(path, home, cwd);
        }
        std::erase_if(dnd.dragSources, [&](const DragSource& d) { return d.mime == s.mime; });
        dnd.dragSources.push_back(std::move(s));
    }
    const bool hasPlain = std::any_of(dnd.dragSources.begin(), dnd.dragSources.end(), [](const DragSource& s) { return s.mime == "text/plain"; });
    if (!stdinRead && !hasPlain && !::isatty(STDIN_FILENO)) {
        std::string data;
        if (readAll(STDIN_FILENO, data) && !data.empty()) {
            dnd.dragSources.push_back({ "text/plain", "STDIN", "", std::move(data), {} });
        }
    }
    std::vector<UriItem> uris;
    for (const auto& arg : parsed->args) {
        UriItem item;
        item.path = Path::absolute(arg, home, cwd);
        if (::lstat(item.path.c_str(), &item.st) != 0) {
            report("dnd", arg + ": " + std::strerror(errno));
            return 1;
        }
        if (!S_ISDIR(item.st.st_mode) && !S_ISREG(item.st.st_mode) && !S_ISLNK(item.st.st_mode)) {
            report("dnd", arg + " is not a directory or regular file");
            return 1;
        }
        // A directory's URL ends with a slash, as the protocol requires.
        item.uri = "file://" + percentEncode(item.path) + (S_ISDIR(item.st.st_mode) && item.path != "/" ? "/" : "");
        uris.push_back(std::move(item));
    }
    if (!uris.empty()) {
        DragSource s { "text/uri-list", "Files", "", "", std::move(uris) };
        for (const auto& item : s.uris) {
            s.data += item.uri + "\r\n";
        }
        std::erase_if(dnd.dragSources, [](const DragSource& d) { return d.mime == "text/uri-list"; });
        dnd.dragSources.push_back(std::move(s));
    }
    if (const auto thumbnail = parsed->value("drag-thumbnail"); !thumbnail.empty()) {
        std::string bytes;
        const int fd = ::open(Path::absolute(thumbnail, home, cwd).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0 || !readAll(fd, bytes)) {
            report("dnd", "Cannot read " + thumbnail + ": " + std::strerror(errno));
            return 1;
        }
        ::close(fd);
        dnd.thumbnail = decodeImage(bytes, ThumbnailBudget, error);
        if (!dnd.thumbnail) {
            report("dnd", "Cannot decode " + thumbnail + ": " + error);
            return 1;
        }
    }
    const auto dropDest = parsed->value("drop-dest");
    dnd.dropDir = dropDest.empty() ? cwd : Path::absolute(dropDest, home, cwd);
    dnd.tempBase = dnd.dropDir + "/.aglow-dnd-drop-XXXXXX";
    if (!::mkdtemp(dnd.tempBase.data())) {
        report("dnd", "Cannot create a staging directory in " + dnd.dropDir + ": " + std::strerror(errno));
        return 1;
    }
    if (!tty.open(error)) {
        removeTree(dnd.tempBase);
        report("dnd", error);
        return 1;
    }
    const int code = dnd.run();
    tty.restore();
    removeTree(dnd.tempBase);
    return code;
}

} // namespace Aglow
