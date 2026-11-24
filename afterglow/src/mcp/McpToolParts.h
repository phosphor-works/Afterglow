// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "config/Settings.h"
#include "mcp/DaemonLink.h"

#include "MuxLayout.h"
#include "MuxProtocol.h"

#include <PhosphorMcp/McpCall.h>
#include <PhosphorMcp/McpTool.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// What the files of the tools of afterglow mcp share: the pieces of their
// schemas, and the helpers of their handlers. The helpers that need no
// daemon are here, and not in the .cpp files, so that McpToolsTest checks
// them.

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
                   QStringLiteral("Milliseconds to wait, from the call. The default and the most are the settings "
                                  "Mcp.waitTimeout and Mcp.maxWaitTimeout."));
}

inline QJsonObject tailArgument(const QString& description)
{
    return integer(1, McpSettings::MaxTailLines, description);
}

inline quint32 paneOf(const McpCall& call)
{
    return quint32(call.arguments().value(L("pane")).toInteger());
}

inline bool hasCode(const DaemonLink::Error* error, const char* code)
{
    return error && error->code == L(code);
}

/// A pane that cannot answer for a moment (it decodes an image): the call
/// did nothing, and the same call can work a moment later.
inline bool tryAgain(const DaemonLink::Error* error)
{
    return hasCode(error, AfterglowMux::Error::TryAgain);
}

/// A failed call of the daemon, for the model: what happened, and what to do.
/// Only for a call that changed nothing, as the call again of TRY_AGAIN says.
inline QString failure(const DaemonLink::Error& error)
{
    if (error.code.isEmpty())
        return QStringLiteral("lost the mux daemon: %1. Restart the server.").arg(error.message);
    if (error.code == L(AfterglowMux::Error::TryAgain))
        return QStringLiteral("%1. The pane is busy for a moment: call the tool again.").arg(error.message);
    return error.message;
}

/// "1 pane", "2 panes".
inline QString panes(qsizetype count)
{
    return count == 1 ? QStringLiteral("1 pane") : QStringLiteral("%1 panes").arg(count);
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
        // The line end before the first line that stays, once @p lines
        // line ends were found before the end.
        qsizetype before = -1;
        int found = 0;
        while (found < lines && at > 0) {
            const qsizetype end = out.text.lastIndexOf(QLatin1Char('\n'), at - 1);
            if (end < 0)
                break;
            ++found;
            before = end;
            at = end;
        }
        if (found == lines && before >= 0) {
            out.text = out.text.mid(before + 1);
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

/// Removes the start of @p text down to its last @p keep UTF-16 units, never
/// half of a surrogate pair.
inline void keepEnd(QString& text, qsizetype keep)
{
    if (text.size() <= keep)
        return;
    qsizetype from = text.size() - keep;
    if (text.at(from).isLowSurrogate())
        ++from;
    text.remove(0, from);
}

/// The exit codes of the closed output regions in @p regions (shellRegions).
inline QJsonArray exitCodes(const QJsonArray& regions)
{
    QJsonArray codes;
    for (const QJsonValue& region : regions) {
        const QJsonObject object = region.toObject();
        if (object.value(L("kind")).toString() == L("output") && !object.value(L("open")).toBool())
            codes.append(object.value(L("exit_code")));
    }
    return codes;
}

/// Whether @p regions, the shell's regions since a command was sent, end it:
/// a prompt after the last output. A shell sends a prompt again each time it
/// redraws it, with no output before it: bash redraws its prompt with a
/// paste of several lines, before Enter. A command of one line, or none,
/// makes no such redraw, and a prompt with no output ends it (an empty
/// line, a comment).
inline bool commandEnded(const QJsonArray& regions, bool severalLines)
{
    bool output = false;
    bool prompt = false;
    for (const QJsonValue& region : regions) {
        const QString kind = region.toObject().value(L("kind")).toString();
        if (kind == L("output")) {
            output = true;
            prompt = false;
        } else if (kind == L("prompt")) {
            prompt = true;
        }
    }
    return prompt && (output || !severalLines);
}

/// Whether @p regions hold an output region.
inline bool hasOutput(const QJsonArray& regions)
{
    return std::any_of(regions.begin(), regions.end(), [](const QJsonValue& region) {
        return region.toObject().value(L("kind")).toString() == L("output");
    });
}

/// The id of the newest region of @p regions, 0 for none.
inline qint64 newestRegion(const QJsonArray& regions)
{
    qint64 newest = 0;
    for (const QJsonValue& region : regions)
        newest = std::max(newest, region.toObject().value(L("id")).toInteger());
    return newest;
}

/// Where a wait for a command that runs starts in @p regions, all the regions
/// of a pane: just before the newest output, which is the command's, also
/// when it ended since the shell said that it runs. Without an output, after
/// the newest region.
inline qint64 runningSince(const QJsonArray& regions)
{
    for (auto it = regions.end(); it != regions.begin();) {
        const QJsonObject region = (--it)->toObject();
        if (region.value(L("kind")).toString() == L("output"))
            return region.value(L("id")).toInteger() - 1;
    }
    return newestRegion(regions);
}

/// A mark of the daemon's mark call at the start of @p row: {row, col, epoch}.
inline QJsonObject rowMark(qint64 row, qint64 epoch)
{
    return {{L("row"), row}, {L("col"), 0}, {L("epoch"), epoch}};
}

/// The epoch of a mark of the daemon's mark call.
inline qint64 epochOf(const QJsonObject& mark)
{
    return mark.value(L("epoch")).toInteger();
}

/// The options of getText for a text that keeps its end when it is cut.
inline QJsonObject keptEnd()
{
    return {{L("keep_end"), true}};
}

/// The arguments of getText @p scope with the options @p options.
inline QJsonArray textArgs(quint32 pane, const QString& scope, const QJsonObject& options = {})
{
    QJsonArray args{qint64(pane), scope};
    if (!options.isEmpty())
        args.append(options);
    return args;
}

/// The value of the variable @p name in @p environment, the [name, value]
/// pairs of a launch of the daemon (toJson of AfterglowMux::Launch).
inline std::optional<QString> variableIn(const QJsonArray& environment, const QString& name)
{
    for (const QJsonValue& pair : environment) {
        const QJsonArray entry = pair.toArray();
        if (entry.size() == 2 && entry.at(0).toString() == name)
            return entry.at(1).toString();
    }
    return std::nullopt;
}

/// Whether a pane whose launch sets AFTERGLOW_SHELL_FEATURES to @p features
/// marks its prompts: shell integration prepared its shell
/// (ShellIntegration::prepare sets it for each shell that it prepares), and
/// no-prompt-mark is not on.
inline bool marksPrompts(const std::optional<QString>& features)
{
    return features && !features->split(QLatin1Char(' '), Qt::SkipEmptyParts).contains(L("no-prompt-mark"));
}

/// The exit status of a script command, as aglow run gives it: 0 to 255, and
/// 1 for a status that is not one.
inline qint64 exitStatus(const QJsonValue& exit)
{
    const qint64 code = exit.toInteger(-1);
    return code >= 0 && code <= 255 ? code : 1;
}

/// A name for a new session from the name of @p directory and @p random: mcp-,
/// the base name with each character that a session name cannot have as _,
/// and the random part, within AfterglowMux::MaxSessionName.
inline QString sessionName(const QString& directory, const QString& random)
{
    QString base = QFileInfo(directory).fileName();
    for (QChar& c : base) {
        const char16_t u = c.unicode();
        const bool kept =
            (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9') || u == u'_' || u == u'-';
        if (!kept)
            c = QLatin1Char('_');
    }
    const QString prefix = QStringLiteral("mcp-");
    const qsizetype room = qsizetype(AfterglowMux::MaxSessionName) - prefix.size() - 1 - random.size();
    base.truncate(std::max<qsizetype>(room, 0));
    return base.isEmpty() ? prefix + random : prefix + base + QLatin1Char('-') + random;
}

/// One step of an open of a project: a pane in a new tab, or a split of a
/// pane of the project that opened before it.
struct ProjectStep
{
    qsizetype leaf = 0; ///< the pane that opens, in the order of ProjectStore::panes
    qsizetype target = -1; ///< the pane that it splits, -1 for a new tab
    QString direction;
    bool operator==(const ProjectStep&) const = default;
};

/// The panes of the split tree @p node of a project.
inline qsizetype leaves(const QJsonObject& node)
{
    if (node.contains(L("pane")))
        return 1;
    return leaves(node.value(L("first")).toObject()) + leaves(node.value(L("second")).toObject());
}

/// The splits that make the tree @p node, whose first pane, @p holder,
/// holds all its room already: each split splits the pane that holds its
/// room, for the first pane of its second side, and then each side is made
/// in its own room. @p next is the leaf of the tree's first pane. The daemon
/// splits a pane in halves, as a window that splits it: the ratios of the
/// project stay out.
inline void planSplits(const QJsonObject& node, qsizetype holder, qsizetype& next, QList<ProjectStep>& steps)
{
    if (node.contains(L("pane"))) {
        ++next;
        return;
    }
    const QJsonObject first = node.value(L("first")).toObject();
    const qsizetype second = next + leaves(first);
    steps.append({second, holder, node.value(L("direction")).toString()});
    planSplits(first, holder, next, steps);
    planSplits(node.value(L("second")).toObject(), second, next, steps);
}

/// The split tree of a project from @p tree, a tab of the daemon's layout in
/// preorder, from the node at @p at, with @p leaf for each pane.
inline QJsonObject nested(const std::vector<AfterglowMux::LayoutNode>& tree, std::size_t& at,
                          const std::function<QJsonObject(std::uint32_t)>& leaf)
{
    const AfterglowMux::LayoutNode& node = tree.at(at++);
    if (node.pane != 0)
        return {{L("pane"), leaf(node.pane)}};
    const QJsonObject first = nested(tree, at, leaf);
    const QJsonObject second = nested(tree, at, leaf);
    return {{L("direction"), node.down ? L("down") : L("right")},
            {L("ratio"), node.ratio},
            {L("first"), first},
            {L("second"), second}};
}

/// The version that serverInfo gives: @p base, and the first 8 hex digits of
/// a hash of what a client caches, the tools and the instructions. A client
/// that keys its cache on the server's name and version (the TypeScript SDK)
/// reads them again when they change, also when the release stays the same.
inline QString serverVersion(const QString& base, const QList<PhosphorMcp::McpTool>& tools, const QString& instructions)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // Compact JSON sorts the keys of each object: the same tools give the
    // same bytes.
    for (const PhosphorMcp::McpTool& tool : tools)
        hash.addData(QJsonDocument(tool.toJson()).toJson(QJsonDocument::Compact));
    hash.addData(instructions.toUtf8());
    return base + QLatin1Char('+') + QString::fromLatin1(hash.result().toHex().left(8));
}

} // namespace Afterglow::McpParts
