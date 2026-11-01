---
name: record-lesson
description: "Record what Afterglow work taught, where the next session will look for it. Use it after a bug took more than one attempt to find, when the same mistake happened two times, when a debug or verification technique worked, or when the user corrects how you work."
---

<!--
SPDX-FileCopyrightText: 2026 fuddlesworth
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Record a lesson

The next session does not see a lesson that stays only in the chat. Put the lesson where a session will read it.

| Lesson | Where | Form |
| --- | --- | --- |
| Why code does something that looks wrong (a protocol rule, a driver quirk, a Qt behaviour) | A comment at that code | One or two lines that say why, and name the spec or the bug |
| A bug pattern that a future change could bring back | CLAUDE.md, "Lessons that cost a session" | One bullet: the trap and the rule |
| A step of a procedure that a session missed | The skill that covers it (`ag-build`, `ag-verify-live`, `ag-shader-pack`, `ag-add-setting`, `ag-script-api`) | An imperative line with the reason |
| Something that must never happen | `.claude/hooks/guard.py` | A rule with the message to show |
| A past bug shape in one area | "Known past-bug shapes" in the `.claude/agents/review/ag-*-reviewer.md` of that area | One bullet: what went wrong, what to check |
| A script that worked | `tools/` with a usage comment, or the skill's own directory if only Claude uses it | The scratchpad does not survive the session |
| How the user wants to work | The session memory | With the reason the user gave |

Prove it first. For a rule from one occurrence, write what that occurrence needed, not a general rule. Write the
general rule only when a second case confirms it. When a lesson turns out wrong, correct it. Do not add a second
lesson that contradicts it.

Commit lessons with the work that found them, or alone with a subject like "Note how ...".
