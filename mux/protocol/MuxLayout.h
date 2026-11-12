// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowProto/Json.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The tabs and splits of a session. The daemon keeps them while no window
// shows the session, and gives them to the next window. While a window shows
// the session, the window changes them and sends them back after each change
// (Attach::InputType::Layout).
//
// A tab's split tree is a list in preorder, not nested objects: a split is
// followed by its first side, then its second, each a pane or a split. A
// deep tree is then no deeper JSON (Json::MaxDepth), and no code recurses
// over it.

namespace AfterglowMux {

/// The most panes in a layout: a window's stream reads no more state channels
/// (Attach::MaxChannelId).
inline constexpr std::size_t MaxLayoutPanes = 4096;
/// The longest tab title, in bytes.
inline constexpr std::size_t MaxTabTitle = 4096;

/// A node of a split tree.
struct LayoutNode
{
    /// A pane: its ID in the daemon. 0 for a split.
    std::uint32_t pane = 0;
    /// A split: the second side is below the first, else right of it.
    bool down = false;
    /// A split: the share of the first side, above 0 and below 1.
    double ratio = 0.5;

    bool operator==(const LayoutNode&) const = default;
};

struct LayoutTab
{
    /// The split tree in preorder: never empty, and one pane alone for a tab
    /// without splits.
    std::vector<LayoutNode> tree;
    /// The pane that has the focus in the tab.
    std::uint32_t focused = 0;
    /// The pane that the tab shows alone (zoom), 0 for none.
    std::uint32_t zoomed = 0;
    /// The title that the user gave the tab, else empty.
    std::string title;

    bool operator==(const LayoutTab&) const = default;
};

/// Where a pane goes beside another one (`afterglow ctl split` and
/// `move-pane`).
enum class Side : std::uint8_t { Right, Down, Left, Up };

struct Layout
{
    std::vector<LayoutTab> tabs;
    /// The tab that shows: below tabs.size() unless there is no tab.
    std::size_t current = 0;

    bool operator==(const Layout&) const = default;

    /// The panes, tab by tab, each tree in preorder.
    std::vector<std::uint32_t> panes() const;
    bool contains(std::uint32_t pane) const;
    /// Adds a tab with @p pane alone, after the last tab.
    void addTab(std::uint32_t pane);
    /// Removes @p pane: the other side of its split takes its place, and a
    /// tab without panes goes. The tab's focus goes to its first pane. False
    /// when the layout has no such pane.
    bool remove(std::uint32_t pane);
    /// Keeps only the panes of @p live, and adds each pane of @p live that it
    /// lacks as a tab of its own: a window's layout that a pane's exit
    /// passed, or that came before a new pane.
    void reconcile(const std::vector<std::uint32_t>& live);

    /// The tab that holds @p pane.
    std::optional<std::size_t> tabOf(std::uint32_t pane) const;
    /// Puts @p added on @p side of @p target, in half of its room, with the
    /// focus, as a window splits a pane. @p added is in no tab. False when
    /// no tab holds @p target.
    bool split(std::uint32_t target, std::uint32_t added, Side side);
    /// A tab with @p added alone, after the tab of @p beside (after the last
    /// when no tab holds it), which shows, as a window opens a tab.
    void insertTab(std::uint32_t added, std::uint32_t beside);
    /// The tab of @p pane shows, with @p pane focused. False when no tab
    /// holds it.
    bool focus(std::uint32_t pane);
};

AfterglowProto::Json::Value toJson(const Layout& layout);
/// nullopt for a value that is not a valid layout: @p error says why. A
/// valid layout names each pane once, has a tab for each tree, and names
/// panes of their own tab as the focus and the zoom.
std::optional<Layout> layoutFrom(const AfterglowProto::Json::Value& value, std::string* error);

} // namespace AfterglowMux
