<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Bidirectional text

Afterglow supports opt-in bidirectional layout on the primary terminal screen. Enable it in your configuration:

```json
{
  "Terminal": {
    "bidi": "auto"
  }
}
```

The choices are `off` (the default), `auto` (infer paragraph direction), `ltr` and `rtl` (force the paragraph's base direction). FriBidi resolves direction across soft-wrapped rows; hard line breaks start new paragraphs. HarfBuzz shapes directional runs, including right-to-left runs. Reordering preserves grapheme groups and the two cells of wide characters.

The terminal retains logical text and application coordinates. A snapshot records both logical-to-visual and visual-to-logical column mappings. Rendering, cursors, pointer hit testing, hyperlink activation, selection geometry, accessibility character rectangles and IME cursor rectangles use those mappings. Copying, terminal text queries and shell semantic regions return logical text. Bidi changes presentation; it does not rewrite shell input or terminal output.

## Application grids and bounds

Reordering is disabled on the alternate screen and whenever terminal mouse tracking is enabled. Paragraphs containing multicell text, double-width/double-height lines or captured inline images also keep their grid order. This protects application-controlled layouts rather than assuming every row is prose.

Paragraph processing is bounded to 65,536 Unicode characters, including combining characters. Paragraphs exceeding that bound keep their logical grid order, including their visible continuation rows. Large retained scrollback does not require an unbounded paragraph scan. Layout remains constrained to terminal rows and cells; this is not a proportional-text editor or a general document layout engine.

When bidi is enabled on an eligible primary screen, incoming directional controls (`U+061C`, `U+200E–U+200F`, `U+202A–U+202E`, `U+2066–U+2069`) occupy a cell and are revealed with a diamond. Their original codepoints remain in copied text and participate in direction resolution. This makes direction changes visible in source listings. Enabling bidi cannot recover leading controls discarded before it was enabled; clear and redraw existing content when changing the mode.

## Verification and a manual check

`Bidi.*` regressions cover Hebrew ordering, logical/visual round trips, wide characters, combining marks, soft-wrapped paragraph context and application-grid opt-outs. `SnapshotCursor.*` covers extra-cursor colors; terminal regressions cover directional-control retention and logical copy. These checks do not certify every shell, editor, font or IME combination.

With `bidi: "auto"`, run this in the primary screen, resize the window, select and copy the output, and compare the copied logical text:

```sh
printf 'English: abc | Hebrew: שלום עולם | Arabic: مرحبا بالعالم | 123 (test)\n'
printf 'Mixed: א界ב é 123 🙂\n'
```

Also check an Arabic-capable font and your actual IME. Full-screen editors retain their application grid when they enter the alternate screen or enable mouse reporting.
