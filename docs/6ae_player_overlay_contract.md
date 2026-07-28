# 6AE native geometry-probe overlay

`tabletennis_6ae_player_renderer` is a comparison overlay for the immutable
`BBB580AA5620D2A6 / 6AE43640A86B33D8` observer publication. It does not
implement the complete 6AE material and cannot replace or suppress a guest
draw.

The overlay:

- selects one unique vf95/index geometry group from the current exact frame;
- uploads the real 36-byte vf95 payload, decoded 16-bit indices, and current
  raw 28-byte-record vf92 palette;
- uses the generated `tabletennis_player_6ae_geometry_probe` shaders with the
  captured vertex constants c12-c15, c19, c29-c36, c46-c47, and c255. The
  shader preserves
  the exact `BBB580AA5620D2A6` palette-basis reconstruction and the resolved
  clip expression `world.x*c14 + world.y*c12 + world.z*c13 + c15`;
- includes the separate `tabletennis_player_6ae_bbb580_vertex.hlsli` port of
  BBB580 instructions 114-161. It decodes both signed-integer
  `FMT_2_10_10_10` fields and exports the real seven-interpolator contract:
  base UV/material selector, `world.yzx`, the two normalized skinned
  directions, their handed cross-axis, and the c29-c36 affine projections;
- uploads the complete mip chain for guest texture fetch/binding 0
  (`material_textures[2]`, owned by texture fetch slot 0), including its captured view
  swizzle;
- requires the snapshot's independently proven 2x-anisotropic repeat,
  point-mip sampler contract. NRHI cannot express its point mip filter
  separately, so the observer overlay explicitly reports its generic
  anisotropic sampler as `mip_point_approximation=true`; and
- draws one triangle strip with private D32 depth and translucent diagnostic
  tint over the untouched guest output.

The overlay is default-off:

```text
--tabletennis_native_player_6ae_observer_overlay=true
--tabletennis_native_player_6ae_observer_geometry_group=0
```

The overlay switch also arms the capture and backend proof dependency.
Family admission itself remains independent of rendering success.

## Geometry-alignment validation

The first live overlay exposed a detached magenta patch on the arena floor.
Comparison with the dumped `BBB580AA5620D2A6` microcode found that the probe's
claimed c12-c15 transform had the wrong row and axis selection. Guest
instructions 132-137 resolve to:

```text
clip = world.x*c14 + world.y*c12 + world.z*c13 + c15
```

The guest also reconstructs the third quaternion basis column from the first
two columns. The probe now preserves both sequences exactly. A subsequent
muted gameplay run (`/tmp/tt_6ae_alignment_fixed.log`) sustained real overlay
draws through frames 2683-2773; the detached floor patch disappeared and the
probe geometry stayed spatially aligned/occluded with the player in
`/tmp/tt_6ae_alignment_fixed_gameplay_final.png`.

This proves the observer geometry alignment, not full material parity or
serving readiness. The diagnostic run remained capture-bound at roughly
6.3-7.8 FPS and still drew the untouched guest frame underneath.

## BBB580 material-varying port

The geometry probe now computes every vertex output consumed by the dumped
185-instruction `6AE43640A86B33D8` pixel program. This is deliberately a
one-sided milestone: the diagnostic pixel entry point still samples only
guest fetch slot 0 and applies its translucent probe tint.

The port follows the dumped register sequence rather than inventing a generic
tangent basis:

- vf95 byte offsets 20 and 32 are decoded as signed integer 10/10/10/2;
- each direction is skinned, explicitly reordered to the guest's `yzx`
  register order, and normalized independently;
- the third frame axis is `cross(direction1, direction0)` multiplied by the
  offset-32 signed 2-bit handedness value;
- `o0.z` uses the exact `c19.z * world.x > 0` selection between `c46.x` and
  `c46.x + c47.x`; and
- `o5/o6` retain the eight dot-plus-translation operations against c29-c36.

No serving gate consumes these varyings yet. Full pixel-material parity and a
guest/native image comparison are still required before promotion.
