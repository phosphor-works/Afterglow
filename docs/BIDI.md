<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Bidirectional text workstream

Status: scoped compatibility investigation; full bidi remains deferred separately from D1's implemented IME context work.

The renderer currently shapes terminal cell runs with HarfBuzz using `HB_DIRECTION_LTR` in `FrameBuilder::shape` (`GridRenderer.cpp`). The emulator, selection, hyperlinks, mouse reporting and cursor coordinates address logical grid cells. Changing HarfBuzz direction alone would reorder glyphs without updating those mappings and would break terminal application positioning.

A future implementation needs an explicit policy for paragraph boundaries (hard newline versus soft wrap), base direction, application-controlled grid layout, and bidi controls. It must retain a logical-to-visual mapping shared by rendering, pointer hit testing, cursor placement, selection, copy, accessibility and IME rectangles. Terminal applications that already arrange visual cells need an opt-out; alternate-screen TUIs must not silently receive a different coordinate system.

Acceptance fixtures should cover:

- Arabic joining and Hebrew mixed with Latin identifiers, digits and punctuation.
- Wide characters, combining marks and emoji within mixed-direction runs.
- Soft-wrapped paragraphs after resize, with selected/copied text in logical order.
- Clicks and mouse reports on visually reordered hyperlinks and editor cells.
- Shell editing and IME preedit spanning direction boundaries.
- Bidi controls in source listings, with a policy for revealing otherwise invisible direction changes.

Implement a mapping prototype behind an opt-in setting and validate a real shell/editor workflow before claiming bidi support. No new setting is exposed by D1; existing grid behavior remains unchanged.
