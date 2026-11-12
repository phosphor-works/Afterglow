// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// aglow dnd: kitten dnd's command line and screen. Drags offer files and data
// from this computer; drops copy files and data here, also from another
// computer over SSH, through the terminal (OSC 72).

#include "Cli.h"
#include "Commands.h"
#include "Image.h"
#include "Mime.h"
#include "Tty.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/DndTree.h>
#include <AfterglowProto/DndUri.h>
#include <AfterglowProto/DragDrop.h>
#include <AfterglowProto/Fd.h>
#include <AfterglowProto/Text.h>
#include <AfterglowProto/TransferPath.h>

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
#include <optional>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace Aglow {

namespace {

using AfterglowProto::DndMeta;
using AfterglowProto::DndUri;
namespace Path = AfterglowProto::TransferPath;

constexpr int CopyOnDrop = 1;
constexpr int MoveOnDrop = 2;
// Reads that wait for the terminal at one time. Afterglow fails a read with
// EMFILE past 64 queued reads.
constexpr std::size_t MaxInFlight = 32;
// Directory handles that a remote drop keeps open in the terminal, with the
// reads in flight, each of which can open one. Afterglow refuses a read
// with ENOMEM past 256. A tree that keeps more directories open than this
// leaves out what is below them.
constexpr std::size_t MaxOpenHandles = 192;
constexpr std::size_t MaxPreSent = 64 * 1024 * 1024;
constexpr std::size_t WriteAhead = 256 * 1024;
// What the terminal can make this side hold: a list of MIME types, a list of
// URIs (Afterglow sends 64 MiB of one type at most), a link target, a
// directory listing (16,384 names of 255 bytes), and the entries of a tree,
// the dropped URIs among them.
constexpr std::size_t MaxControl = 1024 * 1024;
constexpr std::size_t MaxInMemory = 64 * 1024 * 1024;
constexpr std::size_t MaxLinkTarget = 4096;
constexpr std::size_t MaxListing = 16384 * 256;
constexpr std::size_t MaxRemoteEntries = 1 << 20;
// The decoded pixels of a drag image before it is scaled down, and the
// largest image file that is read for one.
constexpr std::size_t ThumbnailBudget = 64 * 1024 * 1024;
constexpr std::size_t ThumbnailFileLimit = 64 * 1024 * 1024;
// A drag image comes from one of the first dragged files: each is probed
// with one read of its first bytes, which hold the image size of nearly
// every file. A JPEG whose metadata comes first and passes them gets no drag
// image.
constexpr std::size_t ThumbnailProbes = 8;
constexpr std::size_t ThumbnailHeader = 64 * 1024;
// The prefix of the staging directory of a drop, in the drop directory.
constexpr std::string_view StagingPrefix = ".aglow-dnd-drop-";

const Cli::Command command {
    "dnd",
    "[files to drag]",
    "Drag and drop files and data between the shell and other programs, even over SSH.",
    "The files and directories on the command line are offered as text/uri-list. Start dragging anywhere in the "
    "window to drag them to a file manager, or to another window that runs aglow dnd. Directories are dragged "
    "with their contents. When a move is dropped on another computer, the files that it took are deleted here.\n"
    "\n"
    "Files dropped on the window are copied, or moved, into the working directory, or into --drop-dest. Drop "
    "them on the Copy or Move box to choose. Data on standard input is offered as text/plain. Dropped text/plain "
    "goes to standard output when it is not a terminal.\n"
    "\n"
    "Press Ctrl+C to quit, which cancels a drag in progress. Esc quits too, unless --exit-on leaves out esc-key.",
    {
        { "--drag", Cli::Option::Kind::List, "", {},
          "When a drag starts, offer this file as the data of this MIME type, as MIME:PATH, for example "
          "image/jpeg:picture.jpg. Give it again for more types. Give the path as - or as /dev/stdin to offer "
          "standard input." },
        { "--drop", Cli::Option::Kind::List, "", {},
          "Write the dropped data of this MIME type to this file, as MIME:PATH. Give it again for more types. "
          "Without a path, the type is not accepted. For example, text/plain: and text/uri-list: turn those types "
          "off." },
        { "--drop-dest", Cli::Option::Kind::String, "", {},
          "The directory where dropped files go. The default is the working directory." },
        { "--copy-mode", Cli::Option::Kind::Choice, "auto", { "auto", "independent" },
          "How a copy drop copies local files. auto makes hard links where it can, and copies the data where it "
          "cannot. independent always copies, so that a change to a dropped file never changes its source. A move "
          "always uses hard links where it can." },
        { "--confirm-drop-overwrite", Cli::Option::Kind::Flag, "", {},
          "Ask before a drop of files replaces existing files. Files that replace nothing are copied without a "
          "question." },
        { "--confirm-drop-timeout", Cli::Option::Kind::Float, "20", {},
          "The seconds that the question of --confirm-drop-overwrite waits for an answer, counted from the last "
          "message of the drop, so the time that aglow takes to copy the files counts too. Then the drop is "
          "canceled. Afterglow stops waiting for a drop after 30 seconds without a message from aglow.",
          1, 25 },
        { "--drop-anywhere", Cli::Option::Kind::Choice, "disallowed", { "disallowed", "copy", "move" },
          "Accept drops anywhere in the window, not only on the Copy and Move boxes, with this action." },
        { "--drag-thumbnail", Cli::Option::Kind::String, "", {},
          "An image to show under the pointer while dragging. Without it, an image that is dragged is shown, or "
          "an icon." },
        { "--drag-thumbnail-size", Cli::Option::Kind::Int, "512", {},
          "The largest side of the drag image in pixels. A larger image is scaled down. A terminal can refuse a "
          "drag with an image that is too large.",
          16, 4096 },
        { "--exit-on", Cli::Option::Kind::String, "esc-key", {},
          "The events to exit on, separated by commas: drag-finish, drop-finish and esc-key. The first two are a "
          "completed drag and a completed drop." },
    },
};

std::string joinWords(const std::vector<std::string>& items)
{
    std::string s;
    for (const auto& i : items) {
        s += (s.empty() ? "" : " ") + i;
    }
    return s;
}

// A number of the metadata; parseDndMeta() checked it.
std::int64_t metaNumber(const DndMeta& meta, const char* key)
{
    const auto it = meta.find(key);
    return it == meta.end() ? 0 : AfterglowProto::parseDndInteger(it->second).value_or(0);
}

// MIME:PATH, split at the first colon.
std::pair<std::string, std::optional<std::string>> splitSpec(const std::string& spec)
{
    const auto colon = spec.find(':');
    if (colon == std::string::npos) {
        return { spec, std::nullopt };
    }
    return { spec.substr(0, colon), spec.substr(colon + 1) };
}

// The events of --exit-on, without the spaces around them.
std::vector<std::string> exitEvents(const std::string& list)
{
    std::vector<std::string> events;
    for (std::size_t start = 0; start <= list.size();) {
        const auto end = std::min(list.find(',', start), list.size());
        std::string item = list.substr(start, end - start);
        item.erase(0, std::min(item.find_first_not_of(' '), item.size()));
        while (!item.empty() && item.back() == ' ') {
            item.pop_back();
        }
        events.push_back(std::move(item));
        start = end + 1;
    }
    return events;
}

bool validUtf8(std::string_view text)
{
    std::size_t i = 0;
    char32_t cp = 0;
    while (i < text.size()) {
        if (!AfterglowProto::nextUtf8(text, i, cp)) {
            return false;
        }
    }
    return true;
}

std::string errorText(int error) { return std::strerror(error); }

// A coordinate or an operation of the metadata, which parseDndMeta() bounds
// to INT32_MIN..UINT32_MAX.
int metaInt(const DndMeta& meta, const char* key)
{
    return int(std::clamp<std::int64_t>(metaNumber(meta, key), INT_MIN, INT_MAX));
}

// The shared state of one drag source: data in memory is shared by every
// --drag spec of standard input and by each request of it.
struct UriItem
{
    std::string path;
    std::string uri;
    struct stat st { };
};

struct DragSource
{
    std::string mime;
    std::string path; // empty: the data is in memory
    std::shared_ptr<const std::string> data;
    std::vector<UriItem> uris;
};

// Creates the missing directories of @p path, as mkdir -p does.
bool makeDirectories(const std::string& path)
{
    struct stat st { };
    if (::stat(path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return true;
        }
        errno = ENOTDIR;
        return false;
    }
    const auto parent = Path::dirName(path);
    if (parent != path && !makeDirectories(parent)) {
        return false;
    }
    return ::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

struct DropDest
{
    std::string mime;
    std::string path; // empty: kept in memory (or standard output)
    bool toStdout = false;
    bool completed = false;
    int fd = -1;
    // A file of its own goes to a temporary name next to it, and replaces the
    // file only when the drop succeeds: a failed drop leaves the file as it
    // was. Other paths are written in place (see open()).
    std::string temporary;
    std::string buffer;
    AfterglowProto::DndBase64Stream decoder;

    bool write(std::string_view bytes, mode_t umask)
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
        if (fd < 0 && !open(umask)) {
            return false;
        }
        return AfterglowProto::writeAll(fd, bytes);
    }
    bool open(mode_t umask)
    {
        if (!makeDirectories(Path::dirName(path))) {
            return false;
        }
        struct stat st { };
        const bool exists = ::lstat(path.c_str(), &st) == 0;
        if (!exists && errno != ENOENT) {
            return false;
        }
        // A rename would put a new file in the place of a device or FIFO
        // (/dev/stdout), of a link, of a file with other names, or of a file
        // of another owner: those are written in place, as a shell
        // redirection writes them.
        if (!exists || (S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_uid == ::geteuid())) {
            std::string name = Path::dirName(path) + "/." + Path::baseName(path) + ".aglow-XXXXXX";
            const int temp = ::mkostemp(name.data(), O_CLOEXEC);
            if (temp < 0) {
                // A file that can be written in a directory that cannot.
                if (!exists || (errno != EACCES && errno != EPERM)) {
                    return false;
                }
            } else if (::fchmod(temp, exists ? st.st_mode & 0777 : 0666 & ~umask) == 0
                       && (!exists || ::fchown(temp, uid_t(-1), st.st_gid) == 0)) {
                fd = temp;
                temporary = std::move(name);
                return true;
            } else {
                const int error = errno;
                ::close(temp);
                ::unlink(name.c_str());
                // The group of the file, which this user is not a member of.
                if (error != EPERM) {
                    errno = error;
                    return false;
                }
            }
        }
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666 & ~umask);
        return fd >= 0;
    }
    bool close()
    {
        const bool ok = fd < 0 || ::close(fd) == 0;
        fd = -1;
        return ok;
    }
    /// Puts the file in place.
    bool commit()
    {
        if (temporary.empty()) {
            return true;
        }
        const bool ok = ::rename(temporary.c_str(), path.c_str()) == 0;
        if (ok) {
            temporary.clear();
        }
        return ok;
    }
    void reset()
    {
        close();
        if (!temporary.empty()) {
            ::unlink(temporary.c_str());
            temporary.clear();
        }
        std::string().swap(buffer);
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

// A file, link or directory that went to the drop target, as it was then:
// a move removes only these, and only when they are still the same. The
// same inode is not enough: a file written after its data went keeps it.
struct Transferred
{
    std::string path;
    dev_t dev = 0;
    ino_t ino = 0;
    bool directory = false;
    off_t size = 0;
    timespec mtime { };

    Transferred() = default;
    Transferred(std::string p, const struct stat& st)
        : path(std::move(p))
        , dev(st.st_dev)
        , ino(st.st_ino)
        , directory(S_ISDIR(st.st_mode))
        , size(st.st_size)
        , mtime(st.st_mtim)
    {
    }
    /// Whether @p st, from lstat now, is still this entry. A directory
    /// changes as its entries go: only the inode counts for it.
    bool same(const struct stat& st) const
    {
        if (st.st_dev != dev || st.st_ino != ino || S_ISDIR(st.st_mode) != directory) {
            return false;
        }
        return directory
            || (st.st_size == size && st.st_mtim.tv_sec == mtime.tv_sec && st.st_mtim.tv_nsec == mtime.tv_nsec);
    }
};

// Data that goes to the terminal in pieces: a file, or bytes in memory.
struct Job
{
    DndMeta meta;
    int fd = -1;
    const std::string* memory = nullptr;
    std::string data;
    std::size_t offset = 0;
    std::optional<Transferred> source; ///< a t=k entry of this computer
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
    RemoteEntry* parent = nullptr;
    std::string base; // the directory it goes in
    std::string name;
    std::int64_t kind = -1;
    std::vector<std::unique_ptr<RemoteEntry>> children;
    std::size_t next = 0; ///< the next child to read
    std::size_t finished = 0;
    bool skipped = false;
    bool requested = false;
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

std::optional<std::vector<std::string>> listDirectory(const std::string& path)
{
    DIR* dir = ::opendir(path.c_str());
    if (!dir) {
        return std::nullopt;
    }
    std::vector<std::string> names;
    errno = 0;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
            names.push_back(name);
        }
    }
    const int error = errno;
    ::closedir(dir);
    if (error != 0) {
        errno = error;
        return std::nullopt;
    }
    std::sort(names.begin(), names.end());
    return names;
}

// The target of a link. A target that fills the buffer may be cut short.
bool readLink(const std::string& path, std::string& target)
{
    char buffer[PATH_MAX + 1];
    const auto n = ::readlink(path.c_str(), buffer, sizeof buffer);
    if (n < 0) {
        return false;
    }
    if (std::size_t(n) >= sizeof buffer) {
        errno = ENAMETOOLONG;
        return false;
    }
    target.assign(buffer, std::size_t(n));
    return true;
}

// Copies the data of the regular file @p from, which lstat gave @p st, to the
// new file @p to, with its mode (no setuid, setgid or sticky bit, and the
// umask applied) and its times.
bool copyFile(const std::string& from, const std::string& to, const struct stat& st, mode_t umask)
{
    const int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (in < 0) {
        return false;
    }
    struct stat opened { };
    if (::fstat(in, &opened) != 0 || opened.st_dev != st.st_dev || opened.st_ino != st.st_ino) {
        ::close(in);
        errno = EAGAIN;
        return false;
    }
    const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) {
        ::close(in);
        return false;
    }
    int error = 0;
    char buffer[65536];
    for (;;) {
        const ssize_t n = ::read(in, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            error = n == 0 ? 0 : errno;
            break;
        }
        if (Tty::signalPending()) {
            error = EINTR;
            break;
        }
        if (!AfterglowProto::writeAll(out, std::string_view(buffer, std::size_t(n)))) {
            error = errno;
            break;
        }
    }
    const timespec times[2] = { st.st_atim, st.st_mtim };
    if (error == 0 && (::fchmod(out, st.st_mode & 0777 & ~umask) != 0 || ::futimens(out, times) != 0)) {
        error = errno;
    }
    ::close(in);
    if (::close(out) != 0 && error == 0) {
        error = errno;
    }
    if (error != 0) {
        ::unlink(to.c_str());
        errno = error;
    }
    return error == 0;
}

// Copies @p from into the directory @p to, as kitten's local drop does: hard
// links where allowed, else copies. Only files, directories and links: the
// others are counted in @p specials. What was copied goes to @p copied, each
// directory before its contents.
struct TreeCopy
{
    bool links = true;
    mode_t umask = 022;
    std::vector<Transferred> copied;
    std::size_t specials = 0;
    std::string error;

    bool copy(const std::string& from, const std::string& to)
    {
        if (Tty::signalPending()) {
            error = "the copy was interrupted";
            return false;
        }
        struct stat st { };
        if (::lstat(from.c_str(), &st) != 0) {
            error = from + ": " + errorText(errno);
            return false;
        }
        const auto dest = to + "/" + Path::baseName(from);
        const timespec times[2] = { st.st_atim, st.st_mtim };
        if (S_ISDIR(st.st_mode)) {
            // Writable while it fills, whatever the source's mode. An entry
            // that is there already (a link of a name that two drops share)
            // is never written through.
            if (::mkdir(dest.c_str(), 0700) != 0) {
                error = dest + ": " + errorText(errno);
                return false;
            }
            copied.emplace_back(from, st);
            const auto names = listDirectory(from);
            if (!names) {
                error = from + ": " + errorText(errno);
                return false;
            }
            for (const auto& name : *names) {
                if (!copy(from + "/" + name, dest)) {
                    return false;
                }
            }
            // The owner keeps rwx: a merge into a directory that exists
            // moves entries out of this one, and a failed drop removes it.
            if (::chmod(dest.c_str(), (st.st_mode & 0777 & ~umask) | 0700) != 0
                || ::utimensat(AT_FDCWD, dest.c_str(), times, AT_SYMLINK_NOFOLLOW) != 0) {
                error = dest + ": " + errorText(errno);
                return false;
            }
            return true;
        }
        if (S_ISLNK(st.st_mode)) {
            std::string target;
            if (!readLink(from, target) || ::symlink(target.c_str(), dest.c_str()) != 0) {
                error = dest + ": " + errorText(errno);
                return false;
            }
            ::utimensat(AT_FDCWD, dest.c_str(), times, AT_SYMLINK_NOFOLLOW);
            copied.emplace_back(from, st);
            return true;
        }
        if (!S_ISREG(st.st_mode)) {
            ++specials;
            return true;
        }
        if (!(links && ::link(from.c_str(), dest.c_str()) == 0) && !copyFile(from, dest, st, umask)) {
            error = dest + ": " + errorText(errno);
            return false;
        }
        copied.emplace_back(from, st);
        return true;
    }
};

bool removeTree(const std::string& path)
{
    struct stat st { };
    if (::lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT;
    }
    if (S_ISDIR(st.st_mode)) {
        for (const auto& name : listDirectory(path).value_or(std::vector<std::string>())) {
            removeTree(path + "/" + name);
        }
        return ::rmdir(path.c_str()) == 0;
    }
    return ::unlink(path.c_str()) == 0;
}

// Removes the entries of a move that went, the last first, so that each
// directory is empty when its turn comes. An entry that changed since is
// kept, and so is a directory that holds files that did not go.
struct Removal
{
    std::size_t kept = 0;
    std::string error;
};
Removal removeTransferred(const std::vector<Transferred>& entries)
{
    Removal result;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        struct stat st { };
        if (::lstat(it->path.c_str(), &st) != 0 || !it->same(st)) {
            ++result.kept;
            continue;
        }
        if (it->directory) {
            if (::rmdir(it->path.c_str()) != 0) {
                if (errno != ENOTEMPTY && errno != EEXIST && result.error.empty()) {
                    result.error = it->path + ": " + errorText(errno);
                }
                ++result.kept;
            }
        } else if (::unlink(it->path.c_str()) != 0) {
            if (result.error.empty()) {
                result.error = it->path + ": " + errorText(errno);
            }
            ++result.kept;
        }
    }
    return result;
}

// The paths under @p staging that exist under @p dest, except directories that exist in both.
void overwrites(const std::string& staging, const std::string& dest, const std::string& relative, std::vector<std::string>& out)
{
    for (const auto& name : listDirectory(staging + relative).value_or(std::vector<std::string>())) {
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

// Moves the contents of the directory @p from into the directory @p to,
// merging directories that exist in both. Both are open descriptors, so that
// a link that another program puts in the drop directory meanwhile cannot
// take the drop out of it. @p fromPath and @p toPath name them in messages,
// and for a copy across file systems.
bool renameContents(int from, int to, const std::string& fromPath, const std::string& toPath, mode_t umask, std::string& error)
{
    const auto names = listDirectory(fromPath);
    if (!names) {
        error = fromPath + ": " + errorText(errno);
        return false;
    }
    for (const auto& name : *names) {
        const auto there = toPath + "/" + name;
        struct stat st { };
        if (::fstatat(from, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            error = fromPath + "/" + name + ": " + errorText(errno);
            return false;
        }
        if (::renameat(from, name.c_str(), to, name.c_str()) == 0) {
            // Two links of one file: rename does nothing and both stay. The
            // drop would then remove the only copy with its source.
            struct stat left { };
            if (::fstatat(from, name.c_str(), &left, AT_SYMLINK_NOFOLLOW) == 0) {
                error = there + " is the dropped file itself";
                return false;
            }
            continue;
        }
        const int renameError = errno;
        // Merged only into a directory that is one: a link that an earlier
        // drop left there is not followed.
        auto merge = [&] {
            const int child = ::openat(from, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            const int target = ::openat(to, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            const int openError = errno;
            const bool ok = child >= 0 && target >= 0
                && renameContents(child, target, fromPath + "/" + name, there, umask, error);
            if (child >= 0) {
                ::close(child);
            }
            if (target >= 0) {
                ::close(target);
            }
            if (child < 0 || target < 0) {
                error = there + ": " + (openError == ELOOP ? std::string("a symbolic link is in the way") : errorText(openError));
            }
            return ok;
        };
        auto directoryThere = [&] {
            struct stat existing { };
            return ::fstatat(to, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(existing.st_mode);
        };
        if (renameError == EXDEV) {
            // @p to is on another file system: the merge went into a mount
            // point or a btrfs subvolume inside the drop directory. A
            // directory that is there takes the entries one by one. Anything
            // else is copied next to its place, on that file system, and
            // renamed into it: it replaces a file as the rename would have.
            if (S_ISDIR(st.st_mode) && directoryThere()) {
                if (!merge()) {
                    return false;
                }
                continue;
            }
            std::string temporary = toPath + "/" + std::string(StagingPrefix) + "XXXXXX";
            if (!::mkdtemp(temporary.data())) {
                error = toPath + ": " + errorText(errno);
                return false;
            }
            TreeCopy copy;
            copy.links = false;
            copy.umask = umask;
            bool ok = copy.copy(fromPath + "/" + name, temporary);
            if (!ok) {
                error = copy.error;
            } else if (::renameat(AT_FDCWD, (temporary + "/" + name).c_str(), to, name.c_str()) != 0) {
                error = there + ": " + errorText(errno);
                ok = false;
            }
            removeTree(temporary);
            if (!ok) {
                return false;
            }
            removeTree(fromPath + "/" + name);
            continue;
        }
        // EBUSY: the directory there is a mount point, which no rename
        // replaces. The merge goes into it, where each entry gets EXDEV.
        // Other file systems (CIFS, FUSE) give EBUSY without a directory
        // there, and the drop names that error.
        if (S_ISDIR(st.st_mode)
            && (renameError == EEXIST || renameError == ENOTEMPTY || renameError == EISDIR
                || (renameError == EBUSY && directoryThere()))) {
            if (!merge()) {
                return false;
            }
            continue;
        }
        error = there + ": " + errorText(renameError);
        return false;
    }
    return true;
}

struct Picture
{
    std::vector<std::string> packets;
};

class Dnd
{
public:
    Dnd(Tty& tty, const Cli::Parsed& options, mode_t umask)
        : m_tty(tty)
        , m_options(options)
        , m_umask(umask)
    {
    }
    // A command that ends with an exception still leaves no staging
    // directory, and tells the terminal that it takes no more drags and drops.
    ~Dnd() { shutdown(); }
    Dnd(const Dnd&) = delete;
    Dnd& operator=(const Dnd&) = delete;

    std::vector<DragSource> dragSources;
    std::vector<DropDest> dropDests;
    std::optional<Image> thumbnail;
    std::string dropDir;

    int run();

private:
    bool exitOn(std::string_view event) const;
    void send(const DndMeta& meta, std::string_view payload = {})
    {
        if (const auto t = meta.find("t"); t != meta.end() && t->second == "r") {
            m_drop.activity = nowMs();
        }
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
    void packet(std::string_view command);
    void abandonMessage();
    void dispatch(const DndMeta& meta, std::string_view payload, bool more);
    void render();
    void shutdown();
    // Drags
    void potentialDrag(int x, int y);
    void dragImage();
    std::optional<Picture> makePicture();
    /// @p operation is the o key, or -1 when the event has none.
    void dragEvent(std::int64_t x, std::int64_t y, std::int64_t operation);
    void dataRequest(std::int64_t index);
    void remoteRequest(std::int64_t index);
    void pump();
    void resetDrag();
    void finishDrag(std::string_view error);
    void dragFailed(std::string_view error, std::string notice);
    // Drops
    void dropMove(int x, int y, bool more, std::string_view mimes, bool drop, int operation);
    void dropData(const DndMeta& meta, std::string_view payload, bool more);
    void remoteDropData(const DndMeta& meta, std::string_view payload, bool more);
    RemoteEntry* routeEntry(const DndMeta& meta);
    void entryDone(RemoteEntry* entry);
    void scheduleReads();
    void readError(const DndMeta& meta, std::string_view error);
    void allMimeDataDropped();
    void allDropDataReceived();
    void endDrop(bool success);
    void dropFailed(std::string notice);
    void resetDrop();
    void confirmDrop(bool accepted);
    bool confirming() const { return !m_drop.overwrites.empty(); }
    void fail(std::string message)
    {
        m_error = std::move(message);
        m_quit = true;
    }

    Tty& m_tty;
    const Cli::Parsed& m_options;
    const mode_t m_umask;
    const std::string m_machine = AfterglowProto::dndMachineId();
    bool m_allowDrops = false;
    bool m_allowDrags = false;
    bool m_quit = false;
    bool m_shutDown = false;
    bool m_supported = false; ///< the terminal answered the OSC 72 query
    int m_signal = 0;
    std::string m_error;
    std::string m_notice;
    Region m_copy;
    Region m_move;
    // What the last drop that succeeded brought.
    std::size_t m_droppedFiles = 0;
    std::size_t m_droppedData = 0;
    std::size_t m_dataCounter = 0;
    std::optional<Picture> m_picture;
    bool m_pictureMade = false;
    // The drag.
    struct
    {
        bool active = false;
        bool accepted = false;
        std::vector<std::string> mimes;
        int operation = 0;
        bool dropped = false;
        std::deque<Job> jobs;
        std::deque<RemoteItem> remote;
        std::set<std::int64_t> requested;
        std::set<std::int64_t> remoteRequested;
        std::vector<Transferred> transferred;
        std::size_t leftOut = 0; ///< names that are not UTF-8
        std::int64_t handles = AfterglowProto::DndFirstHandle;
    } m_drag;
    // The drop.
    struct
    {
        std::vector<std::string> offered;
        std::vector<std::string> accepted;
        std::vector<std::string> requested;
        std::string mimeBuffer;
        int action = 0;
        int answered = -1; ///< the last answer to the terminal: its o
        std::string answeredList;
        bool inWindow = false;
        bool reading = false;
        bool remoteClient = false;
        int sourceOps = 0;
        std::vector<DndUri> uris;
        std::string staging;
        TreeCopy copy;
        std::size_t inPlace = 0;
        std::size_t failed = 0; ///< remote entries that could not be read
        std::size_t others = 0; ///< URIs that are not files of this computer
        std::int64_t uriIndex = 0;
        std::unique_ptr<RemoteEntry> root;
        std::map<std::int64_t, RemoteEntry*> dirs;
        std::vector<RemoteEntry*> stack;
        RemoteEntry* current = nullptr;
        std::size_t inFlight = 0;
        std::size_t entries = 0;
        std::vector<std::string> overwrites;
        long long deadline = 0; ///< when the overwrite question expires
        // The last message of the drop either way: the terminal waits for
        // the program from then on.
        long long activity = 0;
    } m_drop;
    // Chunked messages from the terminal: the packets after the first carry only m.
    bool m_chunking = false;
    bool m_discarding = false;
    DndMeta m_chunkMeta;
};

bool Dnd::exitOn(std::string_view event) const
{
    const auto events = exitEvents(m_options.value("exit-on"));
    return std::find(events.begin(), events.end(), event) != events.end();
}

void Dnd::render()
{
    const auto size = m_tty.size();
    const int width = std::max<int>(20, size.ws_col);
    const int height = std::max<int>(8, size.ws_row);
    std::string out = "\x1b[?2026h\x1b[H\x1b[2J";
    int y = 0;
    using Marks = std::vector<std::pair<std::string_view, std::string_view>>;
    // Wraps @p raw at spaces, counting cells, and colours the first match of
    // each mark on a line.
    auto paragraph = [&](std::string_view raw, const Marks& marks = {}) {
        const std::string text = printable(raw);
        std::size_t at = 0;
        while (at < text.size() && y < height) {
            std::size_t cells = 0;
            std::size_t cut = text.size();
            std::size_t lastSpace = std::string::npos;
            for (std::size_t i = at; i < text.size();) {
                std::size_t next = i;
                char32_t cp = 0;
                if (!AfterglowProto::nextUtf8(text, next, cp)) {
                    next = i + 1;
                }
                const auto w = std::size_t(cellWidth(cp));
                if (cells + w > std::size_t(width)) {
                    cut = lastSpace != std::string::npos && lastSpace > at ? lastSpace : i;
                    break;
                }
                if (cp == ' ') {
                    lastSpace = i;
                }
                cells += w;
                i = next;
            }
            std::string line = text.substr(at, cut - at);
            for (const auto& [word, sgr] : marks) {
                if (const auto p = line.find(word); p != std::string::npos) {
                    line.replace(p, word.size(), std::string(sgr) + std::string(word) + "\x1b[39m");
                }
            }
            out += line + "\r\n";
            ++y;
            at = cut;
            while (at < text.size() && text[at] == ' ') {
                ++at;
            }
        }
    };
    m_copy = m_move = Region {};
    if (!m_notice.empty()) {
        paragraph(m_notice);
        out += "\r\n";
        ++y;
    }
    if (m_drag.active) {
        paragraph("Dragging data…");
        if (m_drag.dropped) {
            paragraph("The dragged data was dropped. Waiting for the target to ask for the data…");
        } else if (m_drag.operation > 0) {
            paragraph("Another window is willing to accept the dragged data.");
        }
        m_tty.write(out + "\x1b[?2026l");
        return;
    }
    if (confirming()) {
        const auto seconds = std::max(0LL, (m_drop.deadline - nowMs() + 999) / 1000);
        paragraph("Some of the dropped files will overwrite existing files, listed below. Press Enter to drop anyway "
                  "or Esc to cancel the drop. The drop is canceled in "
                      + std::to_string(seconds) + (seconds == 1 ? " second." : " seconds."),
                  { { "Enter", "\x1b[32m" }, { "Esc", "\x1b[31m" } });
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
            out += "… (" + std::to_string(m_drop.overwrites.size() - shown) + " more)";
        }
        m_tty.write(out + "\x1b[?2026l");
        return;
    }
    if (m_drop.reading) {
        paragraph("Reading the dropped data. Please wait…");
        m_tty.write(out + "\x1b[?2026l");
        return;
    }
    const std::string quitKey = exitOn("esc-key") ? "Esc" : "Ctrl+C";
    if (m_drop.inWindow) {
        if (m_drop.action == 0) {
            paragraph("A drag is active. Drop it into one of the boxes below to perform that action on the dragged data. "
                      "Available MIME types in the drag:");
            out += "\r\n";
            paragraph(joinWords(m_drop.offered));
        } else if (m_drop.accepted.empty()) {
            paragraph("This window takes none of the MIME types in the drag:");
            out += "\r\n";
            paragraph(joinWords(m_drop.offered));
        } else {
            paragraph("The drag can be dropped. Supported MIME types:");
            out += "\r\n";
            paragraph(joinWords(m_drop.accepted));
        }
    } else {
        if (m_allowDrags) {
            paragraph("Start dragging anywhere in this window to initiate a drag and drop. If you start the drag in one of "
                      "the Copy or Move boxes below, only that action is allowed when dropping. Elsewhere, the drop "
                      "destination can pick either copy or move.");
            out += "\r\n";
        }
        if (m_allowDrops) {
            if (m_droppedFiles > 0 || m_droppedData > 0) {
                std::string what;
                if (m_droppedFiles > 0) {
                    what = std::to_string(m_droppedFiles) + (m_droppedFiles == 1 ? " file" : " files");
                }
                if (m_droppedData > 0) {
                    what += (what.empty() ? "" : " and ") + std::string("the data of ") + std::to_string(m_droppedData)
                        + (m_droppedData == 1 ? " MIME type" : " MIME types");
                }
                paragraph("The last drop brought " + what + ". You can drop more data or press " + quitKey + " to quit.");
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
    // A drop that reads or asks owns the screen and the staging directory,
    // which a drag of the drop directory would take along.
    if (m_drop.reading) {
        m_notice = "A drag cannot start while a drop is in progress.";
        render();
        return;
    }
    m_notice.clear();
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
    send({ { "t", "o" }, { "o", std::to_string(actions) } }, joinWords(mimes));
    // Data in memory goes before the drag starts: some desktops need it then.
    std::size_t preSent = 0;
    for (std::size_t i = 0; i < dragSources.size(); ++i) {
        const auto& s = dragSources[i];
        if (s.path.empty() && s.data && !s.data->empty() && preSent + s.data->size() < MaxPreSent) {
            preSent += s.data->size();
            sendPackets(AfterglowProto::dndDataPackets({ { "t", "p" }, { "x", std::to_string(i) } }, *s.data));
        }
    }
    m_drag.mimes = mimes;
    dragImage();
    send({ { "t", "P" }, { "x", "-1" } });
    m_drag.active = true;
    render();
}

std::optional<Picture> Dnd::makePicture()
{
    std::optional<Image> image = thumbnail;
    std::size_t probes = 0;
    // The first file whose first bytes name an image of a size that fits.
    auto tryPath = [&](const std::string& path) {
        if (image || probes >= ThumbnailProbes) {
            return;
        }
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        struct stat st { };
        if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || std::size_t(st.st_size) > ThumbnailFileLimit) {
            if (fd >= 0) {
                ::close(fd);
            }
            return;
        }
        ++probes;
        std::string header(ThumbnailHeader, '\0');
        const auto n = ::pread(fd, header.data(), header.size(), 0);
        int w = 0;
        int h = 0;
        if (n <= 0 || !imageSize(std::string_view(header).substr(0, std::size_t(n)), w, h)
            || std::size_t(w) * std::size_t(h) * 4 > ThumbnailBudget) {
            ::close(fd);
            return;
        }
        std::string bytes;
        const bool read = readAll(fd, bytes);
        ::close(fd);
        std::string error;
        if (read) {
            image = decodeImage(bytes, ThumbnailBudget, error, 1);
        }
        // One image file is read whole at most, decoded or not.
        probes = ThumbnailProbes;
    };
    for (const auto& s : dragSources) {
        if (!s.path.empty() && s.mime.starts_with("image/")) {
            tryPath(s.path);
        }
    }
    if (const auto* uris = source("text/uri-list")) {
        for (const auto& item : uris->uris) {
            if (S_ISREG(item.st.st_mode)) {
                tryPath(item.path);
            }
        }
    }
    if (!image) {
        return std::nullopt;
    }
    const auto limit = m_options.integer("drag-thumbnail-size");
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
    return Picture { AfterglowProto::dndDataPackets({ { "t", "p" },
                                                      { "x", "-1" },
                                                      { "y", opaque ? "24" : "32" },
                                                      { "X", std::to_string(width) },
                                                      { "Y", std::to_string(height) } },
                                                    pixels) };
}

void Dnd::dragImage()
{
    // The sources do not change between drags: the image is made once.
    if (!m_pictureMade) {
        m_picture = makePicture();
        m_pictureMade = true;
    }
    if (m_picture) {
        sendPackets(m_picture->packets);
        return;
    }
    // A symbol as the drag image: the terminal draws it.
    bool directory = false;
    if (const auto* uris = source("text/uri-list")) {
        directory = !uris->uris.empty() && S_ISDIR(uris->uris.front().st.st_mode);
    }
    sendPackets(AfterglowProto::dndDataPackets({ { "t", "p" }, { "x", "-1" }, { "X", "6" }, { "Y", "1" } },
                                               directory ? "📁" : "📄"));
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
    m_drag.operation = 0;
    m_drag.requested.clear();
    m_drag.remoteRequested.clear();
    m_drag.transferred.clear();
    m_drag.leftOut = 0;
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

void Dnd::dragFailed(std::string_view error, std::string notice)
{
    finishDrag(error);
    m_notice = std::move(notice);
    render();
}

void Dnd::dragEvent(std::int64_t x, std::int64_t y, std::int64_t operation)
{
    if (!m_drag.active) {
        return;
    }
    switch (x) {
    case 2:
        m_drag.operation = int(std::clamp<std::int64_t>(operation, 0, 3));
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
        const auto transferred = std::move(m_drag.transferred);
        const auto leftOut = m_drag.leftOut;
        resetDrag();
        if (dropped && exitOn("drag-finish")) {
            m_quit = true;
        }
        // A move to another computer took copies: the files here that it
        // took go. Files that it never read stay.
        if (dropped && move) {
            const auto removal = removeTransferred(transferred);
            if (!removal.error.empty()) {
                m_notice = "Some files were not removed after the move. " + removal.error;
            } else if (removal.kept > 0 || leftOut > 0) {
                m_notice = "Some files stayed here after the move, because the drop did not take them.";
            }
            dragSources.clear();
            m_allowDrags = false;
            send({ { "t", "o" }, { "x", "2" } });
            if (!m_allowDrops) {
                m_quit = true;
            }
        } else if (leftOut > 0) {
            m_notice = std::to_string(leftOut) + " names that are not UTF-8 were left out of the drag.";
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

void Dnd::dataRequest(std::int64_t index)
{
    if (index < 0 || std::size_t(index) >= m_drag.mimes.size()) {
        dragFailed("EINVAL", "The terminal asked for drag data with an index out of bounds: " + std::to_string(index));
        return;
    }
    // The terminal asks once for each type: a repeat would queue the data again.
    if (!m_drag.requested.insert(index).second) {
        return;
    }
    const auto* s = source(m_drag.mimes[std::size_t(index)]);
    if (!s) {
        dragFailed("ENOENT", "The terminal asked for a MIME type that the drag does not have.");
        return;
    }
    Job job;
    job.meta = { { "t", "e" }, { "y", std::to_string(index) } };
    if (s->path.empty()) {
        job.memory = s->data.get();
    } else {
        // Not blocking: a FIFO that no program writes must not stop the drag.
        job.fd = ::open(s->path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        struct stat st { };
        if (job.fd < 0 || ::fstat(job.fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            const int error = job.fd < 0 ? errno : EINVAL;
            if (job.fd >= 0) {
                ::close(job.fd);
            }
            dragFailed("EIO", "Cannot read " + s->path + ": "
                           + (error == EINVAL ? std::string("it is not a regular file") : errorText(error)));
            return;
        }
    }
    m_drag.jobs.push_back(std::move(job));
}

void Dnd::remoteRequest(std::int64_t index)
{
    if (!m_drag.active) {
        return;
    }
    auto* uris = source("text/uri-list");
    if (!uris || index < 0 || std::size_t(index) >= uris->uris.size()) {
        dragFailed("EINVAL", "The terminal asked for an entry of the URI list that does not exist.");
        return;
    }
    if (!m_drag.remoteRequested.insert(index).second) {
        return;
    }
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
                dragFailed("EIO", "Cannot read " + item.path + ": " + errorText(errno));
                return;
            }
            Job job;
            job.source = Transferred(item.path, st);
            if (S_ISDIR(st.st_mode)) {
                const auto all = listDirectory(item.path);
                if (!all) {
                    dragFailed("EIO", "Cannot read the directory " + item.path + ": " + errorText(errno));
                    return;
                }
                const auto handle = m_drag.handles++;
                std::vector<std::string> names;
                for (const auto& name : *all) {
                    struct stat child { };
                    const auto path = item.path + "/" + name;
                    if (::lstat(path.c_str(), &child) != 0
                        || !(S_ISDIR(child.st_mode) || S_ISREG(child.st_mode) || S_ISLNK(child.st_mode))) {
                        continue;
                    }
                    // The protocol carries UTF-8 names only: the others stay here.
                    if (!validUtf8(name) || name.size() > 255) {
                        ++m_drag.leftOut;
                        continue;
                    }
                    names.push_back(name);
                }
                for (std::size_t i = 0; i < names.size(); ++i) {
                    m_drag.remote.push_back({ item.path + "/" + names[i], item.root, handle, std::int64_t(i + 1) });
                }
                job.meta = AfterglowProto::dndRemoteEntry(item.root, item.parent, item.child, handle);
                job.data = AfterglowProto::dndJoinNames(names);
            } else if (S_ISLNK(st.st_mode)) {
                if (!readLink(item.path, job.data)) {
                    dragFailed("EIO", "Cannot read the link " + item.path + ": " + errorText(errno));
                    return;
                }
                job.meta = AfterglowProto::dndRemoteEntry(item.root, item.parent, item.child, AfterglowProto::DndLinkKind);
            } else {
                job.meta = AfterglowProto::dndRemoteEntry(item.root, item.parent, item.child, AfterglowProto::DndFileKind);
                job.fd = ::open(item.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
                struct stat opened { };
                if (job.fd < 0 || ::fstat(job.fd, &opened) != 0 || !S_ISREG(opened.st_mode) || opened.st_dev != st.st_dev
                    || opened.st_ino != st.st_ino) {
                    const int error = job.fd < 0 ? errno : EAGAIN;
                    if (job.fd >= 0) {
                        ::close(job.fd);
                    }
                    dragFailed("EIO", "Cannot read " + item.path + ": "
                                   + (error == EAGAIN ? std::string("it changed while it was dragged") : errorText(error)));
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
                dragFailed("EIO", "Cannot read the dragged data: " + errorText(errno));
                return;
            }
            chunk.resize(std::size_t(n));
        } else {
            const std::string_view all = job.memory ? std::string_view(*job.memory) : std::string_view(job.data);
            chunk = std::string(all.substr(std::min(job.offset, all.size()), AfterglowProto::DndDataChunk * 16));
            job.offset += chunk.size();
        }
        if (chunk.empty()) {
            // The end of the item: an empty message.
            send(job.meta);
            if (job.fd >= 0) {
                ::close(job.fd);
            }
            if (job.source) {
                m_drag.transferred.push_back(std::move(*job.source));
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
    m_drop.requested.clear();
    m_drop.mimeBuffer.clear();
    m_drop.action = 0;
    m_drop.answered = -1;
    m_drop.answeredList.clear();
    m_drop.inWindow = m_drop.reading = m_drop.remoteClient = false;
    m_drop.sourceOps = 0;
    m_drop.uris.clear();
    m_drop.staging.clear();
    m_drop.copy = TreeCopy {};
    m_drop.inPlace = m_drop.failed = m_drop.others = 0;
    m_drop.uriIndex = 0;
    m_drop.root.reset();
    m_drop.dirs.clear();
    m_drop.stack.clear();
    m_drop.current = nullptr;
    m_drop.inFlight = 0;
    m_drop.entries = 0;
    m_drop.overwrites.clear();
    m_drop.deadline = 0;
    m_drop.activity = 0;
}

void Dnd::endDrop(bool success)
{
    if (m_drop.reading) {
        for (auto& d : dropDests) {
            if (success && d.completed && !d.commit()) {
                m_notice = "The drop failed. " + d.path + ": " + errorText(errno);
                success = false;
            }
        }
        int action = success ? m_drop.action : 0;
        std::size_t files = 0;
        for (const auto& uri : m_drop.uris) {
            files += uri.kind != DndUri::Kind::Other ? 1 : 0;
        }
        files -= std::min(files, m_drop.inPlace);
        if (success && action == MoveOnDrop) {
            std::size_t kept = 0;
            if (!m_drop.remoteClient) {
                const auto removal = removeTransferred(m_drop.copy.copied);
                kept = removal.kept + m_drop.copy.specials;
                if (!removal.error.empty()) {
                    m_notice = "Some files were not removed after the move. " + removal.error;
                }
            }
            // A file that stayed where it was, that was not read or not moved
            // is no move: a source that removes its files on a move would remove it.
            if (m_drop.inPlace > 0 || m_drop.failed > 0 || kept > 0) {
                action = m_drop.sourceOps == 0 || (m_drop.sourceOps & CopyOnDrop) ? CopyOnDrop : 0;
            }
        }
        send({ { "t", "r" }, { "o", std::to_string(action) } });
        if (success) {
            m_droppedFiles = files;
            m_droppedData = 0;
            for (const auto& d : dropDests) {
                m_droppedData += d.mime != "text/uri-list" && d.completed ? 1 : 0;
            }
            if (m_drop.inPlace > 0 && m_notice.empty()) {
                m_notice = std::to_string(m_drop.inPlace) + (m_drop.inPlace == 1 ? " file was" : " files were")
                    + " in the drop directory already.";
            }
            if (m_drop.failed > 0 && m_notice.empty()) {
                m_notice = std::to_string(m_drop.failed) + " dropped entries could not be read.";
            }
            if (m_drop.copy.specials > 0 && m_notice.empty()) {
                m_notice = std::to_string(m_drop.copy.specials) + " special files (devices, sockets or FIFOs) were left out.";
            }
            if (m_drop.others > 0 && m_notice.empty()) {
                m_notice = std::to_string(m_drop.others) + " dropped items were left out. They are not files of this computer.";
            }
        }
    }
    resetDrop();
    if (success && exitOn("drop-finish")) {
        m_quit = true;
    }
}

void Dnd::dropFailed(std::string notice)
{
    m_notice = std::move(notice);
    endDrop(false);
    render();
}

void Dnd::dropMove(int x, int y, bool more, std::string_view mimes, bool drop, int operation)
{
    if (!mimes.empty() || more) {
        if (m_drop.mimeBuffer.size() + mimes.size() > MaxControl) {
            fail("The terminal sent a list of MIME types that is too long");
            return;
        }
        m_drop.mimeBuffer += mimes;
        if (more) {
            return;
        }
    }
    const bool hasList = !m_drop.mimeBuffer.empty();
    const bool leave = x < 0 || y < 0;
    if (m_drop.reading) {
        // After a drop, Afterglow sends no event of it, and a desktop's own
        // leave after the drop changes nothing: the read goes on.
        if (leave && !drop) {
            m_drop.mimeBuffer.clear();
            return;
        }
        // A new drag in the window: the terminal gave this drop up.
        m_notice = "The drop was canceled because a new drag came to the window before it ended.";
        const auto list = std::move(m_drop.mimeBuffer);
        resetDrop();
        m_drop.mimeBuffer = list;
    }
    const auto previousAction = m_drop.action;
    const bool previousIn = m_drop.inWindow;
    if (hasList) {
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
    }
    // Only the operations that the source allows.
    if (m_drop.sourceOps != 0 && m_drop.action != 0 && !(m_drop.sourceOps & m_drop.action)) {
        m_drop.action = 0;
    }
    m_drop.inWindow = !leave;
    if (leave) {
        resetDrop();
    }
    // An answer without types takes any of them: a drop that takes none of
    // the offered types answers 0. So does the window that is dragging.
    const int answer = m_drop.action != 0 && !m_drop.accepted.empty() && !m_drag.active ? m_drop.action : 0;
    const std::string answerList = answer != 0 ? joinWords(m_drop.accepted) : std::string();
    if (drop) {
        m_notice.clear();
        if (answer == 0) {
            // The terminal waits for the end of the drop: it ends now.
            send({ { "t", "r" }, { "o", "0" } });
            resetDrop();
            render();
            return;
        }
        m_drop.reading = true;
        m_drop.requested.clear();
        for (std::size_t i = 0; i < m_drop.offered.size(); ++i) {
            const auto& mime = m_drop.offered[i];
            if (std::find(m_drop.accepted.begin(), m_drop.accepted.end(), mime) != m_drop.accepted.end()
                && std::find(m_drop.requested.begin(), m_drop.requested.end(), mime) == m_drop.requested.end()) {
                m_drop.requested.push_back(mime);
                send({ { "t", "r" }, { "x", std::to_string(i + 1) } });
            }
        }
        render();
        return;
    }
    // A move that carries the types (an enter, or new types) waits for an
    // answer in the terminal, also when the answer is the one before.
    if (!leave && (hasList || answer != m_drop.answered || answerList != m_drop.answeredList)) {
        send({ { "t", "m" }, { "o", std::to_string(answer) } }, answerList);
        m_drop.answered = answer;
        m_drop.answeredList = answerList;
    }
    if (previousAction != m_drop.action || previousIn != m_drop.inWindow || hasList) {
        render();
    }
}

void Dnd::dropData(const DndMeta& meta, std::string_view payload, bool more)
{
    // Data of a drop that ended, or that has all its data: late chunks of
    // a read that was in flight, or data that nobody asked for.
    if (!m_drop.reading || confirming()) {
        return;
    }
    if (m_drop.root) {
        remoteDropData(meta, payload, more);
        return;
    }
    const auto x = metaNumber(meta, "x");
    if (x < 1 || std::size_t(x) > m_drop.offered.size()) {
        dropFailed("The drop failed. The terminal sent data for an index outside the list of offered MIME types.");
        return;
    }
    const auto& mime = m_drop.offered[std::size_t(x - 1)];
    auto* d = dest(mime);
    if (!d || std::find(m_drop.requested.begin(), m_drop.requested.end(), mime) == m_drop.requested.end()) {
        dropFailed("The drop failed. The terminal sent data for a MIME type that was not asked for: " + mime);
        return;
    }
    if (d->completed) {
        return;
    }
    // Without a machine id of its own, this side cannot tell that the files
    // are here: it reads them through the terminal, as from another computer.
    if (mime == "text/uri-list" && ((meta.contains("X") && meta.at("X") == "1") || m_machine.empty())) {
        m_drop.remoteClient = true;
    }
    auto failed = [&](bool base64) {
        const int error = errno;
        if (base64) {
            dropFailed("The drop failed. The terminal sent invalid base64 for " + mime + ".");
        } else if (error == EFBIG) {
            dropFailed("The drop failed. The dropped " + mime + " data is larger than 64 MiB.");
        } else {
            dropFailed("The drop failed. Cannot write the dropped " + mime + " data: " + errorText(error));
        }
    };
    std::string bytes;
    if (!more && payload.empty()) {
        if (!d->decoder.finish(bytes)) {
            failed(true);
            return;
        }
        if (!d->write(bytes, m_umask) || !d->close()) {
            failed(false);
            return;
        }
        d->completed = true;
        if (mime == "text/uri-list") {
            m_drop.uris = AfterglowProto::parseDndUriList(d->buffer);
        }
        const bool pending = std::any_of(m_drop.requested.begin(), m_drop.requested.end(), [&](const std::string& m) {
            const auto* other = dest(m);
            return other && !other->completed;
        });
        if (!pending) {
            allMimeDataDropped();
        }
        return;
    }
    if (!d->decoder.feed(payload, bytes)) {
        failed(true);
    } else if (!d->write(bytes, m_umask)) {
        failed(false);
    }
}

void Dnd::allMimeDataDropped()
{
    if (m_drop.uris.empty()) {
        endDrop(true);
        render();
        return;
    }
    m_drop.entries = m_drop.uris.size();
    if (m_drop.entries > MaxRemoteEntries) {
        dropFailed("The drop failed. It holds more than " + std::to_string(MaxRemoteEntries) + " items.");
        return;
    }
    // Each top-level item goes into the drop directory by its name: two with
    // one name would merge, or one would replace the other.
    std::set<std::string> names;
    for (const auto& uri : m_drop.uris) {
        if (uri.kind == DndUri::Kind::Other) {
            ++m_drop.others;
            continue;
        }
        if (uri.kind == DndUri::Kind::File) {
            if (uri.path == "/") {
                dropFailed("The drop failed. It holds the root directory.");
                return;
            }
            if (!names.insert(Path::baseName(uri.path)).second) {
                dropFailed("The drop failed. It holds two files named " + Path::baseName(uri.path) + ".");
                return;
            }
        }
    }
    // The staging directory exists only while a drop is applied, so that no
    // drag of the drop directory takes it along.
    std::string staging = dropDir + "/" + std::string(StagingPrefix) + "XXXXXX";
    if (!::mkdtemp(staging.data())) {
        dropFailed("The drop failed. Cannot create a staging directory in " + dropDir + ": " + errorText(errno));
        return;
    }
    m_drop.staging = staging;
    // Data URIs become files, with names that no file in the drop directory has.
    for (const auto& uri : m_drop.uris) {
        if (uri.kind != DndUri::Kind::Data) {
            continue;
        }
        std::string name;
        struct stat st { };
        do {
            name = "data-uri-" + std::to_string(++m_dataCounter) + Mime::extensionOf(uri.mime);
        } while (names.contains(name) || ::lstat((dropDir + "/" + name).c_str(), &st) == 0);
        names.insert(name);
        const auto path = m_drop.staging + "/" + name;
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        const bool ok = fd >= 0 && AfterglowProto::writeAll(fd, uri.data);
        const int error = errno;
        if (fd >= 0 && ::close(fd) != 0 && ok) {
            dropFailed("The drop failed. Cannot write " + path + ": " + errorText(errno));
            return;
        }
        if (!ok) {
            dropFailed("The drop failed. Cannot write " + path + ": " + errorText(error));
            return;
        }
    }
    if (m_drop.remoteClient) {
        // Files of another computer: read them through the terminal, depth
        // first, so that few directory handles are open there at one time.
        m_drop.root = std::make_unique<RemoteEntry>();
        m_drop.uriIndex = std::find(m_drop.offered.begin(), m_drop.offered.end(), "text/uri-list") - m_drop.offered.begin();
        for (const auto& uri : m_drop.uris) {
            auto entry = std::make_unique<RemoteEntry>();
            entry->parent = m_drop.root.get();
            if (uri.kind != DndUri::Kind::File) {
                entry->skipped = true;
                ++m_drop.root->finished;
            } else {
                entry->base = m_drop.staging;
                entry->name = Path::baseName(uri.path);
            }
            m_drop.root->children.push_back(std::move(entry));
        }
        m_drop.stack.push_back(m_drop.root.get());
        scheduleReads();
        if (m_drop.stack.empty() || m_drop.root->finished >= m_drop.root->children.size()) {
            m_drop.stack.clear();
            allDropDataReceived();
        }
        return;
    }
    // Files of this computer: copied here directly. A directory that holds
    // the drop directory would be copied into its own copy without end. A
    // file that is in the drop directory already stays: its copy would be
    // the file itself, which the move would then remove.
    struct stat into { };
    if (::stat(dropDir.c_str(), &into) != 0) {
        dropFailed("The drop failed. " + dropDir + ": " + errorText(errno));
        return;
    }
    char resolved[PATH_MAX];
    const std::string intoPath = ::realpath(dropDir.c_str(), resolved) ? std::string(resolved) : dropDir;
    std::vector<const DndUri*> sources;
    for (const auto& uri : m_drop.uris) {
        if (uri.kind != DndUri::Kind::File) {
            continue;
        }
        struct stat parent { };
        struct stat st { };
        if (::lstat(uri.path.c_str(), &st) != 0) {
            dropFailed("The drop failed. " + uri.path + ": " + errorText(errno));
            return;
        }
        if (::stat(Path::dirName(uri.path).c_str(), &parent) == 0 && parent.st_dev == into.st_dev && parent.st_ino == into.st_ino) {
            ++m_drop.inPlace;
            continue;
        }
        // A link is copied as a link: only a directory can hold the drop directory.
        if (S_ISDIR(st.st_mode)) {
            const std::string source = ::realpath(uri.path.c_str(), resolved) ? std::string(resolved) : uri.path;
            const std::string staging = ::realpath(m_drop.staging.c_str(), resolved) ? std::string(resolved) : m_drop.staging;
            if (source == "/" || intoPath == source || intoPath.starts_with(source + "/") || source.starts_with(staging + "/")) {
                dropFailed("The drop failed. " + uri.path + " holds the drop directory.");
                return;
            }
        }
        sources.push_back(&uri);
    }
    m_drop.copy.links = !(m_drop.action == CopyOnDrop && m_options.value("copy-mode") == "independent");
    m_drop.copy.umask = m_umask;
    for (const auto* uri : sources) {
        if (!m_drop.copy.copy(uri->path, m_drop.staging)) {
            dropFailed("The drop failed. " + m_drop.copy.error);
            return;
        }
    }
    allDropDataReceived();
}

void Dnd::allDropDataReceived()
{
    if (!m_drop.staging.empty()) {
        if (m_options.flag("confirm-drop-overwrite")) {
            overwrites(m_drop.staging, dropDir, "", m_drop.overwrites);
            if (!m_drop.overwrites.empty()) {
                // The terminal waits from its last message, not from the
                // question: a copy here took some of that time.
                m_drop.deadline = m_drop.activity + (long long)(m_options.real("confirm-drop-timeout") * 1000);
                render();
                return;
            }
        }
        confirmDrop(true);
        return;
    }
    endDrop(true);
    render();
}

void Dnd::confirmDrop(bool accepted)
{
    m_drop.overwrites.clear();
    if (!accepted) {
        endDrop(false);
        render();
        return;
    }
    std::string error;
    const int from = ::open(m_drop.staging.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const int to = ::open(dropDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (from < 0 || to < 0) {
        error = (from < 0 ? m_drop.staging : dropDir) + ": " + errorText(errno);
    }
    const bool ok = from >= 0 && to >= 0 && renameContents(from, to, m_drop.staging, dropDir, m_umask, error);
    if (from >= 0) {
        ::close(from);
    }
    if (to >= 0) {
        ::close(to);
    }
    if (!ok) {
        dropFailed("The drop failed. " + error);
        return;
    }
    endDrop(true);
    render();
}

RemoteEntry* Dnd::routeEntry(const DndMeta& meta)
{
    const auto parent = metaNumber(meta, "Y");
    const auto x = metaNumber(meta, "x");
    const auto y = metaNumber(meta, "y");
    RemoteEntry* dir = m_drop.root.get();
    std::int64_t index = y - 1;
    if (AfterglowProto::dndIsDirectoryKind(parent)) {
        const auto it = m_drop.dirs.find(parent);
        if (it == m_drop.dirs.end()) {
            return nullptr;
        }
        dir = it->second;
        index = x - 1;
    }
    if (index < 0 || std::size_t(index) >= dir->children.size()) {
        return nullptr;
    }
    auto* entry = dir->children[std::size_t(index)].get();
    return entry->requested ? entry : nullptr;
}

void Dnd::scheduleReads()
{
    while (m_drop.inFlight < MaxInFlight) {
        // The deepest directory with entries left to read.
        RemoteEntry* dir = nullptr;
        for (auto it = m_drop.stack.rbegin(); it != m_drop.stack.rend(); ++it) {
            if ((*it)->next < (*it)->children.size()) {
                dir = *it;
                break;
            }
        }
        if (!dir) {
            return;
        }
        // Each read can open a handle. One read always goes, so that the drop
        // goes on: its directory's handle closes when its entries are read.
        if (m_drop.inFlight > 0 && m_drop.dirs.size() + m_drop.inFlight >= MaxOpenHandles) {
            return;
        }
        const auto index = dir->next++;
        auto& child = dir->children[index];
        if (child->skipped) {
            continue;
        }
        child->requested = true;
        ++m_drop.inFlight;
        if (dir == m_drop.root.get()) {
            send(AfterglowProto::dndReadEntry(m_drop.uriIndex + 1, 0, std::int64_t(index + 1)));
        } else {
            send(AfterglowProto::dndReadEntry(0, dir->kind, std::int64_t(index + 1)));
        }
    }
}

void Dnd::entryDone(RemoteEntry* entry)
{
    if (m_drop.inFlight > 0) {
        --m_drop.inFlight;
    }
    if (m_drop.current == entry) {
        m_drop.current = nullptr;
    }
    if (!entry->skipped && AfterglowProto::dndIsDirectoryKind(entry->kind)) {
        if (entry->children.empty()) {
            send(AfterglowProto::dndReadEntry(0, entry->kind, 0));
        } else if (m_drop.dirs.size() >= MaxOpenHandles) {
            // The read that scheduleReads() lets go with every handle in use
            // opened one more. Kept open, each level of a deeper tree would
            // add one: the directory comes without its entries.
            send(AfterglowProto::dndReadEntry(0, entry->kind, 0));
            entry->children.clear();
            ++m_drop.failed;
        } else {
            m_drop.dirs[entry->kind] = entry;
            m_drop.stack.push_back(entry);
        }
    }
    // A directory whose entries are all read closes its handle.
    auto* parent = entry->parent;
    ++parent->finished;
    if (parent->finished >= parent->children.size()) {
        if (parent != m_drop.root.get()) {
            send(AfterglowProto::dndReadEntry(0, parent->kind, 0));
            m_drop.dirs.erase(parent->kind);
        }
        std::erase(m_drop.stack, parent);
    }
    scheduleReads();
    if (m_drop.stack.empty() && m_drop.inFlight == 0) {
        allDropDataReceived();
    }
}

void Dnd::readError(const DndMeta& meta, std::string_view error)
{
    if (!m_drop.reading || confirming()) {
        return;
    }
    // A read of one entry failed: the drop goes on without it.
    if (m_drop.root && (meta.contains("Y") || meta.contains("y"))) {
        auto* entry = routeEntry(meta);
        if (entry && !entry->complete && !entry->skipped) {
            if (entry->fd >= 0) {
                ::close(entry->fd);
                entry->fd = -1;
                ::unlink((entry->base + "/" + entry->name).c_str());
            }
            entry->skipped = true;
            ++m_drop.failed;
            entryDone(entry);
            render();
            return;
        }
    }
    dropFailed("The drop failed. The terminal could not read the dropped data: " + printable(error));
}

void Dnd::remoteDropData(const DndMeta& meta, std::string_view payload, bool more)
{
    RemoteEntry* entry = m_drop.current;
    if (metaNumber(meta, "x") != 0 || metaNumber(meta, "y") != 0 || metaNumber(meta, "Y") != 0) {
        entry = routeEntry(meta);
        if (!entry) {
            dropFailed("The drop failed. The terminal sent data for an entry that was not asked for.");
            return;
        }
        m_drop.current = entry;
    }
    if (!entry) {
        dropFailed("The drop failed. The terminal sent remote drop data without an entry.");
        return;
    }
    if (entry->complete || entry->skipped) {
        dropFailed("The drop failed. The terminal sent data for an entry that is complete.");
        return;
    }
    const auto path = entry->base + "/" + entry->name;
    if (!entry->started) {
        entry->started = true;
        entry->kind = metaNumber(meta, "X");
        if (entry->kind == AfterglowProto::DndFileKind) {
            entry->fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
            if (entry->fd < 0) {
                dropFailed("The drop failed. Cannot create " + path + ": " + errorText(errno));
                return;
            }
        }
    }
    std::string bytes;
    if (!entry->decoder.feed(payload, bytes) || (!more && payload.empty() && !entry->decoder.finish(bytes))) {
        dropFailed("The drop failed. The terminal sent invalid base64.");
        return;
    }
    // A link holds one target, a listing its names: both are bounded.
    const std::size_t limit = entry->kind == AfterglowProto::DndLinkKind ? MaxLinkTarget : MaxListing;
    if (entry->fd < 0 && entry->buffer.size() + bytes.size() > limit) {
        dropFailed("The drop failed. The terminal sent too much data for " + entry->name + ".");
        return;
    }
    if (entry->fd >= 0) {
        if (!AfterglowProto::writeAll(entry->fd, bytes)) {
            dropFailed("The drop failed. Cannot write " + path + ": " + errorText(errno));
            return;
        }
    } else {
        entry->buffer += bytes;
    }
    if (more || !payload.empty()) {
        return;
    }
    // The end of the entry.
    if (entry->fd >= 0) {
        const bool ok = ::close(entry->fd) == 0;
        entry->fd = -1;
        if (!ok) {
            dropFailed("The drop failed. Cannot write " + path + ": " + errorText(errno));
            return;
        }
    } else if (entry->kind == AfterglowProto::DndLinkKind) {
        if (::symlink(entry->buffer.c_str(), path.c_str()) != 0) {
            dropFailed("The drop failed. Cannot create the link " + path + ": " + errorText(errno));
            return;
        }
    } else if (AfterglowProto::dndIsDirectoryKind(entry->kind)) {
        if (m_drop.dirs.contains(entry->kind)) {
            dropFailed("The drop failed. The terminal sent a directory handle that is in use.");
            return;
        }
        if (::mkdir(path.c_str(), 0777) != 0) {
            dropFailed("The drop failed. Cannot create " + path + ": " + errorText(errno));
            return;
        }
        const auto names = AfterglowProto::dndSplitNames(entry->buffer);
        if (!names) {
            dropFailed("The drop failed. The terminal sent an unsafe name in the directory " + entry->name + ".");
            return;
        }
        m_drop.entries += names->size();
        if (m_drop.entries > MaxRemoteEntries) {
            dropFailed("The drop failed. The terminal sent too many entries.");
            return;
        }
        for (const auto& name : *names) {
            auto child = std::make_unique<RemoteEntry>();
            child->parent = entry;
            child->base = path;
            child->name = name;
            entry->children.push_back(std::move(child));
        }
    } else {
        dropFailed("The drop failed. The terminal sent an entry of an unknown kind.");
        return;
    }
    std::string().swap(entry->buffer);
    entry->complete = true;
    entryDone(entry);
}

// ── The protocol ────────────────────────────────────────────────────────────

void Dnd::abandonMessage()
{
    const auto type = m_chunkMeta.contains("t") ? m_chunkMeta.at("t") : std::string("a");
    if (type == "r" && m_drop.reading) {
        dropFailed("The drop failed. The terminal sent a malformed packet in the dropped data.");
    } else if (type == "m" || type == "M") {
        m_drop.mimeBuffer.clear();
    }
}

void Dnd::packet(std::string_view command)
{
    const auto [raw, payload] = AfterglowProto::splitDnd(command);
    const bool more = AfterglowProto::dndMore(raw);
    if (m_discarding) {
        m_discarding = more;
        return;
    }
    const auto parsed = AfterglowProto::parseDndMeta(raw);
    // A packet that breaks the protocol, or a chunk that names another type:
    // the message that was being put together goes.
    const bool mismatch = parsed && m_chunking && parsed->contains("t") && m_chunkMeta.contains("t")
        && parsed->at("t") != m_chunkMeta.at("t");
    if (!parsed || mismatch) {
        if (m_chunking) {
            abandonMessage();
            m_chunking = false;
        }
        if (!parsed) {
            m_discarding = more;
            return;
        }
    }
    // The chunks after the first carry the metadata of the first.
    DndMeta meta = *parsed;
    if (m_chunking) {
        meta = m_chunkMeta;
        meta["m"] = more ? "1" : "0";
    } else if (more) {
        m_chunkMeta = *parsed;
    }
    m_chunking = more;
    dispatch(meta, payload, more);
}

void Dnd::dispatch(const DndMeta& meta, std::string_view payload, bool more)
{
    const auto type = meta.contains("t") ? meta.at("t") : std::string("a");
    if (type == "M" || type == "r" || type == "R") {
        m_drop.activity = nowMs();
    }
    if (type == "m" || type == "M") {
        dropMove(metaInt(meta, "x"), metaInt(meta, "y"), more, payload, type == "M", metaInt(meta, "o"));
    } else if (type == "R") {
        readError(meta, payload);
    } else if (type == "r") {
        dropData(meta, payload, more);
    } else if (type == "o") {
        potentialDrag(metaInt(meta, "x"), metaInt(meta, "y"));
    } else if (type == "q") {
        m_supported = true;
    } else if (type == "E") {
        if (!m_drag.active) {
            return;
        }
        if (payload == "OK") {
            if (!m_drag.accepted) {
                m_drag.accepted = true;
                render();
            }
        } else {
            m_notice = "The terminal refused the drag: " + printable(payload);
            resetDrag();
            render();
        }
    } else if (type == "e") {
        dragEvent(metaNumber(meta, "x"), metaNumber(meta, "y"), meta.contains("o") ? metaNumber(meta, "o") : -1);
    } else if (type == "k") {
        remoteRequest(metaNumber(meta, "x") - 1);
    }
}

void Dnd::shutdown()
{
    if (m_shutDown) {
        return;
    }
    m_shutDown = true;
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
}

int Dnd::run()
{
    m_allowDrops = !dropDests.empty();
    m_allowDrags = !dragSources.empty();
    resetDrop();
    // The alternate screen, no wrapping, no cursor, and mouse reports, so
    // that a press starts no selection in the terminal. The title goes back
    // to the one before. After a stop, the modes come back without a second
    // title push, which the reset would not undo. The alternate screen comes
    // back too: a full-screen program that ran during the stop left the main
    // screen on, and each screen has a keyboard mode stack of its own.
    m_tty.enterModes("\x1b[22;2t\x1b[?1049h\x1b[?7l\x1b[?25l\x1b[?1002h\x1b[?1006h\x1b]2;Drag and drop\x1b\\",
                     "\x1b[?1006l\x1b[?1002l\x1b[?25h\x1b[?7h\x1b[?1049l\x1b[23;2t",
                     "\x1b[?1049h\x1b[?7l\x1b[?25l\x1b[?1002h\x1b[?1006h\x1b]2;Drag and drop\x1b\\");
    // Chunks of a read can still be on their way when the program ends.
    m_tty.drainOnRestore();
    // A terminal that answers DA1 before the query has no drag and drop.
    send({ { "t", "q" } });
    m_tty.queryAttributes();
    const auto& machine = m_machine;
    if (m_allowDrops) {
        std::vector<std::string> mimes;
        for (const auto& d : dropDests) {
            mimes.push_back(d.mime);
        }
        send({ { "t", "a" } }, joinWords(mimes));
        if (!machine.empty()) {
            send({ { "t", "a" }, { "x", "1" } }, machine);
        }
    }
    if (m_allowDrags) {
        send({ { "t", "o" }, { "x", "1" } }, machine);
    }
    render();
    bool answered = false;
    while (!m_quit) {
        pump();
        int timeout = 200;
        if (confirming()) {
            // The question shows the seconds left: once a second.
            timeout = int(std::clamp<long long>((m_drop.deadline - nowMs()) % 1000 + 1, 1, 1000));
        }
        std::vector<Input> inputs;
        if (!m_tty.turn(timeout, inputs)) {
            fail("The terminal closed");
            break;
        }
        for (const auto& input : inputs) {
            if (m_quit) {
                break;
            }
            if (input.type == Input::Type::Osc && input.data.starts_with("72;")) {
                packet(std::string_view(input.data).substr(3));
            } else if (input.type == Input::Type::Csi && input.data.starts_with('?') && input.data.ends_with('c')) {
                if (!answered && !m_supported) {
                    m_notice = "The terminal did not answer the drag and drop query. It may not support drag and drop "
                               "(OSC 72).";
                    render();
                }
                answered = true;
            } else if (input.type == Input::Type::Interrupt) {
                m_quit = true;
            } else if (input.type == Input::Type::Escape) {
                if (confirming()) {
                    confirmDrop(false);
                } else if (exitOn("esc-key")) {
                    m_quit = true;
                }
            } else if (input.type == Input::Type::Text && confirming() && !input.data.empty()
                       && input.data.find_first_not_of('\r') == std::string::npos) {
                // Enter alone, also pressed twice in one read: a paste arrives
                // as Paste, and typed text ahead of it is no answer.
                confirmDrop(true);
            }
        }
        if (confirming()) {
            if (nowMs() >= m_drop.deadline) {
                m_notice = "The drop was canceled because no answer came in time.";
                confirmDrop(false);
            } else {
                render();
            }
        }
        if (const int signal = m_tty.takeSignal()) {
            m_signal = signal;
            m_quit = true;
        }
        if (m_tty.takeResize()) {
            render();
        }
    }
    shutdown();
    m_tty.restore();
    if (!m_error.empty()) {
        report("dnd", m_error);
        return 1;
    }
    return m_signal ? 128 + m_signal : 0;
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
    // An empty --exit-on exits on no event.
    if (const auto list = parsed->value("exit-on"); !list.empty()) {
        for (const auto& event : exitEvents(list)) {
            if (event != "drag-finish" && event != "drop-finish" && event != "esc-key") {
                report("dnd", "The --exit-on event " + event + " is not drag-finish, drop-finish or esc-key.");
                return 1;
            }
        }
    }
    const auto home = homeDirectory();
    const auto cwd = currentDirectory();
    if (cwd.empty()) {
        report("dnd", "The working directory has no path. It was deleted.");
        return 1;
    }
    // The umask of the process, for the modes of copied files and directories.
    const mode_t umask = ::umask(0);
    ::umask(umask);
    Tty tty;
    Dnd dnd(tty, *parsed, umask);
    // Drop destinations: standard output for text, the files for a list of URIs, and --drop.
    if (!::isatty(STDOUT_FILENO)) {
        DropDest d;
        d.mime = "text/plain";
        d.toStdout = true;
        dnd.dropDests.push_back(std::move(d));
    }
    {
        DropDest d;
        d.mime = "text/uri-list";
        dnd.dropDests.push_back(std::move(d));
    }
    for (const auto& spec : parsed->list("drop")) {
        const auto [mime, path] = splitSpec(spec);
        if (!AfterglowProto::dndValidMime(mime)) {
            report("dnd", "The --drop type " + mime + " is not a MIME type.");
            return 1;
        }
        std::erase_if(dnd.dropDests, [&](const DropDest& d) { return d.mime == mime; });
        if (path && !path->empty()) {
            DropDest d;
            d.mime = mime;
            d.path = Path::absolute(*path, home, cwd);
            dnd.dropDests.push_back(std::move(d));
        }
    }
    std::shared_ptr<const std::string> stdinData;
    for (const auto& spec : parsed->list("drag")) {
        const auto [mime, path] = splitSpec(spec);
        if (!path) {
            report("dnd", "The drag source " + spec + " is not in the form MIME:PATH.");
            return 1;
        }
        if (!AfterglowProto::dndValidMime(mime)) {
            report("dnd", "The --drag type " + mime + " is not a MIME type.");
            return 1;
        }
        DragSource s;
        s.mime = mime;
        if (*path == "-" || *path == "/dev/stdin") {
            if (!stdinData) {
                std::string data;
                if (!readAll(STDIN_FILENO, data)) {
                    report("dnd", "Failed to read standard input: " + errorText(errno));
                    return 1;
                }
                stdinData = std::make_shared<const std::string>(std::move(data));
            }
            s.data = stdinData;
        } else {
            s.path = Path::absolute(*path, home, cwd);
            struct stat st { };
            if (::stat(s.path.c_str(), &st) != 0) {
                report("dnd", *path + ": " + errorText(errno));
                return 1;
            }
            if (!S_ISREG(st.st_mode)) {
                report("dnd", *path + " is not a regular file.");
                return 1;
            }
        }
        std::erase_if(dnd.dragSources, [&](const DragSource& d) { return d.mime == s.mime; });
        dnd.dragSources.push_back(std::move(s));
    }
    const bool hasPlain = std::any_of(dnd.dragSources.begin(), dnd.dragSources.end(), [](const DragSource& s) { return s.mime == "text/plain"; });
    if (!stdinData && !hasPlain && !::isatty(STDIN_FILENO)) {
        struct stat st { };
        // A pipe can keep the program waiting: it says why.
        if (::fstat(STDIN_FILENO, &st) == 0 && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) {
            report("dnd", "Reading standard input to offer it as text/plain. The drag starts when it ends.");
        }
        std::string data;
        if (readAll(STDIN_FILENO, data) && !data.empty()) {
            dnd.dragSources.push_back({ "text/plain", "", std::make_shared<const std::string>(std::move(data)), {} });
        }
    }
    std::vector<UriItem> uris;
    for (const auto& arg : parsed->args) {
        if (arg.empty()) {
            report("dnd", "An empty argument names no file.");
            return 1;
        }
        UriItem item;
        item.path = Path::absolute(arg, home, cwd);
        if (item.path == "/") {
            report("dnd", "The root directory cannot be dragged.");
            return 1;
        }
        if (::lstat(item.path.c_str(), &item.st) != 0) {
            report("dnd", arg + ": " + errorText(errno));
            return 1;
        }
        if (!S_ISDIR(item.st.st_mode) && !S_ISREG(item.st.st_mode) && !S_ISLNK(item.st.st_mode)) {
            report("dnd", arg + " is not a directory or regular file.");
            return 1;
        }
        item.uri = AfterglowProto::dndFileUri(item.path, S_ISDIR(item.st.st_mode));
        uris.push_back(std::move(item));
    }
    if (!uris.empty()) {
        std::string list;
        for (const auto& item : uris) {
            list += item.uri + "\r\n";
        }
        std::erase_if(dnd.dragSources, [](const DragSource& d) { return d.mime == "text/uri-list"; });
        dnd.dragSources.push_back({ "text/uri-list", "", std::make_shared<const std::string>(std::move(list)), std::move(uris) });
    }
    if (const auto thumbnail = parsed->value("drag-thumbnail"); !thumbnail.empty()) {
        std::string bytes;
        const int fd = ::open(Path::absolute(thumbnail, home, cwd).c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        struct stat st { };
        bool ok = fd >= 0 && ::fstat(fd, &st) == 0;
        int readError = errno;
        if (ok && (!S_ISREG(st.st_mode) || std::size_t(st.st_size) > ThumbnailFileLimit)) {
            ok = false;
            readError = S_ISREG(st.st_mode) ? EFBIG : EINVAL;
        }
        if (ok && !readAll(fd, bytes)) {
            ok = false;
            readError = errno;
        }
        if (fd >= 0) {
            ::close(fd);
        }
        if (!ok) {
            report("dnd", "Cannot read " + thumbnail + ": "
                       + (readError == EINVAL ? std::string("it is not a regular file") : errorText(readError)));
            return 1;
        }
        dnd.thumbnail = decodeImage(bytes, ThumbnailBudget, error, 1);
        if (!dnd.thumbnail) {
            report("dnd", "Cannot decode " + thumbnail + ": " + error);
            return 1;
        }
    }
    if (dnd.dropDests.empty() && dnd.dragSources.empty()) {
        report("dnd", "There is nothing to drag and nothing to drop. Name files to drag, or keep a --drop type.");
        return 1;
    }
    const auto dropDest = parsed->value("drop-dest");
    dnd.dropDir = dropDest.empty() ? cwd : Path::absolute(dropDest, home, cwd);
    struct stat st { };
    if (::stat(dnd.dropDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        report("dnd", "The drop directory " + dnd.dropDir + " is not a directory.");
        return 1;
    }
    if (!tty.open(error)) {
        report("dnd", error);
        return 1;
    }
    return dnd.run();
}

} // namespace Aglow
