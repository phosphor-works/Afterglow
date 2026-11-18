// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxLayout.h"
#include "MuxStream.h"

#include <PhosphorChannel/Stream.h>
#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

using namespace AfterglowMux;
namespace Json = AfterglowProto::Json;

namespace {

LayoutNode pane(std::uint32_t id)
{
    return LayoutNode{id, false, 0.5};
}

LayoutNode split(bool down, double ratio = 0.5)
{
    return LayoutNode{0, down, ratio};
}

/// Two tabs: 1 | (2 / 3), then 4 alone.
Layout twoTabs()
{
    Layout layout;
    layout.tabs.push_back(LayoutTab{{split(false, 0.3), pane(1), split(true, 0.6), pane(2), pane(3)}, 2, 0, "work"});
    layout.tabs.push_back(LayoutTab{{pane(4)}, 4, 0, ""});
    layout.current = 1;
    return layout;
}

std::optional<Layout> throughText(const Layout& layout, std::string* error = nullptr)
{
    const auto json = Json::parse(Json::write(toJson(layout)), error);
    if (!json) return std::nullopt;
    return layoutFrom(*json, error);
}

/// The layout's JSON with @p tab as its only tab.
Json::Value withTab(Json::Value tab)
{
    return Json::Object{{"tabs", Json::Array{std::move(tab)}}, {"current", 0}};
}

Json::Value tab(Json::Array tree, Json::Value focused, Json::Value zoomed = 0, Json::Value title = "")
{
    return Json::Object{{"tree", std::move(tree)}, {"focused", std::move(focused)}, {"zoomed", std::move(zoomed)}, {"title", std::move(title)}};
}

/// A layout of pane 1 alone whose moved panes are @p moved.
Json::Value withMoved(Json::Value moved)
{
    return Json::Object{{"tabs", Json::Array{tab({1}, 1)}}, {"current", 0}, {"moved", std::move(moved)}};
}

} // namespace

TEST(MuxLayout, RoundTripsThroughText)
{
    Layout layout = twoTabs();
    layout.tabs[0].zoomed = 3;
    std::string error;
    const auto back = throughText(layout, &error);
    ASSERT_TRUE(back) << error;
    EXPECT_TRUE(*back == layout);
    EXPECT_EQ(back->panes(), (std::vector<std::uint32_t>{1, 2, 3, 4}));
    // A split is [direction, ratio], and a pane is its ID.
    EXPECT_EQ(Json::write(toJson(layout)["tabs"].toArray()[0]["tree"]), R"([["right",0.3],1,["down",0.6],2,3])");
    // The title that the user gave a pane is in the values, not in the tree.
    layout.values[2] = {"logs", {}};
    EXPECT_EQ(Json::write(toJson(layout)["tabs"].toArray()[0]["tree"]), R"([["right",0.3],1,["down",0.6],2,3])");
    // It keeps its title when another pane closes.
    ASSERT_TRUE(layout.remove(3));
    EXPECT_EQ(layout.values.at(2).title, "logs");
}

TEST(MuxLayout, AClosedPaneLeavesItsPlaceToTheOtherSide)
{
    // The first side of the inner split goes: 3 takes the split's place.
    Layout layout = twoTabs();
    ASSERT_TRUE(layout.remove(2));
    EXPECT_EQ(layout.tabs[0].tree, (std::vector<LayoutNode>{split(false, 0.3), pane(1), pane(3)}));
    // The focus was on 2: it goes to the first pane of the tab.
    EXPECT_EQ(layout.tabs[0].focused, 1u);
    // The first side of the outer split goes: the inner split takes the tree.
    layout = twoTabs();
    ASSERT_TRUE(layout.remove(1));
    EXPECT_EQ(layout.tabs[0].tree, (std::vector<LayoutNode>{split(true, 0.6), pane(2), pane(3)}));
    EXPECT_EQ(layout.tabs[0].focused, 2u);
    // The second side.
    layout = twoTabs();
    layout.tabs[0].zoomed = 3;
    ASSERT_TRUE(layout.remove(3));
    EXPECT_EQ(layout.tabs[0].tree, (std::vector<LayoutNode>{split(false, 0.3), pane(1), pane(2)}));
    EXPECT_EQ(layout.tabs[0].zoomed, 0u);
    EXPECT_FALSE(layout.remove(3));
    // The result is still a layout that a window takes.
    std::string error;
    EXPECT_TRUE(throughText(layout, &error)) << error;
    // The second side, whose sibling is a split: that split takes the tree.
    layout = Layout{};
    layout.tabs.push_back(LayoutTab{{split(false, 0.3), split(true, 0.6), pane(1), pane(2), pane(3)}, 3, 0, ""});
    ASSERT_TRUE(layout.remove(3));
    EXPECT_EQ(layout.tabs[0].tree, (std::vector<LayoutNode>{split(true, 0.6), pane(1), pane(2)}));
    EXPECT_EQ(layout.tabs[0].focused, 1u);
    EXPECT_TRUE(throughText(layout, &error)) << error;
}

// The panes beside a pane, as a window finds them: of the panes on that
// side, the lowest sum of four times the distance along it and the distance
// of the centres across it. 0 at the edge, and never a pane of another tab.
TEST(MuxLayout, NeighborsFollowTheSplitsOfTheTab)
{
    const Layout layout = twoTabs();
    EXPECT_EQ(layout.neighbor(1, Side::Right), 2u); // 2 is above 3, both beside 1: 2 is more in line at 0.6
    EXPECT_EQ(layout.neighbor(1, Side::Left), 0u);
    EXPECT_EQ(layout.neighbor(1, Side::Up), 0u);
    EXPECT_EQ(layout.neighbor(2, Side::Left), 1u);
    EXPECT_EQ(layout.neighbor(3, Side::Left), 1u);
    EXPECT_EQ(layout.neighbor(2, Side::Down), 3u);
    EXPECT_EQ(layout.neighbor(3, Side::Up), 2u);
    EXPECT_EQ(layout.neighbor(3, Side::Down), 0u);
    EXPECT_EQ(layout.neighbor(2, Side::Right), 0u);
    EXPECT_EQ(layout.neighbor(4, Side::Left), 0u);
    EXPECT_EQ(layout.neighbor(9, Side::Left), 0u);
    // Four in a grid, (1 / 3) | (2 / 4): each finds the one across, not the
    // one at a corner.
    Layout grid;
    grid.tabs.push_back(LayoutTab{{split(false), split(true), pane(1), pane(3), split(true), pane(2), pane(4)}, 1, 0, ""});
    EXPECT_EQ(grid.neighbor(1, Side::Right), 2u);
    EXPECT_EQ(grid.neighbor(3, Side::Right), 4u);
    EXPECT_EQ(grid.neighbor(4, Side::Left), 3u);
    EXPECT_EQ(grid.neighbor(4, Side::Up), 2u);
    EXPECT_EQ(grid.neighbor(1, Side::Down), 3u);
    for (const std::uint32_t id : {1u, 2u, 3u, 4u}) {
        const std::array<std::uint32_t, 4> all = grid.neighbors(id);
        for (const Side side : {Side::Right, Side::Down, Side::Left, Side::Up}) EXPECT_EQ(all[std::size_t(side)], grid.neighbor(id, side));
    }

    // The nearest counts four times as much as the most in line: 1 | (2 /
    // 3) | 4, where 4 is in line with 1 but past 2 and 3.
    Layout row;
    row.tabs.push_back(LayoutTab{{split(false), pane(1), split(false), split(true), pane(2), pane(3), pane(4)}, 1, 0, ""});
    EXPECT_EQ(row.neighbor(1, Side::Right), 2u);
    EXPECT_EQ(row.neighbor(4, Side::Left), 2u);

    // Only the pane's own tab: in the first tab, 2 is right of 1 and in line
    // with 3 of the second tab, which would win there over 4.
    Layout tabs;
    tabs.tabs.push_back(LayoutTab{{split(false), pane(1), pane(2)}, 1, 0, ""});
    tabs.tabs.push_back(LayoutTab{{split(false), pane(3), split(true, 0.8), pane(4), pane(5)}, 3, 0, ""});
    EXPECT_EQ(tabs.neighbor(3, Side::Right), 4u);
    EXPECT_EQ(tabs.neighbor(4, Side::Left), 3u);
    EXPECT_EQ(tabs.neighbor(1, Side::Right), 2u);
}

// A window weighs the distances in its pixels: in a tall window, a pane far
// along but in line beats one beside but far across, which wins in a square.
TEST(MuxLayout, NeighborsWeighTheSizeOfTheWindow)
{
    // 1 | ((2 / 3) | 4): 1 is 0.4 of the width, 2 the top 0.3 of the middle,
    // and 4 the right 0.3, all the height.
    Layout layout;
    layout.tabs.push_back(
        LayoutTab{{split(false, 0.4), pane(1), split(false, 0.5), split(true, 0.3), pane(2), pane(3), pane(4)}, 1, 0, ""});
    EXPECT_EQ(layout.neighbor(1, Side::Right), 3u);
    // Pane 1 is 40 pixels wide and 3000 high: the tab is 100 by 3000.
    EXPECT_EQ(layout.neighbor(1, Side::Right, 40, 3000), 4u);
    // Wide, the one beside wins again.
    EXPECT_EQ(layout.neighbor(1, Side::Right, 4000, 100), 3u);
}

TEST(MuxLayout, ATabWithoutPanesGoes)
{
    // The current tab, which is the last: the one before it shows.
    Layout layout = twoTabs();
    ASSERT_TRUE(layout.remove(4));
    ASSERT_EQ(layout.tabs.size(), 1u);
    EXPECT_EQ(layout.current, 0u);
    // A tab before the current one: the current one keeps showing.
    layout = twoTabs();
    for (const std::uint32_t id : {1u, 2u, 3u}) ASSERT_TRUE(layout.remove(id));
    ASSERT_EQ(layout.tabs.size(), 1u);
    EXPECT_EQ(layout.tabs[0].focused, 4u);
    EXPECT_EQ(layout.current, 0u);
    // The current tab, with a tab after it: that one slides into its place.
    layout = twoTabs();
    layout.current = 0;
    layout.addTab(5);
    for (const std::uint32_t id : {1u, 2u, 3u}) ASSERT_TRUE(layout.remove(id));
    EXPECT_EQ(layout.current, 0u);
    EXPECT_EQ(layout.tabs[0].focused, 4u);
    // The last pane.
    layout = Layout{};
    layout.addTab(7);
    ASSERT_TRUE(layout.remove(7));
    EXPECT_TRUE(layout.tabs.empty());
    EXPECT_EQ(layout.current, 0u);
}

TEST(MuxLayout, ReconcileDropsTheGonePanesAndAddsTheNewOnes)
{
    // 2 exited after the window sent the layout, and 9 started before the
    // window named it.
    Layout layout = twoTabs();
    layout.reconcile({1, 3, 4, 9});
    EXPECT_EQ(layout.panes(), (std::vector<std::uint32_t>{1, 3, 4, 9}));
    ASSERT_EQ(layout.tabs.size(), 3u);
    EXPECT_EQ(layout.tabs[2].tree, std::vector<LayoutNode>{pane(9)});
    EXPECT_EQ(layout.tabs[2].focused, 9u);
    EXPECT_EQ(layout.current, 1u);
}

TEST(MuxLayout, ADeepTreeNeedsNoDeepJson)
{
    // Each split's second side splits again: a chain of the most panes of a
    // session, past the nesting of JSON (Json::MaxDepth) if the tree nested.
    static_assert(MaxSessionPanes > std::size_t(Json::MaxDepth));
    constexpr std::uint32_t Last = std::uint32_t(MaxSessionPanes);
    Layout layout;
    LayoutTab chain;
    for (std::uint32_t id = 1; id < Last; ++id) {
        chain.tree.push_back(split(id % 2 == 0));
        chain.tree.push_back(pane(id));
    }
    chain.tree.push_back(pane(Last));
    chain.focused = Last;
    layout.tabs.push_back(chain);
    std::string error;
    const auto back = throughText(layout, &error);
    ASSERT_TRUE(back) << error;
    EXPECT_TRUE(*back == layout);
    // The pane deepest in the chain closes: its split's place goes to the
    // pane before it.
    ASSERT_TRUE(layout.remove(Last));
    EXPECT_EQ(layout.tabs[0].tree.back(), pane(Last - 1));
    EXPECT_EQ(layout.tabs[0].tree.size(), 2u * (Last - 1) - 1);
    EXPECT_EQ(layout.tabs[0].focused, 1u);
    EXPECT_TRUE(throughText(layout, &error)) << error;
    // A chain the other way: each split's first side splits again.
    LayoutTab left;
    for (std::uint32_t id = 1; id < Last; ++id) left.tree.push_back(split(false));
    for (std::uint32_t id = 1; id <= Last; ++id) left.tree.push_back(pane(id));
    left.focused = 1;
    Layout comb;
    comb.tabs.push_back(left);
    ASSERT_TRUE(throughText(comb, &error)) << error;
    // Every second pane goes, one by one and at once: the same tree.
    Layout once = comb;
    std::vector<std::uint32_t> live;
    for (std::uint32_t id = 1; id <= Last; ++id) {
        if (id % 2 == 0) {
            ASSERT_TRUE(comb.remove(id));
        } else {
            live.push_back(id);
        }
    }
    once.reconcile(live);
    EXPECT_TRUE(once == comb) << Json::write(toJson(once)) << "\n" << Json::write(toJson(comb));
    EXPECT_EQ(comb.panes(), live);
    EXPECT_TRUE(throughText(comb, &error)) << error;
}

// The largest layout that the daemon takes fits one frame of the attach
// stream, as an event and as input: MaxSessionPanes tabs whose titles, and
// those of their panes, are the longest, of bytes that JSON writes as six
// each, with the largest IDs. The titles of the panes take all the bytes of
// the values (MaxLayoutValueBytes).
TEST(MuxLayout, TheLargestLayoutFitsOneFrame)
{
    Layout layout;
    for (std::size_t i = 0; i < MaxSessionPanes; ++i) {
        const std::uint32_t id = 0xffffffffu - std::uint32_t(i);
        layout.tabs.push_back(LayoutTab{{LayoutNode{id, false, 0.5}}, id, id, std::string(MaxTabTitle, '\x01')});
        layout.values[id] = {std::string(MaxPaneTitle, '\x01'), {}};
    }
    layout.current = layout.tabs.size() - 1;
    ASSERT_EQ(layout.valueBytes(), MaxLayoutValueBytes);
    const std::string json = Json::write(toJson(layout));
    ASSERT_GT(json.size(), MaxSessionPanes * (MaxTabTitle + MaxPaneTitle) * 6);
    std::string error;
    const auto parsed = Json::parse(json, &error);
    ASSERT_TRUE(parsed) << error;
    const auto back = layoutFrom(*parsed, &error);
    ASSERT_TRUE(back) << error;
    EXPECT_TRUE(*back == layout);
    const std::size_t frame = PhosphorChannel::Stream::Options{}.window - PhosphorChannel::Stream::FrameCost;
    // As an event and as input.
    const auto fits = [frame](const std::string& text) {
        Attach::Event event;
        event.type = Attach::EventType::Layout;
        event.payload = text;
        EXPECT_LE(Attach::encode(event).size(), frame);
        Attach::Input input;
        input.type = Attach::InputType::Layout;
        input.bytes = text;
        EXPECT_LE(Attach::encode(input).size(), frame);
    };
    fits(json);
    // So does one tab with every pane, split as far as it goes, with the
    // same values.
    Layout splits;
    LayoutTab all;
    for (std::size_t i = 1; i < MaxSessionPanes; ++i) all.tree.push_back(split(true, 1.0 / 3.0));
    for (std::size_t i = 0; i < MaxSessionPanes; ++i) all.tree.push_back(pane(0xffffffffu - std::uint32_t(i)));
    all.focused = all.zoomed = 0xffffffffu;
    all.title = std::string(MaxTabTitle, '\x01');
    splits.tabs.push_back(all);
    splits.values = layout.values;
    ASSERT_TRUE(throughText(splits, &error)) << error;
    fits(Json::write(toJson(splits)));
    // And the values as commands of one-byte arguments, which JSON writes
    // as nine bytes each with their quotes and comma: every pane has the
    // most arguments, and its title takes the rest of its share of
    // MaxLayoutValueBytes.
    Layout commands = layout;
    for (auto& entry : commands.values) {
        entry.second.command.assign(MaxPaneCommandArgs, std::string(1, '\x01'));
        entry.second.title.assign(MaxLayoutValueBytes / MaxSessionPanes - MaxPaneCommandArgs, '\x01');
    }
    ASSERT_EQ(commands.valueBytes(), MaxLayoutValueBytes);
    const auto commandsBack = throughText(commands, &error);
    ASSERT_TRUE(commandsBack) << error;
    EXPECT_TRUE(*commandsBack == commands);
    const std::string commandsJson = Json::write(toJson(commands));
    EXPECT_LT(commandsJson.size(), std::size_t(4) << 20);
    fits(commandsJson);
}

TEST(MuxLayout, PaneZeroIsASplitAndNoPane)
{
    // twoTabs() has splits, whose nodes hold 0.
    Layout layout = twoTabs();
    EXPECT_FALSE(layout.contains(0));
    EXPECT_FALSE(layout.remove(0));
    EXPECT_FALSE(layout.tabOf(0));
    EXPECT_FALSE(layout.split(0, 9, Side::Right));
    EXPECT_FALSE(layout.split(1, 0, Side::Right));
    EXPECT_FALSE(layout.focus(0));
    layout.addTab(0);
    layout.insertTab(0, 1);
    EXPECT_TRUE(layout == twoTabs());
}

TEST(MuxLayout, ReconcileMovesTheCurrentTabAsClosingTabsDoes)
{
    // The current tab goes with the one after it: the one before shows.
    Layout layout = twoTabs();
    layout.addTab(5);
    layout.current = 1;
    layout.reconcile({1, 2, 3});
    ASSERT_EQ(layout.tabs.size(), 1u);
    EXPECT_EQ(layout.current, 0u);
    // A tab before the current one goes: the current one keeps showing.
    layout = twoTabs();
    layout.addTab(5);
    layout.current = 2;
    layout.reconcile({4, 5});
    ASSERT_EQ(layout.tabs.size(), 2u);
    EXPECT_EQ(layout.current, 1u);
    EXPECT_EQ(layout.tabs[1].focused, 5u);
    // The current tab goes, and one follows it: that one slides into its place.
    layout = twoTabs();
    layout.addTab(5);
    layout.current = 1;
    layout.reconcile({1, 2, 3, 5});
    ASSERT_EQ(layout.tabs.size(), 2u);
    EXPECT_EQ(layout.current, 1u);
    EXPECT_EQ(layout.tabs[1].focused, 5u);
    // The zoomed pane goes: the zoom ends. The focus stays where it was.
    layout = twoTabs();
    layout.tabs[0].focused = 2;
    layout.tabs[0].zoomed = 3;
    layout.reconcile({1, 2, 4});
    EXPECT_EQ(layout.tabs[0].zoomed, 0u);
    EXPECT_EQ(layout.tabs[0].focused, 2u);
}

TEST(MuxLayout, RefusesWhatIsNotALayout)
{
    const Json::Value nan = std::numeric_limits<double>::quiet_NaN();
    const Json::Value bad[] = {
        Json::Array{},
        // No tab, tabs that are not an array, none at all.
        Json::Object{{"tabs", Json::Array{}}, {"current", 0}},
        Json::Object{{"tabs", tab({1}, 1)}, {"current", 0}},
        Json::Object{{"current", 0}},
        // A current tab that is missing, past the tabs, below 0, not a whole
        // number or not a number.
        Json::Object{{"tabs", Json::Array{tab({1}, 1)}}},
        Json::Object{{"tabs", Json::Array{tab({1}, 1)}}, {"current", 1}},
        Json::Object{{"tabs", Json::Array{tab({1}, 1)}}, {"current", -1}},
        Json::Object{{"tabs", Json::Array{tab({1}, 1)}}, {"current", 0.5}},
        Json::Object{{"tabs", Json::Array{tab({1}, 1)}}, {"current", "0"}},
        Json::Object{{"tabs", Json::Array{tab({1}, 1)}}, {"current", 0}, {"other", 1}},
        // Trees: empty, a split that lacks a side, a pane after the end, a
        // split that is not [direction, ratio], a ratio out of (0, 1).
        withTab(tab({}, 1)),
        withTab(tab({Json::Array{"right", 0.5}, 1}, 1)),
        withTab(tab({1, 2}, 1)),
        withTab(tab({Json::Array{"left", 0.5}, 1, 2}, 1)),
        withTab(tab({Json::Array{"right"}, 1, 2}, 1)),
        withTab(tab({Json::Array{"right", "half"}, 1, 2}, 1)),
        withTab(tab({Json::Array{"right", 0}, 1, 2}, 1)),
        withTab(tab({Json::Array{"right", 1}, 1, 2}, 1)),
        withTab(tab({Json::Array{"right", nan}, 1, 2}, 1)),
        // Panes: 0, too large, not a whole number, twice. The pane that is
        // too large has the focus, so that only its ID is wrong.
        withTab(tab({0}, 0)),
        withTab(tab({std::int64_t(1) << 32}, std::int64_t(1) << 32)),
        withTab(tab({1.5}, 1)),
        withTab(tab({Json::Array{"down", 0.5}, 1, 1}, 1)),
        // The focus and the zoom name a pane of the tab.
        withTab(tab({1}, 2)),
        withTab(tab({1}, 0)),
        withTab(tab({1}, 1, 2)),
        withTab(tab({1}, "1")),
        withTab(tab({1}, 1, "0")),
        // A title that is not a string, or too long.
        withTab(tab({1}, 1, 0, 3)),
        withTab(tab({1}, 1, 0, std::string(MaxTabTitle + 1, 't'))),
        // A pane is its ID alone, and its title is in the values: an array
        // in a tree is a split, and [ID, title] is no split.
        withTab(tab({Json::Array{1, ""}}, 1)),
        // A tab that lacks a key, or has one of another version.
        withTab(Json::Object{{"focused", 1}, {"zoomed", 0}, {"title", ""}}),
        withTab(Json::Object{{"tree", Json::Array{1}}, {"zoomed", 0}, {"title", ""}}),
        withTab(Json::Object{{"tree", Json::Array{1}}, {"focused", 1}, {"title", ""}}),
        withTab(Json::Object{{"tree", Json::Array{1}}, {"focused", 1}, {"zoomed", 0}}),
        withTab(Json::Object{{"tree", Json::Array{1}}, {"focused", 1}, {"zoomed", 0}, {"title", ""}, {"other", 1}}),
        // A tab that is not an object.
        withTab(Json::Array{1}),
        withTab(1),
        // The moved panes: a list of panes of the layout, at most
        // MaxSessionPanes.
        withMoved(1),
        withMoved(Json::Array{0}),
        withMoved(Json::Array{1.5}),
        withMoved(Json::Array{"1"}),
        withMoved(Json::Array{9}),
        withMoved(Json::Array(MaxSessionPanes + 1, Json::Value(1))),
    };
    for (const Json::Value& value : bad) {
        std::string error;
        EXPECT_FALSE(layoutFrom(value, &error)) << Json::write(value);
        EXPECT_FALSE(error.empty()) << Json::write(value);
    }
    // A pane in two tabs.
    std::string error;
    EXPECT_FALSE(layoutFrom(Json::Object{{"tabs", Json::Array{tab({1}, 1), tab({1}, 1)}}, {"current", 0}}, &error));
    EXPECT_NE(error.find("more than once"), std::string::npos) << error;
    // The good ones: a zoom of 0 is none, and a title at the limit.
    EXPECT_TRUE(layoutFrom(withTab(tab({1}, 1, 0, std::string(MaxTabTitle, 't'))), &error)) << error;
    EXPECT_TRUE(layoutFrom(withTab(tab({Json::Array{"down", 0.25}, 1, 2}, 2, 1)), &error)) << error;
    const auto moved = layoutFrom(withMoved(Json::Array{1}), &error);
    ASSERT_TRUE(moved) << error;
    EXPECT_EQ(moved->moved, std::vector<std::uint32_t>{1});
    const auto movedBack = throughText(*moved, &error);
    ASSERT_TRUE(movedBack) << error;
    EXPECT_TRUE(*movedBack == *moved);
}

TEST(MuxLayout, RefusesMorePanesThanASessionHas)
{
    Json::Array tabs;
    for (std::size_t id = 1; id <= MaxSessionPanes + 1; ++id) tabs.push_back(tab({std::int64_t(id)}, std::int64_t(id)));
    std::string error;
    EXPECT_FALSE(layoutFrom(Json::Object{{"tabs", tabs}, {"current", 0}}, &error));
    EXPECT_NE(error.find(std::to_string(MaxSessionPanes)), std::string::npos) << error;
    tabs.pop_back();
    EXPECT_TRUE(layoutFrom(Json::Object{{"tabs", std::move(tabs)}, {"current", 0}}, &error)) << error;
    // A tree with more splits than such a tree has is refused before its
    // nodes are read.
    Json::Array tree;
    for (std::size_t i = 0; i < 2 * MaxSessionPanes; ++i) tree.push_back(Json::Array{"right", 0.5});
    EXPECT_FALSE(layoutFrom(withTab(tab(std::move(tree), 1)), &error));
    EXPECT_NE(error.find(std::to_string(MaxSessionPanes)), std::string::npos) << error;
}

TEST(MuxLayout, AnErrorNamesItsCauseAndAKeyOfTheWireSafely)
{
    std::string error;
    EXPECT_FALSE(layoutFrom(Json::Object{{"tabs", Json::Array{tab({1}, 1)}}}, &error));
    EXPECT_NE(error.find("'current'"), std::string::npos) << error;
    EXPECT_FALSE(layoutFrom(Json::Object{{"tabs", 1}, {"current", 0}}, &error));
    EXPECT_NE(error.find("'tabs'"), std::string::npos) << error;
    EXPECT_FALSE(layoutFrom(withTab(Json::Object{{"tree", Json::Array{1}}, {"focused", 1}, {"zoomed", 0}, {"title", 1}}), &error));
    EXPECT_NE(error.find("not a string"), std::string::npos) << error;
    EXPECT_FALSE(layoutFrom(withTab(Json::Object{{"tree", 1}, {"focused", 1}, {"zoomed", 0}, {"title", ""}}), &error));
    EXPECT_NE(error.find("not an array"), std::string::npos) << error;
    const std::string key = std::string(1000, 'k') + "\x1b[2J";
    EXPECT_FALSE(layoutFrom(Json::Object{{"tabs", Json::Array{tab({1}, 1)}}, {"current", 0}, {key, 1}}, &error));
    EXPECT_LT(error.size(), 200u) << error;
    EXPECT_EQ(error.find('\x1b'), std::string::npos);
}

TEST(MuxLayout, ASplitPutsTheNewPaneOnItsSideWithTheFocus)
{
    // Right and down: the new pane second, below or right of the pane.
    // twoTabs() shows its second tab: the split of a pane of the first shows
    // the first, as a window shows the tab of the pane that it splits.
    Layout layout = twoTabs();
    layout.tabs[0].zoomed = 3;
    ASSERT_TRUE(layout.split(3, 7, Side::Right));
    EXPECT_EQ(layout.tabs[0].tree,
              (std::vector<LayoutNode>{split(false, 0.3), pane(1), split(true, 0.6), pane(2), split(false), pane(3), pane(7)}));
    EXPECT_EQ(layout.tabs[0].focused, 7u);
    EXPECT_EQ(layout.current, 0u);
    // A zoom would hide the new pane.
    EXPECT_EQ(layout.tabs[0].zoomed, 0u);
    // Left and up: the new pane first.
    ASSERT_TRUE(layout.split(4, 8, Side::Up));
    EXPECT_EQ(layout.tabs[1].tree, (std::vector<LayoutNode>{split(true), pane(8), pane(4)}));
    EXPECT_EQ(layout.current, 1u);
    EXPECT_FALSE(layout.split(99, 9, Side::Left));
    std::string error;
    EXPECT_TRUE(throughText(layout, &error)) << error;
}

// The moves of a window's layout name panes of the layout: a pane that goes
// takes its move with it, so that the layout still reads back.
TEST(MuxLayout, AGonePaneLeavesMoved)
{
    Layout layout = twoTabs();
    layout.moved = {2, 4};
    ASSERT_TRUE(layout.remove(2));
    EXPECT_EQ(layout.moved, std::vector<std::uint32_t>{4});
    // A pane that is not in the layout changes nothing.
    EXPECT_FALSE(layout.remove(99));
    EXPECT_EQ(layout.moved, std::vector<std::uint32_t>{4});
    layout.reconcile({1, 3});
    EXPECT_TRUE(layout.moved.empty());
    std::string error;
    EXPECT_TRUE(throughText(layout, &error)) << error;
    // The move of a pane that stays stays.
    layout = twoTabs();
    layout.moved = {3};
    ASSERT_TRUE(layout.remove(1));
    EXPECT_EQ(layout.moved, std::vector<std::uint32_t>{3});
    const auto back = throughText(layout, &error);
    ASSERT_TRUE(back) << error;
    EXPECT_EQ(back->moved, std::vector<std::uint32_t>{3});
    // A layout as a window sends it, whose moved pane then exits.
    Json::Object sent = withTab(tab({Json::Array{"down", 0.5}, 1, 2}, 2)).toObject();
    sent.emplace_back("moved", Json::Array{2});
    auto read = layoutFrom(Json::Value(std::move(sent)), &error);
    ASSERT_TRUE(read) << error;
    read->reconcile({1});
    EXPECT_TRUE(read->moved.empty());
    EXPECT_TRUE(layoutFrom(toJson(*read), &error)) << error;
    // And one whose moved pane closes.
    layout = Layout{};
    layout.tabs.push_back(LayoutTab{{split(true), pane(1), pane(2)}, 2, 0, ""});
    layout.moved = {2};
    ASSERT_TRUE(layout.remove(2));
    EXPECT_TRUE(layout.moved.empty());
    EXPECT_TRUE(layoutFrom(toJson(layout), &error)) << error;
}

TEST(MuxLayout, ANewTabGoesAfterTheTabOfItsPaneAndShows)
{
    Layout layout = twoTabs();
    layout.insertTab(7, 2);
    ASSERT_EQ(layout.tabs.size(), 3u);
    EXPECT_EQ(layout.tabs[1].tree, std::vector<LayoutNode>{pane(7)});
    EXPECT_EQ(layout.current, 1u);
    // Without the pane: after the last tab.
    layout.insertTab(8, 99);
    EXPECT_EQ(layout.tabs.back().tree, std::vector<LayoutNode>{pane(8)});
    EXPECT_EQ(layout.current, 3u);
}

TEST(MuxLayout, AFocusShowsTheTabOfThePane)
{
    Layout layout = twoTabs();
    layout.tabs[0].zoomed = 1;
    ASSERT_TRUE(layout.focus(3));
    EXPECT_EQ(layout.current, 0u);
    EXPECT_EQ(layout.tabs[0].focused, 3u);
    // The zoom of another pane would hide it.
    EXPECT_EQ(layout.tabs[0].zoomed, 0u);
    EXPECT_FALSE(layout.focus(99));
    EXPECT_EQ(layout.tabOf(4), 1u);
    EXPECT_FALSE(layout.tabOf(99));
}

// The titles and commands of the panes, which a project saves, go with the
// layout, and with their panes: a pane that goes takes its values.
TEST(MuxLayout, ThePanesKeepTheirTitlesAndCommands)
{
    Layout layout = twoTabs();
    layout.values[2] = {"editor", {}};
    layout.values[4] = {"", {"sleep", "300"}};
    std::string error;
    const auto back = throughText(layout, &error);
    ASSERT_TRUE(back) << error;
    EXPECT_TRUE(*back == layout);
    // A pane without values has no entry.
    layout.values[1] = {};
    EXPECT_EQ(toJson(layout)["values"].toArray().size(), 2u);
    layout.values.erase(1);
    ASSERT_TRUE(layout.remove(2));
    EXPECT_FALSE(layout.values.contains(2));
    layout.reconcile({1, 3});
    EXPECT_TRUE(layout.values.empty());

    const auto withValues = [](Json::Array values) {
        Json::Object object = withTab(tab({Json::Array{"down", 0.25}, 1, 2}, 2)).toObject();
        object.emplace_back("values", std::move(values));
        return Json::Value(std::move(object));
    };
    const auto value = [](Json::Value id, Json::Value title, Json::Value command) {
        return Json::Object{{"id", std::move(id)}, {"title", std::move(title)}, {"command", std::move(command)}};
    };
    EXPECT_TRUE(layoutFrom(withValues({value(1, std::string(MaxPaneTitle, 't'), Json::Array(MaxPaneCommandArgs, Json::Value("a")))}), &error))
        << error;
    // A window that does not know a pane's command sends no "command": the
    // daemon keeps its own. It goes back on the wire as it came.
    const auto kept = layoutFrom(withValues({Json::Object{{"id", 1}, {"title", ""}}}), &error);
    ASSERT_TRUE(kept) << error;
    ASSERT_TRUE(kept->values.contains(1));
    EXPECT_TRUE(kept->values.at(1).keepsCommand);
    EXPECT_TRUE(kept->values.at(1).command.empty());
    const Json::Array keptValues = toJson(*kept)["values"].toArray();
    ASSERT_EQ(keptValues.size(), 1u);
    EXPECT_FALSE(keptValues[0].find("command"));
    const auto keptBack = throughText(*kept, &error);
    ASSERT_TRUE(keptBack) << error;
    EXPECT_TRUE(*keptBack == *kept);
    // An empty "command" is the shell: the daemon's command goes.
    const auto cleared = layoutFrom(withValues({value(1, "", Json::Array{})}), &error);
    ASSERT_TRUE(cleared) << error;
    EXPECT_FALSE(cleared->values.contains(1));
    for (const Json::Value& bad : {
             withValues({value(3, "x", Json::Array{})}),
             withValues({value(1, "x", Json::Array{}), value(1, "y", Json::Array{})}),
             // Twice, also when the first has no title and no command.
             withValues({value(1, "", Json::Array{}), value(1, "y", Json::Array{})}),
             withValues({value(1, std::string(MaxPaneTitle + 1, 't'), Json::Array{})}),
             withValues({value(1, "", Json::Array{""})}),
             withValues({value(1, "", Json::Array(MaxPaneCommandArgs + 1, Json::Value("a")))}),
             withValues({value(1, "", Json::Array{"vi", std::string(MaxPaneCommandArg + 1, 'x')})}),
             withValues({value(1, "", Json::Array{1})}),
             // Each is at MaxLayoutValueBytes, and both are past it.
             withValues({value(1, "", Json::Array(MaxPaneCommandArgs, Json::Value(std::string(MaxPaneCommandArg, 'x')))),
                         value(2, "", Json::Array(MaxPaneCommandArgs, Json::Value(std::string(MaxPaneCommandArg, 'x'))))}),
             withValues({Json::Object{{"id", 1}, {"title", ""}, {"command", "vi"}}}),
             withValues({Json::Object{{"id", 1}}}),
             withValues({Json::Object{{"id", 1}, {"title", ""}, {"command", Json::Array{}}, {"other", 1}}}),
         }) {
        error.clear();
        EXPECT_FALSE(layoutFrom(bad, &error)) << Json::write(bad);
        EXPECT_FALSE(error.empty()) << Json::write(bad);
    }
}
