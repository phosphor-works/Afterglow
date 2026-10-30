# Contextual Phosphor scene

Install next to the Phosphor example:

```sh
mkdir -p ~/.config/afterglow/scripts
cp -r data/examples/packages/contextual-scene ~/.config/afterglow/scripts/
```

Run **Toggle contextual Phosphor exposure** from the palette. It samples once per
second and asks the host to ease exposure over 900 ms. The larger of current-tab
history intensity and overall audio level drives the result. Background tabs
contribute no history. Capture unavailable/silent means zero audio contribution;
queries observe existing shader capture and do not start a new analyzer.

Optional config.json overrides `base_intensity` (0.8) and `peak_intensity` (1.3).
The example starts disabled on each load; toggle off to release its overrides.
It tracks up to eight windows and uses existing bounded jobs/timers. Main
Phosphor command heat and timestamp impulses continue independently. Quality
remains under the preset editor's control.
