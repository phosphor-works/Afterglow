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
    // A pane with a title that the user gave it is [ID, title].
    layout.tabs[0].tree[3].title = "logs";
    EXPECT_EQ(Json::write(toJson(layout)["tabs"].toArray()[0]["tree"]), R"([["right",0.3],1,["down",0.6],[2,"logs"],3])");
    const auto titled = throughText(layout, &error);
    ASSERT_TRUE(titled) << error;
    EXPECT_TRUE(*titled == layout);
    EXPECT_EQ(titled->tabs[0].tree[3].title, "logs");
    // It keeps its title when another pane closes.
    ASSERT_TRUE(layout.remove(3));
    EXPECT_EQ(layout.tabs[0].tree.back().title, "logs");
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
        if (id % 2 == 0) ASSERT_TRUE(comb.remove(id));
        else live.push_back(id);
    }
    once.reconcile(live);
    EXPECT_TRUE(once == comb) << Json::write(toJson(once)) << "\n" << Json::write(toJson(comb));
    EXPECT_EQ(comb.panes(), live);
    EXPECT_TRUE(throughText(comb, &error)) << error;
}

// The largest layout that the daemon takes fits one frame of the attach
// stream, as an event and as input: MaxSessionPanes tabs whose titles, and
// those of their panes, are the longest, of bytes that JSON writes as six
// each, with the largest IDs.
TEST(MuxLayout, TheLargestLayoutFitsOneFrame)
{
    Layout layout;
    for (std::size_t i = 0; i < MaxSessionPanes; ++i) {
        const std::uint32_t id = 0xffffffffu - std::uint32_t(i);
        layout.tabs.push_back(LayoutTab{{LayoutNode{id, false, 0.5, std::string(MaxPaneTitle, '\x01')}}, id, id, std::string(MaxTabTitle, '\x01')});
    }
    layout.current = layout.tabs.size() - 1;
    const std::string json = Json::write(toJson(layout));
    ASSERT_GT(json.size(), MaxSessionPanes * (MaxTabTitle + MaxPaneTitle) * 6);
    std::string error;
    const auto back = layoutFrom(*Json::parse(json), &error);
    ASSERT_TRUE(back) << error;
    EXPECT_TRUE(*back == layout);
    const std::size_t frame = PhosphorChannel::Stream::Options{}.window - PhosphorChannel::Stream::FrameCost;
    Attach::Event event;
    event.type = Attach::EventType::Layout;
    event.payload = json;
    EXPECT_LE(Attach::encode(event).size(), frame);
    Attach::Input input;
    input.type = Attach::InputType::Layout;
    input.bytes = json;
    EXPECT_LE(Attach::encode(input).size(), frame);
    // So does one tab with every pane, split as far as it goes.
    Layout splits;
    LayoutTab all;
    for (std::size_t i = 1; i < MaxSessionPanes; ++i) all.tree.push_back(split(true, 1.0 / 3.0));
    for (std::size_t i = 0; i < MaxSessionPanes; ++i) all.tree.push_back(pane(0xffffffffu - std::uint32_t(i)));
    all.focused = all.zoomed = 0xffffffffu;
    all.title = std::string(MaxTabTitle, '\x01');
    splits.tabs.push_back(all);
    ASSERT_TRUE(throughText(splits, &error)) << error;
    EXPECT_LE(Json::write(toJson(splits)).size(), json.size());
}

TEST(MuxLayout, PaneZeroIsASplitAndNoPane)
{
    Layout layout = twoTabs();
    EXPECT_FALSE(layout.contains(0));
    EXPECT_FALSE(layout.remove(0));
    layout.addTab(0);
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
        // Panes: 0, too large, not a whole number, twice.
        withTab(tab({0}, 0)),
        withTab(tab({std::int64_t(1) << 32}, 1)),
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
        // A pane with a title: [ID, title], the title not empty and not too long.
        withTab(tab({Json::Array{1, ""}}, 1)),
        withTab(tab({Json::Array{1, 2}}, 1)),
        withTab(tab({Json::Array{1, "a", "b"}}, 1)),
        withTab(tab({Json::Array{1}}, 1)),
        withTab(tab({Json::Array{0, "a"}}, 0)),
        withTab(tab({Json::Array{1, std::string(MaxPaneTitle + 1, 't')}}, 1)),
        // A tab that lacks a key, or has one of another version.
        withTab(Json::Object{{"focused", 1}, {"zoomed", 0}, {"title", ""}}),
        withTab(Json::Object{{"tree", Json::Array{1}}, {"zoomed", 0}, {"title", ""}}),
        withTab(Json::Object{{"tree", Json::Array{1}}, {"focused", 1}, {"title", ""}}),
        withTab(Json::Object{{"tree", Json::Array{1}}, {"focused", 1}, {"zoomed", 0}}),
        withTab(Json::Object{{"tree", Json::Array{1}}, {"focused", 1}, {"zoomed", 0}, {"title", ""}, {"other", 1}}),
        // A tab that is not an object.
        withTab(Json::Array{1}),
        withTab(1),
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
    EXPECT_TRUE(layoutFrom(withTab(tab({Json::Array{"down", 0.5}, Json::Array{1, std::string(MaxPaneTitle, 't')}, 2}, 1)), &error)) << error;
    EXPECT_TRUE(layoutFrom(withTab(tab({Json::Array{"down", 0.25}, 1, 2}, 2, 1)), &error)) << error;
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
