// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Terminal.h>

#include <string>

namespace AfterglowVt::Test {

struct RecordingHost : ITerminalHost
{
    std::string written;
    std::string title;
    int bells = 0;

    void writeToPty(std::string_view bytes) override { written.append(bytes); }
    void titleChanged(std::string_view t) override { title.assign(t); }
    void bell() override { ++bells; }
    std::string notified;
    std::string pointer;
    void notify(std::string_view title, std::string_view body) override
    {
        notified = std::string(title) + "|" + std::string(body);
    }
    void pointerShapeChanged(std::string_view shape) override { pointer = shape; }
};

inline void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

/// Row text: empty cells as spaces, tails skipped, trailing spaces trimmed.
inline std::string rowText(const Screen& screen, const Row& row)
{
    std::string out;
    for (int x = 0; x < screen.cols(); ++x) {
        const Cell& c = row.at(x);
        if (c.isTail()) {
            continue;
        }
        if (c.isMulticell() && (c.multicellDx() != 0 || c.multicellDy() != 0)) {
            out += ' '; // a multicell block's text is in its top-left cell
            continue;
        }
        appendUtf8(out, c.isEmpty() ? U' ' : c.codepoint());
        for (char32_t e : screen.graphemeExtra(row, x)) {
            appendUtf8(out, e);
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

inline std::string rowText(const Terminal& t, int y)
{
    return rowText(t.screen(), t.screen().row(y));
}

/// The whole active area, rows joined by '\n', trailing empty rows dropped.
inline std::string screenText(const Terminal& t)
{
    std::string out;
    int lastNonEmpty = -1;
    for (int y = 0; y < t.rows(); ++y) {
        if (!rowText(t, y).empty()) {
            lastNonEmpty = y;
        }
    }
    for (int y = 0; y <= lastNonEmpty; ++y) {
        if (y > 0) {
            out += '\n';
        }
        out += rowText(t, y);
    }
    return out;
}

} // namespace AfterglowVt::Test
