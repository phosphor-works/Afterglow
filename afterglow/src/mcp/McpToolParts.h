// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config/Settings.h"
#include "mcp/DaemonLink.h"

#include "MuxProtocol.h"

#include <PhosphorMcp/McpCall.h>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <utility>

// What the files of the tools of afterglow mcp share: the pieces of their
// schemas, and the helpers of their handlers.

namespace Afterglow::McpParts {

using L = QLatin1String;
using PhosphorMcp::McpCall;

// The schema pieces of the tools. Only the keywords that phosphor-mcp takes
// (its README, "Schemas"): no pattern and no format.

inline QJsonObject integer(qint64 minimum, qint64 maximum, const QString& description)
{
    return {{L("type"), L("integer")},
            {L("minimum"), minimum},
            {L("maximum"), maximum},
            {L("description"), description}};
}

inline QJsonObject text(int maxLength, const QString& description)
{
    return {{L("type"), L("string")}, {L("maxLength"), maxLength}, {L("description"), description}};
}

inline QJsonObject choice(const QStringList& values, const QString& description)
{
    return {{L("type"), L("string")}, {L("enum"), QJsonArray::fromStringList(values)}, {L("description"), description}};
}

inline QJsonObject typed(const char* type)
{
    return {{L("type"), L(type)}};
}

inline QJsonObject nullable(const char* type)
{
    return {{L("type"), QJsonArray{L(type), L("null")}}};
}

inline QJsonObject schema(const QJsonObject& properties, const QStringList& required = {})
{
    QJsonObject out{{L("type"), L("object")}, {L("properties"), properties}, {L("additionalProperties"), false}};
    if (!required.isEmpty())
        out.insert(L("required"), QJsonArray::fromStringList(required));
    return out;
}

/// An output schema: it lists what a result has, and lets more come.
inline QJsonObject result(const QJsonObject& properties, const QStringList& required)
{
    return {{L("type"), L("object")},
            {L("properties"), properties},
            {L("required"), QJsonArray::fromStringList(required)}};
}

inline QJsonObject paneArgument(
    const QString& description = QStringLiteral("The ID of a pane of the session, from list_panes or open_pane."))
{
    return integer(1, 0xffffffffLL, description);
}

inline QJsonObject timeoutArgument()
{
    return integer(1, McpSettings::MaxTimeout,
                   QStringLiteral("Milliseconds to wait. The default and the most are the settings "
                                  "Mcp.waitTimeout and Mcp.maxWaitTimeout."));
}

inline QJsonObject tailArgument(const QString& description)
{
    return integer(1, 1000000, description);
}

inline quint32 paneOf(const McpCall& call)
{
    return quint32(call.arguments().value(L("pane")).toInteger());
}

/// A failed call of the daemon, for the model: what happened, and what to do.
inline QString failure(const DaemonLink::Error& error)
{
    if (error.code.isEmpty())
        return QStringLiteral("lost the mux daemon: %1. Restart the server.").arg(error.message);
    if (error.code == L(AfterglowMux::Error::TryAgain))
        return QStringLiteral("%1. The pane is busy for a moment: call the tool again.").arg(error.message);
    return error.message;
}

/// The last @p lines lines and @p bytes bytes of UTF-8 of @p text. @p cut:
/// it was cut before.
struct Output
{
    QString text;
    bool truncated = false;
};

inline Output shape(QString text, int lines, qsizetype bytes, bool cut)
{
    Output out{std::move(text), cut};
    if (lines > 0) {
        // The text ends at the cursor: a line end at its very end ends the
        // last line, and starts none.
        qsizetype at = out.text.size();
        if (at > 0 && out.text.at(at - 1) == QLatin1Char('\n'))
            --at;
        for (int n = 0; n < lines && at > 0; ++n)
            at = out.text.lastIndexOf(QLatin1Char('\n'), at - 1);
        if (at > 0) {
            out.text = out.text.mid(at + 1);
            out.truncated = true;
        }
    }
    QByteArray utf8 = out.text.toUtf8();
    if (utf8.size() > bytes) {
        qsizetype start = utf8.size() - bytes;
        // Not inside a character.
        while (start < utf8.size() && (quint8(utf8.at(start)) & 0xC0) == 0x80)
            ++start;
        out.text = QString::fromUtf8(utf8.mid(start));
        out.truncated = true;
    }
    return out;
}

} // namespace Afterglow::McpParts
