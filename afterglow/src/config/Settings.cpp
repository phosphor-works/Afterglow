// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Settings.h"

#include "MuxProtocol.h"
#include "Theme.h"
#include "animation/MomentumScroll.h"

#include <AfterglowRender/Snapshot.h>
#include <AfterglowVt/TextSearch.h>

#include <PhosphorAnimation/CurveRegistry.h>
#include <PhosphorAudio/AudioDefaults.h>
#include <PhosphorConfig/JsonBackend.h>
#include <PhosphorConfig/Schema.h>
#include <PhosphorConfig/Store.h>

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStyleHints>

#include <atomic>
#include <cmath>
#include <limits>

Q_LOGGING_CATEGORY(lcConfig, "afterglow.config", QtInfoMsg)

namespace Afterglow {

namespace {

using PhosphorConfig::KeyDef;

const QString Font = QStringLiteral("Font");
const QString Colors = QStringLiteral("Colors");
const QString Terminal = QStringLiteral("Terminal");
const QString Window = QStringLiteral("Window");
const QString Keys = QStringLiteral("Keybinds");
const QString Shaders = QStringLiteral("Shaders");
const QString Animation = QStringLiteral("Animation");
const QString Quick = QStringLiteral("QuickTerminal");
const QString ControlGroup = QStringLiteral("Control");
const QString MuxGroup = QStringLiteral("Mux");
const QString AudioGroup = QStringLiteral("Audio");
const QString ScriptsGroup = QStringLiteral("Scripts");
const QString MouseGroup = QStringLiteral("Mouse");
const QString LinksGroup = QStringLiteral("Links");
const QString GraphicsGroup = QStringLiteral("Graphics");
const QString NotificationsGroup = QStringLiteral("Notifications");
const QString HintsGroup = QStringLiteral("Hints");

constexpr qsizetype PaletteSize = std::tuple_size_v<decltype(AfterglowVt::TerminalOptions::basePalette)>;
/// Each run that waits holds one of the 16 waiting handlers of its script
/// (MaxWaiting in afterglow.luau). A 17th run that waits gets a busy error.
constexpr int MaxCommandRuns = 16;

/// One spelling of an enum setting. Each enum has one table, which gives
/// both the schema's choices and what read() turns them into.
template<typename E>
struct Choice
{
    const char* name;
    E value;
};

using AfterglowRender::Antialiasing;
using AfterglowRender::Hinting;

const Choice<MuxSettings::Close> MuxCloseChoices[] = {{"kill", MuxSettings::Close::Kill}, {"detach", MuxSettings::Close::Detach}};
const Choice<Hinting> HintingChoices[] = {{"none", Hinting::None}, {"light", Hinting::Light}, {"full", Hinting::Full}};
const Choice<Antialiasing> AntialiasingChoices[] = {{"grayscale", Antialiasing::Grayscale},
                                                    {"none", Antialiasing::None},
                                                    {"rgb", Antialiasing::Rgb},
                                                    {"bgr", Antialiasing::Bgr}};
const Choice<AfterglowVt::BidiMode> BidiChoices[] = {{"off", AfterglowVt::BidiMode::Off},
                                                     {"auto", AfterglowVt::BidiMode::Auto},
                                                     {"ltr", AfterglowVt::BidiMode::LeftToRight},
                                                     {"rtl", AfterglowVt::BidiMode::RightToLeft}};
const Choice<WindowSettings::PopupCorners> PopupCornerChoices[] = {{"rounded", WindowSettings::PopupCorners::Rounded},
                                                                 {"square", WindowSettings::PopupCorners::Square}};
const Choice<AfterglowVt::CursorShape> CursorShapeChoices[] = {{"block", AfterglowVt::CursorShape::Block},
                                                               {"underline", AfterglowVt::CursorShape::Underline},
                                                               {"bar", AfterglowVt::CursorShape::Bar}};
const Choice<Settings::CopyOnSelect> CopyOnSelectChoices[] = {{"primary", Settings::CopyOnSelect::Primary},
                                                              {"clipboard", Settings::CopyOnSelect::Clipboard},
                                                              {"off", Settings::CopyOnSelect::Off}};
const Choice<Settings::Bell> BellChoices[] = {{"none", Settings::Bell::None},
                                              {"attention", Settings::Bell::Attention},
                                              {"visual", Settings::Bell::Visual},
                                              {"both", Settings::Bell::Both}};
const Choice<Settings::Permission> PermissionChoices[] = {{"allow", Settings::Permission::Allow},
                                                              {"ask", Settings::Permission::Ask},
                                                              {"deny", Settings::Permission::Deny}};
const Choice<Settings::Permission> GraphicsTransferChoices[] = {{"allow", Settings::Permission::Allow},
                                                                {"deny", Settings::Permission::Deny}};
const Choice<WindowSettings::StartMode> StartModeChoices[] = {{"normal", WindowSettings::StartMode::Normal},
                                                              {"maximized", WindowSettings::StartMode::Maximized},
                                                              {"fullscreen", WindowSettings::StartMode::Fullscreen}};
const Choice<WindowSettings::TabBar> TabBarChoices[] = {{"auto", WindowSettings::TabBar::Auto},
                                                        {"always", WindowSettings::TabBar::Always},
                                                        {"never", WindowSettings::TabBar::Never}};
const Choice<WindowSettings::Scrollbar> ScrollbarChoices[] = {{"auto", WindowSettings::Scrollbar::Auto},
                                                              {"always", WindowSettings::Scrollbar::Always},
                                                              {"never", WindowSettings::Scrollbar::Never}};
const Choice<MouseSettings::RightClick> RightClickChoices[] = {{"menu", MouseSettings::RightClick::Menu},
                                                               {"paste", MouseSettings::RightClick::Paste},
                                                               {"copy-or-paste", MouseSettings::RightClick::CopyOrPaste},
                                                               {"extend", MouseSettings::RightClick::Extend}};
const Choice<MouseSettings::MiddleClick> MiddleClickChoices[] = {{"primary", MouseSettings::MiddleClick::Primary},
                                                                 {"clipboard", MouseSettings::MiddleClick::Clipboard},
                                                                 {"none", MouseSettings::MiddleClick::None}};
const Choice<Qt::KeyboardModifiers> ModifierChoices[] = {{"ctrl", Qt::ControlModifier},
                                                         {"shift", Qt::ShiftModifier},
                                                         {"alt", Qt::AltModifier},
                                                         {"none", Qt::NoModifier}};
const Choice<QuickTerminalSettings::Position> QuickPositionChoices[] = {{"top", QuickTerminalSettings::Position::Top},
                                                                        {"bottom", QuickTerminalSettings::Position::Bottom}};

/// "U+E000-U+F8FF,U+2500" as inclusive ranges; nullopt if any part is malformed.
std::optional<std::vector<std::pair<char32_t, char32_t>>> parseCodepointRanges(const QString& spec)
{
    std::vector<std::pair<char32_t, char32_t>> out;
    auto codepoint = [](QString s) -> std::optional<char32_t> {
        s = s.trimmed();
        if (s.startsWith(QLatin1String("U+"), Qt::CaseInsensitive)) {
            s = s.mid(2);
        }
        bool ok = false;
        const uint v = s.toUInt(&ok, 16);
        return ok && v < 0x110000 ? std::optional<char32_t>(char32_t(v)) : std::nullopt;
    };
    for (const QString& part : spec.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList ends = part.split(QLatin1Char('-'));
        const auto first = codepoint(ends.value(0));
        const auto last = ends.size() == 2 ? codepoint(ends[1]) : first;
        if (!first || !last || ends.size() > 2 || *last < *first) {
            return std::nullopt;
        }
        out.emplace_back(*first, *last);
    }
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

QVariant clampInt(const QVariant& v, int lo, int hi)
{
    return std::clamp(v.toInt(), lo, hi);
}

/// NaN and infinities become @p def: std::clamp would pass NaN through.
QVariant clampDouble(const QVariant& v, double def, double lo, double hi)
{
    const double d = v.toDouble();
    return std::isfinite(d) ? std::clamp(d, lo, hi) : def;
}

QVariant validColor(const QVariant& v, const QString& fallback)
{
    const QString s = v.toString();
    return (s.isEmpty() || Themes::isColorName(s)) ? QVariant(s) : QVariant(fallback);
}

KeyDef intKey(const QString& key, int def, int lo, int hi, const QString& description)
{
    return {key, def, QMetaType::Int, description, [lo, hi](const QVariant& v) { return clampInt(v, lo, hi); }};
}

KeyDef doubleKey(const QString& key, double def, double lo, double hi, const QString& description)
{
    return {key, def, QMetaType::Double, description,
            [def, lo, hi](const QVariant& v) { return clampDouble(v, def, lo, hi); }};
}

KeyDef stringKey(const QString& key, const QString& def, const QString& description)
{
    return {key, def, QMetaType::QString, description};
}

/// A string limited to @p choices; anything else becomes @p def.
KeyDef enumKey(const QString& key, const QString& def, const QString& description, const QStringList& choices)
{
    KeyDef k{key, def, QMetaType::QString, description, [def, choices](const QVariant& v) {
                 const QString s = v.toString().trimmed().toLower();
                 return QVariant(choices.contains(s) ? s : def);
             }};
    for (const QString& c : choices) {
        k.choices.append({c, c});
    }
    return k;
}

template<typename E, std::size_t N>
KeyDef enumKey(const QString& key, const Choice<E> (&choices)[N], E def, const QString& description)
{
    QStringList names;
    QString defName;
    for (const Choice<E>& c : choices) {
        names << QLatin1String(c.name);
        if (c.value == def) {
            defName = QLatin1String(c.name);
        }
    }
    return enumKey(key, defName, description, names);
}

/// The value of an enum key. The schema has already turned anything else into the default.
template<typename E, std::size_t N>
E readEnum(const PhosphorConfig::Store& st, const QString& group, const QString& key, const Choice<E> (&choices)[N], E def)
{
    const QString name = st.read<QString>(group, key);
    for (const Choice<E>& c : choices) {
        if (name == QLatin1String(c.name)) {
            return c.value;
        }
    }
    return def;
}

KeyDef colorKey(const QString& key, const QString& def, const QString& description)
{
    return {key, def, QMetaType::QString, description, [def](const QVariant& v) { return validColor(v, def); }};
}

/// The built-in curves, which are the only ones a curve setting can name.
const PhosphorAnimation::CurveRegistry& curveRegistry()
{
    static const PhosphorAnimation::CurveRegistry Curves;
    return Curves;
}

bool isCurve(const QString& spec)
{
    return curveRegistry().tryCreate(spec.trimmed()) != nullptr;
}

/// A Phosphor curve spec; one that does not parse becomes @p def.
KeyDef curveKey(const QString& key, const QString& def, const QString& description)
{
    return {key, def, QMetaType::QString, description, [def](const QVariant& v) {
                const QString spec = v.toString().trimmed();
                return QVariant(isCurve(spec) ? spec : def);
            }};
}

/// The curve settings: Animation.*Curve and QuickTerminal.curve.
bool isCurveKey(const QString& group, const QString& key)
{
    return (group == Animation && key.endsWith(QLatin1String("Curve"))) || (group == Quick && key == QLatin1String("curve"));
}

/// Hint labels without spaces, in lower case: two or more letters, each one
/// time. Empty when @p value is not such a set.
QString hintLetters(const QString& value)
{
    const QString letters = value.toLower().remove(QLatin1Char(' '));
    for (qsizetype i = 0; i < letters.size(); ++i) {
        if (!letters[i].isLetter() || letters[i].isSurrogate() || letters.indexOf(letters[i]) != i) {
            return {};
        }
    }
    return letters.size() >= 2 ? letters : QString();
}

/// The Terminal.env names that Afterglow does not take, each with what to
/// use instead: a setting gives the value, or Afterglow always sets it.
const QHash<QString, QString>& reservedVariables()
{
    static const QHash<QString, QString> Names{
        {QStringLiteral("TERM"), QStringLiteral("Use Terminal.term")},
        {QStringLiteral("AFTERGLOW_SSH_HELPER"), QStringLiteral("Use Terminal.sshIntegration")},
        {QStringLiteral("AFTERGLOW_SSH_TERMINFO"), QStringLiteral("Use Terminal.sshTerminfo")},
        {QStringLiteral("AFTERGLOW_PANE_ID"), QStringLiteral("Afterglow sets it for each pane")},
        {QStringLiteral("AFTERGLOW_SOCKET"), QStringLiteral("Afterglow sets it for each pane")},
        {QStringLiteral("AFTERGLOW_OVERLAY"), QStringLiteral("Afterglow sets it for the program of an overlay and removes it for other panes")},
        {QStringLiteral("AFTERGLOW_SESSION"), QStringLiteral("Afterglow sets it for each pane of a mux session")},
        {QStringLiteral("AFTERGLOW_RESULT"), QStringLiteral("Afterglow sets it for an overlay that captures a file and removes it for other panes")},
    };
    return Names;
}

QVariantList stringList(const QStringList& strings)
{
    QVariantList out;
    for (const QString& s : strings) {
        out.append(s);
    }
    return out;
}

/// The desktop's fixed-width font, for Font.family "" and Font.size 0.
/// Without a GUI application (or a platform theme that names one), fontconfig's
/// monospace alias at 11 points.
std::pair<QString, double> desktopFixedFont()
{
    QString family;
    double size = 0;
    if (hasGuiApplication()) {
        const QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        family = font.family().trimmed();
        size = font.pointSizeF();
    }
    // Font.size keeps to this range, and so does the desktop's size.
    return {family.isEmpty() ? QStringLiteral("monospace") : family,
            std::isfinite(size) && size > 0 ? std::clamp(size, double(AfterglowRender::FontSpec::MinPoints),
                                                         double(AfterglowRender::FontSpec::MaxPoints))
                                            : AfterglowRender::FontSpec{}.sizePoints};
}

QVariantList defaultPalette()
{
    AfterglowVt::TerminalOptions o;
    QVariantList out;
    for (const AfterglowVt::Rgb& c : o.basePalette) {
        out.append(QColor(c.r, c.g, c.b).name());
    }
    return out;
}

PhosphorConfig::Schema buildSchema()
{
    // The struct defaults are the one source; the schema repeats none of them.
    // Font.family "" and Font.size 0 are the exceptions: they stand for the
    // desktop's fixed-width font, which read() resolves into the struct.
    const Settings d;
    const AfterglowRender::FontSpec& font = d.font;
    QVariantList features;
    for (const std::string& f : font.features) {
        features.append(QString::fromStdString(f));
    }
    const AfterglowVt::TerminalOptions& term = d.terminal;
    const auto hex = [](AfterglowVt::Rgb c) { return QColor(c.r, c.g, c.b).name(); };
    const QString wordSeparators = QString::fromStdU32String(std::u32string(AfterglowVt::DefaultWordBoundaries));

    PhosphorConfig::Schema schema;
    schema.version = 1;
    schema.groups[Font] = {
        stringKey(QStringLiteral("family"), {},
                  QStringLiteral("Font family of regular text. An empty value uses the desktop's fixed-width font.")),
        stringKey(QStringLiteral("boldFamily"), {}, QStringLiteral("Font family of bold text. An empty value uses family.")),
        stringKey(QStringLiteral("italicFamily"), {}, QStringLiteral("Font family of italic text. An empty value uses family.")),
        stringKey(QStringLiteral("boldItalicFamily"), {},
                  QStringLiteral("Font family of bold italic text. An empty value uses the italic family.")),
        {QStringLiteral("size"), 0.0, QMetaType::Double,
         QStringLiteral("Size in points. A value of 0 or less uses the size of the desktop's fixed-width font. Afterglow "
                        "clamps other values to %1–%2.")
             .arg(AfterglowRender::FontSpec::MinPoints)
             .arg(AfterglowRender::FontSpec::MaxPoints),
         [](const QVariant& v) {
             const double size = v.toDouble();
             return QVariant(!std::isfinite(size) || size <= 0
                                 ? 0.0
                                 : std::clamp(size, double(AfterglowRender::FontSpec::MinPoints),
                                              double(AfterglowRender::FontSpec::MaxPoints)));
         }},
        doubleKey(QStringLiteral("sizeStep"), d.fontSizeStep, 0.1, 12.0,
                  QStringLiteral("Points that the font_bigger and font_smaller actions add or remove.")),
        intKey(QStringLiteral("weight"), int(font.regular.weight), 100, 900,
               QStringLiteral("Weight of regular text. It selects a static font and sets the weight axis of a variable font.")),
        intKey(QStringLiteral("boldWeight"), int(font.bold.weight), 100, 900,
               QStringLiteral("Weight of bold text. It selects a static font and sets the weight axis of a variable font.")),
        doubleKey(QStringLiteral("width"), font.regular.width, 50, 200,
                  QStringLiteral("Width axis of variable fonts. A value of 100 is the normal width. Each font limits it to its own "
                                 "range.")),
        doubleKey(QStringLiteral("italicSlant"), font.italic.slant, -90, 90,
                  QStringLiteral("Slant of italic text in degrees, for variable fonts with a slant axis.")),
        {QStringLiteral("features"), features, QMetaType::QVariantList,
         QStringLiteral("OpenType features in HarfBuzz syntax, such as calt, liga, ss01, cv01=2 or -liga.")},
        {QStringLiteral("fallbackFamilies"), QVariantList{}, QMetaType::QVariantList,
         QStringLiteral("Fallback families in priority order. These supply missing characters before system fallback.")},
        doubleKey(QStringLiteral("cellHeight"), font.cellHeightScale, 0.5, 3.0,
                  QStringLiteral("Row-height multiplier relative to font metrics. A value of 1.2 adds 20 percent to the row height.")),
        doubleKey(QStringLiteral("cellWidth"), font.cellWidthScale, 0.5, 3.0,
                  QStringLiteral("Column-width multiplier relative to font metrics.")),
        {QStringLiteral("symbolMap"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Preferred families for codepoint ranges, such as { \"U+E000-U+F8FF,U+F0000-U+FFFFD\": \"Symbols Nerd "
                        "Font Mono\" }. Afterglow draws box drawing, block elements, Braille and the Powerline separators "
                        "U+E0B0 to U+E0BF itself. It also draws the block shapes of Symbols for Legacy Computing. This "
                        "setting does not change them.")},
        enumKey(QStringLiteral("hinting"), HintingChoices, font.hinting,
                QStringLiteral("How glyph outlines align to the pixel grid. The none value keeps the outlines as designed. The light "
                               "value aligns vertical positions only. The full value aligns both axes.")),
        enumKey(QStringLiteral("antialiasing"), AntialiasingChoices, font.antialiasing,
                QStringLiteral("How Afterglow smooths glyph edges. The rgb and bgr values use grayscale while transparency or shaders "
                               "are active. Pane effects and overlay fades count as shaders. Pane effects also run while a pane shows "
                               "a shell on another host, as another user or in a container.")),
        doubleKey(QStringLiteral("gamma"), font.gamma, 0.5, 3.0,
                  QStringLiteral("Glyph coverage gamma. A value of 1 preserves coverage. Higher values increase edge coverage.")),
    };
    const HighlightColors& highlights = d.colors;
    schema.groups[Colors] = {
        stringKey(QStringLiteral("theme"), {},
                  QStringLiteral("Theme name or file path. An empty value uses no theme. Afterglow searches the themes "
                                 "directory beside the config file (~/.config/afterglow/themes), then afterglow/themes in "
                                 "each XDG data directory. Then it searches the bundled themes, such as nord and afterglow-light, "
                                 "and the themes of Ghostty, Kitty and Konsole. Use light:NAME,dark:NAME to follow the desktop. Use "
                                 "plasma:NAME for the background, text, selection and cursor colors of the desktop color scheme "
                                 "over theme NAME. Use plasma alone for these colors over afterglow-dark or afterglow-light, "
                                 "after the brightness of the color scheme. A mux session runs without the desktop. It takes the "
                                 "dark NAME, and plasma takes only the colors that the color scheme file sets. A theme "
                                 "replaces each color that it sets, the divider and popup "
                                 "border colors and the visual bell intensity. Afterglow "
                                 "reads its own JSON themes, Ghostty themes, Kitty themes and Konsole color schemes. A value with "
                                 "a slash is a file path. A path can start with ~/ for the home directory. A relative path starts "
                                 "at the directory of the config file.")),
        colorKey(QStringLiteral("foreground"), hex(term.defaultForeground),
                 QStringLiteral("Default text color. An empty value uses the default.")),
        colorKey(QStringLiteral("background"), hex(term.defaultBackground),
                 QStringLiteral("Default background color. An empty value uses the default.")),
        colorKey(QStringLiteral("cursor"), {}, QStringLiteral("Cursor color. An empty value uses the foreground.")),
        colorKey(QStringLiteral("cursorText"), {}, QStringLiteral("Text color under a block cursor. An empty value uses the background.")),
        colorKey(QStringLiteral("selectionForeground"), {}, QStringLiteral("Selected text color. An empty value reverses the cell colors.")),
        colorKey(QStringLiteral("selectionBackground"), {}, QStringLiteral("Selection background color. An empty value reverses the cell colors.")),
        {QStringLiteral("palette"), defaultPalette(), QMetaType::QVariantList,
         QStringLiteral("The 16 ANSI colors, from black to bright white. With fewer than 16, the others use the default. "
                        "With more, Afterglow uses the first 16. An entry that is not a color uses the default.")},
        colorKey(QStringLiteral("divider"), {},
                 QStringLiteral("Color of the dividers between panes. An empty value uses the color a fifth of the way from the "
                                "background color to the text color. A theme replaces it. A theme without a divider color gets "
                                "the color a fifth of the way from its background color to its text color.")),
        colorKey(QStringLiteral("popupBorder"), {},
                 QStringLiteral("Color of the border around a popup overlay. An empty value uses the color halfway from the "
                                "background color to the text color. A theme replaces it. A theme without a popup border color "
                                "gets the color halfway from its background color to its text color.")),
        doubleKey(QStringLiteral("unfocusedDim"), d.window.unfocusedDim, 0, 1,
                  QStringLiteral("Opacity of a black layer over unfocused panes. A value of 0 turns the darkening off.")),
        doubleKey(QStringLiteral("popupDim"), d.window.popupDim, 0, 1,
                  QStringLiteral("Opacity of a black layer over the pane around a popup overlay. The popup and its border stay as "
                                 "they are. A value of 0 turns the darkening off.")),
        {QStringLiteral("boldIsBright"), term.boldIsBright, QMetaType::Bool,
         QStringLiteral("Use bright colors 8–15 for bold text with colors 0–7.")},
        doubleKey(QStringLiteral("minimumContrast"), term.minimumContrast, 1.0, 21.0,
                  QStringLiteral("Minimum contrast ratio between text and its background. Afterglow changes the color of text "
                                 "with less contrast. A value of 1 keeps every color. A value of 4.5 is the WCAG AA level for "
                                 "normal text.")),
        colorKey(QStringLiteral("searchMatch"), hex(highlights.searchMatch),
                 QStringLiteral("Background color of search matches. An empty value uses the default.")),
        colorKey(QStringLiteral("searchCurrentMatch"), hex(highlights.searchCurrentMatch),
                 QStringLiteral("Background color of the current search match. An empty value uses the default.")),
        colorKey(QStringLiteral("searchMatchText"), hex(highlights.searchMatchText),
                 QStringLiteral("Text color of search matches. An empty value uses the default.")),
        colorKey(QStringLiteral("visualBell"), hex(highlights.visualBell),
                 QStringLiteral("Color of the visual bell flash. An empty value uses the default. A color that a program sets "
                                "with OSC 21 replaces it.")),
        doubleKey(QStringLiteral("visualBellIntensity"), highlights.visualBellIntensity, 0, 1,
                  QStringLiteral("Opacity of the visual bell flash when it starts. A value of 0 hides the flash. A theme that "
                                 "sets it replaces it.")),
    };
    schema.groups[Terminal] = {
        enumKey(QStringLiteral("bidi"), BidiChoices, term.bidi,
                QStringLiteral("Bidirectional layout of the primary screen. The off value keeps text in logical order. The auto value "
                               "finds the direction of each paragraph. The ltr and rtl values set it. Application grids keep "
                               "logical order.")),
        intKey(QStringLiteral("scrollback"), int(term.maxScrollback), 0, 1000000,
               QStringLiteral("Lines of history that each pane keeps. A value of 0 keeps no history.")),
        {QStringLiteral("shellIntegration"), d.shellIntegration, QMetaType::Bool,
         QStringLiteral("Integrate Bash, Zsh and Fish prompts in new panes and new mux sessions automatically.")},
        {QStringLiteral("sshIntegration"), d.sshIntegration, QMetaType::Bool,
         QStringLiteral("Run ssh through afterglow ssh in integrated shells. The pane label then shows the host, and the session "
                        "offers COLORTERM and TERM_PROGRAM to the server. New panes and new mux sessions use this value.")},
        enumKey(QStringLiteral("sshTerminfo"), d.sshTerminfo,
                QStringLiteral("What to do when TERM is afterglow: install its terminfo entry on each ssh host, or use "
                               "xterm-256color there instead. New panes and new mux sessions use this value."),
                {QStringLiteral("install"), QStringLiteral("fallback")}),
        {QStringLiteral("sshRemoteCommands"), d.sshRemoteCommands, QMetaType::Bool,
         QStringLiteral("Allow the script commands that are marked remote to run from aglow run on the hosts that "
                        "afterglow ssh connects to. Every process of the remote user, and the remote root user, can then run "
                        "those commands. It needs Control.enabled and Scripts.enabled. A change to on applies to the ssh "
                        "sessions that start after it. A change to off closes the sockets of the running sessions.")},
        stringKey(QStringLiteral("pager"), d.pager,
                  QStringLiteral("Command for viewing the last command output. Afterglow splits it into words and runs it "
                                 "without a shell. An empty value turns the pager off.")),
        intKey(QStringLiteral("lastOutputLimit"), int(d.lastOutputLimit >> 10), 1, 65536,
               QStringLiteral("Kibibytes of the last command output that the pager gets. For longer output, the pager gets the "
                              "start and a note.")),
        stringKey(QStringLiteral("shell"), {},
                  QStringLiteral("Program that new panes and new mux sessions run, as a name or a path. It gets no "
                                 "arguments. An empty value starts your login shell.")),
        stringKey(QStringLiteral("term"), d.term,
                  QStringLiteral("Value of TERM for the programs in the panes. An empty value uses the default. New panes "
                                 "and new mux sessions use this value.")),
        enumKey(QStringLiteral("cursorShape"), CursorShapeChoices, term.cursorShape,
                QStringLiteral("Shape of the cursor until a program sets another one with DECSCUSR.")),
        {QStringLiteral("cursorBlink"), term.cursorBlink, QMetaType::Bool, QStringLiteral("Make the cursor blink.")},
        enumKey(QStringLiteral("copyOnSelect"), CopyOnSelectChoices, d.copyOnSelect,
                QStringLiteral("Where Afterglow copies a new selection. The primary value copies it to the primary selection. "
                               "The clipboard value copies it to the clipboard too. The off value does not copy it.")),
        enumKey(QStringLiteral("bell"), BellChoices, d.bell,
                QStringLiteral("Response to the bell. The none value does nothing. The attention value marks the window urgent. "
                               "The visual value flashes the pane. The both value does both.")),
        {QStringLiteral("confirmClose"), d.confirmClose, QMetaType::Bool,
         QStringLiteral("Ask before closing a pane, tab or window where a program is running. The command afterglow mux "
                        "kill asks too, before it ends a session where a program is running.")},
        {QStringLiteral("hideMouseWhileTyping"), d.hideMouseWhileTyping, QMetaType::Bool,
         QStringLiteral("Hide the pointer while typing. Pointer movement shows it again.")},
        intKey(QStringLiteral("scrollLines"), d.scrollLines, 1, 50, QStringLiteral("Number of rows scrolled for each mouse-wheel notch.")),
        intKey(QStringLiteral("cursorBlinkInterval"), d.cursorBlinkInterval, 100, 5000,
               QStringLiteral("Duration in milliseconds for each visible and hidden cursor phase.")),
        intKey(QStringLiteral("textBlinkInterval"), d.textBlinkInterval, 100, 5000,
               QStringLiteral("Duration in milliseconds for each visible and hidden phase of blinking text (SGR 5).")),
        {QStringLiteral("scrollOnOutput"), d.scrollOnOutput, QMetaType::Bool,
         QStringLiteral("Scroll to the bottom when new output arrives.")},
        {QStringLiteral("scrollOnInput"), d.scrollOnInput, QMetaType::Bool, QStringLiteral("Scroll to the bottom on typing or paste.")},
        {QStringLiteral("ambiguousWide"), term.ambiguousWide, QMetaType::Bool,
         QStringLiteral("Use two cells for East Asian Ambiguous characters, including Greek, Cyrillic and some symbols.")},
        stringKey(QStringLiteral("bellSound"), {},
                  QStringLiteral("Bell sound. An empty value turns the bell sound off. The value \"default\" plays the bell of "
                                 "the desktop sound theme with canberra-gtk-play. Any other value is a sound file that pw-play plays, "
                                 "or paplay when pw-play is missing. Give an absolute path or a path that starts with ~/.")),
        intKey(QStringLiteral("bellSoundInterval"), d.bellSoundInterval, 0, 10000,
               QStringLiteral("Shortest time in milliseconds between two bell sounds. Bells in between make no sound. A value of 0 "
                              "plays every bell.")),
        stringKey(QStringLiteral("wordSeparators"), wordSeparators,
                  QStringLiteral("Characters that end a word for double-click selection, besides spaces. With an empty value, "
                                 "only spaces end a word.")),
        {QStringLiteral("env"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Variables added to the shell's environment, such as { \"EDITOR\": \"nvim\" }. Each value must be a "
                        "string. A variable here replaces the value that Afterglow gives, such as COLORTERM. Afterglow "
                        "ignores TERM, AFTERGLOW_SSH_HELPER and AFTERGLOW_SSH_TERMINFO here. Terminal.term, "
                        "Terminal.sshIntegration and Terminal.sshTerminfo set them. Afterglow also ignores AFTERGLOW_PANE_ID, "
                        "AFTERGLOW_SOCKET, AFTERGLOW_OVERLAY, AFTERGLOW_SESSION and AFTERGLOW_RESULT, which it sets or "
                        "removes for each pane. New panes and new mux sessions use this value.")},
        {QStringLiteral("confirmUnsafePaste"), d.confirmUnsafePaste, QMetaType::Bool,
         QStringLiteral("Ask before pasting or dropping text that holds a line break, unless the program uses bracketed "
                        "paste. A line break could run a command.")},
        stringKey(QStringLiteral("fileTransferPassword"), {},
                  QStringLiteral("Shared secret for Kitty file-transfer approval. An empty value always asks for approval.")),
        intKey(QStringLiteral("fileTransferDeltaLimit"), int(d.fileTransferDeltaLimit >> 30), 0, 1048576,
               QStringLiteral("Gibibytes that a Kitty delta or compressed file transfer can write when the program does not "
                              "give the file size. A larger file fails with EFBIG. A value of 0 sets no limit.")),
        intKey(QStringLiteral("dragAnswerWait"), d.dragAnswerWait, 0, 1000,
               QStringLiteral("Milliseconds that Afterglow waits for the answer of a program that uses Kitty drag and drop. It "
                              "waits when a drag over the pane shows the program new types. The window does not draw while "
                              "it waits. A value of 0 does not wait.")),
        intKey(QStringLiteral("dragTreeLifetime"), d.dragTreeLifetime, 1, 1440,
               QStringLiteral("Minutes that Afterglow keeps the files of a Kitty drag from another host after the drop. "
                              "A drop target such as a file manager can copy them after the drag ends. A change applies to "
                              "the drops that come after it.")),
        enumKey(QStringLiteral("clipboardRead"), PermissionChoices, d.clipboardRead,
                QStringLiteral("Programs reading the clipboard (OSC 52, OSC 5522). The ask value shows a prompt for each read.")),
        intKey(QStringLiteral("pasteReadTimeout"), d.pasteReadTimeout, 1, 600,
               QStringLiteral("Seconds after a paste in which the program can read the clipboard with the password of the paste "
                              "event (OSC 5522).")),
        enumKey(QStringLiteral("clipboardWrite"), PermissionChoices, d.clipboardWrite,
                QStringLiteral("Programs writing the clipboard (OSC 52, OSC 5522). The ask value shows a prompt for each write.")),
        enumKey(QStringLiteral("graphicsFileTransfer"), GraphicsTransferChoices,
                term.graphicsFileTransfer ? Settings::Permission::Allow : Settings::Permission::Deny,
                QStringLiteral("Images that programs send with the Kitty graphics protocol as a file, a temporary file or shared "
                               "memory. Images sent inline always work.")),
        {QStringLiteral("allowScreenReadback"), term.allowScreenReadback, QMetaType::Bool,
         QStringLiteral("Answer DECRQCRA checksum requests. These let any program in the pane read the screen back.")},
    };
    schema.groups[Window] = {
        intKey(QStringLiteral("padding"), d.window.padding, 0, 200, QStringLiteral("Space around the grid in logical pixels.")),
        intKey(QStringLiteral("initialColumns"), d.window.initialColumns, 10, 1000,
               QStringLiteral("Columns of a new window, and of a session that afterglow mux new starts.")),
        intKey(QStringLiteral("initialRows"), d.window.initialRows, 4, 500,
               QStringLiteral("Rows of a new window, and of a session that afterglow mux new starts.")),
        doubleKey(QStringLiteral("backgroundOpacity"), d.window.backgroundOpacity, 0.05, 1.0,
                  QStringLiteral("Opacity of the default background. Text stays opaque.")),
        {QStringLiteral("blur"), d.window.blur, QMetaType::Bool,
         QStringLiteral("Blur content behind translucent backgrounds with KWin.")},
        {QStringLiteral("paneTitles"), d.window.paneTitles, QMetaType::Bool,
         QStringLiteral("Show pane titles in tabs with two or more panes.")},
        enumKey(QStringLiteral("popupCorners"), PopupCornerChoices, d.window.popupCorners,
                QStringLiteral("Corners of the border around a popup overlay. The rounded value curves each corner. The square "
                               "value makes each corner a right angle.")),
        stringKey(QStringLiteral("workingDirectory"), d.window.workingDirectory,
                  QStringLiteral("Starting directory for new tabs, panes and windows. Use inherit, home or a path. The inherit "
                                 "value, or an empty value, uses the focused pane's directory or the launcher's directory. A "
                                 "path must be absolute, ~, or start with ~/. A path that is not a directory uses inherit. The "
                                 "afterglow -d option overrides this setting. A session of afterglow mux new starts in its "
                                 "--cwd or in the current directory.")),
        enumKey(QStringLiteral("startMode"), StartModeChoices, d.window.startMode,
                QStringLiteral("How new windows open. The normal value opens them at Window.initialColumns and "
                               "Window.initialRows. The maximized value fills the screen beside the panels. The fullscreen "
                               "value covers the whole screen.")),
        {QStringLiteral("decorations"), d.window.decorations, QMetaType::Bool,
         QStringLiteral("Show the compositor's title bar and borders on new windows.")},
        {QStringLiteral("balancePadding"), d.window.balancePadding, QMetaType::Bool,
         QStringLiteral("Distribute unused cell space between opposite edges. Otherwise, space remains at the right and "
                        "bottom.")},
        enumKey(QStringLiteral("tabBar"), TabBarChoices, d.window.tabBar,
                QStringLiteral("When the tab bar shows. The auto value shows it while there are two or more tabs.")),
        intKey(QStringLiteral("tabBarHeight"), d.window.tabBarHeight, 20, 64,
               QStringLiteral("Height of the tab bar, in logical pixels.")),
        enumKey(QStringLiteral("scrollbar"), ScrollbarChoices, d.window.scrollbar,
                QStringLiteral("When the history scrollbar shows. The auto value shows the scrollbar while the view scrolls or the "
                               "pointer is over it, then fades it out.")),
        intKey(QStringLiteral("scrollbarHideDelay"), d.window.scrollbarHideDelay, 0, 60000,
               QStringLiteral("Milliseconds an auto scrollbar stays visible after the last scroll or after the pointer leaves. "
                              "A value of 0 starts the fade at once.")),
        intKey(QStringLiteral("scrollbarWidth"), d.window.scrollbarWidth, 4, 48,
               QStringLiteral("Width of the history scrollbar, in logical pixels.")),
    };
    schema.groups[MouseGroup] = {
        enumKey(QStringLiteral("rightClick"), RightClickChoices, d.mouse.rightClick,
                QStringLiteral("What a right click does. The menu value opens the context menu. The paste value pastes the "
                               "clipboard. The copy-or-paste value copies an existing selection, or else pastes. The extend "
                               "value extends the selection.")),
        enumKey(QStringLiteral("middleClick"), MiddleClickChoices, d.mouse.middleClick,
                QStringLiteral("What a middle click pastes. The none value turns middle-click paste off.")),
        intKey(QStringLiteral("autoScrollInterval"), d.mouse.autoScrollInterval, 10, 1000,
               QStringLiteral("Milliseconds between the rows that a selection scrolls while you extend it past the top or "
                              "bottom of a pane.")),
        doubleKey(QStringLiteral("momentumScroll"), d.mouse.momentumScroll, 0.0, 1.0,
                  QStringLiteral("Part of the speed of a finger scroll on a touchpad or a touchscreen that the scroll keeps "
                                 "every %1 milliseconds after the last finger lifts. A higher value scrolls further. A value "
                                 "of 0 stops the scroll when the finger lifts. A value of 1 does not slow the scroll, which "
                                 "goes on until a click, a key or a touch stops it.")
                      .arg(MomentumScroll::StepMs)),
        doubleKey(QStringLiteral("touchScrollMultiplier"), d.mouse.touchScrollMultiplier, -10.0, 10.0,
                  QStringLiteral("Multiplier for the distance of a finger scroll on a touchpad or a touchscreen. A negative "
                                 "value scrolls the other way. A program that reads the mouse gets the direction only.")),
        intKey(QStringLiteral("touchSelectHold"), d.mouse.touchSelectHold, 100, 2000,
               QStringLiteral("Milliseconds that a finger holds still on a touchscreen before a move selects text. A finger "
                              "that moves sooner scrolls.")),
        {QStringLiteral("dragSelection"), d.mouse.dragSelection, QMetaType::Bool,
         QStringLiteral("Allow a single press without Alt inside the selection to drag its text to another pane or "
                        "another desktop application. A click there without a move clears the selection, and other presses "
                        "there select as they do elsewhere.")},
    };
    schema.groups[LinksGroup] = {
        {QStringLiteral("detectUrls"), d.links.detectUrls, QMetaType::Bool,
         QStringLiteral("Detect plain-text URLs in addition to application-provided OSC 8 links.")},
        stringKey(QStringLiteral("openWith"), {},
                  QStringLiteral("Command for opening links. Afterglow splits it into words, adds the URL as the last "
                                 "argument and runs it without a shell. Quotes keep spaces in one word. An empty value uses the "
                                 "desktop browser.")),
        enumKey(QStringLiteral("modifier"), ModifierChoices, d.links.modifier,
                QStringLiteral("Modifier for link highlighting and activation. With none, a plain click opens the link.")),
        {QStringLiteral("schemes"), stringList(d.links.schemes), QMetaType::QVariantList,
         QStringLiteral("URL schemes that open without a prompt. Links with other schemes ask first.")},
    };
    schema.groups[Quick] = {
        doubleKey(QStringLiteral("height"), d.quickTerminal.height, 0.1, 1.0, QStringLiteral("Height as a fraction of the screen.")),
        stringKey(QStringLiteral("shortcut"), d.quickTerminal.shortcut,
                  QStringLiteral("Global shortcut to show or hide the quick terminal. An empty value turns the shortcut "
                                 "off. A value that is not a key combination uses the default.")),
        {QStringLiteral("animate"), d.quickTerminal.animate, QMetaType::Bool,
         QStringLiteral("Animate quick-terminal opening and closing.")},
        curveKey(QStringLiteral("curve"), d.quickTerminal.curve,
                 QStringLiteral("Curve of the quick terminal slide, in the Animation.splitCurve format.")),
        intKey(QStringLiteral("duration"), d.quickTerminal.duration, 16, 5000,
               QStringLiteral("Duration of the quick terminal slide in milliseconds for curves other than springs.")),
        enumKey(QStringLiteral("position"), QuickPositionChoices, d.quickTerminal.position,
                QStringLiteral("Screen edge the quick terminal slides from.")),
        stringKey(QStringLiteral("screen"), {},
                  QStringLiteral("Screen that shows the quick terminal, by its output name such as DP-1. An empty or unknown name "
                                 "uses the primary screen.")),
    };
    schema.groups[MuxGroup] = {
        enumKey(QStringLiteral("onClose"), MuxCloseChoices, d.mux.onClose,
                QStringLiteral("What closing the window, tab or pane that shows a mux session does. With kill, the session "
                               "and its programs end, as in a window outside a session. With detach, the session keeps "
                               "running, and afterglow mux attach shows it again. The session also keeps running after "
                               "afterglow mux detach, a takeover by another window, a quit of Afterglow, a crash and a lost "
                               "connection.")),
        intKey(QStringLiteral("eventQueue"), d.mux.eventQueue, int(AfterglowMux::MinEventQueue >> 20),
               int(AfterglowMux::MaxEventQueue >> 20),
               QStringLiteral("Most data in MiB that a session holds for its window while the window is slow, for bells, "
                              "notifications, clipboard requests, file transfers and drag and drop. A request that does not "
                              "fit gets the answer of a session that no window shows. Afterglow passes the value when it "
                              "attaches to a session.")),
    };
    schema.groups[ControlGroup] = {
        {QStringLiteral("enabled"), d.control.enabled, QMetaType::Bool,
         QStringLiteral("Allow other programs, such as afterglow ctl and aglow run, to control Afterglow through its "
                        "socket. The aglow run command also needs Scripts.enabled. Later launches of Afterglow use the "
                        "socket also when this is off. A value that is not true or false turns it off.")},
        {QStringLiteral("allowReadText"), d.control.allowReadText, QMetaType::Bool,
         QStringLiteral("Allow control clients, such as afterglow ctl, to read pane text, including scrollback, the commands "
                        "that run and the values of user variables. It also allows screenshots of panes, tabs and windows. A "
                        "script command that runs for a client, such as aglow run or afterglow run, reads pane text only when "
                        "Scripts.allowReadText is on too.")},
        {QStringLiteral("allowRemoteReadText"), d.control.allowRemoteReadText, QMetaType::Bool,
         QStringLiteral("Allow a script command that runs from a remote host, with aglow run in an afterglow ssh session, to "
                        "read pane text. Any process of the remote user, and the remote root user, can run such a "
                        "command and read its output. Scripts.allowReadText and Control.allowReadText must be on too. "
                        "While this is off, only the handler of the run writes to its output.")},
        intKey(QStringLiteral("eventInterval"), d.control.eventInterval, 0, 10000,
               QStringLiteral("Shortest time in milliseconds between two events of one kind for one pane, for titles, status, "
                              "context, bells and each user variable. A newer event replaces one that waits, and the bells in "
                              "between become one. A value of 0 sends every event.")),
    };
    schema.groups[ScriptsGroup] = {
        {QStringLiteral("allowReadText"), d.scriptsAllowReadText, QMetaType::Bool,
         QStringLiteral("Allow Luau scripts to read pane text, selections and semantic regions, and to run actions that "
                        "read pane text. In a script command that runs for a client, such as aglow run or afterglow run, "
                        "read_text, read_region and semantic_regions also need Control.allowReadText.")},
        {QStringLiteral("enabled"), d.scripts, QMetaType::Bool,
         QStringLiteral("Run Luau scripts from the scripts directory beside the config file and from afterglow/scripts in "
                        "each XDG data directory. The aglow run command also needs Control.enabled. A value that is not "
                        "true or false turns it off.")},
        intKey(QStringLiteral("commandTimeout"), d.scriptCommandTimeout, 1000, 3600000,
               QStringLiteral("Longest time in milliseconds that one run of a script command can take. This includes runs "
                              "from a key, a menu or the palette. The time that the run waits for a task, a dialog, an "
                              "overlay or a slow reader of its output counts. The wait for standard input before the run "
                              "starts does not count. At the limit the run ends, and aglow run exits with status 124. A "
                              "change applies to the runs that start after it.")),
        // Kibibytes, as Terminal.lastOutputLimit: read() turns them into bytes.
        intKey(QStringLiteral("commandStdinLimit"), int(d.scriptCommandStdinLimit >> 10), 0, MaxScriptCommandStdinLimitKiB,
               QStringLiteral("Kibibytes of standard input that a script command can get from aglow run. A run with "
                              "more input fails, and aglow run exits with status 2. A value of 0 refuses any input that is not empty.")),
        intKey(QStringLiteral("commandRunLimit"), d.scriptCommandRunLimit, 1, MaxCommandRuns,
               QStringLiteral("Runs of the commands of one script at the same time. A run over the limit fails, and "
                              "aglow run exits with status 1. Each run that waits uses one of the %1 waiting handlers "
                              "of its script.")
                   .arg(MaxCommandRuns)),
        intKey(QStringLiteral("commandOutputLimit"), int(d.scriptCommandOutputLimit >> 10), 64, 262144,
               QStringLiteral("Kibibytes of UTF-8 text that one run of a script command can write to standard output and "
                              "standard error together. A run that writes more ends, and aglow run exits with status "
                              "1. A run from a key, a menu or the palette counts only its standard error.")),
        intKey(QStringLiteral("cpuBudget"), d.scriptCpuBudget, 10, 1000,
               QStringLiteral("Milliseconds of handler time that one script can use in each second. Past it, Afterglow "
                              "drops the script's events, and delays its results, timers, keys and command runs. Each second "
                              "in which a call of the script waits counts. After 5 such seconds, each less than 3 seconds "
                              "after the one before, Afterglow disables the script. Dropped events do not count.")),
    };
    const AudioSettings& audio = d.audio;
    schema.groups[AudioGroup] = {
        {QStringLiteral("enabled"), d.audio.enabled, QMetaType::Bool,
         QStringLiteral("Analyze system playback when a visible shader requests audio. Requires cava.")},
        enumKey(QStringLiteral("inputMethod"), d.audio.inputMethod, QStringLiteral("Sound system that cava captures from. The auto value detects it."),
                {QStringLiteral("auto"), QStringLiteral("pipewire"), QStringLiteral("pulse")}),
        stringKey(QStringLiteral("inputSource"), d.audio.inputSource,
                  QStringLiteral("Playback source for capture. The auto value, or an empty value, follows the default output.")),
        intKey(QStringLiteral("framerate"), audio.framerate, PhosphorAudio::Defaults::MinFramerate,
               PhosphorAudio::Defaults::MaxFramerate, QStringLiteral("Spectrum updates per second.")),
        intKey(QStringLiteral("smoothing"), audio.smoothing, PhosphorAudio::Defaults::MinNoiseReduction,
               PhosphorAudio::Defaults::MaxNoiseReduction,
               QStringLiteral("Noise reduction of the spectrum analyzer. A value of 0 is fast and jumpy. 100 is slow and smooth.")),
        doubleKey(QStringLiteral("extraSmoothing"), audio.extraSmoothing, PhosphorAudio::Defaults::MinExtraSmoothing,
                  PhosphorAudio::Defaults::MaxExtraSmoothing,
                  QStringLiteral("Part of the previous spectrum kept on each update, after noise reduction. A value of 0 turns it "
                                 "off.")),
        intKey(QStringLiteral("lowerCutoff"), audio.lowerCutoff, PhosphorAudio::Defaults::MinLowerCutoffHz,
               PhosphorAudio::Defaults::MaxLowerCutoffHz, QStringLiteral("Lowest analyzed frequency in hertz.")),
        intKey(QStringLiteral("higherCutoff"), audio.higherCutoff, PhosphorAudio::Defaults::MinHigherCutoffHz,
               PhosphorAudio::Defaults::MaxHigherCutoffHz, QStringLiteral("Highest analyzed frequency in hertz.")),
    };
    const AnimationSettings& anim = d.animation;
    schema.groups[Animation] = {
        {QStringLiteral("cursor"), anim.cursor, QMetaType::Bool, QStringLiteral("Animate cursor movement between cells.")},
        {QStringLiteral("scroll"), anim.scroll, QMetaType::Bool, QStringLiteral("Animate scrolling between rows.")},
        {QStringLiteral("splits"), anim.splits, QMetaType::Bool,
         QStringLiteral("Animate split opening, closing and resizing. It also controls the open and close animations, the "
                        "portal and the away frame of Shaders.paneEffects.")},
        curveKey(QStringLiteral("splitCurve"), anim.splitCurve,
                 QStringLiteral("Curve of split animations, as a Phosphor curve spec. Use a spring such as spring:22,1 (angular "
                                "frequency, damping ratio), a cubic Bezier such as 0.2,0,0,1, or a named easing such as "
                                "elastic-out. An invalid spec uses the default.")),
        intKey(QStringLiteral("splitDuration"), anim.splitDuration, 16, 5000,
               QStringLiteral("Duration of split animations in milliseconds for curves other than springs, which settle by "
                              "themselves.")),
        curveKey(QStringLiteral("overviewCurve"), anim.overviewCurve,
                 QStringLiteral("Curve of the pane overview motion, in the splitCurve format. The default spring overshoots "
                                "slightly.")),
        intKey(QStringLiteral("overviewDuration"), anim.overviewDuration, 16, 5000,
               QStringLiteral("Duration of pane overview motion in milliseconds for curves other than springs.")),
        curveKey(QStringLiteral("tabCurve"), anim.tabCurve,
                 QStringLiteral("Curve of tab switches (Shaders.tabTransition), in the splitCurve format.")),
        intKey(QStringLiteral("tabDuration"), anim.tabDuration, 16, 5000,
               QStringLiteral("Duration of tab switches in milliseconds for curves other than springs.")),
        intKey(QStringLiteral("paneEffectDuration"), anim.paneEffectDuration, 16, 5000,
               QStringLiteral("Milliseconds that a pane takes to open or close in Shaders.paneEffects. The portal of a pane "
                              "whose shell moves to another host, user or container takes the same time.")),
        curveKey(QStringLiteral("overlayCurve"), anim.overlayCurve,
                 QStringLiteral("Curve of the fade of an overlay over its pane (Shaders.overlayTransition), in the "
                                "splitCurve format. The fade stops the first time that it reaches its target. A curve that "
                                "overshoots does not bounce back.")),
        intKey(QStringLiteral("overlayDuration"), anim.overlayDuration, 16, 5000,
               QStringLiteral("Duration of the fade of an overlay (Shaders.overlayTransition) in milliseconds for curves "
                              "other than springs.")),
        curveKey(QStringLiteral("cursorCurve"), anim.cursorCurve,
                 QStringLiteral("Curve of cursor motion between cells, in the splitCurve format.")),
        intKey(QStringLiteral("cursorDuration"), anim.cursorDuration, 16, 5000,
               QStringLiteral("Duration of cursor motion in milliseconds for curves other than springs.")),
        curveKey(QStringLiteral("scrollCurve"), anim.scrollCurve,
                 QStringLiteral("Curve of scroll motion, in the splitCurve format.")),
        intKey(QStringLiteral("scrollDuration"), anim.scrollDuration, 16, 5000,
               QStringLiteral("Duration of scroll motion in milliseconds for curves other than springs.")),
        curveKey(QStringLiteral("bellCurve"), anim.bellCurve,
                 QStringLiteral("Curve of the visual bell fade, in the splitCurve format.")),
        intKey(QStringLiteral("bellDuration"), anim.bellDuration, 16, 5000,
               QStringLiteral("Duration of the visual bell fade in milliseconds for curves other than springs.")),
    };
    schema.groups[GraphicsGroup] = {
        intKey(QStringLiteral("quota"), int(term.graphicsQuota >> 20), 16, 4096,
               QStringLiteral("Mebibytes of images (Kitty graphics, sixel and iTerm2) that each screen of a pane keeps, at 4 "
                              "bytes for each pixel. A pane has a main and an alternate screen. When a screen is full, images "
                              "without a placement go first: those sent with the transient hint (N=1), then the oldest.")),
    };
    schema.groups[NotificationsGroup] = {
        {QStringLiteral("enabled"), d.notifications, QMetaType::Bool,
         QStringLiteral("Show desktop notifications that programs send with OSC 9, OSC 99 and OSC 777, and program status "
                        "notifications (OSC 7501). Afterglow's own notifications show also when this is off.")},
        intKey(QStringLiteral("rateLimit"), d.notificationRateLimit, 0, 1000,
               QStringLiteral("Desktop notifications that the programs of one pane can show in Notifications.rateWindow "
                              "seconds. Afterglow drops the notifications over the limit. A value of 0 removes the limit. "
                              "Updates of a shown notification do not count. The notifications of Afterglow itself do not "
                              "count, such as those of afterglow ctl notify and of the notify effect of scripts. The "
                              "notification at the end of a script command run without a client counts against the "
                              "limit of its pane. Such notifications of runs without a pane share one limit.")),
        intKey(QStringLiteral("rateWindow"), d.notificationRateWindow, 1, 3600,
               QStringLiteral("Seconds over which Notifications.rateLimit counts the notifications of one pane.")),
        intKey(QStringLiteral("statusInterval"), d.statusNotificationInterval, 0, 600000,
               QStringLiteral("Shortest time in milliseconds between two program status notifications (OSC 7501) of one pane. "
                              "A value of 0 shows each one.")),
    };
    schema.groups[Shaders] = {
        {QStringLiteral("background"), stringList(d.shaders.background), QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane's background layer alone: cell backgrounds and images under the text.")},
        {QStringLiteral("text"), stringList(d.shaders.text), QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane's text layer alone: glyphs, decorations and images over the text.")},
        {QStringLiteral("cursor"), stringList(d.shaders.cursor), QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane's cursor layer alone.")},
        {QStringLiteral("pane"), stringList(d.shaders.pane), QMetaType::QVariantList,
         QStringLiteral("Shaders run over each pane. Name a pack by its directory name, which is normally its id, such as "
                        "cursor-trail or bloom. Name a pack of a package as <package>.<pack>, such as my-extension.wall.")},
        {QStringLiteral("window"), stringList(d.shaders.window), QMetaType::QVariantList,
         QStringLiteral("Shaders run over the whole window, across panes, after the pane shaders.")},
        stringKey(QStringLiteral("overview"), d.shaders.overview,
                  QStringLiteral("Shader that draws the pane overview (pane_overview). An empty value turns the overview off.")),
        stringKey(QStringLiteral("tabTransition"), d.shaders.tabTransition,
                  QStringLiteral("Shader that animates switching tabs. An empty value switches at once.")),
        stringKey(QStringLiteral("paneEffects"), d.shaders.paneEffects,
                  QStringLiteral("Shader that animates panes as they split open and close. It draws a portal when the shell "
                                 "of a pane moves to another host, user or container. While the shell stays there, it draws "
                                 "the away frame around the pane. Animation.splits off turns these off too. An empty value "
                                 "turns it off.")),
        stringKey(QStringLiteral("overlayTransition"), d.shaders.overlayTransition,
                  QStringLiteral("Shader that blends a pane and the overlay that runs over it, as the overlay opens and closes. "
                                 "An empty value, or a pack that does not load, switches at once.")),
        {QStringLiteral("params"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Parameter values for each shader reference, such as { \"bloom\": { \"strength\": 0.5 } }. The "
                        "value of each reference must be an object.")},
    };
    schema.groups[HintsGroup] = {
        {QStringLiteral("patterns"), d.hintPatterns, QMetaType::QVariantList,
         QStringLiteral("Up to %1 hints. Each is an object with a regular expression in pattern and an action: copy, open or "
                        "command. An entry without an action copies. A command action also needs command, a list of 1 to "
                        "%2 arguments. The first argument names the program.")
             .arg(MaxHintPatterns)
             .arg(MaxHintCommandArguments)},
        {QStringLiteral("alphabet"), d.hintAlphabet, QMetaType::QString,
         QStringLiteral("Letters that label hints and URL hints, the easiest to reach first. It needs two or more letters, "
                        "each one time. Other values use the default."),
         [def = d.hintAlphabet](const QVariant& v) {
             const QString letters = hintLetters(v.toString());
             return QVariant(letters.isEmpty() ? def : letters);
         }},
    };
    schema.groups[Keys] = {
        stringKey(QStringLiteral("leader"), {},
                  QStringLiteral("Key that Leader stands for in sequences. With an empty value, a binding that uses Leader is an "
                                 "error.")),
        intKey(QStringLiteral("sequenceTimeout"), KeyModes::DefaultSequenceTimeout, 100, 10000,
               QStringLiteral("Milliseconds before Afterglow cancels an incomplete sequence.")),
        intKey(QStringLiteral("tableTimeout"), KeyModes::DefaultTableTimeout, 100, 60000, QStringLiteral("Idle milliseconds before leaving a key table.")),
        {QStringLiteral("tables"), QVariantMap{}, QMetaType::QVariantMap,
         QStringLiteral("Named key tables, each a map of actions to keys as in bindings. A binding to table:NAME opens one. "
                        "Its actions keep it open. The pop_key_table action leaves every open table, and so does "
                        "Keybinds.tableTimeout.")},
        {QStringLiteral("bindings"), Keybinds::defaultMap(), QMetaType::QVariantMap,
         QStringLiteral("Map each action to a key combination or a list of them. An empty string removes the binding. "
                        "Besides actions, the map can bind script:ID.COMMAND to run a script command, text:BYTES to send "
                        "text and table:NAME to open a key table.")},
    };
    return schema;
}

AfterglowVt::Rgb toRgb(const QString& name, AfterglowVt::Rgb fallback)
{
    if (!Themes::isColorName(name)) return fallback;
    const QColor c = QColor::fromString(name);
    return AfterglowVt::Rgb{std::uint8_t(c.red()), std::uint8_t(c.green()), std::uint8_t(c.blue())};
}

// What PhosphorConfig's JSON backend reads as each type: anything else
// silently becomes the default.
bool isBoolValue(const QJsonValue& raw)
{
    if (raw.isBool()) return true;
    if (raw.isDouble()) return raw.toDouble() == 0.0 || raw.toDouble() == 1.0;
    static const QStringList Words = {QStringLiteral("true"),  QStringLiteral("1"),  QStringLiteral("yes"), QStringLiteral("on"),
                                      QStringLiteral("false"), QStringLiteral("0"), QStringLiteral("no"),  QStringLiteral("off")};
    return raw.isString() && Words.contains(raw.toString().toLower());
}

bool isIntValue(const QJsonValue& raw)
{
    bool ok = false;
    if (raw.isDouble()) {
        const double d = raw.toDouble();
        return d >= double(std::numeric_limits<int>::min()) && d <= double(std::numeric_limits<int>::max());
    }
    if (raw.isString()) raw.toString().toInt(&ok);
    return ok;
}

/// A whole number that isIntValue() refuses all the same: too large or too
/// small for an int, as a number or as text.
bool isOutOfIntRange(const QJsonValue& raw)
{
    static const QRegularExpression Digits(QStringLiteral("^[+-]?[0-9]+$"));
    return raw.isDouble() || (raw.isString() && Digits.match(raw.toString().trimmed()).hasMatch());
}

/// The validators turn NaN and the infinities into the default, and so does
/// the text of one ("nan", "inf"), which QString::toDouble reads.
bool isNumberValue(const QJsonValue& raw)
{
    bool ok = raw.isDouble();
    double value = raw.toDouble();
    if (raw.isString()) value = raw.toString().toDouble(&ok);
    return ok && std::isfinite(value);
}

/// Whether read() takes @p raw for a list, a map or a string key as it is:
/// malformedValues() lays the default over another JSON type, which
/// PhosphorConfig would read as text or as the JSON that its text holds.
bool hasReadableType(const KeyDef& def, const QJsonValue& raw)
{
    switch (def.expectedType) {
    case QMetaType::QVariantList:
        return raw.isArray();
    case QMetaType::QVariantMap:
        return raw.isObject();
    case QMetaType::QString:
        return raw.isString();
    default:
        return true;
    }
}

/// Values the validators replaced with a default, and keys the schema does
/// not know. The validators stay silent, since every read and the reference
/// run them. PhosphorConfig reads any JSON value of a string key as its text
/// ("pager": false would run false, "alphabet": true would label hints with
/// t, r, u and e), and a list or map of another JSON type as empty or as the
/// JSON that its text holds. So @p defaults gets the default of each such
/// key, for read() to lay over the file: each "Using the default" is true.
/// Scripts.enabled and Control.enabled with a value that is not a boolean
/// stay off, and their text says so.
QStringList malformedValues(const PhosphorConfig::Schema& schema, const QJsonObject& file, const QJsonObject& overrides,
                            QJsonObject* defaults)
{
    QStringList out;
    QSet<QString> groups;
    for (const QJsonObject& source : {file, overrides}) {
        for (auto group = source.begin(); group != source.end(); ++group) {
            if (group.key() == schema.versionKey) continue;
            if (!schema.groups.contains(group.key())) {
                out << QStringLiteral("Unknown setting group %1").arg(group.key());
                continue;
            }
            if (!group.value().isObject()) {
                // Scripts and the control socket fail closed: a file that
                // means to turn them off and says it wrongly turns them off.
                // A command-line override still wins.
                const bool switchGroup = group.key() == ScriptsGroup || group.key() == ControlGroup;
                if (switchGroup && !isBoolValue(overrides.value(group.key()).toObject().value(QLatin1String("enabled")))) {
                    out << QStringLiteral("%1 must be an object of settings. Using the defaults, with %1.enabled off").arg(group.key());
                    QJsonObject keys = defaults->value(group.key()).toObject();
                    keys.insert(QStringLiteral("enabled"), false);
                    defaults->insert(group.key(), keys);
                } else {
                    out << QStringLiteral("%1 must be an object of settings. Using the defaults").arg(group.key());
                }
                continue;
            }
            const QJsonObject keys = group.value().toObject();
            for (auto key = keys.begin(); key != keys.end(); ++key) {
                if (!schema.findKey(group.key(), key.key())) out << QStringLiteral("Unknown setting %1.%2").arg(group.key(), key.key());
            }
            groups.insert(group.key());
        }
    }
    // In the schema's order, not the hash order of the set.
    for (auto group = schema.groups.cbegin(); group != schema.groups.cend(); ++group) {
        if (!groups.contains(group.key())) continue;
        for (const KeyDef& def : group.value()) {
            // An override wins over the file, so it is the value that counts.
            QJsonValue raw = overrides.value(group.key()).toObject().value(def.key);
            if (raw.isUndefined()) raw = file.value(group.key()).toObject().value(def.key);
            if (raw.isUndefined()) continue;
            const QString name = group.key() + QLatin1Char('.') + def.key;
            auto useDefault = [&] {
                QJsonObject keys = defaults->value(group.key()).toObject();
                keys.insert(def.key, QJsonValue::fromVariant(def.defaultValue));
                defaults->insert(group.key(), keys);
            };
            if (def.expectedType == QMetaType::Bool && !isBoolValue(raw)) {
                // Scripts and the control socket fail closed, as for a group
                // that is no object.
                const bool heldOff = def.key == QLatin1String("enabled") && (group.key() == ScriptsGroup || group.key() == ControlGroup);
                if (heldOff) {
                    QJsonObject keys = defaults->value(group.key()).toObject();
                    keys.insert(def.key, false);
                    defaults->insert(group.key(), keys);
                }
                // Off for as long as the value says nothing: also once a
                // file that was not read is read.
                out << (heldOff ? QStringLiteral("%1 must be true or false. It stays off")
                                : QStringLiteral("%1 must be true or false. Using the default"))
                           .arg(name);
            } else if (def.expectedType == QMetaType::Int && !isIntValue(raw)) {
                out << (isOutOfIntRange(raw) ? QStringLiteral("%1 is too large or too small. Using the default")
                                             : QStringLiteral("%1 must be a whole number. Using the default"))
                           .arg(name);
            } else if (def.expectedType == QMetaType::Double && !isNumberValue(raw)) {
                out << QStringLiteral("%1 must be a number. Using the default").arg(name);
            } else if (def.expectedType == QMetaType::QVariantList && !raw.isArray()) {
                out << QStringLiteral("%1 must be a list. Using the default").arg(name);
                useDefault();
            } else if (def.expectedType == QMetaType::QVariantMap && !raw.isObject()) {
                out << QStringLiteral("%1 must be an object. Using the default").arg(name);
                useDefault();
            } else if (def.expectedType == QMetaType::QString && !raw.isString()) {
                // Before the checks below, which would judge an empty text.
                out << QStringLiteral("%1 must be a string. Using the default").arg(name);
                useDefault();
            } else if (!def.choices.isEmpty()) {
                QStringList names;
                for (const auto& choice : def.choices) names << choice.value.toString();
                if (!names.contains(raw.toString().trimmed().toLower())) {
                    out << QStringLiteral("%1 must be one of %2. Using the default").arg(name, names.join(QStringLiteral(", ")));
                }
            } else if (group.key() == Colors && def.expectedType == QMetaType::QString && def.key != QLatin1String("theme")) {
                if (const QString color = raw.toString(); !color.isEmpty() && !Themes::isColorName(color)) {
                    out << QStringLiteral("%1 is not a color. Using the default").arg(name);
                }
            } else if (isCurveKey(group.key(), def.key)) {
                if (!isCurve(raw.toString())) out << QStringLiteral("%1 is not a curve. Using the default").arg(name);
            } else if (group.key() == HintsGroup && def.key == QLatin1String("alphabet")) {
                if (hintLetters(raw.toString()).isEmpty()) {
                    out << QStringLiteral("%1 needs two or more letters, each one time. Using the default").arg(name);
                }
            } else if (group.key() == Colors && def.key == QLatin1String("palette")) {
                const QJsonArray palette = raw.toArray();
                if (palette.size() > PaletteSize) {
                    out << QStringLiteral("Colors.palette needs %1 colors, not %2. Afterglow uses the first %1")
                               .arg(PaletteSize)
                               .arg(palette.size());
                } else if (palette.size() < PaletteSize) {
                    out << QStringLiteral("Colors.palette needs %1 colors, not %2. The others use the default")
                               .arg(PaletteSize)
                               .arg(palette.size());
                }
                for (qsizetype i = 0; i < palette.size() && i < PaletteSize; ++i) {
                    if (!Themes::isColorName(palette[i].toString())) {
                        out << QStringLiteral("Colors.palette[%1] is not a color. Using the default").arg(i);
                    }
                }
            }
        }
    }
    return out;
}

} // namespace

QVariantList defaultHintPatterns()
{
    return {QVariantMap{
        {QStringLiteral("pattern"), QStringLiteral(R"((?:~?/|\.{1,2}/)?[\w.@+-]+(?:/[\w.@+-]+)*(?:\.[\w+-]+|:\d+)(?::\d+){0,2})")},
        {QStringLiteral("action"), QStringLiteral("copy")}}};
}

QString SettingsStore::defaultPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/afterglow/config.json");
}

PhosphorConfig::Schema SettingsStore::schema()
{
    return buildSchema();
}

SettingsStore::SettingsStore(QObject* parent)
    : SettingsStore(defaultPath(), parent)
{
}

SettingsStore::SettingsStore(const QString& path, QObject* parent)
    : SettingsStore(path, QJsonObject{}, parent)
{
}

SettingsStore::SettingsStore(const QString& path, const QJsonObject& overrides, QObject* parent)
    : QObject(parent)
    , m_path(path)
    , m_overrides(overrides)
{
    // Deferred: loadFile() below reads the file, once, and reports what it finds.
    m_backend = std::make_unique<PhosphorConfig::JsonBackend>(path, PhosphorConfig::JsonBackend::Load::Deferred);
    // Command-line overrides are read over the file and never written to it.
    m_backend->setOverlay(m_overrides);
    m_store = std::make_unique<PhosphorConfig::Store>(m_backend.get(), buildSchema());
    // No file means every default. The file holds only what differs, so a
    // later change of a default reaches the user (PhosphorConfig's sparse
    // persistence); write() creates it.
    loadFile();
    m_settings = read();
    m_publishedProblems = problems();

    connect(&m_files, &PhosphorFsLoader::WatchedFileSet::filesChanged, this, &SettingsStore::reload);
    if (hasGuiApplication()) {
        // "light:a,dark:b" themes follow the desktop, also while the file does not parse.
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &SettingsStore::queueApply);
    }
    watch();
}

SettingsStore::~SettingsStore() = default;

namespace {
/// What a file that has not been read since the start leaves on. A -o
/// override of either key that is true or false still wins (read()).
const QString NeverReadNote =
    QStringLiteral(". Using the defaults, with Scripts.enabled and Control.enabled off unless -o sets them");
} // namespace

bool SettingsStore::loadFile()
{
    // One read and one parse: the document checked is the document used. The
    // backend reads it, so that a clean read also ends the refusal to write
    // that an earlier unreadable file set, and the next save compares the
    // file against this read. A missing file is every default.
    const QJsonObject before = m_backend->jsonRootSnapshot();
    // The version stamp that every save adds is no setting: a file that held
    // only it held nothing to keep.
    const bool heldSettings = !before.isEmpty() && !(before.size() == 1 && before.contains(m_store->schema().versionKey));
    QString problem = m_backend->reload();
    if (!problem.isEmpty()) {
        // The backend falls back to the last document it read from the file,
        // which is {} after an empty file kept the settings below: the
        // document in use stays instead.
        m_backend->replaceRoot(before);
        m_backend->clearDirty();
        problem += m_neverRead ? NeverReadNote + QStringLiteral(", until it can be read and holds a JSON object. Delete it for the defaults") : QStringLiteral(". Keeping the current settings");
    } else if (m_backend->blankFile() && m_neverRead && !m_fileProblems.isEmpty()) {
        // An editor that saves in place over a file that could not be read
        // at the start: the text that comes may turn them off.
        problem = m_path + QStringLiteral(" is empty") + NeverReadNote + QStringLiteral(", until it holds a JSON object. Delete it for the defaults");
    } else if (m_backend->blankFile() && heldSettings) {
        // An editor that saves in place empties the file before it writes
        // it. A file that held settings and holds nothing now is such a
        // save: keep the settings until its text comes. An empty file that
        // held none, as after a touch, is every default.
        m_backend->replaceRoot(before);
        m_backend->clearDirty();
        problem = QStringLiteral("%1 is empty. Keeping the current settings. Delete it for the defaults").arg(m_path);
    }
    if (!problem.isEmpty()) {
        // Logged when it starts, not again on each reload that keeps it.
        if (!m_fileProblems.contains(problem)) qCWarning(lcConfig).noquote() << problem;
        m_fileProblems = {problem};
        return false;
    }
    m_neverRead = false;
    m_fileProblems.clear();
    return true;
}

void SettingsStore::publishProblems()
{
    // Once, after the file and the read are both current: the file's
    // problems alone would be published with the last read's.
    if (problems() == m_publishedProblems) return;
    m_publishedProblems = problems();
    Q_EMIT problemsChanged(m_publishedProblems);
}

bool SettingsStore::write(const QString& group, const QString& key, const QVariant& value)
{
    if (!m_store->schema().findKey(group, key)) {
        qCWarning(lcConfig).noquote() << "cannot write" << group + QLatin1Char('.') + key << "- no such setting";
        return false;
    }
    // Each refusal below still applies what the file holds now: its
    // problems are published with the read of the same document.
    // The file first, then its document: a file that another process made
    // after the last load is read below, not replaced by this one's keys.
    if (!ensureFile()) {
        loadFile();
        apply();
        return false;
    }
    // Start from the file as it is now: edits not reloaded yet stay, and a
    // file saved half-written is not replaced by an older copy.
    if (!loadFile()) {
        qCWarning(lcConfig).noquote() << "not writing" << group + QLatin1Char('.') + key << "until" << m_path
                                      << "can be read as a JSON object";
        apply();
        return false;
    }
    // A change that cannot be saved must not stay in memory: it would apply
    // at the next read, and the backend would write it over the user's next
    // edit when it closes.
    const QJsonObject saved = m_backend->jsonRootSnapshot();
    m_store->write(group, key, value);
    if (!m_backend->commit()) {
        qCWarning(lcConfig).noquote() << "cannot write" << m_path;
        m_backend->replaceRoot(saved);
        m_backend->clearDirty();
        apply();
        return false;
    }
    apply();
    return true;
}

bool SettingsStore::ensureFile()
{
    if (QFile::exists(m_path)) {
        return true;
    }
    // A link whose target is gone (a dotfiles checkout not made yet): the
    // file goes where the link points, as the saves through it do. Opened
    // through the link, NewOnly would refuse, since the link exists.
    QString target = m_path;
    for (int depth = 0; depth < 16 && QFileInfo(target).isSymLink(); ++depth) target = QFileInfo(target).symLinkTarget();
    // Here, not only when the store starts: the directory can be gone since.
    if (const QString directory = QFileInfo(target).absolutePath(); !QDir().mkpath(directory)) {
        qCWarning(lcConfig).noquote() << "cannot create" << directory;
        return false;
    }
    // The file can come to hold Terminal.fileTransferPassword. Create it
    // private before the first save, which keeps the mode it finds.
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        // Another process may have made it in the meantime: it is there to open.
        if (QFile::exists(m_path)) {
            return true;
        }
        qCWarning(lcConfig).noquote() << "cannot create" << m_path << "-" << file.errorString();
        return false;
    }
    // A half-written file does not parse, and write() refuses to replace a
    // file that does not parse: remove a half-made one rather than leave it.
    if (file.write("{}\n") < 0 || !file.flush()) {
        qCWarning(lcConfig).noquote() << "cannot write" << m_path << "-" << file.errorString();
        file.remove();
        return false;
    }
    return true;
}

QVariant SettingsStore::persisted(const QString& group, const QString& key) const
{
    const PhosphorConfig::KeyDef* def = m_store->schema().findKey(group, key);
    if (!def) return {};
    // The file as it is now, not as the last reload left it: a write built
    // on this value would otherwise undo an edit saved since. A file that
    // cannot be read as a JSON object leaves the document in use, and
    // write() refuses to replace it anyway. An empty or white-space file
    // leaves it too: {} when the file held no settings, and the settings of a
    // save in progress when it did (see loadFile).
    PhosphorConfig::JsonBackend file(m_path, PhosphorConfig::JsonBackend::Load::Deferred);
    if (!file.reload().isEmpty() || file.blankFile()) file.replaceRoot(m_backend->jsonRootSnapshot());
    // Never written: the destructor flushes a dirty document.
    file.clearDirty();
    const QJsonValue raw = file.jsonRootSnapshot().value(group).toObject().value(key);
    // As read() takes it: the defaults that malformedValues() lays over a
    // list, a map or a string of another JSON type, and PhosphorConfig's own
    // reading of the rest (an integer key truncates 6.7 to 6).
    if (!raw.isUndefined() && !hasReadableType(*def, raw)) return def->defaultValue;
    return PhosphorConfig::Store(&file, m_store->schema()).readVariant(group, key);
}

bool SettingsStore::addOverride(QJsonObject& overrides, const QString& assignment)
{
    const qsizetype eq = assignment.indexOf(QLatin1Char('='));
    const QString path = assignment.left(eq).trimmed();
    const qsizetype dot = path.indexOf(QLatin1Char('.'));
    if (eq < 0 || dot <= 0 || dot == path.size() - 1) {
        return false;
    }
    const QString group = path.left(dot);
    const QString key = path.mid(dot + 1);
    // A misspelt key would otherwise be ignored without a word.
    static const PhosphorConfig::Schema Schema = buildSchema();
    const KeyDef* def = Schema.findKey(group, key);
    if (!def) {
        return false;
    }
    const QString raw = assignment.mid(eq + 1);
    // JSON when it parses as a value (numbers, booleans, lists, objects), else a string.
    const QJsonDocument doc = QJsonDocument::fromJson(QStringLiteral("[%1]").arg(raw).toUtf8());
    QJsonValue value = doc.isArray() && doc.array().size() == 1 ? doc.array().first() : QJsonValue(raw);
    // A string setting takes the text as written: Terminal.term=5 means "5".
    if (def->expectedType == QMetaType::QString && !value.isString()) {
        value = QJsonValue(raw);
    }
    QJsonObject entries = overrides.value(group).toObject();
    entries.insert(key, value);
    overrides.insert(group, entries);
    return true;
}

QString SettingsStore::configDirectory() const
{
    return QFileInfo(m_path).absolutePath();
}

bool SettingsStore::darkScheme() const
{
    // Unknown (no desktop preference) counts as dark, afterglow's own default.
    return !hasGuiApplication() || QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Light;
}

namespace {

/// The theme a picked name loads: itself, or for "plasma:NAME" NAME, and for
/// "plasma" alone Afterglow's dark or light theme, after the brightness of
/// the desktop's own background.
QString baseTheme(const QString& picked, const std::optional<Theme>& desktop)
{
    QString base;
    if (!Themes::isPlasma(picked, &base)) {
        return picked;
    }
    if (base.isEmpty() && desktop) {
        // A desktop that gives no background (no GUI application, and no
        // kdeglobals) is dark, as darkScheme() says without one.
        bool dark = true;
        if (const auto& bg = desktop->background) dark = 0.2126 * bg->r + 0.7152 * bg->g + 0.0722 * bg->b < 128;
        base = dark ? QStringLiteral("afterglow-dark") : QStringLiteral("afterglow-light");
    }
    return base;
}

} // namespace

void SettingsStore::watch()
{
    // Every place the theme that read() resolved could come from, so creating
    // a closer one counts.
    QStringList paths{m_path};
    if (m_plasmaTheme) {
        // A new desktop colour scheme rewrites kdeglobals.
        paths << Themes::plasmaSources();
    }
    // A new palette reaches the application as QEvent::ApplicationPaletteChange
    // (Qt 6 deprecates QGuiApplication::paletteChanged), and a new desktop
    // font as a theme or font change. A plasma theme and a font from the
    // desktop follow them, and the desktop font is the default: every event
    // of the process passes here unless the config names both a font and a
    // size and picks no plasma theme.
    if (QCoreApplication* app = QCoreApplication::instance()) {
        if (m_plasmaTheme || m_desktopFont) {
            app->installEventFilter(this);
        } else {
            app->removeEventFilter(this);
        }
    }
    paths << Themes::candidates(m_themeName, Themes::searchDirs(configDirectory()), configDirectory());
    // The set follows each link to its target, also a target that goes and
    // comes back, and a link that is retargeted.
    m_files.setPaths(paths);
}

bool SettingsStore::eventFilter(QObject* watched, QEvent* event)
{
    const bool application = watched == QCoreApplication::instance();
    // A plasma theme without kdeglobals follows the platform palette instead.
    const bool palette = m_plasmaTheme && application && event->type() == QEvent::ApplicationPaletteChange;
    // QFontDatabase::systemFont reads the platform theme, which is current
    // when either event arrives.
    const bool font = m_desktopFont
        && ((application && event->type() == QEvent::ApplicationFontChange)
            || (event->type() == QEvent::ThemeChange && watched->isWindowType()));
    if (palette || font) queueApply();
    return QObject::eventFilter(watched, event);
}

void SettingsStore::queueApply()
{
    // One desktop change sends ThemeChange to every window, and can add a
    // colour scheme, a palette and a font change: one read after Qt has
    // delivered them all. The flag drops before the read, so an event during
    // it queues another.
    if (m_applyQueued) return;
    m_applyQueued = true;
    QMetaObject::invokeMethod(this, [this] {
        m_applyQueued = false;
        apply();
    }, Qt::QueuedConnection);
}

void SettingsStore::reload()
{
    // A file that does not parse leaves the document in use in the backend
    // (loadFile), so a theme edit or a deleted theme still applies over it.
    loadFile();
    apply();
}

void SettingsStore::apply()
{
    const Settings previous = m_settings;
    m_settings = read();
    watch(); // the theme file may have changed
    // A save that changes nothing, or our own write coming back through the
    // watcher, must not reset key modes or wake every script.
    if (m_settings != previous) {
        qCInfo(lcConfig).noquote() << "reloaded" << m_path;
        Q_EMIT changed(previous);
    }
    publishProblems();
}

Settings SettingsStore::read()
{
    const PhosphorConfig::Store& st = *m_store;
    Settings s;
    QJsonObject defaults;
    QStringList problems = malformedValues(st.schema(), m_backend->jsonRootSnapshot(), m_overrides, &defaults);
    // The defaults of keys that hold another JSON type, over the overrides,
    // which the backend lays over the file for every read below.
    QJsonObject overlay = m_overrides;
    for (auto group = defaults.begin(); group != defaults.end(); ++group) {
        QJsonObject keys = overlay.value(group.key()).toObject();
        const QJsonObject replaced = group.value().toObject();
        for (auto key = replaced.begin(); key != replaced.end(); ++key) keys.insert(key.key(), key.value());
        overlay.insert(group.key(), keys);
    }
    if (m_neverRead) {
        // The file could not be read since the start and may turn them off.
        // A command-line override still wins: it is the user's word. One
        // that is no boolean says nothing, and would read as the default on.
        for (const QString& group : {ScriptsGroup, ControlGroup}) {
            if (isBoolValue(m_overrides.value(group).toObject().value(QLatin1String("enabled")))) continue;
            QJsonObject keys = overlay.value(group).toObject();
            keys.insert(QStringLiteral("enabled"), false);
            overlay.insert(group, keys);
        }
    }
    m_backend->setOverlay(overlay);
    // The backend merges the overlay into the file key by key at every
    // depth, so -o Terminal.env={...} would keep the file's variables. An
    // override replaces a map as it replaces any other value.
    auto mapValue = [&st, &overlay](const QString& group, const QString& key) {
        const QJsonValue over = overlay.value(group).toObject().value(key);
        return over.isObject() ? over.toObject().toVariantMap() : st.read<QVariantMap>(group, key);
    };
    auto problem = [&problems](const QString& text) { problems << text; };
    // A list entry that is not a string would otherwise turn into text the
    // user did not write, or into nothing.
    auto strings = [&st, &problem](const QString& group, const QString& key) {
        QStringList out;
        const QVariantList list = st.read<QVariantList>(group, key);
        for (qsizetype i = 0; i < list.size(); ++i) {
            if (list[i].typeId() != QMetaType::QString) {
                problem(QStringLiteral("%1.%2[%3] must be a string. Ignoring it").arg(group, key).arg(i));
            } else if (const QString value = list[i].toString().trimmed(); !value.isEmpty()) {
                out << value;
            }
        }
        return out;
    };

    // Font
    auto& f = s.font;
    QString family = st.read<QString>(Font, QStringLiteral("family")).trimmed();
    double size = st.read<double>(Font, QStringLiteral("size"));
    m_desktopFont = family.isEmpty() || size <= 0;
    if (m_desktopFont) {
        const auto [desktopFamily, desktopSize] = desktopFixedFont();
        if (family.isEmpty()) {
            family = desktopFamily;
        }
        if (size <= 0) {
            size = desktopSize;
        }
    }
    const QString bold = st.read<QString>(Font, QStringLiteral("boldFamily")).trimmed();
    const QString italic = st.read<QString>(Font, QStringLiteral("italicFamily")).trimmed();
    const QString boldItalic = st.read<QString>(Font, QStringLiteral("boldItalicFamily")).trimmed();
    const float weight = float(st.read<int>(Font, QStringLiteral("weight")));
    const float boldWeight = float(st.read<int>(Font, QStringLiteral("boldWeight")));
    const float width = float(st.read<double>(Font, QStringLiteral("width")));
    const float slant = float(st.read<double>(Font, QStringLiteral("italicSlant")));
    const std::string fam = family.toStdString();
    const std::string italicFam = italic.isEmpty() ? fam : italic.toStdString();
    f.sizePoints = float(size);
    f.regular = {fam, weight, width, 0};
    f.bold = {bold.isEmpty() ? fam : bold.toStdString(), boldWeight, width, 0};
    f.italic = {italicFam, weight, width, slant};
    f.boldItalic = {boldItalic.isEmpty() ? italicFam : boldItalic.toStdString(), boldWeight, width, slant};
    f.features.clear();
    for (const QString& feature : strings(Font, QStringLiteral("features"))) {
        if (!AfterglowRender::FontSet::isFeature(feature.toStdString())) {
            problem(QStringLiteral("Font.features: \"%1\" is not an OpenType feature in HarfBuzz syntax").arg(feature));
            continue;
        }
        f.features.push_back(feature.toStdString());
    }
    for (const QString& fallback : strings(Font, QStringLiteral("fallbackFamilies"))) {
        f.fallbackFamilies.push_back(fallback.toStdString());
    }
    const QVariantMap symbolMap = mapValue(Font, QStringLiteral("symbolMap"));
    for (auto it = symbolMap.cbegin(); it != symbolMap.cend(); ++it) {
        if (it.value().typeId() != QMetaType::QString) {
            // A number or a list would otherwise turn into a family name the user did not write.
            problem(QStringLiteral("Font.symbolMap: ignoring %1. Its family must be a string").arg(it.key()));
            continue;
        }
        const QString symbolFamily = it.value().toString().trimmed();
        const auto ranges = parseCodepointRanges(it.key());
        if (!ranges || symbolFamily.isEmpty()) {
            problem(QStringLiteral("Font.symbolMap: ignoring %1. Use U+XXXX or U+XXXX-U+YYYY ranges and a family").arg(it.key()));
            continue;
        }
        for (const auto& [first, last] : *ranges) {
            f.symbolMap.push_back({first, last, symbolFamily.toStdString()});
        }
    }
    f.hinting = readEnum(st, Font, QStringLiteral("hinting"), HintingChoices, f.hinting);
    f.antialiasing = readEnum(st, Font, QStringLiteral("antialiasing"), AntialiasingChoices, f.antialiasing);
    f.gamma = float(st.read<double>(Font, QStringLiteral("gamma")));
    f.cellHeightScale = float(st.read<double>(Font, QStringLiteral("cellHeight")));
    f.cellWidthScale = float(st.read<double>(Font, QStringLiteral("cellWidth")));
    s.fontSizeStep = st.read<double>(Font, QStringLiteral("sizeStep"));

    // Colours
    auto& t = s.terminal;
    t.defaultForeground = toRgb(st.read<QString>(Colors, QStringLiteral("foreground")), t.defaultForeground);
    t.defaultBackground = toRgb(st.read<QString>(Colors, QStringLiteral("background")), t.defaultBackground);
    const QString cursor = st.read<QString>(Colors, QStringLiteral("cursor"));
    if (!cursor.isEmpty()) {
        t.cursorColor = toRgb(cursor, t.defaultForeground);
    }
    const QVariantList palette = st.read<QVariantList>(Colors, QStringLiteral("palette"));
    for (qsizetype i = 0; i < PaletteSize && i < palette.size(); ++i) {
        t.basePalette[std::size_t(i)] = toRgb(palette[i].toString(), t.basePalette[std::size_t(i)]);
    }
    auto optionalColor = [&st](const char* key) -> std::optional<AfterglowVt::Rgb> {
        const QString name = st.read<QString>(Colors, QLatin1String(key));
        if (name.isEmpty()) {
            return std::nullopt;
        }
        return toRgb(name, {});
    };
    t.cursorTextColor = optionalColor("cursorText");
    t.selectionForeground = optionalColor("selectionForeground");
    t.selectionBackground = optionalColor("selectionBackground");
    // Unset: derived below from the background and the text in use.
    std::optional<AfterglowVt::Rgb> divider = optionalColor("divider");
    std::optional<AfterglowVt::Rgb> popupBorder = optionalColor("popupBorder");
    auto& hl = s.colors;
    hl.searchMatch = toRgb(st.read<QString>(Colors, QStringLiteral("searchMatch")), hl.searchMatch);
    hl.searchCurrentMatch = toRgb(st.read<QString>(Colors, QStringLiteral("searchCurrentMatch")), hl.searchCurrentMatch);
    hl.searchMatchText = toRgb(st.read<QString>(Colors, QStringLiteral("searchMatchText")), hl.searchMatchText);
    hl.visualBell = toRgb(st.read<QString>(Colors, QStringLiteral("visualBell")), hl.visualBell);
    hl.visualBellIntensity = st.read<double>(Colors, QStringLiteral("visualBellIntensity"));

    // A theme's colours replace the ones above.
    std::shared_ptr<const Theme> theme;
    // "plasma[:NAME]": the desktop's colours over theme NAME.
    std::optional<Theme> desktop;
    m_themeName.clear();
    m_plasmaTheme = false;
    if (const QString spec = st.read<QString>(Colors, QStringLiteral("theme")).trimmed(); !spec.isEmpty()) {
        const QString picked = Themes::pick(spec, darkScheme());
        m_plasmaTheme = Themes::isPlasma(picked);
        if (m_plasmaTheme) {
            desktop = Themes::plasmaColors(Themes::plasmaSources());
        }
        const QString name = baseTheme(picked, desktop);
        m_themeName = name;
        const QStringList dirs = Themes::searchDirs(configDirectory());
        const QString path = Themes::locate(name, dirs, configDirectory());
        if (name.isEmpty()) {
            problem(QStringLiteral("Colors.theme \"%1\" names no theme").arg(spec));
        } else if (path.isEmpty()) {
            // The directories are many: the description of Colors.theme lists them.
            problem(name.contains(QLatin1Char('/'))
                        ? QStringLiteral("Theme file %1 not found").arg(name)
                        : QStringLiteral("Theme %1 not found in the theme directories (see Colors.theme)").arg(name));
        } else {
            QFile file(path);
            QString error;
            QStringList ignored;
            QStringList invalid;
            std::optional<Theme> parsed;
            bool json = false;
            if (!file.open(QIODevice::ReadOnly)) {
                error = file.errorString();
            } else {
                const QByteArray data = file.readAll();
                json = Themes::isJson(data);
                parsed = Themes::parse(data, &error, &ignored, &invalid);
            }
            if (parsed) {
                theme = std::make_shared<const Theme>(*parsed);
                m_themeFile = path;
                m_theme = theme;
                // Ghostty, Kitty and Konsole files hold much else (Kitty's
                // "cursor none" among it); Afterglow's own should not. The
                // content, not the file name, says which it is: locate() also
                // finds "mine".
                if (json) {
                    if (!ignored.isEmpty()) {
                        problem(QStringLiteral("Theme %1 has keys Afterglow does not use: %2")
                                    .arg(name, ignored.join(QStringLiteral(", "))));
                    }
                    if (!invalid.isEmpty()) {
                        problem(QStringLiteral("Theme %1 has values Afterglow cannot use: %2")
                                    .arg(name, invalid.join(QStringLiteral(", "))));
                    }
                }
            } else if (path == m_themeFile && m_theme) {
                // Mid-save: keep its colours rather than flash the config's.
                theme = m_theme;
                problem(QStringLiteral("Theme %1: %2. Keeping its last colors").arg(name, error));
            } else {
                problem(QStringLiteral("Theme %1: %2").arg(name, error));
            }
        }
    }
    if (desktop) {
        // Each colour that the desktop gives; one that it does not (no GUI
        // application, see Themes::plasmaColors) stays the theme's.
        Theme over = theme ? *theme : Theme{};
        const auto lay = [](std::optional<AfterglowVt::Rgb>& to, const std::optional<AfterglowVt::Rgb>& from) {
            if (from) to = from;
        };
        lay(over.background, desktop->background);
        lay(over.foreground, desktop->foreground);
        lay(over.selectionBackground, desktop->selectionBackground);
        lay(over.selectionForeground, desktop->selectionForeground);
        lay(over.cursor, desktop->cursor);
        lay(over.cursorText, desktop->cursorText);
        // Derived from the desktop's background and text, not the theme's.
        over.divider.reset();
        over.popupBorder.reset();
        theme = std::make_shared<const Theme>(over);
    }
    if (theme) {
        t.defaultForeground = theme->foreground.value_or(t.defaultForeground);
        t.defaultBackground = theme->background.value_or(t.defaultBackground);
        static_assert(std::tuple_size_v<decltype(Theme::palette)> == std::size_t(PaletteSize));
        for (std::size_t i = 0; i < std::size_t(PaletteSize); ++i) {
            t.basePalette[i] = theme->palette[i].value_or(t.basePalette[i]);
        }
        if (theme->cursor) {
            t.cursorColor = theme->cursor;
        }
        if (theme->cursorText) {
            t.cursorTextColor = theme->cursorText;
        }
        if (theme->selectionForeground) {
            t.selectionForeground = theme->selectionForeground;
        }
        if (theme->selectionBackground) {
            t.selectionBackground = theme->selectionBackground;
        }
        hl.searchMatch = theme->searchMatch.value_or(hl.searchMatch);
        hl.searchCurrentMatch = theme->searchCurrentMatch.value_or(hl.searchCurrentMatch);
        hl.searchMatchText = theme->searchMatchText.value_or(hl.searchMatchText);
        hl.visualBell = theme->visualBell.value_or(hl.visualBell);
        hl.visualBellIntensity = theme->visualBellIntensity.value_or(hl.visualBellIntensity);
        // A theme without a divider or a popup border (every theme of another
        // terminal, most bundled ones, and every bundled one for the popup
        // border) gets one derived from its background and text, which suits
        // light and dark alike. The config's are not used: they need not suit the theme's
        // background.
        divider = theme->divider;
        popupBorder = theme->popupBorder;
    }
    s.window.dividerColor =
        AfterglowRender::packRgba(divider.value_or(Themes::derivedDivider(t.defaultBackground, t.defaultForeground)));
    s.window.popupBorderColor =
        AfterglowRender::packRgba(popupBorder.value_or(Themes::derivedPopupBorder(t.defaultBackground, t.defaultForeground)));
    s.window.unfocusedDim = st.read<double>(Colors, QStringLiteral("unfocusedDim"));
    s.window.popupDim = st.read<double>(Colors, QStringLiteral("popupDim"));
    t.boldIsBright = st.read<bool>(Colors, QStringLiteral("boldIsBright"));
    t.minimumContrast = st.read<double>(Colors, QStringLiteral("minimumContrast"));

    // Terminal
    t.maxScrollback = std::size_t(st.read<int>(Terminal, QStringLiteral("scrollback")));
    t.cursorShape = readEnum(st, Terminal, QStringLiteral("cursorShape"), CursorShapeChoices, t.cursorShape);
    t.cursorBlink = st.read<bool>(Terminal, QStringLiteral("cursorBlink"));
    t.bidi = readEnum(st, Terminal, QStringLiteral("bidi"), BidiChoices, t.bidi);
    s.pager = st.read<QString>(Terminal, QStringLiteral("pager"));
    s.lastOutputLimit = std::size_t(st.read<int>(Terminal, QStringLiteral("lastOutputLimit"))) << 10;
    s.shellIntegration = st.read<bool>(Terminal, QStringLiteral("shellIntegration"));
    s.sshIntegration = st.read<bool>(Terminal, QStringLiteral("sshIntegration"));
    s.sshTerminfo = st.read<QString>(Terminal, QStringLiteral("sshTerminfo"));
    s.sshRemoteCommands = st.read<bool>(Terminal, QStringLiteral("sshRemoteCommands"));
    s.shell = st.read<QString>(Terminal, QStringLiteral("shell")).trimmed();
    // s still holds the default term.
    if (const QString term = st.read<QString>(Terminal, QStringLiteral("term")).trimmed(); !term.isEmpty()) s.term = term;
    s.copyOnSelect = readEnum(st, Terminal, QStringLiteral("copyOnSelect"), CopyOnSelectChoices, s.copyOnSelect);
    s.bell = readEnum(st, Terminal, QStringLiteral("bell"), BellChoices, s.bell);
    s.confirmUnsafePaste = st.read<bool>(Terminal, QStringLiteral("confirmUnsafePaste"));
    s.confirmClose = st.read<bool>(Terminal, QStringLiteral("confirmClose"));
    s.hideMouseWhileTyping = st.read<bool>(Terminal, QStringLiteral("hideMouseWhileTyping"));
    s.scrollLines = st.read<int>(Terminal, QStringLiteral("scrollLines"));
    s.cursorBlinkInterval = st.read<int>(Terminal, QStringLiteral("cursorBlinkInterval"));
    s.textBlinkInterval = st.read<int>(Terminal, QStringLiteral("textBlinkInterval"));
    s.scrollOnOutput = st.read<bool>(Terminal, QStringLiteral("scrollOnOutput"));
    s.scrollOnInput = st.read<bool>(Terminal, QStringLiteral("scrollOnInput"));
    t.ambiguousWide = st.read<bool>(Terminal, QStringLiteral("ambiguousWide"));
    s.bellSound = st.read<QString>(Terminal, QStringLiteral("bellSound")).trimmed();
    s.bellSoundInterval = st.read<int>(Terminal, QStringLiteral("bellSoundInterval"));
    t.wordBoundaries = st.read<QString>(Terminal, QStringLiteral("wordSeparators")).toStdU32String();
    const QVariantMap env = mapValue(Terminal, QStringLiteral("env"));
    for (auto it = env.cbegin(); it != env.cend(); ++it) {
        // The environment of a new pane is C strings: a NUL would cut the
        // name or the value short.
        if (it.key().isEmpty() || it.key().contains(QLatin1Char('=')) || it.key().contains(QChar(u'\0'))) {
            problem(QStringLiteral("Terminal.env: \"%1\" is not a variable name").arg(it.key()));
        } else if (const QString setting = reservedVariables().value(it.key()); !setting.isEmpty()) {
            // It would win over the setting without a word, or lose to the
            // value that Afterglow always sets.
            problem(QStringLiteral("Terminal.env.%1 is not used. %2").arg(it.key(), setting));
        } else if (it.value().typeId() != QMetaType::QString) {
            // null, a number or a list would otherwise turn into text the user did not write.
            problem(QStringLiteral("Terminal.env.%1 must be a string").arg(it.key()));
        } else if (it.value().toString().contains(QChar(u'\0'))) {
            problem(QStringLiteral("Terminal.env.%1 must not hold a NUL character").arg(it.key()));
        } else {
            s.environment.emplace_back(it.key().toStdString(), it.value().toString().toStdString());
        }
    }
    s.fileTransferPassword = st.read<QString>(Terminal, QStringLiteral("fileTransferPassword"));
    // ensureFile() makes the file private, but a file made by hand keeps
    // the mode that it was made with.
    const bool filePassword = !m_backend->jsonRootSnapshot()
                                   .value(Terminal)
                                   .toObject()
                                   .value(QStringLiteral("fileTransferPassword"))
                                   .toString()
                                   .isEmpty();
    if (filePassword && (QFile::permissions(m_path) & (QFileDevice::ReadGroup | QFileDevice::ReadOther))) {
        problem(QStringLiteral("%1 holds Terminal.fileTransferPassword, and other users can read it. Make it readable by "
                               "the owner only (chmod 600)")
                    .arg(m_path));
    }
    s.fileTransferDeltaLimit = qint64(st.read<int>(Terminal, QStringLiteral("fileTransferDeltaLimit"))) << 30;
    s.dragAnswerWait = st.read<int>(Terminal, QStringLiteral("dragAnswerWait"));
    s.dragTreeLifetime = st.read<int>(Terminal, QStringLiteral("dragTreeLifetime"));
    s.clipboardRead = readEnum(st, Terminal, QStringLiteral("clipboardRead"), PermissionChoices, s.clipboardRead);
    s.pasteReadTimeout = st.read<int>(Terminal, QStringLiteral("pasteReadTimeout"));
    s.clipboardWrite = readEnum(st, Terminal, QStringLiteral("clipboardWrite"), PermissionChoices, s.clipboardWrite);
    // The terminal core enforces these two itself, and Graphics.quota below.
    // t holds the default here, the one the schema has.
    t.graphicsFileTransfer = readEnum(st, Terminal, QStringLiteral("graphicsFileTransfer"), GraphicsTransferChoices,
                                      t.graphicsFileTransfer ? Settings::Permission::Allow : Settings::Permission::Deny)
        != Settings::Permission::Deny;
    t.allowScreenReadback = st.read<bool>(Terminal, QStringLiteral("allowScreenReadback"));

    // Graphics
    t.graphicsQuota = std::size_t(st.read<int>(GraphicsGroup, QStringLiteral("quota"))) << 20;

    // Notifications
    s.notifications = st.read<bool>(NotificationsGroup, QStringLiteral("enabled"));
    s.notificationRateLimit = st.read<int>(NotificationsGroup, QStringLiteral("rateLimit"));
    s.notificationRateWindow = st.read<int>(NotificationsGroup, QStringLiteral("rateWindow"));
    s.statusNotificationInterval = st.read<int>(NotificationsGroup, QStringLiteral("statusInterval"));

    // Window
    s.window.padding = st.read<int>(Window, QStringLiteral("padding"));
    s.window.initialColumns = st.read<int>(Window, QStringLiteral("initialColumns"));
    s.window.initialRows = st.read<int>(Window, QStringLiteral("initialRows"));
    s.window.backgroundOpacity = st.read<double>(Window, QStringLiteral("backgroundOpacity"));
    s.window.blur = st.read<bool>(Window, QStringLiteral("blur"));
    s.window.paneTitles = st.read<bool>(Window, QStringLiteral("paneTitles"));
    s.window.popupCorners = readEnum(st, Window, QStringLiteral("popupCorners"), PopupCornerChoices, s.window.popupCorners);
    s.window.workingDirectory = st.read<QString>(Window, QStringLiteral("workingDirectory")).trimmed();
    if (const QString& dir = s.window.workingDirectory; !dir.isEmpty() && dir != QLatin1String("inherit")
        && dir != QLatin1String("home") && dir != QLatin1String("~") && !dir.startsWith(QLatin1String("~/"))
        && QDir::isRelativePath(dir)) {
        // A relative path would start wherever Afterglow was started.
        problem(QStringLiteral("Window.workingDirectory \"%1\" must be an absolute path, ~, or start with ~/. Using inherit")
                    .arg(dir));
        s.window.workingDirectory = QStringLiteral("inherit");
    }
    s.window.startMode = readEnum(st, Window, QStringLiteral("startMode"), StartModeChoices, s.window.startMode);
    s.window.decorations = st.read<bool>(Window, QStringLiteral("decorations"));
    s.window.balancePadding = st.read<bool>(Window, QStringLiteral("balancePadding"));
    s.window.tabBar = readEnum(st, Window, QStringLiteral("tabBar"), TabBarChoices, s.window.tabBar);
    s.window.tabBarHeight = st.read<int>(Window, QStringLiteral("tabBarHeight"));
    s.window.scrollbar = readEnum(st, Window, QStringLiteral("scrollbar"), ScrollbarChoices, s.window.scrollbar);
    s.window.scrollbarHideDelay = st.read<int>(Window, QStringLiteral("scrollbarHideDelay"));
    s.window.scrollbarWidth = st.read<int>(Window, QStringLiteral("scrollbarWidth"));

    // Mouse
    s.mouse.rightClick = readEnum(st, MouseGroup, QStringLiteral("rightClick"), RightClickChoices, s.mouse.rightClick);
    s.mouse.middleClick = readEnum(st, MouseGroup, QStringLiteral("middleClick"), MiddleClickChoices, s.mouse.middleClick);
    s.mouse.autoScrollInterval = st.read<int>(MouseGroup, QStringLiteral("autoScrollInterval"));
    s.mouse.momentumScroll = st.read<double>(MouseGroup, QStringLiteral("momentumScroll"));
    s.mouse.touchScrollMultiplier = st.read<double>(MouseGroup, QStringLiteral("touchScrollMultiplier"));
    s.mouse.touchSelectHold = st.read<int>(MouseGroup, QStringLiteral("touchSelectHold"));
    s.mouse.dragSelection = st.read<bool>(MouseGroup, QStringLiteral("dragSelection"));

    // Links
    s.links.detectUrls = st.read<bool>(LinksGroup, QStringLiteral("detectUrls"));
    s.links.openWith = st.read<QString>(LinksGroup, QStringLiteral("openWith")).trimmed();
    s.links.modifier = readEnum(st, LinksGroup, QStringLiteral("modifier"), ModifierChoices, s.links.modifier);
    s.links.schemes.clear();
    for (const QString& value : strings(LinksGroup, QStringLiteral("schemes"))) {
        // "https:" and "HTTPS" name the same scheme as "https".
        QString scheme = value.toLower();
        if (scheme.endsWith(QLatin1Char(':'))) {
            scheme.chop(1);
        }
        // What QUrl::scheme() can return (RFC 3986): another entry would
        // never match a link, which would then ask each time.
        static const QRegularExpression SchemeSyntax(QStringLiteral("^[a-z][a-z0-9+.-]*$"));
        if (!SchemeSyntax.match(scheme).hasMatch()) {
            problem(QStringLiteral("Links.schemes has %1, which is not a URL scheme. Ignoring it").arg(value));
            continue;
        }
        if (!s.links.schemes.contains(scheme)) {
            s.links.schemes.append(scheme);
        }
    }

    // Quick terminal
    s.quickTerminal.height = st.read<double>(Quick, QStringLiteral("height"));
    // The shortcut backend gets the sequence that this text parses to: one
    // that does not parse would bind nothing, with no word of it.
    if (const QString shortcut = st.read<QString>(Quick, QStringLiteral("shortcut")).trimmed(); !shortcut.isEmpty()) {
        const QKeySequence sequence = QKeySequence::fromString(shortcut, QKeySequence::PortableText);
        bool keys = !sequence.isEmpty();
        for (uint i = 0; i < uint(sequence.count()); ++i) keys = keys && sequence[i].key() != Qt::Key_unknown;
        if (keys) {
            s.quickTerminal.shortcut = shortcut;
        } else {
            problem(QStringLiteral("QuickTerminal.shortcut \"%1\" is not a key combination. Using the default").arg(shortcut));
        }
    } else {
        s.quickTerminal.shortcut.clear();
    }
    s.quickTerminal.animate = st.read<bool>(Quick, QStringLiteral("animate"));
    s.quickTerminal.curve = st.read<QString>(Quick, QStringLiteral("curve")).trimmed();
    s.quickTerminal.duration = st.read<int>(Quick, QStringLiteral("duration"));
    s.quickTerminal.position = readEnum(st, Quick, QStringLiteral("position"), QuickPositionChoices, s.quickTerminal.position);
    s.quickTerminal.screen = st.read<QString>(Quick, QStringLiteral("screen")).trimmed();

    // Mux
    s.mux.onClose = readEnum(st, MuxGroup, QStringLiteral("onClose"), MuxCloseChoices, s.mux.onClose);
    s.mux.eventQueue = st.read<int>(MuxGroup, QStringLiteral("eventQueue"));

    // Control
    s.control.enabled = st.read<bool>(ControlGroup, QStringLiteral("enabled"));
    s.control.allowReadText = st.read<bool>(ControlGroup, QStringLiteral("allowReadText"));
    s.control.allowRemoteReadText = st.read<bool>(ControlGroup, QStringLiteral("allowRemoteReadText"));
    s.control.eventInterval = st.read<int>(ControlGroup, QStringLiteral("eventInterval"));

    // Scripts
    s.scripts = st.read<bool>(ScriptsGroup, QStringLiteral("enabled"));
    s.scriptsAllowReadText = st.read<bool>(ScriptsGroup, QStringLiteral("allowReadText"));
    s.scriptCommandTimeout = st.read<int>(ScriptsGroup, QStringLiteral("commandTimeout"));
    s.scriptCommandStdinLimit = qint64(st.read<int>(ScriptsGroup, QStringLiteral("commandStdinLimit"))) << 10;
    s.scriptCommandRunLimit = st.read<int>(ScriptsGroup, QStringLiteral("commandRunLimit"));
    s.scriptCommandOutputLimit = qint64(st.read<int>(ScriptsGroup, QStringLiteral("commandOutputLimit"))) << 10;
    s.scriptCpuBudget = st.read<int>(ScriptsGroup, QStringLiteral("cpuBudget"));

    // Audio
    s.audio.enabled = st.read<bool>(AudioGroup, QStringLiteral("enabled"));
    s.audio.inputMethod = st.read<QString>(AudioGroup, QStringLiteral("inputMethod"));
    s.audio.inputSource = st.read<QString>(AudioGroup, QStringLiteral("inputSource")).trimmed();
    s.audio.framerate = st.read<int>(AudioGroup, QStringLiteral("framerate"));
    s.audio.smoothing = st.read<int>(AudioGroup, QStringLiteral("smoothing"));
    s.audio.extraSmoothing = st.read<double>(AudioGroup, QStringLiteral("extraSmoothing"));
    s.audio.lowerCutoff = st.read<int>(AudioGroup, QStringLiteral("lowerCutoff"));
    s.audio.higherCutoff = st.read<int>(AudioGroup, QStringLiteral("higherCutoff"));

    // Animation
    s.animation.cursor = st.read<bool>(Animation, QStringLiteral("cursor"));
    s.animation.scroll = st.read<bool>(Animation, QStringLiteral("scroll"));
    s.animation.splits = st.read<bool>(Animation, QStringLiteral("splits"));
    s.animation.splitCurve = st.read<QString>(Animation, QStringLiteral("splitCurve")).trimmed();
    s.animation.splitDuration = st.read<int>(Animation, QStringLiteral("splitDuration"));
    s.animation.overviewCurve = st.read<QString>(Animation, QStringLiteral("overviewCurve")).trimmed();
    s.animation.overviewDuration = st.read<int>(Animation, QStringLiteral("overviewDuration"));
    s.animation.tabCurve = st.read<QString>(Animation, QStringLiteral("tabCurve")).trimmed();
    s.animation.tabDuration = st.read<int>(Animation, QStringLiteral("tabDuration"));
    s.animation.paneEffectDuration = st.read<int>(Animation, QStringLiteral("paneEffectDuration"));
    s.animation.overlayCurve = st.read<QString>(Animation, QStringLiteral("overlayCurve")).trimmed();
    s.animation.overlayDuration = st.read<int>(Animation, QStringLiteral("overlayDuration"));
    s.animation.cursorCurve = st.read<QString>(Animation, QStringLiteral("cursorCurve")).trimmed();
    s.animation.cursorDuration = st.read<int>(Animation, QStringLiteral("cursorDuration"));
    s.animation.scrollCurve = st.read<QString>(Animation, QStringLiteral("scrollCurve")).trimmed();
    s.animation.scrollDuration = st.read<int>(Animation, QStringLiteral("scrollDuration"));
    s.animation.bellCurve = st.read<QString>(Animation, QStringLiteral("bellCurve")).trimmed();
    s.animation.bellDuration = st.read<int>(Animation, QStringLiteral("bellDuration"));

    // Shaders
    auto shaderList = [&strings](const char* key) { return strings(Shaders, QLatin1String(key)); };
    auto shaderRef = [&st](const char* key) { return st.read<QString>(Shaders, QLatin1String(key)).trimmed(); };
    s.shaders = {.background = shaderList("background"),
                 .text = shaderList("text"),
                 .cursor = shaderList("cursor"),
                 .pane = shaderList("pane"),
                 .window = shaderList("window"),
                 .overview = shaderRef("overview"),
                 .tabTransition = shaderRef("tabTransition"),
                 .paneEffects = shaderRef("paneEffects"),
                 .overlayTransition = shaderRef("overlayTransition")};
    s.shaderParams = mapValue(Shaders, QStringLiteral("params"));
    for (auto it = s.shaderParams.begin(); it != s.shaderParams.end();) {
        if (it.value().typeId() == QMetaType::QVariantMap) {
            ++it;
            continue;
        }
        // A number or a list would otherwise set no parameter, without a word.
        problem(QStringLiteral("Shaders.params.%1 must be an object of parameter values").arg(it.key()));
        it = s.shaderParams.erase(it);
    }

    // Hints: checked once here, not on every show_hints, which uses only the
    // entries that pass.
    s.hintAlphabet = st.read<QString>(HintsGroup, QStringLiteral("alphabet"));
    const QVariantList patterns = st.read<QVariantList>(HintsGroup, QStringLiteral("patterns"));
    if (patterns.size() > MaxHintPatterns) {
        problem(QStringLiteral("Hints.patterns has %1 entries. Afterglow uses the first %2").arg(patterns.size()).arg(MaxHintPatterns));
    }
    s.hintPatterns.clear();
    for (qsizetype i = 0; i < patterns.size() && i < MaxHintPatterns; ++i) {
        const QString where = QStringLiteral("Hints.patterns[%1]").arg(i);
        if (patterns[i].typeId() != QMetaType::QVariantMap) {
            problem(where + QStringLiteral(" must be an object"));
            continue;
        }
        const QVariantMap entry = patterns[i].toMap();
        if (entry.value(QStringLiteral("pattern")).typeId() != QMetaType::QString
            || entry.value(QStringLiteral("pattern")).toString().isEmpty()) {
            problem(where + QStringLiteral(" needs a pattern"));
            continue;
        }
        const QString action = entry.value(QStringLiteral("action"), QStringLiteral("copy")).toString();
        if (action != QLatin1String("copy") && action != QLatin1String("open") && action != QLatin1String("command")) {
            problem(where + QStringLiteral(": unknown action \"%1\". Use copy, open or command").arg(action));
            continue;
        }
        const AfterglowVt::TextSearchResult compiled = AfterglowVt::searchText(
            AfterglowVt::TextSnapshot{}, entry.value(QStringLiteral("pattern")).toString().toStdU32String(), true, std::atomic<bool>{false});
        if (compiled.errorKind == AfterglowVt::SearchError::InvalidPattern || compiled.errorKind == AfterglowVt::SearchError::PatternTooLong) {
            problem(where + QStringLiteral(": the pattern does not compile: %1").arg(QString::fromStdString(compiled.error)));
            continue;
        }
        if (action == QLatin1String("command")) {
            const QVariant command = entry.value(QStringLiteral("command"));
            const QVariantList arguments = command.toList();
            const bool allStrings = std::all_of(arguments.begin(), arguments.end(),
                                                [](const QVariant& a) { return a.typeId() == QMetaType::QString; });
            if (command.typeId() != QMetaType::QVariantList || arguments.isEmpty() || arguments.size() > MaxHintCommandArguments
                || !allStrings) {
                problem(where + QStringLiteral(": command must be a list of 1 to %1 strings").arg(MaxHintCommandArguments));
                continue;
            }
            // The first argument is the program, which a hint could never start.
            if (arguments.first().toString().trimmed().isEmpty()) {
                problem(where + QStringLiteral(": the first argument of command must name a program"));
                continue;
            }
        }
        s.hintPatterns << entry;
    }

    // Keybinds
    QStringList errors;
    const QVariantMap bindings = mapValue(Keys, QStringLiteral("bindings"));
    s.keybinds = Keybinds::fromMap(bindings, &errors);
    s.keyModes = KeyModes::fromConfig(bindings, mapValue(Keys, QStringLiteral("tables")),
        st.read<QString>(Keys, QStringLiteral("leader")), st.read<int>(Keys, QStringLiteral("sequenceTimeout")),
        st.read<int>(Keys, QStringLiteral("tableTimeout")), s.keybinds, &errors);
    for (const QString& e : std::as_const(errors)) {
        problem(QStringLiteral("Keybinds: ") + e);
    }

    // Logged when it starts: a reload or a theme change that keeps a problem
    // does not repeat it.
    for (const QString& p : std::as_const(problems)) {
        if (!m_readProblems.contains(p)) qCWarning(lcConfig).noquote() << p;
    }
    m_readProblems = problems;
    return s;
}

QJsonObject SettingsStore::reference()
{
    const auto schema = buildSchema();
    QJsonObject groups;
    for (auto group = schema.groups.cbegin(); group != schema.groups.cend(); ++group) {
        QJsonObject keys;
        for (const auto& key : group.value()) {
            QJsonObject entry{{QStringLiteral("default"), QJsonValue::fromVariant(key.defaultValue)},
                              {QStringLiteral("description"), key.description}};
            QJsonArray choices;
            for (const auto& choice : key.choices) choices.append(QJsonValue::fromVariant(choice.value));
            if (!choices.isEmpty()) entry.insert(QStringLiteral("choices"), choices);
            if (key.validator && (key.expectedType == QMetaType::Int || key.expectedType == QMetaType::Double)) {
                const QVariant lo = key.expectedType == QMetaType::Int ? QVariant(std::numeric_limits<int>::min()) : QVariant(-1e100);
                const QVariant hi = key.expectedType == QMetaType::Int ? QVariant(std::numeric_limits<int>::max()) : QVariant(1e100);
                QVariant minimum = key.validator(lo);
                const QVariant maximum = key.validator(hi);
                if (key.expectedType == QMetaType::Double) {
                    // A value of its own below the range (Font.size 0, the
                    // desktop's size) is no part of it: the range is what
                    // the values just above that one clamp to.
                    const double above = std::nextafter(minimum.toDouble(), maximum.toDouble());
                    if (const QVariant clamped = key.validator(above); clamped.toDouble() != above) minimum = clamped;
                }
                entry.insert(QStringLiteral("minimum"), QJsonValue::fromVariant(minimum));
                entry.insert(QStringLiteral("maximum"), QJsonValue::fromVariant(maximum));
            }
            keys.insert(key.key, entry);
        }
        groups.insert(group.key(), keys);
    }
    return {{QStringLiteral("schema_version"), schema.version}, {QStringLiteral("groups"), groups}};
}

} // namespace Afterglow
