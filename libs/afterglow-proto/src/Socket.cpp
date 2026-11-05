// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Socket.h>

#include <charconv>
#include <cstdlib>

namespace AfterglowProto {

namespace {

std::string_view value(const EnvironmentLookup& env, const char* name)
{
    const char* v = env(name);
    return v ? std::string_view(v) : std::string_view();
}

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
    const std::string_view runtime = value(env, "XDG_RUNTIME_DIR");
    if (runtime.empty()) return {};
    return std::string(runtime) + "/afterglow-" + displayName(value(env, "WAYLAND_DISPLAY")) + ".sock";
}

std::string ownSocketPath(std::int64_t pid, const EnvironmentLookup& env)
{
    const std::string_view runtime = value(env, "XDG_RUNTIME_DIR");
    if (runtime.empty()) return {};
    return std::string(runtime) + "/afterglow-" + displayName(value(env, "WAYLAND_DISPLAY")) + "-" + std::to_string(pid)
        + ".sock";
}

SocketLookup findSocket(const EnvironmentLookup& env)
{
    const std::string_view owner = value(env, "AFTERGLOW_SOCKET");
    const std::string_view paneText = value(env, "AFTERGLOW_PANE_ID");
    if (owner.empty() && !paneText.empty()) return {{}, false, 0, "the afterglow that runs this pane has no socket"};
    if (!owner.empty()) {
        std::int64_t pane = 0;
        const auto [end, ec] = std::from_chars(paneText.data(), paneText.data() + paneText.size(), pane);
        const bool valid = ec == std::errc() && end == paneText.data() + paneText.size() && pane > 0 && pane <= 0xFFFFFFFFLL;
        return {std::string(owner), true, valid ? pane : 0, {}};
    }
    std::string shared = sharedSocketPath(env);
    if (shared.empty()) return {{}, false, 0, "XDG_RUNTIME_DIR is not set"};
    return {std::move(shared), false, 0, {}};
}

} // namespace AfterglowProto
