<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Documentation writing rules

Use ASD-STE100 controlled-English principles for prose.
The reference is [ASD-STE100, Issue 9](https://www.asd-ste100.org/assets/files/ASD-STE100_ISSUE9.pdf).
These repository rules do not certify full compliance with the standard's dictionary.

Keep the root README limited to the product purpose, main supported features and document links.
Put procedures, API details, limits, examples and validation results in the linked documents.

## Sentences and procedures

- Use active voice and present tense where possible.
- Give each term one meaning.
- Use the same term for the same item.
- Keep a procedural sentence to 20 words or fewer.
- Keep a descriptive sentence to 25 words or fewer.
- Give one instruction in each numbered step.
- State the condition before the instruction when the condition affects the action.
- Separate actions from their expected results.
- Use tables for fields, choices, limits and comparisons.
- State limits with units and explain what happens at the limit.
- Preserve exact code, commands, paths, option names and API identifiers.

## Technical terms

Technical names can remain unchanged when ordinary words would lose precision.
Examples include Luau, QRhi, Vulkan, UTF-16, RGBA16F, shader, pane, tab, scrollback and premultiplied alpha.
Explain a new technical term at its first use when the intended reader needs the explanation.
Code and identifiers do not follow ordinary prose spelling rules.

## Verification

1. Check each procedure against the implementation.
2. Check local links and heading anchors.
3. Check configuration examples for valid JSON.
4. Preserve recorded measurements and their test conditions.
5. Regenerate the configuration reference after schema or description changes.
