// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Socket.h>

#include <charconv>
#include <cstdlib>

#include <sys/un.h>

namespace AfterglowProto {

namespace {

std::string_view value(const EnvironmentLookup& env, const char* name)
{
    const char* v = env(name);
    return v ? std::string_view(v) : std::string_view();
}

/// $XDG_RUNTIME_DIR, when it is an absolute path as the XDG base directory
/// specification asks: a relative one would follow the working directory.
std::string_view runtimeDirectory(const EnvironmentLookup& env)
{
    const std::string_view runtime = value(env, "XDG_RUNTIME_DIR");
    return runtime.starts_with('/') ? runtime : std::string_view();
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
    return std::string(runtime) + "/afterglow-" + display(env, runtime) + ".sock";
}

std::string ownSocketPath(std::int64_t pid, const EnvironmentLookup& env)
{
    const std::string_view runtime = runtimeDirectory(env);
    if (runtime.empty()) return {};
    return std::string(runtime) + "/afterglow-" + display(env, runtime) + "-" + std::to_string(pid) + ".sock";
}

SocketLookup findSocket(const EnvironmentLookup& env)
{
    const std::string_view owner = value(env, "AFTERGLOW_SOCKET");
    const std::string_view paneText = value(env, "AFTERGLOW_PANE_ID");
    if (owner.empty() && !paneText.empty()) return {{}, false, 0, "the Afterglow that runs this pane has no socket"};
    const auto tooLong = [](std::string_view path) {
        return "the socket path is over " + std::to_string(MaxSocketPathBytes) + " bytes: " + std::string(path);
    };
    if (!owner.empty()) {
        if (owner.size() > MaxSocketPathBytes) return {{}, true, 0, tooLong(owner)};
        std::int64_t pane = 0;
        const auto [end, ec] = std::from_chars(paneText.data(), paneText.data() + paneText.size(), pane);
        const bool valid = ec == std::errc() && end == paneText.data() + paneText.size() && pane > 0 && pane <= 0xFFFFFFFFLL;
        return {std::string(owner), true, valid ? pane : 0, {}};
    }
    std::string shared = sharedSocketPath(env);
    if (shared.empty()) {
        return {{}, false, 0, value(env, "XDG_RUNTIME_DIR").empty() ? "XDG_RUNTIME_DIR is not set" : "XDG_RUNTIME_DIR is not an absolute path"};
    }
    if (shared.size() > MaxSocketPathBytes) return {{}, false, 0, tooLong(shared)};
    return {std::move(shared), false, 0, {}};
}

MuxSocket muxSocket(const EnvironmentLookup& env)
{
    MuxSocket socket;
    if (const std::string_view runtime = runtimeDirectory(env); !runtime.empty()) {
        socket.path = std::string(runtime) + (runtime.ends_with('/') ? "" : "/") + "afterglow-mux.sock";
    } else {
        std::string state(value(env, "XDG_STATE_HOME"));
        // As the specification says, a relative value is ignored.
        if (!state.starts_with('/')) {
            const std::string_view home = value(env, "HOME");
            if (!home.starts_with('/')) {
                socket.error = "neither XDG_RUNTIME_DIR nor HOME is an absolute path";
                return socket;
            }
            state = std::string(home) + "/.local/state";
        }
        if (state.ends_with('/')) state.pop_back();
        socket.directory = state + "/afterglow";
        socket.path = socket.directory + "/mux.sock";
    }
    if (socket.path.size() > MaxSocketPathBytes) {
        socket.error = "the socket path is over " + std::to_string(MaxSocketPathBytes) + " bytes: " + socket.path;
        socket.path.clear();
        socket.directory.clear();
    }
    return socket;
}

} // namespace AfterglowProto
