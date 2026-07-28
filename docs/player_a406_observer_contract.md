# Player `A406367569E5F6F8 / 0F9CCE179F32DA36` observer contract

This module captures immutable title payloads for the rigid-player
`A406367569E5F6F8 / 0F9CCE179F32DA36` family and publishes them only after an
exact same-frame translated-backend proof. It has no renderer, route, or
suppression path.

## Trace contract

The reference gameplay trace contains three indexed triangle-list draws per
EDRAM tile:

```text
indices: 1344, 8028, 6684
total:   16056
```

The tile block repeats three times. Live MAIN coverage has also observed a
culled two-draw, 14730-index form. The reference count, index total, order,
player pointers, pass descriptors, and title ordinals are therefore
diagnostics only. The observer uses dynamic vectors and never treats those
values as readiness conditions.

Every accepted translated-backend draw must use:

```text
VS                              = A406367569E5F6F8
PS                              = 0F9CCE179F32DA36
primitive                       = indexed triangle list (4)
index format                    = big-endian uint16
vertex stride / endian          = 96 / 8-in-32
normalized depth control        = 00700736
normalized color mask           = 0000000F
color control                   = 87000005
blend control 0                 = 00010001
rasterizer mode control         = 00018000
color attachment                = RGBA8
depth/stencil                   = matching D24S8 or D32S8
samples / mask                  = 4 / all bits
primitive restart               = disabled
```

The MAIN target proof requires late render-pass key `0xE` and raw Xenos
target registers whose independently decoded identity is:

```text
color EDRAM base = 0x400
depth EDRAM base = 0
surface pitch    = 1280
EDRAM mode       = 4 (color + depth)
```

## Immutable title payload

The synchronous title hook accepts only a player-scoped occurrence with exact
mesh bounds, matrices, pixel shader, declaration, and immutable material
payload. A valid low-level bound shader is authoritative. ApplyPass metadata
is used only when that shader stage has no valid bound object; a present,
mismatching bound pixel shader fails closed.

The title path has been observed with vertex shader `BAAE9101192B33EA`, while
the translated backend identifies the submitted family as
`A406367569E5F6F8`. This is an intentional title/backend boundary: title
eligibility requires a nonzero vertex program and the exact pixel program,
while same-frame publication compares draw identity and the exact pixel hash.
It does not incorrectly require the title vertex hash to equal the backend
vertex hash.

The exact live title declaration has eight elements, maximum stream `3`, and
stream masks `FF0000FF00000000 / 0000000000000000`. The translated backend
shader consumes the first four stream-zero float4 values from `vf95`; the
remaining title elements are retained as exact identity evidence:

| Stream | Offset | Packed type | Usage/index |
| ---: | ---: | --- | --- |
| 0 | 0 | `001A23A6` | 0/0 |
| 0 | 16 | `001A23A6` | 3/0 |
| 0 | 32 | `001A23A6` | 6/0 |
| 0 | 48 | `001A23A6` | 5/0 |
| 0 | 64 | `001A23A6` | 1/0 |
| 0 | 80 | `00182886` | 2/0 |
| 3 | 0 | `001A23A6` | 0/2 |
| 3 | 16 | `002A23B9` | 0/3 |

The snapshot owns:

- a stable double-copy of the complete stride-96 vertex buffer;
- a stable double-copy of the submitted uint16 index range;
- decoded big-endian indices, rejected if any index exceeds the copied vertex
  count;
- nonzero player, scope shader/model, successfully-read material vtable, and
  vertex-aggregate identities, plus semantic owner, geometry, LOD, pass, and
  exact title program identity;
- world and world-view-projection matrices;
- the exact declaration probe;
- all seven pixel fetch descriptors and descriptor-selected full texture data
  for slots `0`, `1`, `2`, and `6`;
- VS constants `c0-c3`, `c12-c15`, `c19`, `c29-c36`, and `c46-c47`;
- PS constants `c19`, `c21-c27`, `c46-c73`, and `c254-c255`.

Slots `3`, `4`, and `5` remain descriptor-only evidence. The observed
descriptors identify a depth resource, a small RGBA8 resource, and a DXT1
resource respectively; they are not promoted to owned texture payloads until
live telemetry proves that serving requires their texels.

## Publication proof

For title/backend frame sequence `N`, publication requires:

1. a non-empty backend event count divisible into exactly three tile blocks;
2. blocks two and three exactly repeat block one's dynamic draw identities and
   full backend contracts;
3. forward-earliest and reverse-latest ordered joins select the same unique
   title subsequence for every first-block event by primitive, submitted
   count, physical index base, physical vertex base/byte count/endian, and
   exact pixel hash;
4. every selected title token has complete immutable geometry, owned textures,
   constants, declaration, matrices, player scope, and material-vtable proof;
5. no selected read, copy, texture, material, sequence, or capacity failure
   occurred.

Extra same-family title candidates may remain unmatched, but cannot be
reordered or reused. A newer authoritative backend sequence closes the older
frame. Incomplete or ambiguous evidence is rejected and never replaces the
latest valid snapshot.

## Integration boundary

The observer is armed only by:

```text
tabletennis_native_player_a406_observer=true
```

Title capture is called from the scene draw catalog, backend capture runs
before replacement route selection, and the observer closes at title Swap.
Published proof is attached to MAIN coverage in the same frame. Every entry
point returns no route and mutates no GPU or guest state. Native serving must
be added later, one proven field at a time, after live telemetry verifies this
snapshot.
