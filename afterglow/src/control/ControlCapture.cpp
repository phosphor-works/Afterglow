// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

// Control: the text of panes, and their pictures.

#include "Control.h"

#include "../Pane.h"
#include "../TerminalWindow.h"
#include "../config/Settings.h"
#include "../scripting/ScriptHost.h"
#include "MuxProtocol.h"

#include <QBuffer>
#include <QImage>

#include <limits>

namespace Afterglow {

namespace {

// ScriptHost checks the effect's max_bytes first, in the same words.
QString maxBytesRefusal(int limit)
{
    if (limit >= 1 && limit <= ScriptHost::MaxReadTextBytes)
        return {};
    return ScriptHost::maxBytesError();
}

// The text a script command reads can reach the client of its run: then
// Control.allowReadText, which guards text for clients, applies too.
QString scriptReadRefusal(const Settings& settings, bool clientRun)
{
    if (!settings.scriptsAllowReadText)
        return QStringLiteral("reading pane text is off (Scripts.allowReadText)");
    if (clientRun && !settings.control.allowReadText)
        return QStringLiteral("reading pane text for a command run with a client is off (Control.allowReadText)");
    return {};
}

// A client reads a text of at most Pane::MaxText bytes: the result of a
// tool of afterglow mcp holds no more (Mcp.outputLimit).
static_assert(McpSettings::MaxOutputLimit == int(Pane::MaxText));

/// A whole number that a double holds exactly, from 0 to @p most.
bool wholeNumber(const QVariant& v, double most)
{
    const bool number = v.typeId() == QMetaType::Double || v.typeId() == QMetaType::Int
        || v.typeId() == QMetaType::LongLong || v.typeId() == QMetaType::UInt || v.typeId() == QMetaType::ULongLong;
    return number && Control::isWhole(v.toDouble()) && v.toDouble() >= 0 && v.toDouble() <= most;
}

} // namespace

void Control::encodePng(QImage image, std::function<void(const QString& base64)> done)
{
    // As offThread, on the pool of the encodes. The answer is posted to this
    // object, and m_encoder waits for the work before it goes.
    m_encoder.start([this, image = std::move(image), done = std::move(done)]() mutable {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        QString base64 = image.save(&buffer, "PNG") ? QString::fromLatin1(png.toBase64()) : QString();
        QMetaObject::invokeMethod(
            this,
            [done = std::move(done), base64 = std::move(base64)] {
                done(base64);
            },
            Qt::QueuedConnection);
    });
}

QString Control::screenshot(quint32 id, CaptureScope scope, ScreenshotReply reply, QString& code, bool agentSession)
{
    // The permission goes first: it holds for every pane.
    if (QString refused = agentSession ? QString() : captureRefusal(code); !refused.isEmpty())
        return refused;
    if (m_screenshots >= MaxScreenshots) {
        code = QStringLiteral("busy");
        return QStringLiteral("%1 screenshots are in progress").arg(MaxScreenshots);
    }
    ++m_screenshots;
    const QPointer<Control> self(this);
    const QString error = capture(
        id, scope,
        [self, reply, agentSession](const QString& error, const QString& code, const QImage& image) {
            if (!self)
                return reply(QStringLiteral("Afterglow quits"), QStringLiteral("failed"), {});
            if (!error.isEmpty()) {
                --self->m_screenshots;
                return reply(error, code, {});
            }
            // A 4K PNG takes a moment to encode: not on the thread that draws
            // the windows. encodePng answers only while this object lives.
            Control* control = self.data();
            control->encodePng(image, [control, reply, agentSession, size = image.size()](const QString& base64) {
                --control->m_screenshots;
                // Checked again as the picture goes out: read access, or
                // control itself, can go off while the frame is drawn and
                // encoded.
                QString code;
                if (const QString refused = agentSession ? QString() : control->captureRefusal(code);
                    !refused.isEmpty())
                    return reply(refused, code, {});
                if (!control->m_settings.settings().control.enabled) {
                    return reply(QStringLiteral("Control.enabled is off"), QStringLiteral("cancelled"), {});
                }
                if (base64.isEmpty())
                    return reply(QStringLiteral("the PNG could not be written"), QStringLiteral("failed"), {});
                reply({}, {},
                      QJsonObject{{QStringLiteral("png"), base64},
                                  {QStringLiteral("width"), size.width()},
                                  {QStringLiteral("height"), size.height()}});
            });
        },
        code, agentSession);
    if (!error.isEmpty())
        --m_screenshots;
    return error;
}

QString Control::readText(quint32 id, TextScope scope, QString& out, const AfterglowVt::TextFormat& format,
                          bool* truncated, QString* code, bool agentSession)
{
    bool cut = false;
    if (!truncated)
        truncated = &cut;
    *truncated = false;
    if (!agentSession && !m_settings.settings().control.allowReadText) {
        return QStringLiteral("reading pane text is off (Control.allowReadText)");
    }
    // Said, not dropped: the selection is plain text that is cut at its
    // end, and the text of a command's output ends before the cursor's
    // place.
    if (scope == TextScope::Selection && (format.ansi || format.cursor || format.wrapMarkers || format.keepEnd))
        return QStringLiteral(
            "the selection is plain text: ansi, cursor, wrap_markers and keep_end are for the other scopes");
    if ((scope == TextScope::FirstOutputOnScreen || scope == TextScope::LastVisitedOutput) && format.cursor)
        return QLatin1String(AfterglowMux::CursorScopeError);
    Pane* p = find(id);
    if (!p) {
        return noPane(id);
    }
    // The check of the first frame and the read: one refresh.
    const Pane::RefreshOnce once(*p);
    if (QString error; notSynced(*p, error, code))
        return error;
    // The end of a command's output holds its result: a cut keeps it, for
    // every reader (getText, stdin_text of a launch, a session's client).
    AfterglowVt::TextFormat read = format;
    read.keepEnd = format.keepEnd || scope == TextScope::LastOutput;
    using AfterglowVt::Terminal;
    bool arriving = false;
    switch (scope) {
    case TextScope::Screen:
        out = p->text(Terminal::TextScope::Screen, read, nullptr, truncated);
        break;
    case TextScope::Scrollback:
    case TextScope::LastOutput:
        // A session pane's rows that are still on their way are blank lines
        // in the text: it is not the program's yet.
        out = p->text(scope == TextScope::Scrollback ? Terminal::TextScope::All : Terminal::TextScope::LastOutput, read,
                      &arriving, truncated);
        if (arriving) {
            out.clear();
            if (code)
                *code = QLatin1String(AfterglowMux::Error::TryAgain);
            return arrivingError(id);
        }
        break;
    case TextScope::FirstOutputOnScreen:
    case TextScope::LastVisitedOutput: {
        // Empty when there is none, as the last output of a pane that ran
        // nothing. Not when the rows of the region are on their way: an
        // empty text would say that there is none.
        const auto which = scope == TextScope::FirstOutputOnScreen ? Pane::OutputRegion::FirstOnScreen
                                                                   : Pane::OutputRegion::LastVisited;
        const QVariantMap region = p->readOutputRegion(which, Pane::MaxText, read, &arriving);
        if (arriving) {
            out.clear();
            if (code)
                *code = QLatin1String(AfterglowMux::Error::TryAgain);
            return arrivingError(id);
        }
        out = region.value(QStringLiteral("text")).toString();
        *truncated = region.value(QStringLiteral("truncated")).toBool();
        break;
    }
    case TextScope::Selection: {
        // Bounded as the other scopes: a selection can be megabytes.
        const QVariantMap read = p->readSelection(Pane::MaxText);
        out = read.value(QStringLiteral("text")).toString();
        *truncated = read.value(QStringLiteral("truncated")).toBool();
        break;
    }
    }
    return {};
}

QString Control::arrivingError(quint32 id)
{
    return QStringLiteral("the rows of pane %1 are still arriving from its session: try again").arg(id);
}

bool Control::notSynced(const Pane& pane, QString& error, QString* code)
{
    if (pane.sessionSynced())
        return false;
    // A pane that nobody saw yet may get its first frame only with the
    // answer to a refresh, which this waits for.
    pane.refreshSession();
    if (pane.sessionSynced())
        return false;
    error = arrivingError(pane.id());
    if (code)
        *code = QLatin1String(AfterglowMux::Error::TryAgain);
    return true;
}

QString Control::readTextSince(quint32 id, const AfterglowVt::TextMark& mark, const AfterglowVt::TextFormat& format,
                               QString& out, bool& truncated, bool& trimmed, QString& code)
{
    truncated = trimmed = false;
    if (!m_settings.settings().control.allowReadText)
        return QStringLiteral("reading pane text is off (Control.allowReadText)");
    if (format.cursor)
        return QLatin1String(AfterglowMux::CursorScopeError);
    Pane* p = find(id);
    if (!p)
        return noPane(id);
    const Pane::RefreshOnce once(*p);
    if (QString error; notSynced(*p, error, &code))
        return error;
    // What came since a mark ends with its result: a cut keeps it.
    AfterglowVt::TextFormat read = format;
    read.keepEnd = true;
    bool arriving = false;
    bool stale = false;
    out = p->textSince(mark, read, &truncated, &trimmed, &stale, &arriving);
    // The rows of the mark hold other text now: what came since it cannot
    // be told apart.
    if (stale) {
        out.clear();
        truncated = trimmed = false;
        code = QLatin1String(AfterglowMux::Error::StaleMark);
        return QLatin1String(AfterglowMux::StaleMarkMessage);
    }
    if (arriving) {
        out.clear();
        truncated = trimmed = false;
        code = QLatin1String(AfterglowMux::Error::TryAgain);
        return arrivingError(id);
    }
    return {};
}

QString Control::markOfPane(quint32 id, QVariantMap& mark, QString& code)
{
    Pane* p = find(id);
    if (!p)
        return noPane(id);
    // A blank replica's cursor and epoch are none of the program's: a mark
    // there would be stale, or take output that came before it.
    const Pane::RefreshOnce once(*p);
    if (QString error; notSynced(*p, error, &code))
        return error;
    const AfterglowVt::TextMark position = p->textMark();
    mark = {{QStringLiteral("row"), double(position.row)},
            {QStringLiteral("col"), position.col},
            {QStringLiteral("epoch"), double(position.epoch)}};
    return {};
}

const QString Control::BadRegionAfter = QStringLiteral("shellRegions takes (pane[, after])");

QString Control::shellRegions(quint32 id, quint64 after, QVariantList& regions, QString& code)
{
    Pane* p = find(id);
    if (!p)
        return noPane(id);
    const Pane::RefreshOnce once(*p);
    if (QString error; notSynced(*p, error, &code))
        return error;
    regions.clear();
    for (const AfterglowVt::SemanticRegion& region : p->semanticRegionsAfter(after)) {
        regions.append(QVariantMap{
            {QStringLiteral("id"), double(region.id)},
            {QStringLiteral("kind"), QString::fromLatin1(AfterglowMux::ShellRegionKinds[std::size_t(region.kind)])},
            {QStringLiteral("open"), region.open},
            {QStringLiteral("exit_code"), region.exitCode ? QVariant(*region.exitCode) : QVariant::fromValue(nullptr)},
            {QStringLiteral("start_trimmed"), region.startTrimmed},
            {QStringLiteral("available"), region.available},
        });
    }
    return {};
}

QString Control::readTextBounded(quint32 id, const QString& scope, int limit, QVariantMap& result, bool clientRun)
{
    if (const QString refusal = scriptReadRefusal(m_settings.settings(), clientRun); !refusal.isEmpty())
        return refusal;
    if (const QString refusal = maxBytesRefusal(limit); !refusal.isEmpty())
        return refusal;
    auto* pane = find(id);
    if (!pane)
        return noPane(id);
    if (scope == QLatin1String("selection")) {
        result = pane->readSelection(std::size_t(limit));
        return {};
    }
    // A command's output region, as kitty's first_cmd_output_on_screen and
    // last_visited_cmd_output: unavailable when there is none.
    if (scope == QLatin1String("first-output-on-screen") || scope == QLatin1String("last-visited-output")) {
        result =
            pane->readOutputRegion(scope == QLatin1String("first-output-on-screen") ? Pane::OutputRegion::FirstOnScreen
                                                                                    : Pane::OutputRegion::LastVisited,
                                   std::size_t(limit));
        return {};
    }
    using Scope = AfterglowVt::Terminal::TextScope;
    Scope kind;
    if (scope == QLatin1String("screen"))
        kind = Scope::Screen;
    else if (scope == QLatin1String("scrollback"))
        kind = Scope::All;
    else if (scope == QLatin1String("last-output"))
        kind = Scope::LastOutput;
    else
        return BadTextScope;
    result = pane->readText(kind, std::size_t(limit));
    return {};
}

QString Control::semanticRegions(quint32 id, QVariantMap& result, bool clientRun)
{
    if (const QString refusal = scriptReadRefusal(m_settings.settings(), clientRun); !refusal.isEmpty())
        return refusal;
    auto* pane = find(id);
    if (!pane)
        return noPane(id);
    result = pane->semanticRegions();
    return {};
}

QString Control::readRegion(quint32 id, const QString& region, int limit, QVariantMap& result, bool clientRun)
{
    if (const QString refusal = scriptReadRefusal(m_settings.settings(), clientRun); !refusal.isEmpty())
        return refusal;
    if (const QString refusal = maxBytesRefusal(limit); !refusal.isEmpty())
        return refusal;
    bool ok = false;
    const auto regionId = region.toULongLong(&ok);
    if (!ok || !regionId || QString::number(regionId) != region)
        return QStringLiteral("invalid region ID");
    auto* pane = find(id);
    if (!pane)
        return noPane(id);
    result = pane->readRegion(regionId, std::size_t(limit));
    return {};
}

// The daemon's words: a session's pane says the same.
const QString Control::TextScopeNames = QLatin1String(AfterglowMux::TextScopeNames);
const QString Control::BadTextScope = QStringLiteral("scope is ") + TextScopeNames;

QString Control::textFormat(const QVariantMap& options, AfterglowVt::TextFormat& format,
                            std::optional<AfterglowVt::TextMark>* mark)
{
    format = {};
    for (auto it = options.constBegin(); it != options.constEnd(); ++it) {
        if (it.key() == QLatin1String("mark") && mark) {
            // As the daemon reads it (Json::Value::toInteger): {row, col,
            // epoch}, whole numbers below 2^53, and no other key.
            const QVariantMap value = it.value().toMap();
            const QVariant row = value.value(QStringLiteral("row"));
            const QVariant col = value.value(QStringLiteral("col"));
            const QVariant epoch = value.value(QStringLiteral("epoch"));
            constexpr double MostExact = double((1LL << 53) - 1);
            if (it.value().typeId() != QMetaType::QVariantMap || value.size() != 3 || !wholeNumber(row, MostExact)
                || !wholeNumber(col, double(std::numeric_limits<int>::max())) || !wholeNumber(epoch, MostExact))
                return QLatin1String(AfterglowMux::MarkOptionError);
            *mark = AfterglowVt::TextMark{std::uint64_t(row.toDouble()), int(col.toDouble()),
                                          std::uint64_t(epoch.toDouble())};
            continue;
        }
        bool* flag = it.key() == QLatin1String("ansi")  ? &format.ansi
            : it.key() == QLatin1String("cursor")       ? &format.cursor
            : it.key() == QLatin1String("wrap_markers") ? &format.wrapMarkers
            : it.key() == QLatin1String("keep_end")     ? &format.keepEnd
                                                        : nullptr;
        // A caller that takes no mark (a call that the daemon passes on,
        // whose since scope the daemon answers) names the options it takes.
        if (!flag)
            return QStringLiteral("unknown getText option %1: %2")
                .arg(it.key(),
                     mark ? QLatin1String(AfterglowMux::TextOptionNames)
                          : QLatin1String("the options are ansi, cursor, wrap_markers and keep_end"));
        if (it.value().typeId() != QMetaType::Bool)
            return QStringLiteral("the getText option %1 is true or false").arg(it.key());
        *flag = it.value().toBool();
    }
    return {};
}

QString Control::scopeName(TextScope scope)
{
    switch (scope) {
    case TextScope::Screen:
        return QStringLiteral("screen");
    case TextScope::Scrollback:
        return QStringLiteral("scrollback");
    case TextScope::Selection:
        return QStringLiteral("selection");
    case TextScope::LastOutput:
        return QStringLiteral("last-output");
    case TextScope::FirstOutputOnScreen:
        return QStringLiteral("first-output-on-screen");
    case TextScope::LastVisitedOutput:
        return QStringLiteral("last-visited-output");
    }
    return {};
}

QString Control::captureRefusal(QString& code) const
{
    if (!m_settings.settings().control.allowReadText) {
        code = QStringLiteral("denied");
        return QStringLiteral("screenshots are off (Control.allowReadText)");
    }
    return {};
}

const QString Control::NoRoomForPane = QStringLiteral("not enough room for another pane");

QString Control::capture(quint32 id, CaptureScope scope, CaptureReply reply, QString& code, bool agentSession)
{
    if (QString refused = agentSession ? QString() : captureRefusal(code); !refused.isEmpty()) {
        return refused;
    }
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    if (!pane) {
        code = QStringLiteral("no_pane");
        return noPane(id);
    }
    window->captureFrame(pane, scope, std::move(reply));
    return {};
}

std::optional<CaptureScope> Control::captureScopeFromName(const QString& name)
{
    if (name == QLatin1String("pane"))
        return CaptureScope::Pane;
    if (name == QLatin1String("tab"))
        return CaptureScope::Tab;
    if (name == QLatin1String("window"))
        return CaptureScope::Window;
    return std::nullopt;
}

std::optional<Control::TextScope> Control::scopeFromName(const QString& name)
{
    for (TextScope s : {TextScope::Screen, TextScope::Scrollback, TextScope::Selection, TextScope::LastOutput,
                        TextScope::FirstOutputOnScreen, TextScope::LastVisitedOutput}) {
        if (scopeName(s) == name) {
            return s;
        }
    }
    return std::nullopt;
}

} // namespace Afterglow
