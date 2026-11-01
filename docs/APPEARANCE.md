<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Settings and appearance

## Configuration file

Afterglow reads `$XDG_CONFIG_HOME/afterglow/config.json`.
The usual path is `~/.config/afterglow/config.json`.
Afterglow reloads this file when it changes.
A file that does not parse, or that does not hold a JSON object, keeps the current settings.
A deleted file gives every setting its default.
Afterglow shows a notice and logs a warning for unknown keys and for values that it replaces with a default.

The file holds only the values that differ from the defaults.
Afterglow does not create it on the first start.
Afterglow creates it when it saves a setting for the first time, for example the shaders that a script package suggests.
The `open_config` action creates an empty file if there is no file.
Only your user can read the file that Afterglow creates.

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
| `-e`, `--execute` | Runs the rest of the command line instead of the shell |
| `-d`, `--working-directory` | Starts in the given directory |
| `--quick-terminal` | Shows or hides the quick terminal |

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
The variable applies only when the launch starts a new process.
Terminal fonts and colors have separate settings.

## Fonts

| Setting | Default | Function |
| --- | --- | --- |
| `Font.family` | empty | Font family of regular text. Empty uses the desktop's fixed-width font. |
| `Font.size` | 0 | Size in points, from 4 to 96. 0 uses the size of the desktop's fixed-width font. |
| `Font.features` | `calt`, `liga` | OpenType features in HarfBuzz syntax |
| `Font.fallbackFamilies` | empty | Families that supply missing characters before the system fallback |
| `Font.symbolMap` | empty | Families for codepoint ranges, such as Nerd Font symbols |

If the family is not installed, fontconfig selects the nearest match.
Without a desktop font setting, Afterglow uses the `monospace` alias at 11 points.
Afterglow draws box drawing, block elements, Braille and the Powerline separators itself.
`Font.symbolMap` does not change these characters.

## Font rendering

| Setting | Values | Default |
| --- | --- | --- |
| `Font.antialiasing` | `grayscale`, `none`, `rgb`, `bgr` | `grayscale` |
| `Font.hinting` | `none`, `light`, `full` | `light` |
| `Font.gamma` | 0.5–3.0 | 1.0 |

`Font.antialiasing=none` gives monochrome glyph edges.
Light hinting aligns glyphs vertically. Full hinting aligns both axes.
RGB and BGR refer to the horizontal order of display subpixels.
Select the order that agrees with your display.

```sh
afterglow -o Font.antialiasing=rgb -o Font.gamma=1.2
```

Subpixel modes use separate filtered coverage for each color channel.
Active shaders or window transparency cause an automatic change to grayscale text.
Pane effects are shaders. They run while a pane is remote (`afterglow ssh`) or in another context.
Such a pane makes all text in its window grayscale.
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

`Colors.theme` selects a theme. A theme replaces each color that it sets.
Colors that it leaves out keep their `Colors` values.
A theme without a divider color gets one between its background and text colors.

Afterglow first searches `$XDG_CONFIG_HOME/afterglow/themes`.
It then searches `afterglow/themes` in each XDG data directory, starting with `~/.local/share`.
Last, it searches the supplied themes in the table below.
An `afterglow` binary in a build directory searches the themes of its source tree before the XDG data directories.

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
Supported formats are Afterglow JSON, Ghostty themes and Kitty themes.
An Afterglow JSON theme is one object with color keys.
The keys are `foreground`, `background`, `cursor`, `cursorText`, `selectionForeground`, `selectionBackground` and `divider`.
`searchMatch`, `searchCurrentMatch`, `searchMatchText`, `visualBell`, `visualBellIntensity` and `palette` are also keys.
Afterglow logs other keys in a JSON theme.
A color in a theme or in `Colors` is a Qt color name, such as `#1e1e2e`, `#fff` or `red`.
These colors have no alpha. Afterglow does not accept an eight-digit value such as `#1e1e2eff`.

Use this value to follow the desktop preference:

```json
{"Colors": {"theme": "light:afterglow-light,dark:nord"}}
```

A theme value can also be a file path. Either half of a `light:` and `dark:` value can be a path.
A relative path starts at the directory of the configuration file.
Afterglow monitors that path for changes, including deletion and replacement.
A theme file that does not parse keeps its last colors until it parses again.

## Transparency

| Setting | Default | Function |
| --- | --- | --- |
| `Window.backgroundOpacity` | 1.0 | Opacity of the default background, from 0.05 to 1.0. Text stays opaque. |
| `Window.blur` | `false` | Blurs the content behind a translucent background. This requires KWin. |

## Motion

| Setting | Default | Function |
| --- | --- | --- |
| `Animation.cursor` | `true` | The cursor glides between cells |
| `Animation.scroll` | `true` | Scrolling eases instead of jumping by rows |

`Animation.cursorCurve` and `Animation.scrollCurve` take a Phosphor curve spec, as `Animation.splitCurve` does.

See [shaders](SHADERS.md) for audio, history and visual effects.
See [bidirectional text](BIDI.md) for text direction settings.
