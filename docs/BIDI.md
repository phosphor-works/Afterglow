<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Bidirectional text

This page tells users how Afterglow lays out bidirectional text, such as Hebrew and Arabic, and how to check it.
Afterglow has optional bidirectional layout on the main terminal screen.

## Turn on bidirectional layout

Set `Terminal.bidi` in the configuration:

```json
{"Terminal": {"bidi": "auto"}}
```

| Value | Behavior |
| --- | --- |
| `off` | Disable bidirectional layout. This is the default. |
| `auto` | Infer the paragraph direction. |
| `ltr` | Set a left-to-right paragraph direction. |
| `rtl` | Set a right-to-left paragraph direction. |

## Layout and logical text

FriBidi resolves direction across soft-wrapped rows. Hard line breaks start new paragraphs.
HarfBuzz shapes directional runs, including right-to-left runs.
Reordering keeps grapheme groups and both cells of wide characters together.

The terminal keeps logical text and application coordinates.
Snapshots contain mappings between logical and visual columns.
Rendering, cursors, pointer selection, hyperlinks, accessibility rectangles and IME cursor rectangles use these mappings.
Copy, text queries and shell semantic regions return logical text.
Bidirectional layout does not rewrite shell input or terminal output.

## Application grids and bounds

Afterglow keeps the logical grid order when one of these conditions applies:

- The alternate screen is active.
- Terminal mouse tracking is enabled.
- A paragraph contains multicell text.
- A paragraph contains double-width or double-height lines.
- A paragraph contains captured inline images.

Afterglow processes a paragraph of up to 65,536 Unicode characters, including combining characters.
A longer paragraph keeps its logical order, also on its visible continuation rows.
A large scrollback does not cause an unlimited paragraph scan.
The layout stays in the terminal rows and cells.

On eligible main screens, directional controls occupy one cell and appear as a diamond.
These controls are `U+061C`, `U+200E–U+200F`, `U+202A–U+202E` and `U+2066–U+2069`.
Their original codepoints stay in copied text and take part in direction resolution.
Thus, direction changes are visible in source listings.
When you turn on the mode, Afterglow cannot recover leading controls that it discarded before.
After a mode change, clear and redraw the existing content.

## Verification and a manual check

`Bidi.*` tests cover Hebrew order, mapping round trips, wide characters, combining marks, soft wraps and application-grid exclusions.
Terminal tests cover how directional controls stay in the text, and logical copy.
These tests do not cover every shell, editor, font or IME combination.

1. Set `Terminal.bidi` to `auto`.
2. Run these commands on the main screen.

```sh
printf 'English: abc | Hebrew: שלום עולם | Arabic: مرحبا بالعالم | 123 (test)\n'
printf 'Mixed: א界ב é 123 🙂\n'
```

3. Resize the window.
4. Select and copy the output.
5. Compare the copied logical text with the command strings.
6. Repeat with an Arabic-capable font and your IME.

A full-screen editor keeps its grid when it uses the alternate screen or mouse reporting.
