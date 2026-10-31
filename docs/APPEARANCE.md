<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Settings and appearance

## Configuration file

Afterglow reads `$XDG_CONFIG_HOME/afterglow/config.json`.
The usual path is `~/.config/afterglow/config.json`.
Afterglow reloads this file when it changes.

The [configuration reference](CONFIGURATION.md) lists all keys, defaults and ranges.
Group names and key names are case-sensitive.

```json
{
  "Font": {"size": 12, "antialiasing": "grayscale"},
  "Colors": {"theme": "afterglow-dark"},
  "Terminal": {"shellIntegration": true, "scrollback": 10000},
  "Window": {"padding": 8}
}
```

## Launch options

| Option | Function |
| --- | --- |
| `-o Group.key=value` | Changes one setting for this launch |
| `--config` | Reads another configuration file |
| `--class` | Sets the Wayland application ID for window rules |
| `--title` | Sets a fixed window title |
| `--hold` | Keeps the window open after an `-e` program stops |

```sh
afterglow -o Font.size=14 -o Colors.theme=afterglow-light
```

The first three options apply to the complete process.
A launch with these options starts a separate process.
Other launches can use an existing process that runs the same executable file.
A development build or replacement executable starts a separate process.

## Desktop controls

Tabs, menus, prompts and search use the desktop Qt style, palette and font.
On Plasma, this requires `qqc2-desktop-style` and platform theme integration.
`QT_QUICK_CONTROLS_STYLE` can select another installed style.
Terminal fonts and colors have separate settings.

## Font rendering

| Setting | Values | Default |
| --- | --- | --- |
| `Font.antialiasing` | `grayscale`, `none`, `rgb`, `bgr` | `grayscale` |
| `Font.hinting` | `none`, `light`, `full` | `light` |
| `Font.gamma` | 0.5–3.0 | 1.0 |

`none` gives monochrome glyph edges.
Light hinting aligns glyphs vertically. Full hinting aligns both axes.
RGB and BGR refer to the horizontal order of display subpixels.
Select the order that agrees with your display.

```sh
afterglow -o Font.antialiasing=rgb -o Font.gamma=1.2
```

Subpixel modes use separate filtered coverage for each color channel.
Active shaders or window transparency cause an automatic change to grayscale text.
A transparent pane background also uses grayscale text.
An opaque window without shaders can use subpixel text again.
Grayscale can give better results when the compositor scales or rotates the output.

Gamma changes glyph coverage with `coverage^(1/gamma)`.
It does not change display gamma or scene color space.
Higher values make antialiased edges stronger. A value of 1.0 keeps the standard coverage.
Monochrome edges remain binary. Color emoji keep their original pixels.
Procedural box and powerline shapes use their own antialiasing with coverage gamma.
Qt Quick renders the controls separately from terminal cells.

## Themes

`Colors.theme` selects a theme. Theme colors replace the values in `Colors`.
An empty theme name lets explicit `Colors` values apply.

Afterglow first searches `~/.config/afterglow/themes`.
It then searches the installed `share/afterglow/themes` directory.
The supplied themes are:

| Family | Themes |
| --- | --- |
| Afterglow | `afterglow-dark`, `afterglow-light` |
| Ayu | `ayu-dark`, `ayu-mirage`, `ayu-light` |
| Catppuccin | `catppuccin-mocha`, `catppuccin-macchiato`, `catppuccin-frappe`, `catppuccin-latte` |
| Dracula | `dracula` |
| Everforest | `everforest-dark`, `everforest-light` |
| GitHub | `github-dark`, `github-light` |
| Gruvbox | `gruvbox-dark`, `gruvbox-light` |
| Kanagawa | `kanagawa-wave`, `kanagawa-dragon`, `kanagawa-lotus` |
| Monokai | `monokai` |
| Nord | `nord`, `nord-light` |
| One | `one-dark`, `one-light` |
| Rosé Pine | `rose-pine`, `rose-pine-moon`, `rose-pine-dawn` |
| Solarized | `solarized-dark`, `solarized-light` |
| Tokyo Night | `tokyonight-night`, `tokyonight-storm`, `tokyonight-moon`, `tokyonight-day` |

The third-party palettes come from [iTerm2-Color-Schemes](https://github.com/mbadolato/iTerm2-Color-Schemes) under the MIT license.
Supported formats are Afterglow JSON, Ghostty themes and kitty themes.
Afterglow JSON themes use the `Colors` keys.

Use this value to follow the desktop preference:

```json
{"Colors": {"theme": "light:afterglow-light,dark:nord"}}
```

A theme value can also be a file path.
Afterglow monitors that path for changes, including deletion and replacement.

See [shaders](SHADERS.md) for audio, history and visual effects.
See [bidirectional text](BIDI.md) for text direction settings.
