// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The tool definitions of afterglow mcp, with no daemon: phosphor-mcp takes
// each schema, the list is the same at each call, and each tool says what it
// does to the session in all four hints. The tools themselves run against a
// daemon in mcp_session.py.

#include "mcp/McpTools.h"

#include <PhosphorMcp/McpServer.h>

#include <QJsonArray>
#include <QJsonDocument>

#include <gtest/gtest.h>

#include <map>

using namespace Afterglow;

namespace {

QByteArray listJson(const QList<PhosphorMcp::McpTool>& tools)
{
    QJsonArray list;
    for (const PhosphorMcp::McpTool& tool : tools)
        list.append(tool.toJson());
    return QJsonDocument(list).toJson(QJsonDocument::Compact);
}

} // namespace

TEST(McpTools, EachSchemaPassesTheChecksOfTheServer)
{
    PhosphorMcp::McpServer server;
    for (const PhosphorMcp::McpTool& tool : McpTools::definitions()) {
        QString error;
        EXPECT_TRUE(server.addTool(
            tool, [](PhosphorMcp::McpCall) { }, &error))
            << tool.name().toStdString() << ": " << error.toStdString();
    }
    EXPECT_EQ(listJson(server.tools()), listJson(McpTools::definitions()));
}

TEST(McpTools, TheListIsTheSameEachTime)
{
    const QList<PhosphorMcp::McpTool> first = McpTools::definitions();
    QStringList names;
    for (const PhosphorMcp::McpTool& tool : first)
        names << tool.name();
    EXPECT_EQ(names,
              (QStringList{QStringLiteral("list_panes"), QStringLiteral("open_pane"), QStringLiteral("run"),
                           QStringLiteral("wait"), QStringLiteral("send"), QStringLiteral("read"),
                           QStringLiteral("screenshot"), QStringLiteral("close_pane")}));
    EXPECT_TRUE(listJson(first) == listJson(McpTools::definitions()));
}

// MCP gives a hint that a tool leaves out its least safe value: a client
// would ask before each new pane as before a destructive call.
TEST(McpTools, EachToolSetsTheFourHints)
{
    struct Hints
    {
        bool readOnly;
        bool destructive;
    };
    const std::map<QString, Hints> expected{
        {QStringLiteral("list_panes"), {true, false}}, {QStringLiteral("open_pane"), {false, false}},
        {QStringLiteral("run"), {false, true}},        {QStringLiteral("wait"), {true, false}},
        {QStringLiteral("send"), {false, true}},       {QStringLiteral("read"), {true, false}},
        {QStringLiteral("screenshot"), {true, false}}, {QStringLiteral("close_pane"), {false, true}},
    };
    for (const PhosphorMcp::McpTool& tool : McpTools::definitions()) {
        SCOPED_TRACE(tool.name().toStdString());
        ASSERT_TRUE(tool.readOnlyHint() && tool.destructiveHint() && tool.idempotentHint() && tool.openWorldHint());
        const auto it = expected.find(tool.name());
        ASSERT_NE(it, expected.end());
        EXPECT_EQ(*tool.readOnlyHint(), it->second.readOnly);
        EXPECT_EQ(*tool.destructiveHint(), it->second.destructive);
        // Each acts only on the server's own session.
        EXPECT_FALSE(*tool.openWorldHint());
        const QJsonObject annotations = tool.toJson().value(QLatin1String("annotations")).toObject();
        EXPECT_EQ(annotations.size(), 4);
    }
}

// The pane is required wherever a tool acts on one: never the focused pane.
TEST(McpTools, EveryToolOfAPaneRequiresIt)
{
    for (const PhosphorMcp::McpTool& tool : McpTools::definitions()) {
        const QJsonObject schema = tool.inputSchema();
        if (!schema.value(QLatin1String("properties")).toObject().contains(QLatin1String("pane")))
            continue;
        const bool required =
            schema.value(QLatin1String("required")).toArray().contains(QJsonValue(QStringLiteral("pane")));
        // open_pane takes a pane only for a split, which its handler checks.
        EXPECT_EQ(required, tool.name() != QLatin1String("open_pane")) << tool.name().toStdString();
    }
}
