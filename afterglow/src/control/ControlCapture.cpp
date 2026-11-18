// SPDX-FileCopyrightText: 2026 fuddlesworth
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

namespace Afterglow {

namespace {

// ScriptHost checks the effect's max_bytes first, in the same words.
QString maxBytesRefusal(int limit)
{
    if (limit >= 1 && limit <= ScriptHost::MaxReadTextBytes) return {};
    return ScriptHost::maxBytesError();
}

// The text a script command reads can reach the client of its run: then
// Control.allowReadText, which guards text for clients, applies too.
QString scriptReadRefusal(const Settings& settings, bool clientRun)
{
    if (!settings.scriptsAllowReadText) return QStringLiteral("reading pane text is off (Scripts.allowReadText)");
    if (clientRun && !settings.control.allowReadText)
        return QStringLiteral("reading pane text for a command run with a client is off (Control.allowReadText)");
    return {};
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
            this, [done = std::move(done), base64 = std::move(base64)] { done(base64); }, Qt::QueuedConnection);
    });
}

QString Control::screenshot(quint32 id, CaptureScope scope, ScreenshotReply reply, QString& code)
{
    // The permission goes first: it holds for every pane.
    if (QString refused = captureRefusal(code); !refused.isEmpty()) return refused;
    if (m_screenshots >= MaxScreenshots) {
        code = QStringLiteral("busy");
        return QStringLiteral("%1 screenshots are in progress").arg(MaxScreenshots);
    }
    ++m_screenshots;
    const QPointer<Control> self(this);
    const QString error = capture(
        id, scope,
        [self, reply](const QString& error, const QString& code, const QImage& image) {
            if (!self) return reply(QStringLiteral("Afterglow quits"), QStringLiteral("failed"), {});
            if (!error.isEmpty()) {
                --self->m_screenshots;
                return reply(error, code, {});
            }
            // A 4K PNG takes a moment to encode: not on the thread that draws
            // the windows. encodePng answers only while this object lives.
            Control* control = self.data();
            control->encodePng(image, [control, reply, size = image.size()](const QString& base64) {
                --control->m_screenshots;
                // Checked again as the picture goes out: read access, or
                // control itself, can go off while the frame is drawn and
                // encoded.
                QString code;
                if (const QString refused = control->captureRefusal(code); !refused.isEmpty()) return reply(refused, code, {});
                if (!control->m_settings.settings().control.enabled) {
                    return reply(QStringLiteral("Control.enabled is off"), QStringLiteral("cancelled"), {});
                }
                if (base64.isEmpty()) return reply(QStringLiteral("the PNG could not be written"), QStringLiteral("failed"), {});
                reply({}, {},
                      QJsonObject{{QStringLiteral("png"), base64}, {QStringLiteral("width"), size.width()},
                                  {QStringLiteral("height"), size.height()}});
            });
        },
        code);
    if (!error.isEmpty()) --m_screenshots;
    return error;
}

QString Control::readText(quint32 id, TextScope scope, QString& out, const AfterglowVt::TextFormat& format)
{
    if (!m_settings.settings().control.allowReadText) {
        return QStringLiteral("reading pane text is off (Control.allowReadText)");
    }
    // Said, not dropped: the selection is plain text, and the text of a
    // command's output ends before the cursor's place.
    if (scope == TextScope::Selection && (format.ansi || format.cursor || format.wrapMarkers))
        return QStringLiteral("the selection is plain text: ansi, cursor and wrap_markers are for the other scopes");
    if ((scope == TextScope::FirstOutputOnScreen || scope == TextScope::LastVisitedOutput) && format.cursor)
        return QLatin1String(AfterglowMux::CursorScopeError);
    Pane* p = find(id);
    if (!p) {
        return noPane(id);
    }
    using AfterglowVt::Terminal;
    bool arriving = false;
    switch (scope) {
    case TextScope::Screen:
        out = p->text(Terminal::TextScope::Screen, format);
        break;
    case TextScope::Scrollback:
    case TextScope::LastOutput:
        // A session pane's rows that are still on their way are blank lines
        // in the text: it is not the program's yet.
        out = p->text(scope == TextScope::Scrollback ? Terminal::TextScope::All : Terminal::TextScope::LastOutput, format, &arriving);
        if (arriving) {
            out.clear();
            return QStringLiteral("the rows of pane %1 are still arriving from its session: try again").arg(id);
        }
        break;
    case TextScope::FirstOutputOnScreen:
    case TextScope::LastVisitedOutput:
        // Empty when there is none, as the last output of a pane that ran nothing.
        out = p->readOutputRegion(scope == TextScope::FirstOutputOnScreen ? Pane::OutputRegion::FirstOnScreen : Pane::OutputRegion::LastVisited,
                                  Pane::MaxText, format)
                  .value(QStringLiteral("text"))
                  .toString();
        break;
    case TextScope::Selection:
        // Bounded as the other scopes: a selection can be megabytes.
        out = p->readSelection(Pane::MaxText).value(QStringLiteral("text")).toString();
        break;
    }
    return {};
}

QString Control::readTextBounded(quint32 id, const QString& scope, int limit, QVariantMap& result, bool clientRun)
{
    if (const QString refusal = scriptReadRefusal(m_settings.settings(), clientRun); !refusal.isEmpty()) return refusal;
    if (const QString refusal = maxBytesRefusal(limit); !refusal.isEmpty()) return refusal;
    auto* pane = find(id);
    if (!pane) return noPane(id);
    if (scope == QLatin1String("selection")) {
        result = pane->readSelection(std::size_t(limit));
        return {};
    }
    // A command's output region, as kitty's first_cmd_output_on_screen and
    // last_visited_cmd_output: unavailable when there is none.
    if (scope == QLatin1String("first-output-on-screen") || scope == QLatin1String("last-visited-output")) {
        result = pane->readOutputRegion(scope == QLatin1String("first-output-on-screen") ? Pane::OutputRegion::FirstOnScreen
                                                                                        : Pane::OutputRegion::LastVisited,
                                        std::size_t(limit));
        return {};
    }
    using Scope = AfterglowVt::Terminal::TextScope;
    Scope kind;
    if (scope == QLatin1String("screen")) kind = Scope::Screen;
    else if (scope == QLatin1String("scrollback")) kind = Scope::All;
    else if (scope == QLatin1String("last-output")) kind = Scope::LastOutput;
    else return BadTextScope;
    result = pane->readText(kind, std::size_t(limit));
    return {};
}

QString Control::semanticRegions(quint32 id, QVariantMap& result, bool clientRun)
{
    if (const QString refusal = scriptReadRefusal(m_settings.settings(), clientRun); !refusal.isEmpty()) return refusal;
    auto* pane = find(id);
    if (!pane) return noPane(id);
    result = pane->semanticRegions();
    return {};
}

QString Control::readRegion(quint32 id, const QString& region, int limit, QVariantMap& result, bool clientRun)
{
    if (const QString refusal = scriptReadRefusal(m_settings.settings(), clientRun); !refusal.isEmpty()) return refusal;
    if (const QString refusal = maxBytesRefusal(limit); !refusal.isEmpty()) return refusal;
    bool ok = false;
    const auto regionId = region.toULongLong(&ok);
    if (!ok || !regionId || QString::number(regionId) != region) return QStringLiteral("invalid region ID");
    auto* pane = find(id);
    if (!pane) return noPane(id);
    result = pane->readRegion(regionId, std::size_t(limit));
    return {};
}

// The daemon's words: a session's pane says the same.
const QString Control::TextScopeNames = QLatin1String(AfterglowMux::TextScopeNames);
const QString Control::BadTextScope = QStringLiteral("scope is ") + TextScopeNames;

QString Control::textFormat(const QVariantMap& options, AfterglowVt::TextFormat& format)
{
    format = {};
    for (auto it = options.constBegin(); it != options.constEnd(); ++it) {
        bool* flag = it.key() == QLatin1String("ansi") ? &format.ansi
            : it.key() == QLatin1String("cursor")      ? &format.cursor
            : it.key() == QLatin1String("wrap_markers") ? &format.wrapMarkers
                                                         : nullptr;
        if (!flag) return QStringLiteral("unknown getText option %1: the options are ansi, cursor and wrap_markers").arg(it.key());
        if (it.value().typeId() != QMetaType::Bool) return QStringLiteral("the getText option %1 is true or false").arg(it.key());
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

QString Control::capture(quint32 id, CaptureScope scope, CaptureReply reply, QString& code)
{
    if (QString refused = captureRefusal(code); !refused.isEmpty()) {
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
    if (name == QLatin1String("pane")) return CaptureScope::Pane;
    if (name == QLatin1String("tab")) return CaptureScope::Tab;
    if (name == QLatin1String("window")) return CaptureScope::Window;
    return std::nullopt;
}

std::optional<Control::TextScope> Control::scopeFromName(const QString& name)
{
    for (TextScope s : {TextScope::Screen, TextScope::Scrollback, TextScope::Selection, TextScope::LastOutput, TextScope::FirstOutputOnScreen,
                        TextScope::LastVisitedOutput}) {
        if (scopeName(s) == name) {
            return s;
        }
    }
    return std::nullopt;
}

} // namespace Afterglow
