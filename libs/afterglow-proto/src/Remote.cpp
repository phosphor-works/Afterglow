// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Remote.h>

#include <cerrno>
#include <cstring>
#include <iterator>

#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace AfterglowProto::Remote {

namespace {

constexpr std::string_view Prefix = "24569;";
constexpr std::string_view QueryPayload = "q=remote";
constexpr std::string_view PathKey = "path=";
constexpr std::string_view ErrorKey = "error=";

constexpr std::string_view Names[] = {"off", "forward", "version", "length"};

/// Whether connecting to @p path is refused: nothing listens there.
bool refused(const std::string& path)
{
    sockaddr_un address{};
    if (path.size() >= sizeof address.sun_path) return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;
    int result;
    do {
        result = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address);
    } while (result != 0 && errno == EINTR);
    // EAGAIN: a listener whose queue is full is alive.
    const bool none = result != 0 && errno == ECONNREFUSED;
    ::close(fd);
    return none;
}

} // namespace

std::string_view errorName(Error error)
{
    return Names[std::size_t(error)];
}

std::optional<Error> errorFromName(std::string_view name)
{
    for (std::size_t i = 0; i < std::size(Names); ++i) {
        if (Names[i] == name) return Error(i);
    }
    return std::nullopt;
}

std::string query()
{
    return "\x1b]" + std::string(Prefix) + std::string(QueryPayload) + "\x1b\\";
}

bool isQuery(std::string_view payload)
{
    return payload == QueryPayload;
}

bool validPath(std::string_view path)
{
    if (path.empty() || path.front() != '/' || path.size() > MaxPathBytes) return false;
    for (const char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f) return false;
    }
    return true;
}

std::string reply(const Answer& answer, bool bel)
{
    std::string out = "\x1b]" + std::string(Prefix);
    if (validPath(answer.path)) {
        out += PathKey;
        out += answer.path;
    } else {
        out += ErrorKey;
        out += errorName(answer.path.empty() ? answer.error : Error::Off);
    }
    out += bel ? "\x07" : "\x1b\\";
    return out;
}

std::optional<Answer> parseReply(std::string_view payload)
{
    if (payload.starts_with(PathKey)) {
        const std::string_view path = payload.substr(PathKey.size());
        if (!validPath(path)) return std::nullopt;
        return Answer{std::string(path), Error::Off};
    }
    if (payload.starts_with(ErrorKey)) {
        const auto error = errorFromName(payload.substr(ErrorKey.size()));
        if (!error) return std::nullopt;
        return Answer{{}, *error};
    }
    return std::nullopt;
}

std::string tmuxPassthrough(std::string_view sequences)
{
    std::string out = "\x1bPtmux;";
    for (const char c : sequences) {
        out += c;
        if (c == '\x1b') out += '\x1b';
    }
    out += "\x1b\\";
    return out;
}

std::string directory(std::string_view runtime, std::string_view home)
{
    if (runtime.starts_with('/')) return std::string(runtime) + (runtime.ends_with('/') ? "" : "/") + "afterglow";
    if (home.starts_with('/')) return std::string(home) + (home.ends_with('/') ? "" : "/") + ".local/state/afterglow/run";
    return {};
}

std::string socketPath(std::string_view directory, std::string_view token)
{
    std::string path(directory);
    if (!path.ends_with('/')) path += '/';
    return path + std::string(token) + ".sock";
}

Pruned prune(const std::string& directory, uid_t owner)
{
    Pruned result;
    DIR* dir = ::opendir(directory.c_str());
    if (!dir) {
        result.error = directory + ": " + std::strerror(errno);
        return result;
    }
    const int fd = ::dirfd(dir);
    std::vector<std::string> names;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string_view name = entry->d_name;
        if (name.size() > 5 && name.ends_with(".sock")) names.emplace_back(name);
    }
    for (const std::string& name : names) {
        struct stat info{};
        if (::fstatat(fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (!S_ISSOCK(info.st_mode) || info.st_uid != owner) continue;
        if (!refused(socketPath(directory, name.substr(0, name.size() - 5)))) continue;
        if (::unlinkat(fd, name.c_str(), 0) == 0) result.removed.push_back(name);
    }
    ::closedir(dir);
    return result;
}

} // namespace AfterglowProto::Remote
