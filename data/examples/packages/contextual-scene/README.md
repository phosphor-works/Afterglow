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

Optional `config.json` values are `base_intensity` (default 0.8) and `peak_intensity` (default 1.3).
The package starts disabled on each load. Disable it to release its overrides.
It tracks up to eight windows with bounded jobs and timers.
Phosphor command activity and timestamp impulses remain independent.
The preset editor continues to control quality.
