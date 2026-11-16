// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxLayout.h"

#include <AfterglowProto/Text.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string_view>
#include <unordered_set>

namespace AfterglowMux {

namespace Json = AfterglowProto::Json;

namespace {

std::uint32_t firstPane(const std::vector<LayoutNode>& tree)
{
    for (const LayoutNode& node : tree) {
        if (node.pane != 0) return node.pane;
    }
    return 0;
}

bool fail(std::string* error, std::string text)
{
    if (error) *error = std::move(text);
    return false;
}

/// A key of the wire in an error, which the daemon writes on its stderr:
/// safe to print, and short.
std::string keyText(std::string_view key)
{
    constexpr std::size_t Shown = 64;
    return "'" + AfterglowProto::printable(key.substr(0, Shown)) + (key.size() > Shown ? "...'" : "'");
}

/// Reads one tab, and adds its panes to @p seen.
bool readTab(const Json::Value& value, LayoutTab& tab, std::set<std::uint32_t>& seen, std::string* error)
{
    if (!value.isObject()) return fail(error, "a tab is not an object");
    for (const auto& [key, item] : value.toObject()) {
        if (key != "tree" && key != "focused" && key != "zoomed" && key != "title") return fail(error, keyText(key) + " is not a key of a tab");
    }
    const Json::Value* tree = value.find("tree");
    if (!tree) return fail(error, "a tab has no 'tree'");
    if (!tree->isArray()) return fail(error, "the 'tree' of a tab is not an array");
    if (tree->toArray().empty()) return fail(error, "the 'tree' of a tab is empty");
    // A tree of n panes has n - 1 splits: one with more than the panes of a
    // session can have is refused before its nodes are read.
    if (tree->toArray().size() > 2 * MaxSessionPanes - 1) {
        return fail(error, "a tab's tree has more panes than the " + std::to_string(MaxSessionPanes) + " of a session");
    }
    // Each split needs two sides: the tree ends where nothing more is needed,
    // and only there.
    std::size_t need = 1;
    std::set<std::uint32_t> panes;
    for (const Json::Value& item : tree->toArray()) {
        if (need == 0) return fail(error, "a tree goes on after its last pane");
        LayoutNode node;
        if (item.isArray()) {
            const Json::Array& split = item.toArray();
            if (split.size() != 2 || !split[0].isString() || (split[0].toString() != "right" && split[0].toString() != "down")
                || !split[1].isNumber()) {
                return fail(error, "a split is not [\"right\" or \"down\", ratio]");
            }
            node.down = split[0].toString() == "down";
            node.ratio = split[1].toNumber();
            if (!std::isfinite(node.ratio) || node.ratio <= 0 || node.ratio >= 1) return fail(error, "a split's ratio is not between 0 and 1");
            need += 1;
        } else {
            const auto id = paneIdFrom(item);
            if (!id) return fail(error, "a pane of a tree is not a pane ID");
            if (!seen.insert(*id).second) return fail(error, "pane " + std::to_string(*id) + " is in the layout more than once");
            if (seen.size() > MaxSessionPanes) return fail(error, "the layout has more than " + std::to_string(MaxSessionPanes) + " panes");
            panes.insert(*id);
            node.pane = *id;
            need -= 1;
        }
        tab.tree.push_back(std::move(node));
    }
    if (need != 0) return fail(error, "a split of a tree lacks a side");
    const auto paneOfTab = [&](const char* key, std::uint32_t& out, bool optional) {
        const Json::Value* v = value.find(key);
        if (!v) return fail(error, std::string("a tab has no '") + key + "'");
        if (optional && v->toInteger() == 0) {
            out = 0;
            return true;
        }
        const auto id = paneIdFrom(*v);
        if (!id || !panes.contains(*id)) return fail(error, std::string("the '") + key + "' pane of a tab is not in its tree");
        out = *id;
        return true;
    };
    if (!paneOfTab("focused", tab.focused, false) || !paneOfTab("zoomed", tab.zoomed, true)) return false;
    const Json::Value* title = value.find("title");
    if (!title) return fail(error, "a tab has no 'title'");
    if (!title->isString()) return fail(error, "the 'title' of a tab is not a string");
    if (title->toString().size() > MaxTabTitle) return fail(error, "a tab's title is longer than " + std::to_string(MaxTabTitle) + " bytes");
    tab.title = title->toString();
    return true;
}

/// Reads the values of the panes in @p value into @p layout, whose tabs
/// hold @p panes.
bool readValues(const Json::Value& value, Layout& layout, const std::set<std::uint32_t>& panes, std::string* error)
{
    if (!value.isArray()) return fail(error, "the values of the panes are not a list");
    // Values without a title or a command are not kept, so the layout's
    // values do not show each pane that came.
    std::set<std::uint32_t> named;
    for (const Json::Value& item : value.toArray()) {
        if (!item.isObject()) return fail(error, "the values of a pane are not an object");
        for (const auto& [key, field] : item.toObject()) {
            if (key != "id" && key != "title" && key != "command") return fail(error, keyText(key) + " is not a value of a pane");
        }
        const Json::Value* id = item.find("id");
        const auto pane = id ? paneIdFrom(*id) : std::nullopt;
        if (!pane || !panes.contains(*pane)) return fail(error, "values name a pane that is not in the layout");
        if (!named.insert(*pane).second) return fail(error, "pane " + std::to_string(*pane) + " has values more than once");
        PaneValues values;
        const Json::Value* title = item.find("title");
        if (!title || !title->isString()) return fail(error, "the values of a pane have no 'title'");
        if (title->toString().size() > MaxPaneTitle) return fail(error, "a pane's title is longer than " + std::to_string(MaxPaneTitle) + " bytes");
        values.title = title->toString();
        // Without "command" the sender does not know it, and the daemon
        // keeps its own: an empty one clears it.
        const Json::Value* command = item.find("command");
        values.keepsCommand = !command;
        if (command && !command->isArray()) return fail(error, "the 'command' of a pane is not a list");
        // Refused before its words are copied.
        if (command && command->toArray().size() > MaxPaneCommandArgs) {
            return fail(error, "a pane's command has more than " + std::to_string(MaxPaneCommandArgs) + " arguments");
        }
        if (command) {
            for (const Json::Value& word : command->toArray()) {
                if (!word.isString()) return fail(error, "a pane's command has an argument that is not text");
                values.command.push_back(word.toString());
            }
        }
        if (!values.command.empty() && !validPaneCommand(values.command)) {
            return fail(error, "a pane's command has an empty program, more than " + std::to_string(MaxPaneCommandArgs)
                                   + " arguments, or one longer than " + std::to_string(MaxPaneCommandArg) + " bytes");
        }
        if (!values.empty() || values.keepsCommand) layout.values.emplace(*pane, std::move(values));
    }
    if (layout.valueBytes() > MaxLayoutValueBytes) {
        return fail(error, "the titles and commands of the panes are more than " + std::to_string(MaxLayoutValueBytes) + " bytes");
    }
    return true;
}

} // namespace

std::size_t PaneValues::bytes() const
{
    std::size_t total = title.size();
    for (const std::string& arg : command) total += arg.size();
    return total;
}

std::size_t Layout::valueBytes() const
{
    std::size_t total = 0;
    for (const auto& [pane, value] : values) total += value.bytes();
    return total;
}

bool validPaneCommand(const std::vector<std::string>& command)
{
    return !command.empty() && !command.front().empty() && command.size() <= MaxPaneCommandArgs
        && std::none_of(command.begin(), command.end(), [](const std::string& arg) { return arg.size() > MaxPaneCommandArg; });
}

std::optional<std::uint32_t> paneIdFrom(const Json::Value& value)
{
    const auto id = value.toInteger();
    if (!id || *id <= 0 || *id > 0xffffffffLL) return std::nullopt;
    return std::uint32_t(*id);
}

std::vector<std::uint32_t> Layout::panes() const
{
    std::vector<std::uint32_t> out;
    for (const LayoutTab& tab : tabs) {
        for (const LayoutNode& node : tab.tree) {
            if (node.pane != 0) out.push_back(node.pane);
        }
    }
    return out;
}

bool Layout::contains(std::uint32_t pane) const
{
    if (pane == 0) return false;
    return std::any_of(tabs.begin(), tabs.end(), [pane](const LayoutTab& tab) {
        return std::any_of(tab.tree.begin(), tab.tree.end(), [pane](const LayoutNode& node) { return node.pane == pane; });
    });
}

void Layout::addTab(std::uint32_t pane)
{
    if (pane == 0) return;
    LayoutTab tab;
    tab.tree.push_back(LayoutNode{pane, false, 0.5});
    tab.focused = pane;
    tabs.push_back(std::move(tab));
}

namespace {

/// The tab that shows when the old current tab went, and @p keptBefore of
/// the tabs before it stayed, of @p tabs that stay: as when a window closes
/// its tabs one by one, the tab that slides into the place of the current
/// one shows, else the new last one.
std::size_t currentAfter(std::size_t keptBefore, std::size_t tabs)
{
    return tabs == 0 ? 0 : std::min(keptBefore, tabs - 1);
}

} // namespace

bool Layout::remove(std::uint32_t pane)
{
    if (pane == 0) return false;
    // Its values and its move go with it, and only when it goes.
    const auto forget = [this, pane] {
        values.erase(pane);
        std::erase(moved, pane);
    };
    for (std::size_t t = 0; t < tabs.size(); ++t) {
        std::vector<LayoutNode>& tree = tabs[t].tree;
        const auto leaf = std::find_if(tree.begin(), tree.end(), [pane](const LayoutNode& node) { return node.pane == pane; });
        if (leaf == tree.end()) continue;
        const std::size_t at = std::size_t(leaf - tree.begin());
        if (tree.size() == 1) {
            tabs.erase(tabs.begin() + std::ptrdiff_t(t));
            if (t < current) --current;
            else if (t == current) current = currentAfter(t, tabs.size());
            forget();
            return true;
        }
        // Its split, in one walk back: the subtrees that end before the pane
        // and after the split count. A split joins two of them into one; the
        // first split with fewer than two is the pane's own. With none, the
        // pane is its first side, else its second.
        std::size_t whole = 0;
        std::size_t split = at;
        bool found = false;
        while (!found && split > 0) {
            --split;
            if (tree[split].pane != 0) ++whole;
            else if (whole < 2) found = true;
            else --whole;
        }
        // Only a tree that is not one (never one of layoutFrom()).
        if (!found) return false;
        if (whole == 0) {
            // The split and its first side go: the second side is in place.
            tree.erase(tree.begin() + std::ptrdiff_t(split), tree.begin() + std::ptrdiff_t(at + 1));
        } else {
            tree.erase(tree.begin() + std::ptrdiff_t(at));
            tree.erase(tree.begin() + std::ptrdiff_t(split));
        }
        if (tabs[t].focused == pane) tabs[t].focused = firstPane(tree);
        if (tabs[t].zoomed == pane) tabs[t].zoomed = 0;
        forget();
        return true;
    }
    return false;
}

void Layout::reconcile(const std::vector<std::uint32_t>& live)
{
    const std::unordered_set<std::uint32_t> alive(live.begin(), live.end());
    std::unordered_set<std::uint32_t> present;
    // Each tree in one pass: a split whose two sides keep a pane stays, one
    // with one such side gives its place to that side, as remove() does, and
    // one with none goes with its sides.
    struct Open
    {
        std::size_t node; ///< its index in kept, which keep marks
        int sides = 2; ///< the sides that did not end yet
        int kept = 0; ///< the sides that keep a pane
    };
    std::vector<Open> open;
    std::vector<LayoutNode> kept;
    std::vector<bool> keep;
    std::size_t tabsBefore = 0; ///< the tabs that stay before the current one
    bool currentStays = false;
    std::vector<LayoutTab> out;
    for (std::size_t t = 0; t < tabs.size(); ++t) {
        LayoutTab& tab = tabs[t];
        open.clear();
        kept.clear();
        keep.clear();
        for (const LayoutNode& node : tab.tree) {
            if (node.pane == 0) {
                kept.push_back(node);
                keep.push_back(false);
                open.push_back({kept.size() - 1});
                continue;
            }
            bool hasPane = alive.contains(node.pane) && present.insert(node.pane).second;
            if (hasPane) {
                kept.push_back(node);
                keep.push_back(true);
            }
            // The sides that end here, up to the first split that waits for
            // its second.
            while (!open.empty()) {
                Open& split = open.back();
                split.kept += hasPane ? 1 : 0;
                if (--split.sides > 0) break;
                keep[split.node] = split.kept == 2;
                hasPane = split.kept > 0;
                open.pop_back();
            }
        }
        tab.tree.clear();
        for (std::size_t i = 0; i < kept.size(); ++i) {
            if (keep[i]) tab.tree.push_back(kept[i]);
        }
        if (t == current) currentStays = !tab.tree.empty();
        if (tab.tree.empty()) continue;
        if (t < current) ++tabsBefore;
        const auto has = [&tab](std::uint32_t pane) {
            return std::any_of(tab.tree.begin(), tab.tree.end(), [pane](const LayoutNode& node) { return node.pane == pane; });
        };
        if (!has(tab.focused)) tab.focused = firstPane(tab.tree);
        if (tab.zoomed != 0 && !has(tab.zoomed)) tab.zoomed = 0;
        out.push_back(std::move(tab));
    }
    current = currentStays ? tabsBefore : currentAfter(tabsBefore, out.size());
    tabs = std::move(out);
    for (const std::uint32_t pane : live) {
        if (pane != 0 && present.insert(pane).second) addTab(pane);
    }
    if (current >= tabs.size()) current = tabs.empty() ? 0 : tabs.size() - 1;
    // The values and the moves of the panes that went go with them.
    std::erase_if(values, [&alive](const auto& value) { return !alive.contains(value.first); });
    std::erase_if(moved, [&alive](std::uint32_t pane) { return !alive.contains(pane); });
}

std::optional<std::size_t> Layout::tabOf(std::uint32_t pane) const
{
    // 0 marks a split, which every tab with a split has.
    if (pane == 0) return std::nullopt;
    for (std::size_t t = 0; t < tabs.size(); ++t) {
        for (const LayoutNode& node : tabs[t].tree) {
            if (node.pane == pane) return t;
        }
    }
    return std::nullopt;
}

bool Layout::split(std::uint32_t target, std::uint32_t added, Side side)
{
    const auto t = tabOf(target);
    if (!t || added == 0) return false;
    std::vector<LayoutNode>& tree = tabs[*t].tree;
    const auto leaf = std::find_if(tree.begin(), tree.end(), [target](const LayoutNode& node) { return node.pane == target; });
    // The leaf becomes a split of the two: the added pane first on its left
    // or above it.
    const bool addedFirst = side == Side::Left || side == Side::Up;
    const LayoutNode splitNode{0, side == Side::Down || side == Side::Up, 0.5};
    const LayoutNode first{addedFirst ? added : target, false, 0.5};
    const LayoutNode second{addedFirst ? target : added, false, 0.5};
    const auto at = tree.erase(leaf);
    tree.insert(at, {splitNode, first, second});
    tabs[*t].focused = added;
    tabs[*t].zoomed = 0;
    // A window shows the pane's tab before it splits the pane.
    current = *t;
    return true;
}

LayoutTab Layout::soleTab(std::uint32_t added)
{
    LayoutTab tab;
    if (added == 0) return tab;
    tab.tree.push_back(LayoutNode{added, false, 0.5});
    tab.focused = added;
    return tab;
}

void Layout::insertTab(std::uint32_t added, std::uint32_t beside)
{
    insertTab(soleTab(added), beside);
}

void Layout::insertTab(LayoutTab tab, std::uint32_t beside)
{
    if (tab.tree.empty()) return;
    const auto t = tabOf(beside);
    const std::size_t at = t ? *t + 1 : tabs.size();
    tabs.insert(tabs.begin() + std::ptrdiff_t(at), std::move(tab));
    current = at;
}

bool Layout::focus(std::uint32_t pane)
{
    const auto t = tabOf(pane);
    if (!t) return false;
    current = *t;
    tabs[*t].focused = pane;
    // A pane that another one hides with its zoom shows, as in a window.
    if (tabs[*t].zoomed != 0 && tabs[*t].zoomed != pane) tabs[*t].zoomed = 0;
    return true;
}

Json::Value toJson(const Layout& layout)
{
    Json::Array tabs;
    for (const LayoutTab& tab : layout.tabs) {
        Json::Array tree;
        for (const LayoutNode& node : tab.tree) {
            if (node.pane != 0) tree.push_back(std::int64_t(node.pane));
            else tree.push_back(Json::Array{node.down ? "down" : "right", node.ratio});
        }
        tabs.push_back(Json::Object{{"tree", std::move(tree)},
                                    {"focused", std::int64_t(tab.focused)},
                                    {"zoomed", std::int64_t(tab.zoomed)},
                                    {"title", tab.title}});
    }
    Json::Array values;
    for (const auto& [pane, value] : layout.values) {
        if (value.empty() && !value.keepsCommand) continue;
        Json::Object entry{{"id", std::int64_t(pane)}, {"title", value.title}};
        if (!value.keepsCommand) entry.emplace_back("command", Json::Array(value.command.begin(), value.command.end()));
        values.push_back(std::move(entry));
    }
    Json::Object out{{"tabs", std::move(tabs)}, {"current", std::int64_t(layout.current)}, {"values", std::move(values)}};
    if (!layout.moved.empty()) {
        Json::Array moved;
        for (const std::uint32_t pane : layout.moved) moved.push_back(std::int64_t(pane));
        out.emplace_back("moved", std::move(moved));
    }
    return out;
}

std::optional<Layout> layoutFrom(const Json::Value& value, std::string* error)
{
    if (!value.isObject()) {
        fail(error, "the layout is not an object");
        return std::nullopt;
    }
    for (const auto& [key, item] : value.toObject()) {
        if (key != "tabs" && key != "current" && key != "values" && key != "moved") {
            fail(error, keyText(key) + " is not a key of a layout");
            return std::nullopt;
        }
    }
    const Json::Value* tabs = value.find("tabs");
    const Json::Value* current = value.find("current");
    if (!tabs || !tabs->isArray()) {
        fail(error, "the layout has no 'tabs' array");
        return std::nullopt;
    }
    if (tabs->toArray().empty()) {
        fail(error, "the layout has no tab");
        return std::nullopt;
    }
    if (!current) {
        fail(error, "the layout has no 'current'");
        return std::nullopt;
    }
    Layout layout;
    std::set<std::uint32_t> seen;
    for (const Json::Value& item : tabs->toArray()) {
        LayoutTab tab;
        if (!readTab(item, tab, seen, error)) return std::nullopt;
        layout.tabs.push_back(std::move(tab));
    }
    const auto index = current->toInteger();
    if (!index || *index < 0 || std::size_t(*index) >= layout.tabs.size()) {
        fail(error, "the current tab is not a tab of the layout");
        return std::nullopt;
    }
    layout.current = std::size_t(*index);
    // A layout of panes without values has none.
    if (const Json::Value* values = value.find("values"); values && !readValues(*values, layout, seen, error)) return std::nullopt;
    if (const Json::Value* moved = value.find("moved")) {
        if (!moved->isArray() || moved->toArray().size() > MaxSessionPanes) {
            fail(error, "the moved panes are not a list of the layout's panes");
            return std::nullopt;
        }
        for (const Json::Value& item : moved->toArray()) {
            const auto pane = paneIdFrom(item);
            if (!pane || !seen.contains(*pane)) {
                fail(error, "the moved panes are not a list of the layout's panes");
                return std::nullopt;
            }
            layout.moved.push_back(*pane);
        }
    }
    return layout;
}

} // namespace AfterglowMux
