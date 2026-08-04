# App icon generator

`gen_icon.py` renders the Mirobody app icon — a brand-blue (`#3F7FC1` family) gradient
rounded tile with a white heartbeat/ECG pulse — and writes all icon assets in place:

- `AppScope/resources/base/media/app_icon.png` — desktop icon (rounded tile + pulse)
- `entry/.../media/background.png` — layered-icon background (full-bleed gradient; system masks it)
- `entry/.../media/foreground.png` — layered-icon foreground (transparent + centered pulse)
- `entry/.../media/startIcon.png` — splash icon (same tile)

## Run

```
python gen_icon.py     # needs Pillow (PIL)
```

Tweak `TOP`/`BOT` (gradient blues), `pulse_points()` (the ECG shape), or `width_ratio`
(stroke thickness) at the top of the script, then re-run. Output is 1024×1024.
