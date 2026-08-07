# App icon generator

`gen_icon.py` renders the HarmonyOS icon assets — the Mirobody brand mark on a white
rounded tile, the same mark and proportions android's adaptive icon and the Qt desktop
icon use — and writes them in place:

- `AppScope/resources/base/media/app_icon.png` — launcher icon (rounded tile + mark)
- `entry/.../media/background.png` — layered-icon background (solid white; system masks it)
- `entry/.../media/foreground.png` — layered-icon foreground (transparent + centered mark)
- `entry/.../media/startIcon.png` — splash icon (same tile)

## Run

```
python gen_icon.py     # needs Pillow (PIL)
```

The mark's geometry, colors, and proportions are NOT here — they live in
`tools/brandmark.py`, shared with `qt/icon/gen_icon.py`, so a change reaches every
client instead of only this one. The paths there are copied from
`htdoc/src/assets/mirobody.svg`, the single source (see `docs/colors-and-fonts.md` S5).
Output is 1024×1024.
