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
                           QStringLiteral("screenshot"), QStringLiteral("close_pane"), QStringLiteral("open_project"),
                           QStringLiteral("save_project"), QStringLiteral("list_script_commands"),
                           QStringLiteral("run_script_command")}));
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
        bool idempotent;
    };
    const std::map<QString, Hints> expected{
        {QStringLiteral("list_panes"), {true, false, true}},
        {QStringLiteral("open_pane"), {false, false, false}},
        {QStringLiteral("run"), {false, true, false}},
        {QStringLiteral("wait"), {true, false, false}},
        {QStringLiteral("send"), {false, true, false}},
        {QStringLiteral("read"), {true, false, true}},
        {QStringLiteral("screenshot"), {true, false, true}},
        {QStringLiteral("close_pane"), {false, true, false}},
        // A project adds panes, and a save can replace a saved project.
        {QStringLiteral("open_project"), {false, false, false}},
        {QStringLiteral("save_project"), {false, true, true}},
        {QStringLiteral("list_script_commands"), {true, false, true}},
        // A script command can do all that its script can.
        {QStringLiteral("run_script_command"), {false, true, false}},
    };
    for (const PhosphorMcp::McpTool& tool : McpTools::definitions()) {
        SCOPED_TRACE(tool.name().toStdString());
        ASSERT_TRUE(tool.readOnlyHint() && tool.destructiveHint() && tool.idempotentHint() && tool.openWorldHint());
        const auto it = expected.find(tool.name());
        ASSERT_NE(it, expected.end());
        EXPECT_EQ(*tool.readOnlyHint(), it->second.readOnly);
        EXPECT_EQ(*tool.destructiveHint(), it->second.destructive);
        EXPECT_EQ(*tool.idempotentHint(), it->second.idempotent);
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

// The tools of projects and script commands take no pane where they act on
// the session as a whole, and their names have the limits of the files and
// the scripts that they name.
TEST(McpTools, TheWorkflowToolsTakeNamesWithinTheirLimits)
{
    std::map<QString, QJsonObject> tools;
    for (const PhosphorMcp::McpTool& tool : McpTools::definitions())
        tools[tool.name()] = tool.inputSchema();
    for (const char* name : {"open_project", "save_project"}) {
        const QJsonObject schema = tools.at(QLatin1String(name));
        EXPECT_EQ(schema.value(QLatin1String("required")).toArray(), QJsonArray{QStringLiteral("name")}) << name;
        const QJsonObject project =
            schema.value(QLatin1String("properties")).toObject().value(QLatin1String("name")).toObject();
        EXPECT_EQ(project.value(QLatin1String("maxLength")).toInt(), 64) << name;
    }
    EXPECT_TRUE(
        tools.at(QStringLiteral("list_script_commands")).value(QLatin1String("properties")).toObject().isEmpty());
    const QJsonArray required =
        tools.at(QStringLiteral("run_script_command")).value(QLatin1String("required")).toArray();
    EXPECT_EQ(required, (QJsonArray{QStringLiteral("pane"), QStringLiteral("script"), QStringLiteral("command")}));
}
