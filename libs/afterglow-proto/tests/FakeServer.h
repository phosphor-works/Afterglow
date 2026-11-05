// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Json.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace AfterglowProto::Test {

/// The server end of a phosphor-ipc connection, on a socket in a directory
/// of its own: it reads the client's lines, and writes the lines that a test
/// gives it.
class FakeServer
{
public:
    /// @p backlog 0 holds one connection that waits for accept(), no more.
    explicit FakeServer(int backlog = 4)
    {
        // Under /tmp: TMPDIR can be too long for a socket path.
        char dir[] = "/tmp/afterglow-proto-XXXXXX";
        if (!::mkdtemp(dir)) return;
        m_dir = dir;
        m_path = m_dir + "/s.sock";
        m_listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, m_path.data(), m_path.size());
        if (m_listener < 0 || ::bind(m_listener, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0
            || ::listen(m_listener, backlog) != 0) {
            if (m_listener >= 0) ::close(m_listener);
            m_listener = -1;
        }
    }
    ~FakeServer()
    {
        hangUp();
        if (m_listener >= 0) ::close(m_listener);
        if (!m_path.empty()) ::unlink(m_path.c_str());
        if (!m_dir.empty()) ::rmdir(m_dir.c_str());
    }
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;

    bool valid() const { return m_listener >= 0; }
    const std::string& path() const { return m_path; }
    int fd() const { return m_fd; }

    /// Takes the next connection. False when none came in @p timeoutMs.
    bool accept(int timeoutMs = 5000)
    {
        if (!connectionWaits(timeoutMs)) return false;
        hangUp();
        m_fd = ::accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC);
        return m_fd >= 0;
    }
    bool connectionWaits(int timeoutMs = 0) const
    {
        pollfd fds{m_listener, POLLIN, 0};
        return ::poll(&fds, 1, timeoutMs) == 1;
    }
    void hangUp()
    {
        if (m_fd >= 0) ::close(m_fd);
        m_fd = -1;
        m_buffer.clear();
    }

    /// The client's next line, '\n' cut. Nothing when the client closed the
    /// connection, or no line came in @p timeoutMs.
    std::optional<std::string> line(int timeoutMs = 5000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            if (const auto end = m_buffer.find('\n'); end != std::string::npos) {
                std::string out = m_buffer.substr(0, end);
                m_buffer.erase(0, end + 1);
                return out;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            pollfd fds{m_fd, POLLIN, 0};
            if (left.count() <= 0 || ::poll(&fds, 1, int(left.count())) != 1) return std::nullopt;
            char chunk[65536];
            const ssize_t n = ::recv(m_fd, chunk, sizeof chunk, 0);
            if (n <= 0) return std::nullopt;
            m_buffer.append(chunk, std::size_t(n));
        }
    }
    /// The client's next line as JSON.
    std::optional<Json::Value> call(int timeoutMs = 5000)
    {
        const auto text = line(timeoutMs);
        return text ? Json::parse(*text) : std::nullopt;
    }

    bool write(std::string_view text)
    {
        while (!text.empty()) {
            const ssize_t n = ::send(m_fd, text.data(), text.size(), MSG_NOSIGNAL);
            if (n <= 0) return false;
            text.remove_prefix(std::size_t(n));
        }
        return true;
    }
    bool frame(const Json::Value& value) { return write(Json::write(value) + "\n"); }
    bool reply(const Json::Value& call, Json::Value result)
    {
        return frame(Json::Object{{"type", "reply"}, {"id", call["id"]}, {"result", std::move(result)}});
    }
    bool error(const Json::Value& call, std::string code, std::string message)
    {
        return frame(Json::Object{{"type", "error"}, {"id", call["id"]}, {"code", std::move(code)}, {"message", std::move(message)}});
    }
    bool event(std::string target, std::string signal, Json::Array args)
    {
        return frame(Json::Object{{"type", "event"}, {"target", std::move(target)}, {"signal", std::move(signal)}, {"args", std::move(args)}});
    }

private:
    std::string m_dir;
    std::string m_path;
    int m_listener = -1;
    int m_fd = -1;
    std::string m_buffer;
};

} // namespace AfterglowProto::Test
