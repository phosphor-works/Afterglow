// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowProto/Json.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// The tabs and splits of a session. The daemon keeps them while no window
// shows the session, and gives them to the next window. While a window shows
// the session, the window changes them and sends them back after each change
// (Attach::InputType::Layout). The daemon changes them too: a pane that it
// starts (Fn::NewPane) is a tab of its own until the window sends its
// layout, and a pane that ends or leaves the session leaves the layout.
//
// A tab's split tree is a list in preorder, not nested objects: a split is
// followed by its first side, then its second, each a pane or a split. A
// split is ["right" or "down", ratio], a pane its ID. A deep tree is then
// no deeper JSON
// (Json::MaxDepth). The daemon's code walks the list without recursion. The
// window's code that recurses over a tree goes no deeper than
// MaxSessionPanes.

namespace AfterglowMux {

/// The most panes of a session: the daemon starts no more (Fn::NewPane), and
/// takes no layout with more. A window's stream reads the state channels of
/// that many panes and the channel of the events at once.
inline constexpr std::size_t MaxSessionPanes = 256;
/// The longest tab title, in bytes. A layout of MaxSessionPanes tabs with
/// such titles and with the most values (MaxLayoutValueBytes) still fits
/// one frame of the attach stream (MuxLayoutTest).
inline constexpr std::size_t MaxTabTitle = 1024;
/// The longest title that the user gives a pane, in bytes.
inline constexpr std::size_t MaxPaneTitle = 1024;
/// The most arguments of a pane's command, and the longest one, in bytes:
/// a project keeps 64 of at most 4096 characters, so it keeps each command
/// that fits.
inline constexpr std::size_t MaxPaneCommandArgs = 64;
inline constexpr std::size_t MaxPaneCommandArg = 4096;
/// The bytes of the titles and commands of all the panes of a layout
/// (Layout::valueBytes). With JSON's escapes, 6 bytes for a control
/// character, the layout stays well under the largest frame of a channel's
/// default window (4 MiB), as one pane's command in a placePane call does.
inline constexpr std::size_t MaxLayoutValueBytes = std::size_t(256) << 10;
static_assert(MaxPaneCommandArgs * MaxPaneCommandArg <= MaxLayoutValueBytes);
static_assert(MaxSessionPanes * MaxPaneTitle <= MaxLayoutValueBytes);

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

/// What a project saves of a pane that only a window, or the client that
/// started the pane, knows: the title that the user gave it, and the command
/// that it was asked to run. The daemon keeps them for the next window.
struct PaneValues
{
    /// Empty: the program's title.
    std::string title;
    /// Empty: the shell.
    std::vector<std::string> command;
    bool operator==(const PaneValues&) const = default;
    bool empty() const { return title.empty() && command.empty(); }
    /// The bytes of the title and the arguments.
    std::size_t bytes() const;
};

/// Whether @p command fits in a layout (MaxPaneCommandArgs,
/// MaxPaneCommandArg), with a program name first.
bool validPaneCommand(const std::vector<std::string>& command);

/// Where a pane goes beside another one (`afterglow ctl split` and
/// `move-pane`).
enum class Side : std::uint8_t { Right, Down, Left, Up };

struct Layout
{
    std::vector<LayoutTab> tabs;
    /// The tab that shows: below tabs.size() unless there is no tab.
    std::size_t current = 0;
    /// The values of the panes that have any, each a pane of the tabs, at
    /// most MaxLayoutValueBytes in all.
    std::map<std::uint32_t, PaneValues> values;
    /// The panes that the window moved since its last layout, which
    /// `afterglow ctl watch` hears of (paneMoved): a layout that the window
    /// sends has them, and the daemon keeps none.
    std::vector<std::uint32_t> moved;
    /// The bytes of the values.
    std::size_t valueBytes() const;

    bool operator==(const Layout&) const = default;

    /// The panes, tab by tab, each tree in preorder.
    std::vector<std::uint32_t> panes() const;
    /// False for 0, which marks a split and is no pane.
    bool contains(std::uint32_t pane) const;
    /// Adds a tab with @p pane alone, after the last tab. Nothing for 0.
    void addTab(std::uint32_t pane);
    /// Removes @p pane, with its values: the other side of its split takes its place, and a
    /// tab without panes goes. When the pane had the focus of its tab, the
    /// tab's first pane gets it. When the tab showed the pane alone, the
    /// zoom ends. False when the layout has no such pane. Allocates nothing:
    /// a pane that leaves a session (Session::take) is never lost on the way.
    bool remove(std::uint32_t pane);
    /// Keeps only the panes of @p live, and adds each pane of @p live that it
    /// lacks as a tab of its own: a window's layout that a pane's exit
    /// passed, or that came before a new pane. The focus and the zoom follow
    /// the rules of remove(). Linear in the panes of both.
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

/// The ID of a pane in a value of the wire: a whole number from 1 to
/// 2^32 - 1. 0 marks a split.
std::optional<std::uint32_t> paneIdFrom(const AfterglowProto::Json::Value& value);

AfterglowProto::Json::Value toJson(const Layout& layout);
/// nullopt for a value that is not a valid layout: @p error says why. A
/// valid layout is an object with the keys "tabs" and "current", and
/// optionally "values" and "moved": at least one tab, and the index of one
/// of them. Each tab has only "tree", "focused", "zoomed" and "title". Its
/// tree is a whole split tree in preorder (each split ["right" or "down", a
/// ratio above 0 and below 1], each pane its ID). The focus is a pane of the
/// tab's tree, the zoom one too or 0, and the title a string of at most
/// MaxTabTitle bytes. The values give a pane of the layout a title of at
/// most MaxPaneTitle bytes and a command (validPaneCommand), each pane once
/// and MaxLayoutValueBytes in all. The moved panes are panes of the layout.
/// The layout names each pane once, and at most MaxSessionPanes panes.
std::optional<Layout> layoutFrom(const AfterglowProto::Json::Value& value, std::string* error);

} // namespace AfterglowMux
