// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The checks of afterglow_tuifuzz: what must hold for a screen of the
// terminal core after any input. ScreenChecksTest breaks screens on purpose
// to show that each check sees it.

#pragma once

#include <AfterglowVt/Cell.h>
#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Terminal.h>

#include <cstdint>
#include <optional>
#include <string>

namespace AfterglowConformance {

/// The checks after each step. Returns what is wrong, or nothing.
inline std::optional<std::string> checkScreen(const AfterglowVt::Screen& s, const char* name)
{
    const auto fail = [name](std::string what) { return std::optional<std::string>(std::string(name) + ": " + what); };
    if (s.cursor.x < 0 || s.cursor.x >= s.cols() || s.cursor.y < 0 || s.cursor.y >= s.rows()) {
        return fail("cursor " + std::to_string(s.cursor.x) + "," + std::to_string(s.cursor.y) + " outside "
                    + std::to_string(s.cols()) + "x" + std::to_string(s.rows()));
    }
    const std::size_t history = s.historySize();
    for (std::size_t index = 0; index < s.totalRows(); ++index) {
        const AfterglowVt::Row& row = s.absoluteRow(index);
        const bool onScreen = index >= history;
        const std::string where = onScreen ? "row " + std::to_string(index - history)
                                           : "scrollback row -" + std::to_string(history - index);
        // History rows drop their trailing blanks; screen rows keep them all.
        if (onScreen ? row.cells.size() != std::size_t(s.cols()) : row.cells.size() > std::size_t(s.cols())) {
            return fail(where + " has " + std::to_string(row.cells.size()) + " cells, the screen "
                        + std::to_string(s.cols()));
        }
        for (int x = 0; x < int(row.cells.size()); ++x) {
            const AfterglowVt::Cell& cell = row.cells[std::size_t(x)];
            const std::string at = where + " column " + std::to_string(x);
            if (cell.isMulticell()) {
                // The block's top-left cell is in the row dy above, at dx
                // to the left, and its parameters cover this offset.
                const int dx = cell.multicellDx();
                const int dy = cell.multicellDy();
                if (std::size_t(dy) > index || dx > x) return fail(at + ": a sized block with no top-left cell");
                const AfterglowVt::Row& origin = s.absoluteRow(index - std::size_t(dy));
                const AfterglowVt::Cell& first = origin.at(x - dx);
                const auto params = origin.multicell ? origin.multicell->find(std::uint16_t(x - dx))
                                                     : AfterglowVt::MulticellMap::const_iterator{};
                if (!first.isMulticell() || first.multicellDx() != 0 || first.multicellDy() != 0 || !origin.multicell
                    || params == origin.multicell->end()) {
                    return fail(at + ": a sized block with no top-left cell");
                }
                if (dx >= params->second.columns() || dy >= params->second.scale) {
                    return fail(at + ": a cell outside the size of its block");
                }
                if (dx == 0 && dy == 0) {
                    // Every cell of the block that is still stored is in it.
                    for (int by = 0; by < params->second.scale && index + std::size_t(by) < s.totalRows(); ++by) {
                        const AfterglowVt::Row& part = s.absoluteRow(index + std::size_t(by));
                        for (int bx = 0; bx < params->second.columns() && x + bx < s.cols(); ++bx) {
                            const AfterglowVt::Cell& c = part.at(x + bx);
                            if (!c.isMulticell() || c.multicellDx() != bx || c.multicellDy() != by) {
                                return fail(at + ": a sized block that lost its cell " + std::to_string(bx) + ","
                                            + std::to_string(by));
                            }
                        }
                    }
                }
                continue;
            }
            if (cell.width() == 2) {
                if (x + 1 >= s.cols()) return fail(at + ": a wide character in the last column");
                const AfterglowVt::Cell& tail = row.at(x + 1);
                if (!tail.isTail() || tail.isMulticell()) return fail(at + ": a wide character with no tail cell");
            } else if (cell.isTail()) {
                if (x == 0 || row.cells[std::size_t(x - 1)].width() != 2 || row.cells[std::size_t(x - 1)].isMulticell()) {
                    return fail(at + ": a tail cell with no wide character");
                }
            }
        }
    }
    return std::nullopt;
}

inline std::optional<std::string> checkTerminal(const AfterglowVt::Terminal& t)
{
    if (t.screen().cols() != t.cols() || t.screen().rows() != t.rows()) {
        return "the screen is " + std::to_string(t.screen().cols()) + "x" + std::to_string(t.screen().rows())
            + ", the terminal " + std::to_string(t.cols()) + "x" + std::to_string(t.rows());
    }
    if (auto wrong = checkScreen(t.screen(), &t.screen() == &t.primaryScreen() ? "main screen" : "alternate screen")) {
        return wrong;
    }
    if (&t.screen() != &t.primaryScreen()) return checkScreen(t.primaryScreen(), "main screen");
    return std::nullopt;
}

} // namespace AfterglowConformance
