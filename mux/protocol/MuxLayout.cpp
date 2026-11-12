// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxLayout.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace AfterglowMux {

namespace Json = AfterglowProto::Json;

namespace {

/// The end of the subtree that starts at @p at: a split's two sides follow it.
std::size_t subtreeEnd(const std::vector<LayoutNode>& tree, std::size_t at)
{
    std::size_t need = 1;
    while (at < tree.size() && need > 0) {
        need = need - 1 + (tree[at].pane == 0 ? 2 : 0);
        ++at;
    }
    return at;
}

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

std::optional<std::uint32_t> paneId(const Json::Value& value)
{
    const auto id = value.toInteger();
    if (!id || *id <= 0 || *id > 0xffffffffLL) return std::nullopt;
    return std::uint32_t(*id);
}

/// Reads one tab, and adds its panes to @p seen.
bool readTab(const Json::Value& value, LayoutTab& tab, std::set<std::uint32_t>& seen, std::string* error)
{
    if (!value.isObject()) return fail(error, "a tab is not an object");
    for (const auto& [key, item] : value.toObject()) {
        if (key != "tree" && key != "focused" && key != "zoomed" && key != "title") return fail(error, "'" + key + "' is not a key of a tab");
    }
    const Json::Value* tree = value.find("tree");
    if (!tree || !tree->isArray() || tree->toArray().empty()) return fail(error, "a tab has no tree");
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
            const auto id = paneId(item);
            if (!id) return fail(error, "a pane of a tree is not a pane ID");
            if (!seen.insert(*id).second) return fail(error, "pane " + std::to_string(*id) + " is in the layout more than once");
            if (seen.size() > MaxLayoutPanes) return fail(error, "the layout has more than " + std::to_string(MaxLayoutPanes) + " panes");
            panes.insert(*id);
            node.pane = *id;
            need -= 1;
        }
        tab.tree.push_back(node);
    }
    if (need != 0) return fail(error, "a split of a tree lacks a side");
    const auto paneOfTab = [&](const char* key, std::uint32_t& out, bool optional) {
        const Json::Value* v = value.find(key);
        if (!v) return fail(error, std::string("a tab has no '") + key + "'");
        if (optional && v->toInteger() == 0) {
            out = 0;
            return true;
        }
        const auto id = paneId(*v);
        if (!id || !panes.contains(*id)) return fail(error, std::string("the '") + key + "' pane of a tab is not in its tree");
        out = *id;
        return true;
    };
    if (!paneOfTab("focused", tab.focused, false) || !paneOfTab("zoomed", tab.zoomed, true)) return false;
    const Json::Value* title = value.find("title");
    if (!title || !title->isString()) return fail(error, "a tab has no 'title'");
    if (title->toString().size() > MaxTabTitle) return fail(error, "a tab's title is longer than " + std::to_string(MaxTabTitle) + " bytes");
    tab.title = title->toString();
    return true;
}

} // namespace

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
    return std::any_of(tabs.begin(), tabs.end(), [pane](const LayoutTab& tab) {
        return std::any_of(tab.tree.begin(), tab.tree.end(), [pane](const LayoutNode& node) { return node.pane == pane; });
    });
}

void Layout::addTab(std::uint32_t pane)
{
    LayoutTab tab;
    tab.tree.push_back(LayoutNode{pane, false, 0.5});
    tab.focused = pane;
    tabs.push_back(std::move(tab));
}

bool Layout::remove(std::uint32_t pane)
{
    for (std::size_t t = 0; t < tabs.size(); ++t) {
        std::vector<LayoutNode>& tree = tabs[t].tree;
        const auto leaf = std::find_if(tree.begin(), tree.end(), [pane](const LayoutNode& node) { return node.pane == pane; });
        if (leaf == tree.end()) continue;
        const std::size_t at = std::size_t(leaf - tree.begin());
        if (tree.size() == 1) {
            tabs.erase(tabs.begin() + std::ptrdiff_t(t));
            // The tab that slides into its place shows, or the new last one,
            // as in a window.
            if (t < current || (t == current && current > 0 && current >= tabs.size())) --current;
            return true;
        }
        // Its split: the closest one before it whose first or second side
        // starts at it. That split's place goes to the other side.
        for (std::size_t split = at; split-- > 0;) {
            if (tree[split].pane != 0) continue;
            const std::size_t second = subtreeEnd(tree, split + 1);
            if (split + 1 != at && second != at) continue;
            const std::size_t end = subtreeEnd(tree, second);
            std::vector<LayoutNode> other = split + 1 == at ? std::vector<LayoutNode>(tree.begin() + std::ptrdiff_t(second), tree.begin() + std::ptrdiff_t(end))
                                                            : std::vector<LayoutNode>(tree.begin() + std::ptrdiff_t(split + 1), tree.begin() + std::ptrdiff_t(second));
            tree.erase(tree.begin() + std::ptrdiff_t(split), tree.begin() + std::ptrdiff_t(end));
            tree.insert(tree.begin() + std::ptrdiff_t(split), other.begin(), other.end());
            break;
        }
        if (tabs[t].focused == pane) tabs[t].focused = firstPane(tree);
        if (tabs[t].zoomed == pane) tabs[t].zoomed = 0;
        return true;
    }
    return false;
}

void Layout::reconcile(const std::vector<std::uint32_t>& live)
{
    for (const std::uint32_t pane : panes()) {
        if (std::find(live.begin(), live.end(), pane) == live.end()) remove(pane);
    }
    for (const std::uint32_t pane : live) {
        if (!contains(pane)) addTab(pane);
    }
    if (current >= tabs.size()) current = tabs.empty() ? 0 : tabs.size() - 1;
}

std::optional<std::size_t> Layout::tabOf(std::uint32_t pane) const
{
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
    if (!t) return false;
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
    return true;
}

void Layout::insertTab(std::uint32_t added, std::uint32_t beside)
{
    const auto t = tabOf(beside);
    LayoutTab tab;
    tab.tree.push_back(LayoutNode{added, false, 0.5});
    tab.focused = added;
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
    return Json::Object{{"tabs", std::move(tabs)}, {"current", std::int64_t(layout.current)}};
}

std::optional<Layout> layoutFrom(const Json::Value& value, std::string* error)
{
    if (!value.isObject()) {
        fail(error, "the layout is not an object");
        return std::nullopt;
    }
    for (const auto& [key, item] : value.toObject()) {
        if (key != "tabs" && key != "current") {
            fail(error, "'" + key + "' is not a key of a layout");
            return std::nullopt;
        }
    }
    const Json::Value* tabs = value.find("tabs");
    const Json::Value* current = value.find("current");
    if (!tabs || !tabs->isArray() || tabs->toArray().empty() || !current) {
        fail(error, "the layout has no tabs, or no current tab");
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
    return layout;
}

} // namespace AfterglowMux
