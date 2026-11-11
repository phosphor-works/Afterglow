// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The integers of the mux formats (MuxStream.h, MuxHost.h): LEB128 varints
// in their shortest form. One writer and one reader, so that both formats
// refuse the same bytes.

namespace AfterglowMux::Codec {

inline void varint(std::string& out, std::uint64_t v)
{
    while (v >= 0x80) {
        out.push_back(char((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(char(v));
}

class Reader
{
public:
    explicit Reader(std::string_view s)
        : m_s(s)
    {
    }
    bool ok() const { return m_ok; }
    bool done() const { return m_at == m_s.size(); }
    std::size_t left() const { return m_s.size() - m_at; }
    void fail() { m_ok = false; }
    std::uint8_t byte()
    {
        if (m_at >= m_s.size()) {
            m_ok = false;
            return 0;
        }
        return std::uint8_t(m_s[m_at++]);
    }
    /// A varint in its shortest form, up to @p max.
    std::uint64_t varint(std::uint64_t max)
    {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64 && m_ok; shift += 7) {
            const std::uint8_t b = byte();
            const std::uint64_t part = b & 0x7f;
            // The tenth byte holds the 64th bit only. A last byte of 0 after
            // the first is a longer form of a shorter number.
            if ((shift == 63 && part > 1) || (!(b & 0x80) && shift > 0 && part == 0)) break;
            v |= part << shift;
            if (!(b & 0x80)) {
                if (v > max) m_ok = false;
                return m_ok ? v : 0;
            }
        }
        m_ok = false;
        return 0;
    }
    /// The next @p n bytes.
    std::string_view take(std::size_t n)
    {
        if (!m_ok || n > left()) {
            m_ok = false;
            return {};
        }
        const std::string_view r = m_s.substr(m_at, n);
        m_at += n;
        return r;
    }
    std::string_view rest() { return take(left()); }

private:
    std::string_view m_s;
    std::size_t m_at = 0;
    bool m_ok = true;
};

} // namespace AfterglowMux::Codec
