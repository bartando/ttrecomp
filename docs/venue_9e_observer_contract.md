# Venue `9E1AF02A96682354` observer contract

This module captures immutable title payloads for the
`37F2AEC8A23E44E0 / 9E1AF02A96682354` static-venue family. It publishes them
only after an exact same-frame translated-backend proof. It has no renderer,
route, or suppression path.

## Trace contract

The reference gameplay trace `545407DF_5661.xtr` contains nine logical draws
repeated in three EDRAM tile blocks:

```text
indices: 70, 294, 224, 237, 248, 83, 224, 237, 46
total:   1663
```

Live MAIN coverage observed a culled four-draw, 658-index form. Therefore the
reference count, index total, and order are diagnostics only. The observer
uses dynamic vectors and never treats `9`, `1663`, or that sequence as
readiness conditions.

Every reference draw uses:

```text
VS                              = 37F2AEC8A23E44E0
PS                              = 9E1AF02A96682354
primitive                       = indexed triangle strip (6)
index format                    = big-endian uint16
vertex stride / endian          = 32 / 8-in-32
normalized depth control        = 00700736
normalized color mask           = 00000007
color control                   = 87000005
blend control 0                 = 00010001
rasterizer mode control         = 00018002
color attachment                = RGBA8
depth/stencil                   = matching D24S8 or D32S8
samples / mask                  = 4 / all bits
primitive restart               = disabled
```

This is an opaque RGB-write family. Blend is One/Zero, alpha testing and
alpha-to-mask are disabled, and the alpha channel is masked off. The shader
must not be classified as translucent merely because it computes output
alpha.

The MAIN target proof requires both the late render-pass key `0xE` and raw
Xenos target registers whose independently decoded identity is:

```text
color EDRAM base = 0x400
depth EDRAM base = 0
surface pitch    = 1280
EDRAM mode       = 4 (color + depth)
```

The raw registers and decoded values are retained in the backend contract, so
the three-tile equality proof covers target state as well as pipeline state.

## Immutable title payload

The synchronous title hook accepts only a non-player
`DrawIndexedPrimitive` occurrence with exact mesh bounds, matrices, shader
hashes, declaration, and immutable material payload. This family is submitted
outside the catalog's `DrawModelGeometry` scope in the observed build, so
scope shader/model fields are retained when available but are not eligibility
requirements. A valid low-level bound shader is authoritative. ApplyPass
metadata is used only when that shader stage has no valid bound object; a
present, mismatching bound shader fails closed. Exact same-frame backend
identity still selects the MAIN occurrences and excludes earlier same-shader
submissions with incomplete target state.

The declaration must exactly contain five stream-zero elements:

| Offset | Packed type | Usage | Shader meaning |
| ---: | --- | ---: | --- |
| 0 | `002A23B9` | 0 | float3 position |
| 12 | `001A2387` | 3 | signed 2_10_10_10 normal |
| 16 | `00182886` | 10 | 8_8_8_8 color, zyxw |
| 20 | `002C23A5` | 5 | float2 UV |
| 28 | `001A2387` | 6 | title tangent, unused by this VS |

The snapshot owns:

- a stable double-copy of the complete stride-32 vertex buffer;
- a stable double-copy of the submitted uint16 index range;
- decoded big-endian indices, rejected if any index exceeds the copied vertex
  count;
- semantic owner, material/vtable, model, geometry, LOD, and pass fields when
  the title scope supplies them, plus exact shader-object/hash identity;
- world and world-view-projection matrices;
- the exact declaration probe;
- the slot-zero fetch and descriptor-selected full texture mip chain;
- VS constants `c0-c6` and `c12-c15`;
- PS constants `c20`, `c254`, and `c255`.

The pixel program samples slot zero and computes sampled texture times vertex
color times distance fog. Reference materials use an unsigned tiled 2D DXT1
texture with packed mips, repeat addressing, linear min/mag, point mip
filtering, 2:1 anisotropy, RGBA swizzle `0x688`, and zero LOD bias. Those
sampler semantics are validated; dimensions, addresses, and mip count remain
per-draw payload.

## Publication proof

For title/backend frame sequence `N`, publication requires:

1. a non-empty backend event count divisible into exactly three blocks;
2. blocks two and three exactly repeat block one's draw identities and full
   backend contracts;
3. forward-earliest and reverse-latest ordered joins select the same unique
   title subsequence for every first-block event by primitive, submitted
   count, physical index base, primary vertex base/byte count/endian, and
   exact shader pair;
4. every selected title token has complete immutable geometry, texture,
   constants, declaration, matrices, and material-vtable proof;
5. no selected read, copy, texture, material, sequence, or capacity failure
   occurred.

Extra same-shader title candidates may remain unmatched, but cannot be
reordered or reused. A newer authoritative backend sequence closes the older
frame. Incomplete or ambiguous evidence is rejected and never replaces the
latest valid snapshot.

## Integration boundary

The observer is armed only by:

```text
tabletennis_native_venue_9e_observer=true
```

Title capture is called from the scene draw catalog, backend capture runs
before replacement route selection, and the ledger closes at title Swap. All
entry points return no route and mutate no GPU or guest state. Native serving
must be added later, one proven field at a time, after live telemetry verifies
this snapshot.
