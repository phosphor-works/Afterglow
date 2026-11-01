// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QImage>

namespace AfterglowRender::Sprites {

/// Characters drawn procedurally to fill the cell exactly instead of taken
/// from the font: box drawing, block elements, Braille and Powerline
/// separators. Font versions follow the font's own metrics and leave gaps
/// between rows.
bool isSprite(char32_t cp);

/// Draws @p cp into @p canvas (Format_Alpha8, the sprite's cells). @p thickness
/// is the light line width in pixels. Returns false if @p cp is not a sprite.
bool draw(char32_t cp, QImage& canvas, int thickness);

} // namespace AfterglowRender::Sprites
