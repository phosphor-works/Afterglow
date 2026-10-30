// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Clipboard.h>
#include <AfterglowVt/Graphics.h>
#include <AfterglowVt/Notifications.h>
#include <AfterglowVt/ProgramStatus.h>
#include <AfterglowVt/Parser.h>
#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Selection.h>
#include <AfterglowVt/ShellContext.h>
#include <AfterglowVt/Unicode.h>
#include <AfterglowVt/afterglowvt_export.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AfterglowVt {

struct Rgb
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    bool operator==(const Rgb&) const = default;
};

enum class CursorShape : std::uint8_t { Block, Underline, Bar };

enum class MouseTracking : std::uint8_t {
    None,
    X10, ///< 9: press only
    Normal, ///< 1000: press and release
    Button, ///< 1002: plus motion while a button is held
    Any, ///< 1003: all motion
};

enum class MouseFormat : std::uint8_t {
    X10,
    Utf8, ///< 1005
    Sgr, ///< 1006
    Urxvt, ///< 1015
    SgrPixels, ///< 1016
};

struct Modes
{
    bool insert = false; ///< IRM
    bool newline = false; ///< LNM
    bool cursorKeysApp = false; ///< DECCKM
    bool keypadApp = false; ///< DECKPAM
    bool reverseVideo = false; ///< DECSCNM
    bool originMode = false; ///< DECOM
    bool autowrap = true; ///< DECAWM
    bool cursorVisible = true; ///< DECTCEM
    bool cursorBlink = true;
    bool altScreen = false;
    bool leftRightMargins = false; ///< DECLRMM
    bool bracketedPaste = false;
    bool focusEvents = false;
    bool synchronizedOutput = false; ///< 2026
    bool graphemeClustering = true; ///< 2027
    bool altScroll = false; ///< 1007
    bool reverseWrap = false; ///< 45: backspace into the soft-wrapped line above
    bool reverseWrapExtended = false; ///< 1045: backspace wraps across any line, top to bottom
    bool moreFix = false; ///< 41: a tab in the pending-wrap position wraps first
    bool vt52 = false; ///< DECANM reset: VT52 compatibility mode
    bool pasteEvents = false; ///< 5522: pastes arrive as OSC 5522 MIME lists
    bool themeReports = false; ///< 2031: report dark/light changes
    bool visibilityReports = false; ///< 2033: report visibility changes
    bool sizeReports = false; ///< 2048: report resizes in band
    /// xterm modifyOtherKeys: 0 off, 1 keys without a legacy encoding,
    /// 2 every modified key (except Shift with non-letters).
    std::uint8_t modifyOtherKeys = 0;
    bool sixelDisplay = false; ///< DECSDM (80): images at the top left, no scrolling
    bool sixelPrivateColors = true; ///< 1070: each image starts from the default palette
    bool sixelCursorRight = false; ///< 8452: the cursor ends right of an image
    bool allow132 = false; ///< 40: DECCOLM may switch the width
    bool columns132 = false; ///< DECCOLM
    bool noClearOnColumnChange = false; ///< DECNCSM (95)
    bool backarrowSendsBs = false; ///< DECBKM (67)
    bool smoothScroll = false; ///< DECSCLM (4); accepted, scrolling is never slowed
    bool printFormFeed = false; ///< DECPFF (18); accepted, there is no printer
    bool printExtent = false; ///< DECPEX (19); accepted, there is no printer
    MouseTracking mouseTracking = MouseTracking::None;
    MouseFormat mouseFormat = MouseFormat::X10;
    /// Kitty keyboard flags in effect on the active screen (top of its stack).
    std::uint8_t kittyKeyboard = 0;
};

/// Callbacks out of the terminal. Invoked synchronously from Terminal::feed(),
/// on whatever thread feeds it, while the caller holds its terminal lock.
class ITerminalHost
{
public:
    virtual ~ITerminalHost() = default;
    /// Bytes the terminal answers with (device reports, colour queries).
    virtual void writeToPty(std::string_view bytes) = 0;
    virtual void titleChanged(std::string_view) { }
    virtual void workingDirectoryChanged(std::string_view) { }
    virtual void bell() { }
    /// OSC 52 write. @p selection is the target list ("c", "p", "s"...).
    virtual void clipboardWrite(std::string_view selection, std::string_view data) { }
    /// A desktop notification (OSC 9 or OSC 777;notify). @p title may be empty.
    virtual void notify(std::string_view title, std::string_view body) { }
    /// The pointer shape the program asks for (OSC 22), a CSS cursor name;
    /// empty restores the default.
    virtual void pointerShapeChanged(std::string_view shape) { }
    /// Program status records (OSC 7501, or legacy OSC 9;4) changed; read
    /// them through Terminal::programStatus().
    virtual void programStatusChanged() { }
    /// OSC 555: flash the whole terminal.
    virtual void flash() { }
    /// Shell integration (OSC 133): a command began running, as typed after
    /// the prompt, and finished with an exit code (-1 when not reported).
    virtual void commandStarted(std::string_view command) { }
    virtual void commandFinished(int exitCode) { }
    /// A kitty desktop notification (OSC 99) to show, close or poll.
    virtual void desktopNotification(const NotificationRequest& request) { }
    /// What the host's OSC 99 support covers, as the query reply's key=value
    /// list ("a=focus,report:c=1:p=title,body..."); empty for no support.
    virtual std::string notificationCapabilities() { return {}; }
    /// The pane's contexts changed (OSC 3008, iTerm2 RemoteHost); read them
    /// through Terminal::contexts().
    virtual void contextChanged() { }
    /// iTerm2's SetUserVar (OSC 1337): a named value for the host to keep.
    virtual void userVarChanged(std::string_view name, std::string_view value) { }
    /// A kitty clipboard request (OSC 5522): a read to answer (with a prompt)
    /// or a complete write. OSC 52 reads come here too (ClipboardRequest::osc52). Replies go through ClipboardAssembler's encoders.
    /// Returning false means the host has no such clipboard support.
    virtual bool clipboardRequest(const ClipboardRequest& request) { return false; }
    /// Decodes PNG data for the kitty graphics protocol into RGBA8. The core
    /// has no image codecs of its own; return false if decoding fails.
    /// A program asks for a new size in cells (DECCOLM, XTWINOPS 8). Return
    /// true to let the terminal take that size; the host then follows it.
    virtual bool requestResize(int cols, int rows) { return false; }
    /// Size of the display the terminal is on, in pixels, for XTWINOPS
    /// reports and maximum sizes. Return false if unknown.
    virtual bool screenSize(int& width, int& height) { return false; }
    virtual bool decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height,
                           std::vector<std::uint8_t>& rgba)
    {
        return false;
    }
    /// Decodes an image in any format the host reads (iTerm2 inline images:
    /// PNG, JPEG, GIF...) into RGBA8. Defaults to PNG only.
    virtual bool decodeImage(std::string_view data, std::uint32_t& width, std::uint32_t& height,
                             std::vector<std::uint8_t>& rgba)
    {
        return decodePng(data, width, height, rgba);
    }
};

struct TerminalOptions
{
    std::size_t maxScrollback = 10000;
    Rgb defaultForeground{0xd8, 0xd8, 0xd8};
    Rgb defaultBackground{0x1c, 0x1c, 0x1c};
    std::array<Rgb, 16> basePalette{{
        {0x1c, 0x1c, 0x1c}, {0xcc, 0x55, 0x55}, {0x55, 0xcc, 0x55}, {0xcd, 0xcd, 0x55},
        {0x54, 0x55, 0xcb}, {0xcc, 0x55, 0xcc}, {0x7a, 0xca, 0xca}, {0xcc, 0xcc, 0xcc},
        {0x55, 0x55, 0x55}, {0xff, 0x55, 0x55}, {0x55, 0xff, 0x55}, {0xff, 0xff, 0x55},
        {0x55, 0x55, 0xff}, {0xff, 0x55, 0xff}, {0x55, 0xff, 0xff}, {0xff, 0xff, 0xff},
    }};
    /// Cursor colour; unset follows the foreground.
    std::optional<Rgb> cursorColor;
    /// Text under a block cursor; unset takes the background.
    std::optional<Rgb> cursorTextColor;
    /// Selection colours; unset ones invert the cell. OSC 17/19 override them.
    std::optional<Rgb> selectionForeground;
    std::optional<Rgb> selectionBackground;
    /// Bold text in colours 0-7 draws in their bright versions, 8-15.
    bool boldIsBright = false;
    /// Lowest contrast ratio (1-21) between text and its background; text
    /// below it is lightened or darkened when drawn. 1 leaves colours alone.
    double minimumContrast = 1.0;
    /// Characters that end a word for double-click selection, besides spaces.
    std::u32string wordBoundaries{DefaultWordBoundaries};
    /// East Asian Ambiguous characters (Greek, Cyrillic, box drawing, some
    /// symbols) take two cells, as CJK fonts and programs expect.
    bool ambiguousWide = false;
    /// The cursor DECSCUSR 0 and a reset return to.
    CursorShape cursorShape = CursorShape::Block;
    bool cursorBlink = true;
    bool operator==(const TerminalOptions&) const = default;
};

/// The terminal state machine: owns the primary and alternate screens and
/// applies the parsed control stream to them. Not thread-safe; callers lock.
class AFTERGLOWVT_EXPORT Terminal
{
public:
    Terminal(int cols, int rows, ITerminalHost& host, const TerminalOptions& options = {});
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    void feed(std::string_view bytes);

    /// Applies new defaults to a running terminal (live config reload).
    /// Colours set by the application (OSC 4/10/11/12) are replaced.
    void setOptions(const TerminalOptions& options);

    /// @param cellWidth, cellHeight cell size in pixels, reported by CSI 14/16 t.
    void resize(int cols, int rows, int cellWidth, int cellHeight);

    int cols() const noexcept { return m_active->cols(); }
    int rows() const noexcept { return m_active->rows(); }

    Screen& screen() noexcept { return *m_active; }
    const Screen& screen() const noexcept { return *m_active; }
    const Screen& primaryScreen() const noexcept { return *m_primary; }

    const Modes& modes() const noexcept { return m_modes; }
    CursorShape cursorShape() const noexcept { return m_cursorShape; }

    /// Bumped on every state change the renderer could see.
    std::uint64_t generation() const noexcept { return m_generation; }

    const std::string& title() const noexcept { return m_title; }
    const std::string& workingDirectory() const noexcept { return m_cwd; }
    /// True while the cursor is in a shell prompt or its input (OSC 133 A/B, before C).
    bool atShellPrompt() const noexcept { return m_semanticZone != SemanticZone::Output; }
    /// A resize kept the prompt's previous layout for the shell to repaint,
    /// and the shell has not finished yet (OSC 133;B). Renderers can keep showing
    /// the previous frame meanwhile instead of showing a partial repaint.
    bool promptRedrawPending() const noexcept { return m_promptRedrawPending; }
    /// The shell asked (OSC 133 click_events) for clicks on its command line
    /// to move the cursor there, and the user is at that line now.
    bool promptTakesClicks() const noexcept
    {
        return m_promptClickEvents && m_semanticZone == SemanticZone::Input && !m_modes.altScreen;
    }
    const ShellContexts& contexts() const noexcept { return m_contexts; }

    Rgb paletteColor(int index) const noexcept { return m_palette[std::size_t(index & 0xFF)]; }
    Rgb defaultForeground() const noexcept { return m_defaultFg; }
    Rgb defaultBackground() const noexcept { return m_defaultBg; }
    /// Cursor colour set by OSC 12, if any.
    const Rgb* cursorColor() const noexcept
    {
        return m_hasCursorColor ? &m_cursorColor : (m_options.cursorColor ? &*m_options.cursorColor : nullptr);
    }
    /// Text colour under a block cursor, if configured.
    const Rgb* cursorTextColor() const noexcept
    {
        return m_options.cursorTextColor ? &*m_options.cursorTextColor : nullptr;
    }
    Rgb resolve(Color color, bool foreground) const noexcept;
    const TerminalOptions& options() const noexcept { return m_options; }
    /// Cells a character with @p props takes under these options.
    int charWidth(const Unicode::Props& props) const noexcept
    {
        return props.width == 1 && props.ambiguous && m_options.ambiguousWide ? 2 : props.width;
    }

    /// Kitty graphics state of the active screen.
    const GraphicsStore& graphics() const noexcept { return m_graphics[m_modes.altScreen ? 1 : 0]; }

    /// What programs report doing (OSC 7501). Records belong to the terminal,
    /// not a screen.
    const ProgramStatus& programStatus() const noexcept { return m_programStatus; }
    /// The program attached to the terminal exited: in-flight status ends.
    void programExited();
    /// Whether the user can see the terminal (mode 2033 reports changes).
    void setVisible(bool visible);

    enum class TextScope : std::uint8_t {
        Screen, ///< the visible rows of the active screen
        All, ///< the primary screen's history and visible rows
        LastOutput, ///< what the last command printed (OSC 133 C to D)
    };
    /// Plain text; soft-wrapped rows join, trailing blanks are trimmed.
    std::string text(TextScope scope) const;
    /// Selection colours: set by OSC 17/19, else configured; unset ones invert the cell.
    const Rgb* selectionForeground() const noexcept
    {
        return m_hasSelectionFg ? &m_selectionFg
                                : (m_options.selectionForeground ? &*m_options.selectionForeground : nullptr);
    }
    const Rgb* selectionBackground() const noexcept
    {
        return m_hasSelectionBg ? &m_selectionBg
                                : (m_options.selectionBackground ? &*m_options.selectionBackground : nullptr);
    }
    /// Advances image animations. Returns the next time (steady clock, ms) a
    /// frame is due, or 0 when nothing is animating.
    std::uint64_t advanceAnimations(std::uint64_t nowMs);
    int cellWidth() const noexcept { return m_cellWidth; }
    int cellHeight() const noexcept { return m_cellHeight; }

    /// Scrolls the viewport through history. Positive scrolls back.
    void scrollViewport(long rows);
    /// Sets the viewport to @p rows back from the bottom (clamped to history).
    void setViewportOffset(std::size_t rows);
    void resetViewport();

    /// The user's reset: back to the initial state, as RIS.
    void reset();
    /// The user's clear: the screen and its history emptied, the cursor home.
    /// While a full-screen program has the alternate screen, only the main
    /// screen's history goes, as the program owns what is shown.
    void clearScreenAndHistory();

private:
    friend class Parser<Terminal>;

    // Parser handler interface.
    void print(char32_t cp);
    void printAscii(const char* text, std::size_t length);
    void printCodepoints(const char32_t* cps, std::size_t n);
    void execute(std::uint8_t c);
    void escDispatch(const Intermediates& inter, std::uint8_t final);
    void csiDispatch(const Params& params, const Intermediates& inter, std::uint8_t final);
    void oscDispatch(std::string_view payload, bool belTerminated);
    void dcsDispatch(const Params& params, const Intermediates& inter, std::uint8_t final, std::string_view data);
    void apcDispatch(std::string_view payload);

    // Operations.
    Cursor& cursor() noexcept { return m_active->cursor; }
    Region scrollRegion() const noexcept;
    std::uint32_t acquireFill();
    void releaseFill(std::uint32_t id);

    void index();
    void reverseIndex();
    void carriageReturn();
    void lineFeed();
    void backspace();
    void horizontalTab(int count);
    void backTab(int count);
    void setCursor(int row, int col);
    void moveCursorRelative(int dx, int dy);
    /// @p selective: DECSED/DECSEL, which spare DEC-protected cells.
    void eraseDisplay(int mode, bool selective = false);
    void eraseLine(int mode, bool selective = false);
    void eraseSpan(Row& row, int x0, int x1, std::uint32_t fill, bool selective);
    void eraseWholeRow(Row& row, std::uint32_t fill, bool selective);
    bool spares(bool selective) const noexcept;
    void setProtection(bool on, bool iso);
    /// @p stream: a stream extent (DECSACE), whose start column may lie right of its end.
    bool rectFromParams(const Params& params, int first, int& top, int& left, int& bottom, int& right,
                        bool stream = false) const;
    void fillRect(const Params& params);
    void eraseRect(const Params& params, bool selective);
    void copyRect(const Params& params);
    void insertLines(int n);
    void deleteLines(int n);
    void insertChars(int n);
    void deleteChars(int n);
    void eraseChars(int n);
    void scrollUp(int n);
    void scrollDown(int n);
    void repeatLast(int n);
    void setTopBottomMargins(int top, int bottom);
    void setLeftRightMargins(int left, int right);
    void saveCursor();
    void restoreCursor();
    void setAltScreen(bool on, bool saveRestoreCursor, bool clear);
    void fullReset();
    void decaln();
    void insertColumns(int n);
    void deleteColumns(int n);
    void backIndex();
    void forwardIndex();
    void reportDeviceAttributes();
    void softReset();
    void sixel(const Params& params, std::string_view data);
    void reportPresentationState(int which);
    void changeRectAttributes(const Params& params, bool reverse);
    void reportTheme(bool force);
    void reportVisibility();
    void reportSize();
    bool backgroundIsDark() const noexcept;
    void textSizing(std::string_view args);
    void placeMulticell(char32_t cp, std::u32string_view extra, const MulticellParams& params);
    void setLineSize(LineSize size);
    /// Keeps the cursor in the left half of a double-width line.
    void clampToLine();
    int lineRight(int right) const;
    void vt52Dispatch(std::uint8_t final);
    void reverseWrapStep();
    void setColumnMode(bool wide);
    void saveDecModes(const Params& params);
    void restoreDecModes(const Params& params);

    void selectGraphicRendition(const Params& params);
    void setMode(const Params& params, bool priv, bool on);
    void setDecMode(int mode, bool on);
    int decModeState(int mode) const;
    int ansiModeState(int mode) const;
    void reportMode(const Params& params, bool priv);
    void reportRectChecksum(const Params& params);
    void deviceStatusReport(const Params& params, bool priv);
    void windowOp(const Params& params);
    void setCursorStyle(int ps);

    void oscColor(int which, std::string_view spec, bool bel);
    void oscColorControl(std::string_view args, bool bel);
    void pushColors(int slot);
    void popColors(int slot);
    void oscPalette(std::string_view args, bool bel);
    void reply(std::string_view bytes);
    void touch() noexcept { ++m_generation; }

    ITerminalHost& m_host;
    TerminalOptions m_options;
    Parser<Terminal> m_parser;

    std::unique_ptr<Screen> m_primary;
    std::unique_ptr<Screen> m_alternate;
    Screen* m_active;

    Modes m_modes;
    /// DECSCL conformance level, 1 (VT100) to 5 (VT500).
    int m_conformance = 5;
    /// Which erases honour protected cells: the last of DECSCA (DEC) and
    /// SPA (ISO) to protect something decides.
    enum class Protection : std::uint8_t { Off, Dec, Iso };
    Protection m_protection = Protection::Off;
    int m_attributeChangeExtent = 0; ///< DECSACE, reported only
    std::unordered_map<int, bool> m_savedDecModes; ///< XTSAVE
    CursorShape m_cursorShape = CursorShape::Block;
    Region m_margins;
    std::vector<bool> m_tabStops;
    Charset m_charsets[4] = {};
    int m_gl = 0;
    int m_singleShift = 0; ///< SS2/SS3: G2 or G3 for the next character only
    /// VT52 ESC Y: the row and column bytes still to come (2, 1, 0).
    int m_vt52Address = 0;
    int m_vt52Row = 0;

    std::array<Rgb, 256> m_palette{};
    Rgb m_defaultFg;
    Rgb m_defaultBg;
    Rgb m_cursorColor;
    Rgb m_selectionFg;
    Rgb m_selectionBg;
    bool m_hasSelectionFg = false;
    bool m_hasSelectionBg = false;
    bool m_visible = true;

    /// OSC 133 positions on the primary screen, as stable row ids.
    struct Mark
    {
        std::uint64_t row = 0;
        int col = 0;
        bool valid = false;
    };
    Mark m_inputStart; ///< B: where the command is typed
    Mark m_outputStart; ///< C
    Mark m_outputEnd; ///< D
    bool m_commandRunning = false;
    Mark cursorMark() const;
    std::string textBetween(const Mark& from, const Mark& to) const;
    void finishCommand(int exitCode);
    int m_reportedDark = -1; ///< the theme mode last reported (mode 2031)
    bool m_hasCursorColor = false;

    /// XTPUSHCOLORS / kitty's OSC 30001: saved palettes and dynamic colours.
    struct SavedColors
    {
        std::array<Rgb, 256> palette;
        Rgb foreground;
        Rgb background;
        Rgb cursor;
        bool hasCursor;
    };
    static constexpr std::size_t MaxColorStack = 10;
    std::vector<SavedColors> m_colorStack;

    /// Shell integration (OSC 133): what the cursor is in, and whether the
    /// shell repaints its prompt after a resize (redraw=0 opts out).
    enum class SemanticZone : std::uint8_t { Output, Prompt, Input };
    SemanticZone m_semanticZone = SemanticZone::Output;
    bool m_shellRedrawsPrompt = true;
    bool m_promptRedrawPending = false;
    bool m_promptClickEvents = false;
    /// The command line a shell reported (133 cmdline, 633 E) for the next C.
    std::optional<std::string> m_explicitCommand;
    ShellContexts m_contexts;
    /// An iTerm2 MultipartFile under way: its arguments and data so far.
    bool m_iterm2Receiving = false;
    std::string m_iterm2Args;
    std::string m_iterm2Data;
    void semanticPrompt(std::string_view args);
    void vscodeShellIntegration(std::string_view args);
    void iterm2(std::string_view args, bool bel);
    /// Shows an iTerm2 file: @p args as given with File=, @p base64 its data.
    void iterm2File(std::string_view args, std::string_view base64);
    void setWorkingDirectory(std::string_view path);
    std::optional<std::size_t> redrawablePromptStart() const;

    std::string m_title;
    std::vector<std::string> m_titleStack;
    std::string m_cwd;

    int m_cellWidth = 0;
    int m_cellHeight = 0;

    // Grapheme clustering and REP: the last printed cell.
    Unicode::GraphemeSegmenter m_segmenter;
    char32_t m_lastPrinted = 0;
    int m_lastX = -1;
    int m_lastY = -1;

    // Kitty graphics, one store per screen.
    GraphicsStore m_graphics[2];

    ProgramStatus m_programStatus;
    std::array<std::uint32_t, 1024> m_sixelPalette{}; ///< shared registers while mode 1070 is off
    bool m_sixelPaletteReady = false;
    NotificationAssembler m_notifications;
    ClipboardAssembler m_clipboard;

    // Kitty keyboard flag stacks, one per screen as the protocol requires.
    std::vector<std::uint8_t> m_kittyStack[2];
    void kittyKeyboard(const Params& params, char marker);
    void syncKittyFlags();

    std::uint64_t m_generation = 0;
};

} // namespace AfterglowVt
