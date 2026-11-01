<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Contextual Phosphor scene

Install this package with the Phosphor example:

```sh
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/contextual-scene ~/.config/afterglow/scripts/
```

Select **Toggle contextual Phosphor exposure** in the command palette.
The package samples once each second. The host changes exposure over 900 ms.
The larger value of current-tab history intensity and overall audio level sets the target.
Background tabs do not contribute history.
Unavailable or silent capture contributes zero audio.
Queries observe existing capture without starting an analyzer.

Create `config.json` beside `package.json` to change the exposure range:

| Key | Default | Range |
| --- | --- | --- |
| `base_intensity` | 0.8 | 0 to 3 |
| `peak_intensity` | 1.3 | `base_intensity` to 3 |

A value outside its range is clamped to the nearest limit.
The package starts disabled on each load. Disable it to release its overrides.
It tracks up to eight open windows with one timer.
Phosphor command activity and timestamp impulses remain independent.
The preset editor continues to control quality.
