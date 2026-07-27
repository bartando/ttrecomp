# Venue `526A35DC94475116` observer contract

This module is an observer. It records value-only title metadata and translated
backend state, proves the same logical draws across all three EDRAM tile
submissions, and publishes a snapshot. It has no renderer or suppression path.

## Trace evidence

The reference gameplay trace `545407DF_5661.xtr` contains 21 backend events,
representing seven logical draws repeated across three EDRAM tiles.

Six logical draws are contiguous at commands 208-213:

```text
indices: 121, 121, 369, 40, 555, 50
VS:      37F2,37F2,37F2,1FCF,37F2,37F2
blend:   00010001
```

The seventh logical draw is command 413:

```text
indices: 34
VS:      37F2
blend:   07060706
```

The blocks repeat at 649-654 and 1090-1095; the late draw repeats at 854 and
1295. This disproves a single contiguous-bucket or single-blend assumption.
Live MAIN coverage independently reported a culled six-draw form: four
`37F2` draws (1,008 indices) and two `1FCF` draws (56 indices). Draw counts,
index counts, order, and the split between the two vertex programs are
therefore dynamic.

Both programs use vertex fetch 95 and big-endian 16-bit indices:

| Backend VS | Stride | Vertex attributes |
| --- | ---: | --- |
| `37F2AEC8A23E44E0` | 32 | float3 position, signed 2_10_10_10 normal, 8_8_8_8 color, float2 UV |
| `1FCFF2D75A7DCA98` | 40 | the same four consumed attributes; ten words per source vertex |

The pixel program has 207 dwords and one texture binding. The reference
textures are tiled 2D resources, mostly DXT1; dimensions and mip ranges vary.
The late draw uses a tiled `8_8_8_8` texture, so texture format and dimensions
are material data, not family identity.

All seven reference draws share:

```text
MAIN pass key               = 0000000E
surface pitch               = 1280
primitive                   = indexed triangle strip (6)
normalized depth control    = 00700736
normalized color mask       = 00000007
color control               = 87000005
rasterizer mode control     = 00018002
color attachment            = RGBA8
depth/stencil               = matching D24S8 or D32S8
samples                     = 4
sample mask                 = all bits
primitive restart           = disabled
```

`PA_SU_SC_MODE_CNTL=00018002` means back-face culling with counter-clockwise
front faces. A future renderer must reproduce both fields; host default winding
is not an adequate substitute.

## Title identity and ownership boundary

The title side is captured at the established synchronous chain:

```text
grmShaderFx::DrawModelGeometry
  -> grmShaderFx::ApplyPass
  -> grmModelGeom draw helper
  -> DrawIndexedPrimitive
```

For every selected draw the observer retains:

- semantic owner token, including renderable and vtable when a proven
  table/ball/paddle scope exists;
- real `grmShaderFx` material pointer and vtable `8202F2DC`;
- model, geometry index, LOD, and alternate-pass bit;
- pass descriptor, program pair, and title shader objects;
- vertex aggregate, declaration, vertex/index aliases and byte bounds;
- texture fetch 0, world matrix, and world-view-projection matrix.

The GPU trace cannot contain guest renderable pointers or vtables. It places
the first six draws between the 14D and E33 venue blocks, while the late draw
appears immediately before player geometry. That ordering is not sufficient
to assert a renderable owner. The observer therefore records the actual live
owner token and logs it only after an exact backend proof; it does not invent
one common renderable contract for the two command regions.

The authoritative title pixel hash comes from the low-level bound shader. The
ApplyPass hash is used only when no bound pixel shader is available. If a
bound shader exists and disagrees, the draw is rejected instead of accepting
stale ApplyPass metadata.

## Same-frame publication proof

Backend callbacks carry the command processor's authoritative
`backend_frame_sequence`. For frame `N`, publication requires:

1. every admitted backend event is Vulkan MAIN state matching the full
   contract above and one of the two trace-proven blend values;
2. the backend event count divides into exactly three non-empty blocks;
3. blocks two and three repeat block one's complete draw identity and backend
   contract;
4. the logical block count equals the same-frame title candidate count;
5. the ordered join matches primitive, submitted count, physical index base,
   pixel hash, and the stride-to-VS mapping (`32 -> 37F2`, `40 -> 1FCF`);
6. all retained material, mesh, matrix, pass, and vtable values are readable.

The physical index address is derived from the selected title buffer alias, so
matching counts alone cannot classify a draw. A newer backend sequence closes
an older title frame. Missing, partial, reordered, stale, or overflowing
evidence fails closed.

## Promotion blockers

The snapshot currently contains contract metadata, not immutable vertex,
index, or texture payloads. Before adding a native renderer:

1. live-proof the published owner/material/model values for both command
   regions;
2. copy both stride-32 and stride-40 vertex payloads plus submitted indices;
3. capture the descriptor-selected full mip chain and sampler state;
4. port the real `526A` pixel program and both vertex programs;
5. validate the opaque and late-blended draws separately in the private
   transaction target;
6. serve only after offscreen output parity, then suppress the corresponding
   guest draws atomically.

No reference draw count or reference index sequence is a serving condition.
