# Vivid plain-ball scene

An Eevee color preview of the selected `tabletennis_icon_3d.png` composition.
The camera, plain ball, paddle and net geometry come from the selected Blender scene.

The preview has a stronger red rubber edge, warm amber backdrop and higher contrast.
A material AOV masks the green tabletop for a 1.3× saturation adjustment, keeping
the white ball, white table lines, wood and backdrop outside that color adjustment.

- `preview_eevee.png`: the original 1024 px preview.
- `vivid_preview.blend`: editable scene with the selective saturation compositor.

Regenerate with:

```sh
blender --background --python-exit-code 1 --python tools/render_vivid_icon.py
```

This is now the active app icon; see `assets/icon/README.md` for the exported files.
