<!-- SPDX-FileCopyrightText: 2026 fuddlesworth -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Example shader packs

These packs show what the [shader pass API](../../docs/SHADER-API.md) can do.
Afterglow does not install them. Its own packs are in [data/shaders](../../data/shaders).

To use a pack, copy its directory into the `shaders` directory beside your configuration file:

```sh
mkdir -p ~/.config/afterglow/shaders
cp -r examples/shaders/crt-curve ~/.config/afterglow/shaders/
```

Then name the pack in a [shader chain](../../docs/SHADERS.md#shader-chains).
Afterglow loads the pack without a restart.

| Pack | Use it as | Shows |
| --- | --- | --- |
| [crt-curve](crt-curve) | `Shaders.pane` or `Shaders.window` | A bent screen with a [pointer map](../../docs/SHADER-API.md#pointer-maps). |
| [tilt](tilt) | `Shaders.pane` | A screen in perspective with a pointer map. |
| [feedback-glow](feedback-glow/README.md) | `Shaders.pane` or `Shaders.text` | One buffer pass that keeps bright pixels. |
| [depth-trails](depth-trails/README.md) | `Shaders.background` | Two buffer passes, depth, a vertex stage and an image parameter. |
| [orrery](orrery) | `Shaders.background` | A mesh pass with an object hierarchy. |
| [mesh-reactor](mesh-reactor/README.md) | `Shaders.background` | Instanced geometry with audio, scrollback and script input. |
| [pane-deck](pane-deck) | `Shaders.overview` | The pane overview as a carousel of cards in 3D. |
| [tab-switch](tab-switch) | `Shaders.tabTransition` | A cube, a page flip, a shatter and a slide. |
| [pane-effects](pane-effects) | `Shaders.paneEffects` | A hinge and a picture tube, and a ring and a vortex for a pane that changes host. |

To preview a pack from a built checkout, without a copy, name this directory:

```sh
printf 'hello\n' | build/tools/render/afterglow_renderpng --shader-dir examples/shaders --pane-shader crt-curve - crt-curve.png
```
