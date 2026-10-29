// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Graphics.h>
#include <AfterglowVt/Parser.h>
#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Unicode.h>
#include <AfterglowVt/afterglowvt_export.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
    /// Decodes PNG data for the kitty graphics protocol into RGBA8. The core
    /// has no image codecs of its own; return false if decoding fails.
    virtual bool decodePng(std::string_view png, std::uint32_t& width, std::uint32_t& height,
                           std::vector<std::uint8_t>& rgba)
    {
        return false;
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
    /// A resize blanked the prompt for the shell to repaint, and the shell has
    /// not finished repainting it (OSC 133;B) yet. Renderers can keep showing
    /// the previous frame meanwhile instead of flashing an empty prompt.
    bool promptRedrawPending() const noexcept { return m_promptRedrawPending; }

    Rgb paletteColor(int index) const noexcept { return m_palette[std::size_t(index & 0xFF)]; }
    Rgb defaultForeground() const noexcept { return m_defaultFg; }
    Rgb defaultBackground() const noexcept { return m_defaultBg; }
    /// Cursor colour set by OSC 12, if any.
    const Rgb* cursorColor() const noexcept { return m_hasCursorColor ? &m_cursorColor : nullptr; }
    Rgb resolve(Color color, bool foreground) const noexcept;

    /// Kitty graphics state of the active screen.
    const GraphicsStore& graphics() const noexcept { return m_graphics[m_modes.altScreen ? 1 : 0]; }
    /// Advances image animations. Returns the next time (steady clock, ms) a
    /// frame is due, or 0 when nothing is animating.
    std::uint64_t advanceAnimations(std::uint64_t nowMs);
    int cellWidth() const noexcept { return m_cellWidth; }
    int cellHeight() const noexcept { return m_cellHeight; }

    /// Scrolls the viewport through history. Positive scrolls back.
    void scrollViewport(long rows);
    void resetViewport();

private:
    friend class Parser<Terminal>;

    // Parser handler interface.
    void print(char32_t cp);
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
    void eraseDisplay(int mode);
    void eraseLine(int mode);
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

    void selectGraphicRendition(const Params& params);
    void setMode(const Params& params, bool priv, bool on);
    void setDecMode(int mode, bool on);
    int decModeState(int mode) const;
    int ansiModeState(int mode) const;
    void reportMode(const Params& params, bool priv);
    void deviceStatusReport(const Params& params, bool priv);
    void windowOp(const Params& params);
    void setCursorStyle(int ps);

    void oscColor(int which, std::string_view spec, bool bel);
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
    CursorShape m_cursorShape = CursorShape::Block;
    Region m_margins;
    std::vector<bool> m_tabStops;
    Charset m_charsets[4] = {};
    int m_gl = 0;

    std::array<Rgb, 256> m_palette{};
    Rgb m_defaultFg;
    Rgb m_defaultBg;
    Rgb m_cursorColor;
    bool m_hasCursorColor = false;

    /// Shell integration (OSC 133): what the cursor is in, and whether the
    /// shell repaints its prompt after a resize (redraw=0 opts out).
    enum class SemanticZone : std::uint8_t { Output, Prompt, Input };
    SemanticZone m_semanticZone = SemanticZone::Output;
    bool m_shellRedrawsPrompt = true;
    bool m_promptRedrawPending = false;
    void semanticPrompt(std::string_view args);
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

    // Kitty keyboard flag stacks, one per screen as the protocol requires.
    std::vector<std::uint8_t> m_kittyStack[2];
    void kittyKeyboard(const Params& params, char marker);
    void syncKittyFlags();

    std::uint64_t m_generation = 0;
};

} // namespace AfterglowVt
