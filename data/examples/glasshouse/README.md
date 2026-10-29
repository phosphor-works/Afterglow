<!--
SPDX-FileCopyrightText: 2026 fuddlesworth
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Glasshouse for afterglow

Afterglow dressed for [Glasshouse](https://github.com/Dsaquel/dots), Dsaquel's
stained-glass Hyprland desktop. The look follows Glasshouse's own shaders and
shell: quarry glazing in its jewels, lit lead came with a keyline beside it, a
lamp above, light added in the dark world and ink laid on in the light one.
The window's frame stays Vitrail's; afterglow fills it.

![Glasshouse in afterglow: split panes in the dark world, a failed command cracking the glass, a long command's glaze with cursor sparks, and the light world](preview.webp)

## What it does

| Piece | Where | |
|---|---|---|
| `glasshouse-glass` | background layer | quarry glazing behind the text, stained by the light behind it and lit by the lamp |
| `glasshouse-gilt` | text layer | the lamp's glow behind the letters; light sweeps over them now and then |
| `glasshouse-ember` | cursor layer | the cursor glows like the lamp; jumps throw sparks |
| `glasshouse-mullion` | pane chain | splits meet at a leaded mullion; focus arriving gilds it behind a hot tip |
| `glasshouse.luau` | script | turns shell events into the shaders' signals |

The text sits on its own layer, so none of the glass ever blurs or tints it.

What the shell does to the glass (with shell integration, OSC 133):

- **A command fails**: the pane cracks from the prompt like struck glass, the
  cracks glowing hot, then cooling to scars, and the alert calls through it.
  The glass mends after a few seconds. Ctrl+C does not count.
- **A long command succeeds** (5 s or more), or a program reports its task done
  (OSC 7501): light sweeps along the lead and over the text.
- **A command runs**: the lamp and keylines warm, and cool when it ends.
- **The bell**: a shock ring rolls out from the cursor and bends the glass.
- **Program status** (OSC 7501): working makes the lamp breathe; blocked and
  error make the glass call in the alert colour.
- A command of 20 s or more finishing in a pane you are not looking at sends a
  notification.

The command palette has *Glasshouse: strike / glaze / ring / heat* to see each
effect, and they bind like any action, for example
`"Keybinds": { "bindings": { "script:glasshouse.strike": "Ctrl+Alt+S" } }`.
Other programs can drive the glass too: `afterglow ctl signal 0 1` strikes the
focused pane (channels below).

## Install

```sh
bin/glasshouse-afterglow install
```

This copies the shader packs and the script into `~/.config/afterglow` and
merges the settings (font, padding, shaders, dark-world colours) into
`config.json`; the rest of your settings stay. Running afterglow windows pick
it up at once.

## Following the world switch

The shaders read the world from the terminal's background, so only the colours
need switching:

```sh
bin/glasshouse-afterglow world dark    # or light
```

To have Glasshouse's `theme-switch.sh` do it, add afterglow's config to the
files it snapshots (so a failed switch rolls it back too):

```bash
MUTABLE=(
    ...
    "${HOME}/.config/afterglow/config.json"
)
```

and switch it beside kitty's theme:

```bash
put "${TMP_KITTY}" "${KITTY_DIR}/theme-active.conf"
glasshouse-afterglow world "${MODE}"
```

## Signals

The shaders listen on afterglow's shader signals (`iSignal`), which scripts
set with `afterglow.signal(pane, channel, value)` and other programs with
`afterglow ctl signal CHANNEL VALUE` or D-Bus `Signal`:

| Channel | Meaning | Value |
|---|---|---|
| 0 | strike | the exit code |
| 1 | glaze | seconds the command took |
| 2 | heat | 1 while running, 0 after |
| 3 | ring | anything |
| 4 | status | 1 working, 2 blocked, 3 error, 0 none |

## Credits

Glasshouse, its palettes and its look are Dsaquel's
([Dsaquel/dots](https://github.com/Dsaquel/dots)). These shaders and the script
are written for afterglow after it; none of Glasshouse's code is copied.
