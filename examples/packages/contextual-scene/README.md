<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Contextual Phosphor scene

This Luau package changes the exposure of the Phosphor scene with the history intensity of the current tab and the audio level.

## Install

Install this package with the Phosphor example:

```sh
mkdir -p ~/.config/phosphor-works/afterglow/scripts
cp -r examples/packages/contextual-scene ~/.config/phosphor-works/afterglow/scripts/
```

## Use

Select **Toggle contextual Phosphor exposure** in the command palette.
This is the command `toggle`. Bind it as `script:contextual-scene.toggle` in `Keybinds.bindings`.
From a shell, `aglow run contextual-scene toggle` prints whether the exposure is on or off.
The package samples once each second. The host changes exposure over 900 ms.
The larger value of current-tab history intensity and overall audio level sets the target.
Background tabs do not add history.
A capture that is not available or is silent gives an audio level of zero.
Queries read the existing capture and do not start an analyzer.

## Configuration

Create `config.json` beside `package.json` to change the exposure range:

| Key | Default | Range |
| --- | --- | --- |
| `base_intensity` | 0.8 | 0 to 3 |
| `peak_intensity` | 1.3 | `base_intensity` to 3 |

A value outside its range changes to the nearest limit.
The package starts disabled on each load. Disable it to release its overrides.
It tracks up to eight open windows with one timer.
Phosphor command activity and timestamp impulses stay independent.
The preset editor continues to control quality.
