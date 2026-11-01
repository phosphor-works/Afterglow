// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Sprites.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

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

} // namespace

bool isSprite(char32_t cp)
{
    return (cp >= 0x2500 && cp <= 0x259F) || (cp >= 0x2800 && cp <= 0x28FF) || (cp >= 0xE0B0 && cp <= 0xE0BF);
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
    return false;
}

} // namespace AfterglowRender::Sprites
