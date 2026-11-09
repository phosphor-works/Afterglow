// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Sprites.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace AfterglowRender::Sprites {

namespace {

// Box drawing U+2500-U+257F as arms: up, right, down, left.
// '.' none, 'l' light, 'h' heavy, 'd' double. Entries starting with '*' are
// drawn by special cases (dashes, arcs, diagonals).
constexpr const char* BoxArms[128] = {
    ".l.l", ".h.h", "l.l.", "h.h.", "*", "*", "*", "*", "*", "*", "*", "*", ".ll.", ".hl.", ".lh.", ".hh.", // 2500
    "..ll", "..lh", "..hl", "..hh", "ll..", "lh..", "hl..", "hh..", "l..l", "l..h", "h..l", "h..h", "lll.", "lhl.", "hll.", "llh.", // 2510
    "hlh.", "hhl.", "lhh.", "hhh.", "l.ll", "l.lh", "h.ll", "l.hl", "h.hl", "h.lh", "l.hh", "h.hh", ".lll", ".llh", ".hll", ".hlh", // 2520
    ".lhl", ".lhh", ".hhl", ".hhh", "ll.l", "ll.h", "lh.l", "lh.h", "hl.l", "hl.h", "hh.l", "hh.h", "llll", "lllh", "lhll", "lhlh", // 2530
    "hlll", "llhl", "hlhl", "hllh", "hhll", "llhh", "lhhl", "hhlh", "lhhh", "hlhh", "hhhl", "hhhh", "*", "*", "*", "*", // 2540
    ".d.d", "d.d.", ".dl.", ".ld.", ".dd.", "..ld", "..dl", "..dd", "ld..", "dl..", "dd..", "l..d", "d..l", "d..d", "ldl.", "dld.", // 2550
    "ddd.", "l.ld", "d.dl", "d.dd", ".dld", ".ldl", ".ddd", "ld.d", "dl.l", "dd.d", "ldld", "dldl", "dddd", "*", "*", "*", // 2560
    "*", "*", "*", "*", "...l", "l...", ".l..", "..l.", "...h", "h...", ".h..", "..h.", ".h.l", "l.h.", ".l.h", "h.l.", // 2570
};

struct Canvas
{
    QImage& img;
    int w;
    int h;

    void fill(int x0, int y0, int x1, int y1, int alpha = 255)
    {
        x0 = std::clamp(x0, 0, w);
        x1 = std::clamp(x1, 0, w);
        y0 = std::clamp(y0, 0, h);
        y1 = std::clamp(y1, 0, h);
        for (int y = y0; y < y1; ++y) {
            uchar* line = img.scanLine(y);
            for (int x = x0; x < x1; ++x) {
                line[x] = uchar(std::max<int>(line[x], alpha));
            }
        }
    }
};

void drawBox(Canvas& c, const char* arms, int t)
{
    const char up = arms[0];
    const char right = arms[1];
    const char down = arms[2];
    const char left = arms[3];
    auto width = [t](char a) { return a == 'h' ? t * 2 : a == '.' ? 0 : t; };

    // Centre lines. Odd cell sizes centre exactly; even ones bias up/left.
    const int cx = c.w / 2;
    const int cy = c.h / 2;

    // Double strokes sit one stroke width either side of the centre.
    const int vl = cx - t - t / 2; // left vertical stroke x
    const int vr = cx + t - t / 2; // right vertical stroke x
    const int ht = cy - t - t / 2; // top horizontal stroke y
    const int hb = cy + t - t / 2; // bottom horizontal stroke y

    const bool upD = up == 'd';
    const bool downD = down == 'd';
    const bool leftD = left == 'd';
    const bool rightD = right == 'd';
    const bool anyVD = upD || downD;
    const bool anyHD = leftD || rightD;
    const int vWidth = std::max(width(up == 'd' ? '.' : up), width(down == 'd' ? '.' : down));
    const int hWidth = std::max(width(left == 'd' ? '.' : left), width(right == 'd' ? '.' : right));

    // Horizontal arms.
    if (left == 'd') {
        const int topEnd = upD ? vl + t : downD ? vr + t : (vWidth ? cx + vWidth / 2 + 1 : cx);
        const int botEnd = downD ? vl + t : upD ? vr + t : (vWidth ? cx + vWidth / 2 + 1 : cx);
        c.fill(0, ht, topEnd, ht + t);
        c.fill(0, hb, botEnd, hb + t);
    } else if (left != '.') {
        const int w = width(left);
        const int end = anyVD ? vr + t : (vWidth ? cx - vWidth / 2 + vWidth : cx - w / 2 + w);
        c.fill(0, cy - w / 2, end, cy - w / 2 + w);
    }
    if (right == 'd') {
        const int topStart = upD ? vr : downD ? vl : (vWidth ? cx - vWidth / 2 : cx);
        const int botStart = downD ? vr : upD ? vl : (vWidth ? cx - vWidth / 2 : cx);
        c.fill(topStart, ht, c.w, ht + t);
        c.fill(botStart, hb, c.w, hb + t);
    } else if (right != '.') {
        const int w = width(right);
        const int start = anyVD ? vl : (vWidth ? cx - vWidth / 2 : cx - w / 2);
        c.fill(start, cy - w / 2, c.w, cy - w / 2 + w);
    }

    // Vertical arms.
    if (up == 'd') {
        const int leftEnd = leftD ? ht + t : rightD ? hb + t : (hWidth ? cy + hWidth / 2 + 1 : cy);
        const int rightEnd = rightD ? ht + t : leftD ? hb + t : (hWidth ? cy + hWidth / 2 + 1 : cy);
        c.fill(vl, 0, vl + t, leftEnd);
        c.fill(vr, 0, vr + t, rightEnd);
    } else if (up != '.') {
        const int w = width(up);
        const int end = anyHD ? hb + t : (hWidth ? cy - hWidth / 2 + hWidth : cy - w / 2 + w);
        c.fill(cx - w / 2, 0, cx - w / 2 + w, end);
    }
    if (down == 'd') {
        const int leftStart = leftD ? hb : rightD ? ht : (hWidth ? cy - hWidth / 2 : cy);
        const int rightStart = rightD ? hb : leftD ? ht : (hWidth ? cy - hWidth / 2 : cy);
        c.fill(vl, leftStart, vl + t, c.h);
        c.fill(vr, rightStart, vr + t, c.h);
    } else if (down != '.') {
        const int w = width(down);
        const int start = anyHD ? ht : (hWidth ? cy - hWidth / 2 : cy - w / 2);
        c.fill(cx - w / 2, start, cx - w / 2 + w, c.h);
    }
}

void drawDashes(Canvas& c, bool vertical, bool heavy, int count, int t)
{
    const int w = heavy ? t * 2 : t;
    const int len = vertical ? c.h : c.w;
    // Each dash and each gap needs a pixel; a cell too narrow for that draws
    // the line solid rather than losing dashes.
    if (len < 2 * count) {
        if (vertical) c.fill(c.w / 2 - w / 2, 0, c.w / 2 - w / 2 + w, len);
        else c.fill(0, c.h / 2 - w / 2, len, c.h / 2 - w / 2 + w);
        return;
    }
    for (int i = 0; i < count; ++i) {
        const int start = i * len / count;
        const int end = (i + 1) * len / count;
        const int gap = std::clamp(int(std::lround(double(end - start) * 0.3)), 1, end - start - 1);
        const int a = start + gap / 2;
        const int b = end - (gap - gap / 2);
        if (vertical) {
            c.fill(c.w / 2 - w / 2, a, c.w / 2 - w / 2 + w, b);
        } else {
            c.fill(a, c.h / 2 - w / 2, b, c.h / 2 - w / 2 + w);
        }
    }
}

QPainter& prepare(QPainter& p)
{
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    return p;
}

void drawArc(Canvas& c, char32_t cp, int t)
{
    // Quarter circle between the centres of two cell edges.
    QPainter p(&c.img);
    prepare(p);
    p.setPen(QPen(Qt::black, t, Qt::SolidLine, Qt::FlatCap));
    // Centre of the pixel-snapped straight strokes drawBox() uses, so arcs
    // meet straight lines in the neighbouring cells without a step.
    const int strokeX = c.w / 2 - t / 2;
    const int strokeY = c.h / 2 - t / 2;
    const qreal cx = strokeX + t / 2.0;
    const qreal cy = strokeY + t / 2.0;
    const qreal r = std::min(cx, cy);
    QPainterPath path;
    switch (cp) {
    case 0x256D: // down and right
        path.moveTo(cx, c.h);
        path.lineTo(cx, cy + r);
        path.quadTo(cx, cy, cx + r, cy);
        path.lineTo(c.w, cy);
        break;
    case 0x256E: // down and left
        path.moveTo(cx, c.h);
        path.lineTo(cx, cy + r);
        path.quadTo(cx, cy, cx - r, cy);
        path.lineTo(0, cy);
        break;
    case 0x256F: // up and left
        path.moveTo(cx, 0);
        path.lineTo(cx, cy - r);
        path.quadTo(cx, cy, cx - r, cy);
        path.lineTo(0, cy);
        break;
    default: // 0x2570 up and right
        path.moveTo(cx, 0);
        path.lineTo(cx, cy - r);
        path.quadTo(cx, cy, cx + r, cy);
        path.lineTo(c.w, cy);
        break;
    }
    p.drawPath(path);
}

void drawDiagonals(Canvas& c, bool forward, bool backward, int t)
{
    QPainter p(&c.img);
    prepare(p);
    p.setPen(QPen(Qt::black, t, Qt::SolidLine, Qt::SquareCap));
    if (forward) {
        p.drawLine(QPointF(c.w, 0), QPointF(0, c.h));
    }
    if (backward) {
        p.drawLine(QPointF(0, 0), QPointF(c.w, c.h));
    }
}

void drawBlock(Canvas& c, char32_t cp)
{
    const int w = c.w;
    const int h = c.h;
    auto lower = [&](int eighths) { c.fill(0, h - int(std::lround(h * eighths / 8.0)), w, h); };
    auto leftPart = [&](int eighths) { c.fill(0, 0, int(std::lround(w * eighths / 8.0)), h); };
    auto quad = [&](bool ul, bool ur, bool ll, bool lr) {
        if (ul) {
            c.fill(0, 0, w / 2, h / 2);
        }
        if (ur) {
            c.fill(w / 2, 0, w, h / 2);
        }
        if (ll) {
            c.fill(0, h / 2, w / 2, h);
        }
        if (lr) {
            c.fill(w / 2, h / 2, w, h);
        }
    };
    switch (cp) {
    case 0x2580:
        c.fill(0, 0, w, h / 2);
        return;
    case 0x2588:
        c.fill(0, 0, w, h);
        return;
    case 0x2590:
        c.fill(w / 2, 0, w, h);
        return;
    case 0x2591:
        c.fill(0, 0, w, h, 64);
        return;
    case 0x2592:
        c.fill(0, 0, w, h, 128);
        return;
    case 0x2593:
        c.fill(0, 0, w, h, 192);
        return;
    case 0x2594:
        c.fill(0, 0, w, std::max(1, int(std::lround(h / 8.0))));
        return;
    case 0x2595:
        c.fill(w - std::max(1, int(std::lround(w / 8.0))), 0, w, h);
        return;
    case 0x2596:
        quad(false, false, true, false);
        return;
    case 0x2597:
        quad(false, false, false, true);
        return;
    case 0x2598:
        quad(true, false, false, false);
        return;
    case 0x2599:
        quad(true, false, true, true);
        return;
    case 0x259A:
        quad(true, false, false, true);
        return;
    case 0x259B:
        quad(true, true, true, false);
        return;
    case 0x259C:
        quad(true, true, false, true);
        return;
    case 0x259D:
        quad(false, true, false, false);
        return;
    case 0x259E:
        quad(false, true, true, false);
        return;
    case 0x259F:
        quad(false, true, true, true);
        return;
    default:
        break;
    }
    if (cp >= 0x2581 && cp <= 0x2587) {
        lower(int(cp - 0x2580));
    } else if (cp >= 0x2589 && cp <= 0x258F) {
        leftPart(int(0x2590 - cp));
    }
}

void drawPowerline(Canvas& c, char32_t cp, int t)
{
    QPainter p(&c.img);
    prepare(p);
    const qreal w = c.w;
    const qreal h = c.h;
    QPainterPath path;
    switch (cp) {
    case 0xE0B0: // solid right-pointing triangle
        path.moveTo(0, 0);
        path.lineTo(w, h / 2);
        path.lineTo(0, h);
        path.closeSubpath();
        p.fillPath(path, Qt::black);
        return;
    case 0xE0B2: // solid left-pointing triangle
        path.moveTo(w, 0);
        path.lineTo(0, h / 2);
        path.lineTo(w, h);
        path.closeSubpath();
        p.fillPath(path, Qt::black);
        return;
    case 0xE0B1: // thin right chevron
    case 0xE0B3: { // thin left chevron
        p.setPen(QPen(Qt::black, t, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        const bool rightward = cp == 0xE0B1;
        path.moveTo(rightward ? 0 : w, 0);
        path.lineTo(rightward ? w - t / 2.0 : t / 2.0, h / 2);
        path.lineTo(rightward ? 0 : w, h);
        p.drawPath(path);
        return;
    }
    case 0xE0B4: // solid right half-circle
        path.moveTo(0, 0);
        path.cubicTo(w * 1.33, 0, w * 1.33, h, 0, h);
        path.closeSubpath();
        p.fillPath(path, Qt::black);
        return;
    case 0xE0B6: // solid left half-circle
        path.moveTo(w, 0);
        path.cubicTo(-w * 0.33, 0, -w * 0.33, h, w, h);
        path.closeSubpath();
        p.fillPath(path, Qt::black);
        return;
    case 0xE0B5: // thin right arc
    case 0xE0B7: { // thin left arc
        p.setPen(QPen(Qt::black, t, Qt::SolidLine, Qt::FlatCap));
        if (cp == 0xE0B5) {
            path.moveTo(0, 0);
            path.cubicTo(w * 1.33 - t, 0, w * 1.33 - t, h, 0, h);
        } else {
            path.moveTo(w, 0);
            path.cubicTo(-w * 0.33 + t, 0, -w * 0.33 + t, h, w, h);
        }
        p.drawPath(path);
        return;
    }
    case 0xE0B8: // lower-left triangle
        path.moveTo(0, 0);
        path.lineTo(w, h);
        path.lineTo(0, h);
        break;
    case 0xE0BA: // lower-right triangle
        path.moveTo(w, 0);
        path.lineTo(w, h);
        path.lineTo(0, h);
        break;
    case 0xE0BC: // upper-left triangle
        path.moveTo(0, 0);
        path.lineTo(w, 0);
        path.lineTo(0, h);
        break;
    case 0xE0BE: // upper-right triangle
        path.moveTo(0, 0);
        path.lineTo(w, 0);
        path.lineTo(w, h);
        break;
    case 0xE0B9: // backslash separator
    case 0xE0BF:
        p.setPen(QPen(Qt::black, t, Qt::SolidLine, Qt::SquareCap));
        p.drawLine(QPointF(0, 0), QPointF(w, h));
        return;
    case 0xE0BB: // slash separator
    case 0xE0BD:
        p.setPen(QPen(Qt::black, t, Qt::SolidLine, Qt::SquareCap));
        p.drawLine(QPointF(w, 0), QPointF(0, h));
        return;
    default:
        return;
    }
    path.closeSubpath();
    p.fillPath(path, Qt::black);
}

void drawBraille(Canvas& c, char32_t cp)
{
    // Dot order: 1 2 3 down the left column, 4 5 6 down the right, then 7 and 8.
    static constexpr int Col[8] = {0, 0, 0, 1, 1, 1, 0, 1};
    static constexpr int RowOf[8] = {0, 1, 2, 0, 1, 2, 3, 3};
    const unsigned bits = unsigned(cp - 0x2800);
    const int dot = std::max(1, int(std::lround(std::min(c.w / 4.0, c.h / 8.0) * 1.2)));
    for (int i = 0; i < 8; ++i) {
        if (!(bits & (1u << i))) {
            continue;
        }
        const int x = int(std::lround((Col[i] * 2 + 1) * c.w / 4.0)) - dot / 2;
        const int y = int(std::lround((RowOf[i] * 2 + 1) * c.h / 8.0)) - dot / 2;
        c.fill(x, y, x + dot, y + dot);
    }
}

// --- Symbols for Legacy Computing (U+1FB00) and its Supplement (U+1CC00) ---

// Pixel edge @p k of @p n equal parts of @p size. Neighbouring parts share
// their edge, so a mosaic has no gaps or overlaps.
int edge(int size, int k, int n)
{
    return int(std::lround(double(size) * k / n));
}

// Fills the parts of a grid of @p cols x @p rows that @p mask names, bit 0
// at the upper left, row by row. @p inset shrinks each part on every side:
// the separated mosaics.
void drawGrid(Canvas& c, unsigned mask, int cols, int rows, int inset = 0)
{
    for (int i = 0; i < cols * rows; ++i) {
        if (!(mask & (1u << i))) {
            continue;
        }
        const int col = i % cols;
        const int row = i / cols;
        c.fill(edge(c.w, col, cols) + inset, edge(c.h, row, rows) + inset, edge(c.w, col + 1, cols) - inset,
               edge(c.h, row + 1, rows) - inset);
    }
}

// Fills a rectangle given in @p n parts of the cell on each axis.
void fillParts(Canvas& c, int x0, int y0, int x1, int y1, int n, int alpha = 255)
{
    c.fill(edge(c.w, x0, n), edge(c.h, y0, n), edge(c.w, x1, n), edge(c.h, y1, n), alpha);
}

// Sextants U+1FB00-1FB3B go up in bit order and leave out the three that
// exist as other blocks: the left half (21), the right half (42) and the
// full block (63).
unsigned sextantMask(char32_t cp)
{
    unsigned m = unsigned(cp - 0x1FB00) + 1;
    if (m >= 21) ++m;
    if (m >= 42) ++m;
    return m;
}

// Octants U+1CD00-1CDE5 go up in bit order and leave out the 25 patterns
// that exist as other blocks (halves, quadrants, eighths and the like).
unsigned octantMask(char32_t cp)
{
    static constexpr auto Masks = [] {
        constexpr unsigned Elsewhere[] = {1,   2,   3,   5,   10,  15,  20,  40,  63,  64,  80,  85, 90,
                                          95,  128, 160, 165, 170, 175, 192, 240, 245, 250, 252, 255};
        std::array<std::uint8_t, 230> out{};
        std::size_t n = 0;
        for (unsigned m = 1; m < 256; ++m) {
            if (std::find(std::begin(Elsewhere), std::end(Elsewhere), m) == std::end(Elsewhere)) {
                out[n++] = std::uint8_t(m);
            }
        }
        return out;
    }();
    return Masks[cp - 0x1CD00];
}

// The smooth mosaics U+1FB3C-1FB67: the part of the cell on the side of a
// line that holds a corner. Points are in halves across (0-2) and sixths
// down (0-6): the line ends sit on the edges, the centre and the thirds.
struct Wedge
{
    std::int8_t cornerX, cornerY, ax, ay, bx, by;
};
constexpr Wedge Wedges[] = {
    {0, 6, 0, 4, 1, 6}, // 1FB3C lower left, lower middle left to lower centre
    {0, 6, 0, 4, 2, 6}, // 1FB3D lower left, lower middle left to lower right
    {0, 6, 0, 2, 1, 6}, // 1FB3E lower left, upper middle left to lower centre
    {0, 6, 0, 2, 2, 6}, // 1FB3F lower left, upper middle left to lower right
    {0, 6, 0, 0, 1, 6}, // 1FB40 lower left, upper left to lower centre
    {2, 6, 0, 2, 1, 0}, // 1FB41 lower right, upper middle left to upper centre
    {2, 6, 0, 2, 2, 0}, // 1FB42 lower right, upper middle left to upper right
    {2, 6, 0, 4, 1, 0}, // 1FB43 lower right, lower middle left to upper centre
    {2, 6, 0, 4, 2, 0}, // 1FB44 lower right, lower middle left to upper right
    {2, 6, 0, 6, 1, 0}, // 1FB45 lower right, lower left to upper centre
    {2, 6, 0, 4, 2, 2}, // 1FB46 lower right, lower middle left to upper middle right
    {2, 6, 1, 6, 2, 4}, // 1FB47 lower right, lower centre to lower middle right
    {2, 6, 0, 6, 2, 4}, // 1FB48 lower right, lower left to lower middle right
    {2, 6, 1, 6, 2, 2}, // 1FB49 lower right, lower centre to upper middle right
    {2, 6, 0, 6, 2, 2}, // 1FB4A lower right, lower left to upper middle right
    {2, 6, 1, 6, 2, 0}, // 1FB4B lower right, lower centre to upper right
    {0, 6, 1, 0, 2, 2}, // 1FB4C lower left, upper centre to upper middle right
    {0, 6, 0, 0, 2, 2}, // 1FB4D lower left, upper left to upper middle right
    {0, 6, 1, 0, 2, 4}, // 1FB4E lower left, upper centre to lower middle right
    {0, 6, 0, 0, 2, 4}, // 1FB4F lower left, upper left to lower middle right
    {0, 6, 1, 0, 2, 6}, // 1FB50 lower left, upper centre to lower right
    {0, 6, 0, 2, 2, 4}, // 1FB51 lower left, upper middle left to lower middle right
    {2, 0, 0, 4, 1, 6}, // 1FB52 upper right, lower middle left to lower centre
    {2, 0, 0, 4, 2, 6}, // 1FB53 upper right, lower middle left to lower right
    {2, 0, 0, 2, 1, 6}, // 1FB54 upper right, upper middle left to lower centre
    {2, 0, 0, 2, 2, 6}, // 1FB55 upper right, upper middle left to lower right
    {2, 0, 0, 0, 1, 6}, // 1FB56 upper right, upper left to lower centre
    {0, 0, 0, 2, 1, 0}, // 1FB57 upper left, upper middle left to upper centre
    {0, 0, 0, 2, 2, 0}, // 1FB58 upper left, upper middle left to upper right
    {0, 0, 0, 4, 1, 0}, // 1FB59 upper left, lower middle left to upper centre
    {0, 0, 0, 4, 2, 0}, // 1FB5A upper left, lower middle left to upper right
    {0, 0, 0, 6, 1, 0}, // 1FB5B upper left, lower left to upper centre
    {0, 0, 0, 4, 2, 2}, // 1FB5C upper left, lower middle left to upper middle right
    {0, 0, 1, 6, 2, 4}, // 1FB5D upper left, lower centre to lower middle right
    {0, 0, 0, 6, 2, 4}, // 1FB5E upper left, lower left to lower middle right
    {0, 0, 1, 6, 2, 2}, // 1FB5F upper left, lower centre to upper middle right
    {0, 0, 0, 6, 2, 2}, // 1FB60 upper left, lower left to upper middle right
    {0, 0, 1, 6, 2, 0}, // 1FB61 upper left, lower centre to upper right
    {2, 0, 1, 0, 2, 2}, // 1FB62 upper right, upper centre to upper middle right
    {2, 0, 0, 0, 2, 2}, // 1FB63 upper right, upper left to upper middle right
    {2, 0, 1, 0, 2, 4}, // 1FB64 upper right, upper centre to lower middle right
    {2, 0, 0, 0, 2, 4}, // 1FB65 upper right, upper left to lower middle right
    {2, 0, 1, 0, 2, 6}, // 1FB66 upper right, upper centre to lower right
    {2, 0, 0, 2, 2, 4}, // 1FB67 upper right, upper middle left to lower middle right
};

void fillPolygon(Canvas& c, const QPolygonF& polygon, int alpha = 255)
{
    QPainter p(&c.img);
    prepare(p);
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    path.addPolygon(polygon);
    path.closeSubpath();
    p.fillPath(path, QColor(0, 0, 0, alpha));
}

void drawWedge(Canvas& c, const Wedge& wedge)
{
    auto at = [&](int x, int y) { return QPointF(c.w * x / 2.0, c.h * y / 6.0); };
    const QPointF a = at(wedge.ax, wedge.ay);
    const QPointF b = at(wedge.bx, wedge.by);
    auto side = [&](QPointF q) { return (b.x() - a.x()) * (q.y() - a.y()) - (b.y() - a.y()) * (q.x() - a.x()); };
    const bool positive = side(at(wedge.cornerX, wedge.cornerY)) > 0;
    // The cell's rectangle cut by the line (one Sutherland-Hodgman step).
    const QPointF corners[4] = {at(0, 0), at(2, 0), at(2, 6), at(0, 6)};
    QPolygonF out;
    for (int i = 0; i < 4; ++i) {
        const QPointF p = corners[i];
        const QPointF q = corners[(i + 1) % 4];
        const double sp = side(p);
        const double sq = side(q);
        const bool inP = positive ? sp >= 0 : sp <= 0;
        const bool inQ = positive ? sq >= 0 : sq <= 0;
        if (inP) out << p;
        if (inP != inQ) out << p + (q - p) * (sp / (sp - sq));
    }
    fillPolygon(c, out);
}

// Triangles from the centre to one edge: left, upper, right, lower.
enum : unsigned { TriLeft = 1, TriUpper = 2, TriRight = 4, TriLower = 8 };

void drawTriangles(Canvas& c, unsigned edges, int alpha = 255)
{
    const QPointF centre(c.w / 2.0, c.h / 2.0);
    const QPointF ul(0, 0), ur(c.w, 0), lr(c.w, c.h), ll(0, c.h);
    QPainter p(&c.img);
    prepare(p);
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    // One path, so the shared edges of two triangles leave no seam.
    if (edges & TriLeft) path.addPolygon(QPolygonF({ul, centre, ll}));
    if (edges & TriUpper) path.addPolygon(QPolygonF({ul, ur, centre}));
    if (edges & TriRight) path.addPolygon(QPolygonF({ur, lr, centre}));
    if (edges & TriLower) path.addPolygon(QPolygonF({ll, centre, lr}));
    p.fillPath(path, QColor(0, 0, 0, alpha));
}

// The diagonal box drawings U+1FBA0-1FBAE and U+1FBD0-1FBDF: lines through
// the corners, the centres of the edges and the middle of the cell.
//   a b c
//   d e f
//   g h i
// A space starts a new line.
constexpr const char* DiagonalsA0[] = {
    "bd", "bf", "dh", "fh", "bdh", "bfh", "dhf", "dbf", "bd fh", "bf dh", "bfhd", "bdhf", "dbfh", "fbdh", "bfhdb",
};
constexpr const char* DiagonalsD0[] = {
    "fg", "cd", "af", "di", "ah", "bi", "ch", "bg", "aec", "cei", "gei", "aeg", "ahc", "cdi", "gbi", "afg",
};

void drawDiagonalLines(Canvas& c, const char* lines, int t)
{
    QPainter p(&c.img);
    prepare(p);
    // Square caps run past the cell edge and are cut there, so a line meets
    // its neighbour in the next cell without a notch.
    p.setPen(QPen(Qt::black, t, Qt::SolidLine, Qt::SquareCap, Qt::RoundJoin));
    // The centre of the straight strokes of drawBox(), as drawArc() uses.
    const int strokeX = c.w / 2 - t / 2;
    const int strokeY = c.h / 2 - t / 2;
    const qreal cx = strokeX + t / 2.0;
    const qreal cy = strokeY + t / 2.0;
    const qreal xs[3] = {0, cx, qreal(c.w)};
    const qreal ys[3] = {0, cy, qreal(c.h)};
    QPainterPath path;
    bool start = true;
    for (const char* s = lines; *s; ++s) {
        if (*s == ' ') {
            start = true;
            continue;
        }
        const int i = *s - 'a';
        const QPointF point(xs[i % 3], ys[i / 3]);
        if (start) path.moveTo(point);
        else path.lineTo(point);
        start = false;
    }
    p.drawPath(path);
}

// A rectangle in quarters of the cell: x0, y0, x1, y1.
struct Quarters
{
    std::int8_t x0, y0, x1, y1;
};
// U+1CEA0-1CEAF: one quarter of the cell across part of one edge.
constexpr Quarters EdgeQuarters[] = {
    {2, 3, 4, 4}, {1, 3, 4, 4}, {0, 3, 3, 4}, {0, 3, 2, 4}, // lower: right half, right 3/4, left 3/4, left half
    {0, 2, 1, 4}, {0, 1, 1, 4}, {0, 0, 1, 3}, {0, 0, 1, 2}, // left: lower half, lower 3/4, upper 3/4, upper half
    {0, 0, 2, 1}, {0, 0, 3, 1}, {1, 0, 4, 1}, {2, 0, 4, 1}, // upper: left half, left 3/4, right 3/4, right half
    {3, 0, 4, 2}, {3, 0, 4, 3}, {3, 1, 4, 4}, {3, 2, 4, 4}, // right: upper half, upper 3/4, lower 3/4, lower half
};
// U+1FBE4-1FBE7: upper centre, lower centre, middle left, middle right.
constexpr Quarters CentreQuarters[] = {{1, 0, 3, 2}, {1, 2, 3, 4}, {0, 1, 2, 3}, {2, 1, 4, 3}};

enum class Legacy : std::uint8_t {
    None,
    Sextant,
    Wedge,
    Triangles,
    Eighths,
    Shade,
    TriangleShade,
    Diagonals,
    Thirds,
    CentreQuarter,
    SeparatedQuadrant,
    Octant,
    SeparatedSextant,
    Sixteenth,
    EdgeQuarter,
};

// The family of @p cp, or None when the font draws it. One table for
// isSprite() and draw(), so a character that is a sprite is always drawn.
Legacy legacyShape(char32_t cp)
{
    if (cp >= 0x1FB00 && cp <= 0x1FB3B) return Legacy::Sextant;
    if (cp >= 0x1FB3C && cp <= 0x1FB67) return Legacy::Wedge;
    if ((cp >= 0x1FB68 && cp <= 0x1FB6F) || cp == 0x1FB9A || cp == 0x1FB9B) return Legacy::Triangles;
    if (cp >= 0x1FB70 && cp <= 0x1FB8B) return Legacy::Eighths;
    if ((cp >= 0x1FB8C && cp <= 0x1FB92) || cp == 0x1FB94) return Legacy::Shade;
    if (cp >= 0x1FB9C && cp <= 0x1FB9F) return Legacy::TriangleShade;
    if ((cp >= 0x1FBA0 && cp <= 0x1FBAE) || (cp >= 0x1FBD0 && cp <= 0x1FBDF)) return Legacy::Diagonals;
    if (cp == 0x1FBCE || cp == 0x1FBCF) return Legacy::Thirds;
    if (cp >= 0x1FBE4 && cp <= 0x1FBE7) return Legacy::CentreQuarter;
    if (cp >= 0x1CC21 && cp <= 0x1CC2F) return Legacy::SeparatedQuadrant;
    if (cp >= 0x1CD00 && cp <= 0x1CDE5) return Legacy::Octant;
    if (cp >= 0x1CE51 && cp <= 0x1CE8F) return Legacy::SeparatedSextant;
    if (cp >= 0x1CE90 && cp <= 0x1CE9F) return Legacy::Sixteenth;
    if (cp >= 0x1CEA0 && cp <= 0x1CEAF) return Legacy::EdgeQuarter;
    return Legacy::None;
}

void drawEighths(Canvas& c, char32_t cp)
{
    auto column = [&](int k) { fillParts(c, k, 0, k + 1, 8, 8); }; // k from 0, left to right
    auto row = [&](int k) { fillParts(c, 0, k, 8, k + 1, 8); };   // k from 0, top to bottom
    if (cp <= 0x1FB75) {
        column(int(cp - 0x1FB70) + 1); // VERTICAL ONE EIGHTH BLOCK-2 to -7
        return;
    }
    if (cp <= 0x1FB7B) {
        row(int(cp - 0x1FB76) + 1); // HORIZONTAL ONE EIGHTH BLOCK-2 to -7
        return;
    }
    static constexpr int Upper[] = {2, 3, 5, 6, 7}; // eighths of U+1FB82-1FB86 and U+1FB87-1FB8B
    switch (cp) {
    case 0x1FB7C: column(0); row(7); return; // left and lower
    case 0x1FB7D: column(0); row(0); return; // left and upper
    case 0x1FB7E: column(7); row(0); return; // right and upper
    case 0x1FB7F: column(7); row(7); return; // right and lower
    case 0x1FB80: row(0); row(7); return; // upper and lower
    case 0x1FB81: row(0); row(2); row(4); row(7); return; // HORIZONTAL ONE EIGHTH BLOCK-1358
    default: break;
    }
    if (cp <= 0x1FB86) {
        fillParts(c, 0, 0, 8, Upper[cp - 0x1FB82], 8);
    } else {
        fillParts(c, 8 - Upper[cp - 0x1FB87], 0, 8, 8, 8);
    }
}

void drawShade(Canvas& c, char32_t cp)
{
    // A medium shade is half coverage, as U+2592 draws it. Its inverse
    // covers the other half of each spot, so it draws the same.
    constexpr int Medium = 128;
    switch (cp) {
    case 0x1FB8C: fillParts(c, 0, 0, 1, 2, 2, Medium); return; // left half
    case 0x1FB8D: fillParts(c, 1, 0, 2, 2, 2, Medium); return; // right half
    case 0x1FB8E: fillParts(c, 0, 0, 2, 1, 2, Medium); return; // upper half
    case 0x1FB8F: fillParts(c, 0, 1, 2, 2, 2, Medium); return; // lower half
    case 0x1FB90: fillParts(c, 0, 0, 1, 1, 1, Medium); return; // inverse medium shade
    case 0x1FB91: fillParts(c, 0, 0, 2, 1, 2); fillParts(c, 0, 1, 2, 2, 2, Medium); return;
    case 0x1FB92: fillParts(c, 0, 0, 2, 1, 2, Medium); fillParts(c, 0, 1, 2, 2, 2); return;
    default: fillParts(c, 0, 0, 1, 2, 2, Medium); fillParts(c, 1, 0, 2, 2, 2); return; // 1FB94
    }
}

void drawTriangleShade(Canvas& c, char32_t cp)
{
    const QPointF ul(0, 0), ur(c.w, 0), lr(c.w, c.h), ll(0, c.h);
    static constexpr int Medium = 128;
    switch (cp) {
    case 0x1FB9C: fillPolygon(c, QPolygonF({ul, ur, ll}), Medium); return;
    case 0x1FB9D: fillPolygon(c, QPolygonF({ul, ur, lr}), Medium); return;
    case 0x1FB9E: fillPolygon(c, QPolygonF({ur, lr, ll}), Medium); return;
    default: fillPolygon(c, QPolygonF({ul, lr, ll}), Medium); return; // 1FB9F
    }
}

void drawLegacy(Canvas& c, char32_t cp, Legacy shape, int t)
{
    // The separated mosaics leave a gap of about a line width between parts.
    const int inset = std::max(1, (t + 1) / 2);
    switch (shape) {
    case Legacy::Sextant: drawGrid(c, sextantMask(cp), 2, 3); return;
    case Legacy::Wedge: drawWedge(c, Wedges[cp - 0x1FB3C]); return;
    case Legacy::Triangles: {
        static constexpr unsigned Edges[] = {
            TriUpper | TriRight | TriLower, TriLeft | TriLower | TriRight, TriUpper | TriLeft | TriLower,
            TriLeft | TriUpper | TriRight, TriLeft, TriUpper, TriRight, TriLower,
        };
        drawTriangles(c, cp == 0x1FB9A ? TriUpper | TriLower : cp == 0x1FB9B ? TriLeft | TriRight : Edges[cp - 0x1FB68]);
        return;
    }
    case Legacy::Eighths: drawEighths(c, cp); return;
    case Legacy::Shade: drawShade(c, cp); return;
    case Legacy::TriangleShade: drawTriangleShade(c, cp); return;
    case Legacy::Diagonals:
        drawDiagonalLines(c, cp <= 0x1FBAE ? DiagonalsA0[cp - 0x1FBA0] : DiagonalsD0[cp - 0x1FBD0], t);
        return;
    case Legacy::Thirds: fillParts(c, 0, 0, cp == 0x1FBCE ? 2 : 1, 3, 3); return;
    case Legacy::CentreQuarter: {
        const Quarters& q = CentreQuarters[cp - 0x1FBE4];
        fillParts(c, q.x0, q.y0, q.x1, q.y1, 4);
        return;
    }
    case Legacy::SeparatedQuadrant: drawGrid(c, unsigned(cp - 0x1CC20), 2, 2, inset); return;
    case Legacy::Octant: drawGrid(c, octantMask(cp), 2, 4); return;
    case Legacy::SeparatedSextant: drawGrid(c, unsigned(cp - 0x1CE50), 2, 3, inset); return;
    case Legacy::Sixteenth: drawGrid(c, 1u << (cp - 0x1CE90), 4, 4); return;
    case Legacy::EdgeQuarter: {
        const Quarters& q = EdgeQuarters[cp - 0x1CEA0];
        fillParts(c, q.x0, q.y0, q.x1, q.y1, 4);
        return;
    }
    case Legacy::None: return;
    }
}

} // namespace

bool isSprite(char32_t cp)
{
    return (cp >= 0x2500 && cp <= 0x259F) || (cp >= 0x2800 && cp <= 0x28FF) || (cp >= 0xE0B0 && cp <= 0xE0BF)
        || legacyShape(cp) != Legacy::None;
}

bool draw(char32_t cp, QImage& canvas, int thickness)
{
    canvas.fill(0);
    Canvas c{canvas, canvas.width(), canvas.height()};
    const int t = std::max(1, thickness);

    if (cp >= 0x2500 && cp <= 0x257F) {
        const char* arms = BoxArms[cp - 0x2500];
        if (arms[0] != '*') {
            drawBox(c, arms, t);
            return true;
        }
        switch (cp) {
        case 0x2504:
        case 0x2505:
            drawDashes(c, false, cp == 0x2505, 3, t);
            return true;
        case 0x2506:
        case 0x2507:
            drawDashes(c, true, cp == 0x2507, 3, t);
            return true;
        case 0x2508:
        case 0x2509:
            drawDashes(c, false, cp == 0x2509, 4, t);
            return true;
        case 0x250A:
        case 0x250B:
            drawDashes(c, true, cp == 0x250B, 4, t);
            return true;
        case 0x254C:
        case 0x254D:
            drawDashes(c, false, cp == 0x254D, 2, t);
            return true;
        case 0x254E:
        case 0x254F:
            drawDashes(c, true, cp == 0x254F, 2, t);
            return true;
        case 0x256D:
        case 0x256E:
        case 0x256F:
        case 0x2570:
            drawArc(c, cp, t);
            return true;
        case 0x2571:
            drawDiagonals(c, true, false, t);
            return true;
        case 0x2572:
            drawDiagonals(c, false, true, t);
            return true;
        case 0x2573:
            drawDiagonals(c, true, true, t);
            return true;
        default:
            return false;
        }
    }
    if (cp >= 0x2580 && cp <= 0x259F) {
        drawBlock(c, cp);
        return true;
    }
    if (cp >= 0x2800 && cp <= 0x28FF) {
        drawBraille(c, cp);
        return true;
    }
    if (cp >= 0xE0B0 && cp <= 0xE0BF) {
        drawPowerline(c, cp, t);
        return true;
    }
    if (const Legacy shape = legacyShape(cp); shape != Legacy::None) {
        drawLegacy(c, cp, shape, t);
        return true;
    }
    return false;
}

} // namespace AfterglowRender::Sprites
