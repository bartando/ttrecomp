# Blender app icon

The final icon is a Cycles render of editable 3D geometry. It replaces the initial
image-generated concept. The supplied paddle photograph guided the blade outline,
continuous neck, pale flared grip, and dark diagonal accents; the second photograph
guided the low camera, left-hand grip, and off-centre supporting ball. Branding and
lettering are omitted. Neither reference photograph is used as a scene texture.

The table is green, with a complete frame and a woven net mounted to clamps outside
both sidelines. A single 40 mm ball supports the blade near its front edge. The
paddle leans away from the camera; the handle also contacts the table. The support
angle is solved geometrically. The low hero camera is orbited 18 degrees to the
right of the initial frontal view, keeping its aim on the paddle. The soft net and warm arena background are rendered
with real camera depth of field. There are no decorative background balls or lamps.

## Active icon: vivid

The shipped icon is the vivid Eevee grade of this scene (`tools/render_vivid_icon.py`):
deeper red rubber edge, warm amber backdrop, higher contrast, and a material-AOV
mask that saturates only the green tabletop by 1.3×.

- `tabletennis_icon_vivid_master.png`: 2048 × 2048 render with the inset and rounded alpha corners.
- `tabletennis_icon_vivid_square.png`: the same render full-bleed and opaque.
- `tabletennis_icon_vivid.png` / `.ico`: 1024 px desktop icon and Windows sizes 16–256,
  exported from the rounded master. CMake embeds the PNG for the macOS Dock and
  uses the ICO for Windows resources; macOS packaging builds ICNS from the PNG.
- `ps5/game/icon0.png`: 512 × 512 PS5 tile from the square render. The PS5 home
  screen applies its own rounded mask, so a transparent margin shows as padding.

Rebuild (Blender 5.2, Eevee on Metal, ~30 s):

```sh
blender --background --python-exit-code 1 --python tools/render_vivid_icon.py
python3 tools/export_icon.py
```

## Cycles scene files

- `tabletennis_icon.blend`: editable scene, cameras, materials, lights, compositor,
  and packed texture. The hero camera is active; F12 renders the icon. The vivid
  script starts from this file.
- `tabletennis_icon_3d_master.png`, `tabletennis_icon_3d.png`, `tabletennis_icon_3d.ico`:
  the earlier neutral Cycles render, no longer used by the build.
- `textures/rubber_normal.png`: imagegen-created rubber normal map, used as Non-Color
  data on both rubber faces and packed into the blend file.
- `textures/README.md`: exact texture-generation prompt and material settings.

The final picture, geometry, depth of field, and lighting are produced in Blender.
Imagegen was used only for the rubber normal map in this version, as requested.
The image-generation service is not needed to rerender the scene.

To rerender the Cycles scene itself:

```sh
blender --background --python-exit-code 1 --python tools/render_icon.py -- --preview
blender --background --python-exit-code 1 --python tools/render_icon.py -- --final
```

The preview command writes five camera views to `out/icon-blender-review/`.
Add `--hero-only` for a faster camera/composition preview.
The original flat `tabletennis_icon.png` and `.ico` remain as prior artwork.

## Game atmosphere reference

The green table and dark tournament setting were researched from gameplay:
https://en.wikipedia.org/wiki/Rockstar_Games_Presents_Table_Tennis

No retail game texture, logo, character, or branded equipment asset is included.
