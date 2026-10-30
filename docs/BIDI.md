<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Bidirectional text

Afterglow supports optional bidirectional layout on the primary terminal screen.
Set `Terminal.bidi` in configuration:

```json
{"Terminal": {"bidi": "auto"}}
```

| Value | Behavior |
| --- | --- |
| `off` | Disable bidirectional layout. This is the default. |
| `auto` | Infer the paragraph direction. |
| `ltr` | Set a left-to-right paragraph direction. |
| `rtl` | Set a right-to-left paragraph direction. |

FriBidi resolves direction across soft-wrapped rows. Hard line breaks start new paragraphs.
HarfBuzz shapes directional runs, including right-to-left runs.
Reordering preserves grapheme groups and both cells of wide characters.

The terminal retains logical text and application coordinates.
Snapshots contain mappings between logical and visual columns.
Rendering, cursors, pointer selection, hyperlinks, accessibility rectangles and IME cursor rectangles use these mappings.
Copy, text queries and shell semantic regions return logical text.
Bidirectional layout does not rewrite shell input or terminal output.

## Application grids and bounds

The following conditions preserve logical grid order:

- The alternate screen is active.
- Terminal mouse tracking is enabled.
- A paragraph contains multicell text.
- A paragraph contains double-width or double-height lines.
- A paragraph contains captured inline images.

Paragraph processing is limited to 65,536 Unicode characters, including combining characters.
Longer paragraphs retain logical order, including their visible continuation rows.
Large scrollback does not cause an unlimited paragraph scan.
Layout remains limited to terminal rows and cells.

On eligible primary screens, directional controls occupy one cell and appear as a diamond.
These controls are `U+061C`, `U+200E–U+200F`, `U+202A–U+202E` and `U+2066–U+2069`.
Their original codepoints remain in copied text and participate in direction resolution.
This makes direction changes visible in source listings.
Enabling the mode cannot recover leading controls discarded earlier.
Clear and redraw existing content after a mode change.

## Verification and a manual check

`Bidi.*` tests cover Hebrew order, mapping round trips, wide characters, combining marks, soft wraps and application-grid exclusions.
`SnapshotCursor.*` tests cover extra-cursor colors.
Terminal tests cover directional-control retention and logical copy.
These tests do not cover every shell, editor, font or IME combination.

1. Set `Terminal.bidi` to `auto`.
2. Run these commands on the primary screen.

```sh
printf 'English: abc | Hebrew: שלום עולם | Arabic: مرحبا بالعالم | 123 (test)\n'
printf 'Mixed: א界ב é 123 🙂\n'
```

3. Resize the window.
4. Select and copy the output.
5. Compare the copied logical text with the command strings.
6. Repeat with an Arabic-capable font and your IME.

Full-screen editors retain their grid when they use the alternate screen or mouse reporting.
