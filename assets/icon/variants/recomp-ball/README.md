# RECOMP ball — preview variant

Separate from the active app icon. This variant uses a 120 mm camera from
the same low side angle, with focus on the ball's matte print at f/11. It has only been
rendered in Eevee; no final Cycles render or icon replacement has been made.

- `preview_eevee.png`: 1024 px preview with transparent icon corners.
- `recomp_ball_preview.blend`: editable scene, with the print packed inside.
- `recomp_ball_print.png`: transparent text-only print, typeset with Futura Condensed ExtraBold.
  TABLE TENNIS curves across the top and RECOMP curves along the bottom.
  There are no rings, paddle graphics, extra markings, or reference-image pixels.

The lettering is mixed into the ball's base color using a front-facing projection,
with matte roughness. It shares the sphere's geometry, normals, light and shadows.
No image generation is needed for this decal; the lettering is deterministic.

Rebuild on macOS (Pillow and the installed Futura font are required for typesetting):

```sh
python3 tools/make_recomp_ball_print.py
blender --background --python-exit-code 1 --python tools/preview_recomp_icon.py
```
