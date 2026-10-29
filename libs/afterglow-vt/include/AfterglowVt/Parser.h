// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Unicode.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace AfterglowVt {

/// CSI / DCS parameters. A ':' separator marks the following value as a
/// sub-parameter of the one before it (SGR 4:3, 38:2::r:g:b).
struct Params
{
    static constexpr int Max = 32;

    std::uint16_t values[Max] = {};
    std::uint32_t subMask = 0; ///< bit i set: values[i] is a sub-parameter of values[i - 1]
    int count = 0;

    std::uint16_t at(int i, std::uint16_t fallback = 0) const { return i < count ? values[i] : fallback; }
    /// Value at @p i where 0 or absent means @p fallback (the usual "default 1").
    std::uint16_t atOr(int i, std::uint16_t fallback) const
    {
        return (i < count && values[i] != 0) ? values[i] : fallback;
    }
    bool isSub(int i) const { return i > 0 && i < count && ((subMask >> i) & 1); }
};

/// Up to two intermediate bytes plus an optional private marker ('?', '>', '<', '=').
struct Intermediates
{
    char marker = 0;
    char bytes[2] = {};
    int count = 0;
    bool overflow = false;

    bool empty() const { return marker == 0 && count == 0; }
    bool is(char m, char i0 = 0) const { return marker == m && (i0 == 0 ? count == 0 : (count == 1 && bytes[0] == i0)); }
};

/// DEC/ANSI escape sequence parser, modelled on Paul Williams' state machine,
/// with UTF-8 decoding in the ground state and payload collection for OSC,
/// DCS and APC strings. C1 controls are not recognised as 8-bit bytes because
/// the stream is UTF-8.
///
/// Handler requirements:
///   void print(char32_t cp);
///   void printAscii(const char* text, std::size_t length); // printable ASCII only
///   void execute(std::uint8_t c);
///   void escDispatch(const Intermediates&, std::uint8_t final);
///   void csiDispatch(const Params&, const Intermediates&, std::uint8_t final);
///   void oscDispatch(std::string_view payload, bool belTerminated);
///   void dcsDispatch(const Params&, const Intermediates&, std::uint8_t final, std::string_view data);
///   void apcDispatch(std::string_view payload);
template<typename Handler>
class Parser
{
public:
    static constexpr std::size_t MaxStringPayload = std::size_t(1) << 22;

    explicit Parser(Handler& handler)
        : m_handler(handler)
    {
    }

    void feed(std::string_view bytes)
    {
        const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
        const auto* end = p + bytes.size();
        while (p < end) {
            if (m_state == State::Ground && m_utf8Needed == 0) {
                // Fast path: printable ASCII runs go to the handler whole.
                const auto* run = p;
                while (p < end && *p >= 0x20 && *p < 0x7F) {
                    ++p;
                }
                if (p != run) {
                    m_handler.printAscii(reinterpret_cast<const char*>(run), std::size_t(p - run));
                }
                if (p == end) {
                    break;
                }
            }
            advance(*p++);
        }
    }

    void reset()
    {
        m_state = State::Ground;
        m_utf8Needed = 0;
        clearSequence();
        m_string.clear();
    }

private:
    enum class State : std::uint8_t {
        Ground,
        Escape,
        EscapeIntermediate,
        CsiEntry,
        CsiParam,
        CsiIntermediate,
        CsiIgnore,
        DcsEntry,
        DcsParam,
        DcsIntermediate,
        DcsPassthrough,
        DcsIgnore,
        OscString,
        SosPmApcString,
    };

    void advance(std::uint8_t b)
    {
        // Anywhere transitions.
        if (b == 0x18 || b == 0x1A) { // CAN, SUB
            abortString();
            m_utf8Needed = 0;
            m_handler.execute(b);
            m_state = State::Ground;
            return;
        }
        if (b == 0x1B) {
            if (m_utf8Needed != 0) {
                m_utf8Needed = 0;
                m_handler.print(Unicode::ReplacementCharacter);
            }
            finishString(false);
            clearSequence();
            m_state = State::Escape;
            return;
        }

        switch (m_state) {
        case State::Ground:
            ground(b);
            break;
        case State::Escape:
            if (b < 0x20) {
                m_handler.execute(b);
            } else if (b < 0x30) {
                collect(b);
                m_state = State::EscapeIntermediate;
            } else if (b == 'P') {
                m_state = State::DcsEntry;
            } else if (b == '[') {
                m_state = State::CsiEntry;
            } else if (b == ']') {
                m_string.clear();
                m_state = State::OscString;
            } else if (b == 'X' || b == '^' || b == '_') {
                m_string.clear();
                m_stringIsApc = (b == '_');
                m_state = State::SosPmApcString;
            } else if (b < 0x7F) {
                m_handler.escDispatch(m_inter, b);
                m_state = State::Ground;
            } else if (b >= 0x80) {
                m_state = State::Ground;
                ground(b);
            }
            break;
        case State::EscapeIntermediate:
            if (b < 0x20) {
                m_handler.execute(b);
            } else if (b < 0x30) {
                collect(b);
            } else if (b < 0x7F) {
                m_handler.escDispatch(m_inter, b);
                m_state = State::Ground;
            } else if (b >= 0x80) {
                m_state = State::Ground;
                ground(b);
            }
            break;
        case State::CsiEntry:
        case State::CsiParam:
            if (b < 0x20) {
                m_handler.execute(b);
            } else if (b < 0x30) {
                collect(b);
                m_state = State::CsiIntermediate;
            } else if (b <= ';') {
                param(b);
                m_state = State::CsiParam;
            } else if (b < 0x40) {
                if (m_state == State::CsiEntry && m_inter.marker == 0) {
                    m_inter.marker = char(b);
                    m_state = State::CsiParam;
                } else {
                    m_state = State::CsiIgnore;
                }
            } else if (b < 0x7F) {
                dispatchCsi(b);
            } else if (b >= 0x80) {
                m_state = State::Ground;
                ground(b);
            }
            break;
        case State::CsiIntermediate:
            if (b < 0x20) {
                m_handler.execute(b);
            } else if (b < 0x30) {
                collect(b);
            } else if (b < 0x40) {
                m_state = State::CsiIgnore;
            } else if (b < 0x7F) {
                dispatchCsi(b);
            } else if (b >= 0x80) {
                m_state = State::Ground;
                ground(b);
            }
            break;
        case State::CsiIgnore:
            if (b < 0x20) {
                m_handler.execute(b);
            } else if (b >= 0x40 && b < 0x7F) {
                m_state = State::Ground;
            } else if (b >= 0x80) {
                m_state = State::Ground;
                ground(b);
            }
            break;
        case State::DcsEntry:
        case State::DcsParam:
            if (b < 0x20 || b == 0x7F) {
                // ignored
            } else if (b < 0x30) {
                collect(b);
                m_state = State::DcsIntermediate;
            } else if (b <= ';') {
                param(b);
                m_state = State::DcsParam;
            } else if (b < 0x40) {
                if (m_state == State::DcsEntry && m_inter.marker == 0) {
                    m_inter.marker = char(b);
                    m_state = State::DcsParam;
                } else {
                    m_state = State::DcsIgnore;
                }
            } else {
                hookDcs(b);
            }
            break;
        case State::DcsIntermediate:
            if (b < 0x20 || b == 0x7F) {
                // ignored
            } else if (b < 0x30) {
                collect(b);
            } else if (b < 0x40) {
                m_state = State::DcsIgnore;
            } else {
                hookDcs(b);
            }
            break;
        case State::DcsPassthrough:
            if (b != 0x7F) {
                appendString(b);
            }
            break;
        case State::DcsIgnore:
            break;
        case State::OscString:
            if (b == 0x07) {
                finishString(true);
                m_state = State::Ground;
            } else if (b >= 0x20) {
                appendString(b);
            }
            break;
        case State::SosPmApcString:
            if (m_stringIsApc) {
                appendString(b);
            }
            break;
        }
    }

    void ground(std::uint8_t b)
    {
        if (m_utf8Needed != 0) {
            if ((b & 0xC0) == 0x80) {
                m_utf8Cp = (m_utf8Cp << 6) | (b & 0x3F);
                if (--m_utf8Needed == 0) {
                    const bool overlong = m_utf8Cp < m_utf8Min;
                    const bool surrogate = m_utf8Cp >= 0xD800 && m_utf8Cp <= 0xDFFF;
                    m_handler.print((overlong || surrogate || m_utf8Cp > 0x10FFFF) ? Unicode::ReplacementCharacter
                                                                                   : m_utf8Cp);
                }
                return;
            }
            // Truncated sequence: emit U+FFFD and reprocess this byte.
            m_utf8Needed = 0;
            m_handler.print(Unicode::ReplacementCharacter);
        }

        if (b < 0x20) {
            m_handler.execute(b);
        } else if (b < 0x7F) {
            m_handler.print(char32_t(b));
        } else if (b == 0x7F) {
            // DEL is ignored.
        } else if (b >= 0xC2 && b <= 0xDF) {
            m_utf8Cp = b & 0x1F;
            m_utf8Needed = 1;
            m_utf8Min = 0x80;
        } else if (b >= 0xE0 && b <= 0xEF) {
            m_utf8Cp = b & 0x0F;
            m_utf8Needed = 2;
            m_utf8Min = 0x800;
        } else if (b >= 0xF0 && b <= 0xF4) {
            m_utf8Cp = b & 0x07;
            m_utf8Needed = 3;
            m_utf8Min = 0x10000;
        } else {
            m_handler.print(Unicode::ReplacementCharacter);
        }
    }

    void collect(std::uint8_t b)
    {
        if (m_inter.count < 2) {
            m_inter.bytes[m_inter.count++] = char(b);
        } else {
            m_inter.overflow = true;
        }
    }

    void param(std::uint8_t b)
    {
        if (b == ';' || b == ':') {
            pushParam();
            m_nextIsSub = (b == ':');
            return;
        }
        m_paramActive = true;
        const std::uint32_t v = std::uint32_t(m_current) * 10 + (b - '0');
        m_current = v > 0xFFFF ? 0xFFFF : std::uint16_t(v);
    }

    void pushParam()
    {
        if (m_params.count < Params::Max) {
            if (m_nextIsSub) {
                m_params.subMask |= 1u << m_params.count;
            }
            m_params.values[m_params.count++] = m_current;
        }
        m_current = 0;
        m_paramActive = false;
        m_sawSeparator = true;
    }

    void finishParams()
    {
        if (m_paramActive || m_sawSeparator) {
            pushParam();
        }
    }

    void dispatchCsi(std::uint8_t final)
    {
        finishParams();
        if (!m_inter.overflow) {
            m_handler.csiDispatch(m_params, m_inter, final);
        }
        m_state = State::Ground;
    }

    void hookDcs(std::uint8_t final)
    {
        finishParams();
        m_dcsFinal = final;
        m_string.clear();
        m_state = m_inter.overflow ? State::DcsIgnore : State::DcsPassthrough;
    }

    void appendString(std::uint8_t b)
    {
        if (m_string.size() < MaxStringPayload) {
            m_string.push_back(char(b));
        } else {
            m_stringOverflow = true;
        }
    }

    void finishString(bool bel)
    {
        const bool overflow = m_stringOverflow;
        m_stringOverflow = false;
        switch (m_state) {
        case State::OscString:
            if (!overflow) {
                m_handler.oscDispatch(m_string, bel);
            }
            break;
        case State::DcsPassthrough:
            if (!overflow) {
                m_handler.dcsDispatch(m_params, m_inter, m_dcsFinal, m_string);
            }
            break;
        case State::SosPmApcString:
            if (m_stringIsApc && !overflow) {
                m_handler.apcDispatch(m_string);
            }
            break;
        default:
            return;
        }
        m_string.clear();
    }

    void abortString()
    {
        m_string.clear();
        m_stringOverflow = false;
    }

    void clearSequence()
    {
        m_params = Params{};
        m_inter = Intermediates{};
        m_current = 0;
        m_paramActive = false;
        m_sawSeparator = false;
        m_nextIsSub = false;
    }

    Handler& m_handler;
    State m_state = State::Ground;

    Params m_params;
    Intermediates m_inter;
    std::uint16_t m_current = 0;
    bool m_paramActive = false;
    bool m_sawSeparator = false;
    bool m_nextIsSub = false;

    std::string m_string;
    bool m_stringIsApc = false;
    bool m_stringOverflow = false;
    std::uint8_t m_dcsFinal = 0;

    char32_t m_utf8Cp = 0;
    char32_t m_utf8Min = 0;
    int m_utf8Needed = 0;
};

} // namespace AfterglowVt
