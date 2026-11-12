// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxLayout.h"

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
}

TEST(MuxLayout, ATabWithoutPanesGoesAndTheNextTabShows)
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

TEST(MuxLayout, ReconcileKeepsTheLivePanesOnly)
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
    // Each split's second side splits again: a chain of 3000 panes, past the
    // nesting of JSON (Json::MaxDepth) if the tree nested.
    Layout layout;
    LayoutTab chain;
    for (std::uint32_t id = 1; id < 3000; ++id) {
        chain.tree.push_back(split(id % 2 == 0));
        chain.tree.push_back(pane(id));
    }
    chain.tree.push_back(pane(3000));
    chain.focused = 3000;
    layout.tabs.push_back(chain);
    std::string error;
    const auto back = throughText(layout, &error);
    ASSERT_TRUE(back) << error;
    EXPECT_TRUE(*back == layout);
    // The pane deepest in the chain closes: its split's place goes to 2999.
    ASSERT_TRUE(layout.remove(3000));
    EXPECT_EQ(layout.tabs[0].tree.back(), pane(2999));
    EXPECT_EQ(layout.tabs[0].tree.size(), 2u * 2999 - 1);
    EXPECT_EQ(layout.tabs[0].focused, 1u);
}

TEST(MuxLayout, RefusesWhatIsNotALayout)
{
    const Json::Value nan = std::numeric_limits<double>::quiet_NaN();
    const Json::Value bad[] = {
        Json::Array{},
        Json::Object{{"tabs", Json::Array{}}, {"current", 0}},
        Json::Object{{"tabs", Json::Array{tab({1}, 1)}}},
        Json::Object{{"tabs", Json::Array{tab({1}, 1)}}, {"current", 1}},
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
        withTab(tab({1}, 1, 0, 3)),
        withTab(tab({1}, 1, 0, std::string(MaxTabTitle + 1, 't'))),
        withTab(Json::Object{{"tree", Json::Array{1}}, {"focused", 1}, {"zoomed", 0}}),
        withTab(Json::Object{{"tree", Json::Array{1}}, {"focused", 1}, {"zoomed", 0}, {"title", ""}, {"other", 1}}),
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
}

TEST(MuxLayout, RefusesMorePanesThanAWindowReads)
{
    Json::Array tabs;
    for (std::size_t id = 1; id <= MaxLayoutPanes + 1; ++id) tabs.push_back(tab({std::int64_t(id)}, std::int64_t(id)));
    std::string error;
    EXPECT_FALSE(layoutFrom(Json::Object{{"tabs", tabs}, {"current", 0}}, &error));
    EXPECT_NE(error.find(std::to_string(MaxLayoutPanes)), std::string::npos) << error;
    tabs.pop_back();
    EXPECT_TRUE(layoutFrom(Json::Object{{"tabs", std::move(tabs)}, {"current", 0}}, &error)) << error;
}
