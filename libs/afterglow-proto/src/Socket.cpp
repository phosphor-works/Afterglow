// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Socket.h>

#include <AfterglowProto/MachineId.h>
#include <AfterglowProto/Sha256.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>

#include <limits.h>
#include <sys/un.h>
#include <unistd.h>

namespace AfterglowProto {

namespace {

std::string_view value(const EnvironmentLookup& env, const char* name)
{
    const char* v = env(name);
    return v ? std::string_view(v) : std::string_view();
}

/// @p path without the "/" at its end, which a path joined to it then gets
/// again: "/run/user/1000/" and "/run/user/1000" name one directory.
std::string_view trimmed(std::string_view path)
{
    while (path.size() > 1 && path.ends_with('/')) path.remove_suffix(1);
    return path;
}

std::string join(std::string_view directory, std::string_view name)
{
    std::string path(directory);
    if (!path.ends_with('/')) path += '/';
    return path += name;
}

/// $XDG_RUNTIME_DIR, when it is an absolute path as the XDG base directory
/// specification asks: a relative one would follow the working directory.
std::string_view runtimeDirectory(const EnvironmentLookup& env)
{
    const std::string_view runtime = value(env, "XDG_RUNTIME_DIR");
    return runtime.starts_with('/') ? trimmed(runtime) : std::string_view();
}

/// Why runtimeDirectory() is empty.
const char* noRuntimeDirectory(const EnvironmentLookup& env)
{
    return value(env, "XDG_RUNTIME_DIR").empty() ? "XDG_RUNTIME_DIR is not set" : "XDG_RUNTIME_DIR is not an absolute path";
}

/// $XDG_STATE_HOME, else ~/.local/state. As the specification says, a
/// relative value is ignored. Empty when HOME is not absolute either.
std::string stateDirectory(const EnvironmentLookup& env)
{
    if (const std::string_view state = value(env, "XDG_STATE_HOME"); state.starts_with('/')) return std::string(trimmed(state));
    const std::string_view home = value(env, "HOME");
    if (!home.starts_with('/')) return {};
    return join(trimmed(home), ".local/state");
}

std::string hostName()
{
    // Room for a name of HOST_NAME_MAX bytes and its NUL, and one NUL more
    // that gethostname never writes: POSIX leaves a cut name unterminated.
    char host[HOST_NAME_MAX + 2] = {};
    if (::gethostname(host, sizeof host - 1) != 0) return {};
    return host;
}

std::string display(const EnvironmentLookup& env, std::string_view runtime)
{
    std::string_view display = value(env, "WAYLAND_DISPLAY");
    std::string prefix(runtime);
    if (!prefix.ends_with('/')) prefix += '/';
    if (display.starts_with(prefix)) display.remove_prefix(prefix.size());
    return displayName(display);
}

static_assert(MaxSocketPathBytes + 1 == sizeof(sockaddr_un::sun_path));

} // namespace

const char* systemEnvironment(const char* name)
{
    return std::getenv(name);
}

std::string displayName(std::string_view waylandDisplay)
{
    if (!waylandDisplay.empty() && waylandDisplay.front() == '/') waylandDisplay.remove_prefix(1);
    std::string name(waylandDisplay);
    for (char& c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
            || c == '.';
        if (!plain) c = '_';
    }
    return name.empty() ? "wayland-0" : name;
}

std::string sharedSocketPath(const EnvironmentLookup& env)
{
    const std::string_view runtime = runtimeDirectory(env);
    if (runtime.empty()) return {};
    return join(runtime, "afterglow-" + display(env, runtime) + ".sock");
}

std::string ownSocketPath(std::int64_t pid, const EnvironmentLookup& env)
{
    const std::string_view runtime = runtimeDirectory(env);
    if (runtime.empty()) return {};
    return join(runtime, "afterglow-" + display(env, runtime) + "-" + std::to_string(pid) + ".sock");
}

std::string socketPathError(std::string_view path)
{
    if (path.empty()) return "the socket path is empty";
    if (path.size() > MaxSocketPathBytes) {
        return "the socket path is over " + std::to_string(MaxSocketPathBytes) + " bytes: " + std::string(path);
    }
    return {};
}

SocketLookup findSocket(const EnvironmentLookup& env)
{
    const std::string_view owner = value(env, "AFTERGLOW_SOCKET");
    const std::string_view paneText = value(env, "AFTERGLOW_PANE_ID");
    if (owner.empty() && !paneText.empty()) return {{}, false, 0, "the Afterglow that runs this pane has no socket"};
    if (!owner.empty()) {
        if (std::string error = socketPathError(owner); !error.empty()) return {{}, true, 0, std::move(error)};
        std::int64_t pane = 0;
        const auto [end, ec] = std::from_chars(paneText.data(), paneText.data() + paneText.size(), pane);
        const bool valid = ec == std::errc() && end == paneText.data() + paneText.size() && pane > 0 && pane <= 0xFFFFFFFFLL;
        return {std::string(owner), true, valid ? pane : 0, {}};
    }
    std::string shared = sharedSocketPath(env);
    if (shared.empty()) return {{}, false, 0, noRuntimeDirectory(env)};
    if (std::string error = socketPathError(shared); !error.empty()) return {{}, false, 0, std::move(error)};
    return {std::move(shared), false, 0, {}};
}

MuxSockets muxSockets(const EnvironmentLookup& env)
{
    MuxSockets sockets;
    const auto add = [&](std::string directory, std::string_view name) {
        std::string path = join(directory, name);
        if (std::string error = socketPathError(path); !error.empty()) {
            sockets.error += (sockets.error.empty() ? "" : " and ") + error;
            return;
        }
        sockets.candidates.push_back({std::move(path), std::move(directory)});
    };
    // A directory of its own, never a file next to the terminal's sockets:
    // InstanceSocket removes stale afterglow-*.sock files, and a
    // WAYLAND_DISPLAY of "mux" would name afterglow-mux.sock.
    if (const std::string_view runtime = runtimeDirectory(env); !runtime.empty()) {
        add(join(runtime, "afterglow-mux"), "socket");
    } else {
        sockets.error = noRuntimeDirectory(env);
    }
    if (const std::string state = stateDirectory(env); !state.empty()) {
        add(join(state, "afterglow-mux"), muxStateSocketName());
    } else {
        sockets.error += std::string(sockets.error.empty() ? "" : " and ") + "neither XDG_STATE_HOME nor HOME is an absolute path";
    }
    return sockets;
}

std::string muxStateSocketName(std::string_view machineId, std::string_view host)
{
    while (!machineId.empty() && std::isspace(static_cast<unsigned char>(machineId.back()))) machineId.remove_suffix(1);
    while (!machineId.empty() && std::isspace(static_cast<unsigned char>(machineId.front()))) machineId.remove_prefix(1);
    const bool valid = machineId.size() == 32 && std::all_of(machineId.begin(), machineId.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
    // machine-id(5): the ID is confidential and is not used directly, but
    // through a keyed hash with a key of the application's own. The file
    // name is on an NFS server and in its traffic.
    if (valid) return toHex(hmacSha256("afterglow-mux-state-socket", machineId)).substr(0, 32) + ".sock";
    return hostFileName(host) + ".sock";
}

std::string muxStateSocketName()
{
    return muxStateSocketName(machineIdContents(), hostName());
}

std::string hostFileName(std::string_view host)
{
    std::string name(host);
    for (char& c : name) {
        if (c == '/') c = '_';
    }
    return name.empty() ? "localhost" : name;
}

} // namespace AfterglowProto
