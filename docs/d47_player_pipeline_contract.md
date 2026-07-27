# D47 opaque-player pipeline contract

Evidence: deterministic gameplay trace
`out/trace/frames/545407DF_5661.xtr`, replayed with
`--trace_dump_shader=D47C83252CF2B765`. The contract applies to pixel shader
`D47C83252CF2B765` and its four skinned vertex-shader variants.

## Draw inventory

The trace contains the same 23-draw block three times, at command bases 445,
886, and 1327. Each repeated command is exactly 441 commands after the first.

| Relative command | First command | Indices | Vertex shader |
| ---: | ---: | ---: | --- |
| 0 | 445 | 9222 | `20DD150A38FA9949` |
| 1 | 446 | 2519 | `20DD150A38FA9949` |
| 2 | 447 | 2502 | `20DD150A38FA9949` |
| 3 | 448 | 2586 | `20DD150A38FA9949` |
| 4 | 449 | 2557 | `20DD150A38FA9949` |
| 5 | 450 | 678 | `05AB26C749FFA8B3` |
| 6 | 451 | 1155 | `05AB26C749FFA8B3` |
| 7 | 452 | 1193 | `05AB26C749FFA8B3` |
| 8 | 453 | 347 | `9013322A360FE8D5` |
| 18 | 463 | 10624 | `20DD150A38FA9949` |
| 19 | 464 | 3611 | `84D1A8EF1D71CF60` |
| 20 | 465 | 3608 | `84D1A8EF1D71CF60` |
| 21 | 466 | 1546 | `20DD150A38FA9949` |
| 22 | 467 | 1520 | `20DD150A38FA9949` |
| 23 | 468 | 538 | `05AB26C749FFA8B3` |
| 24 | 469 | 329 | `9013322A360FE8D5` |
| 25 | 470 | 694 | `9013322A360FE8D5` |
| 26 | 471 | 272 | `05AB26C749FFA8B3` |
| 27 | 472 | 424 | `9013322A360FE8D5` |
| 28 | 473 | 439 | `9013322A360FE8D5` |
| 29 | 474 | 805 | `9013322A360FE8D5` |
| 30 | 475 | 658 | `05AB26C749FFA8B3` |
| 31 | 476 | 657 | `05AB26C749FFA8B3` |

The second and third instances are relative commands 886-894 plus 904-917,
and 1327-1335 plus 1345-1358. All 69 draws use indexed DMA, 16-bit indices,
and triangle strips (`VGT_DRAW_INITIATOR.prim_type = 6`).

## Vertex contract

Every variant is four-weight quaternion-palette skinned. The mesh stream is
`vf95`, the dynamic palette stream is `vf92`, and both use endian mode 2.

Two `vf95` layouts occur:

| Vertex shader | Stride | Position | Weights | Indices | Normal | UV | Tangent |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `20DD150A38FA9949`, `05AB26C749FFA8B3` | 36 bytes | word 0, float3 | word 3, UNORM8x4 | word 4, U8x4 | word 5, signed 2_10_10_10 | word 6, float2 | word 8, signed 2_10_10_10 |
| `9013322A360FE8D5`, `84D1A8EF1D71CF60` | 44 bytes | word 0, float3 | word 3, UNORM8x4 | word 4, U8x4 | word 5, signed 2_10_10_10 | word 6, float2 | word 10, signed 2_10_10_10 |

The packed U8 attributes are fetched with `zyxw` swizzle. The 44-byte
variants do not fetch words 8-9. Register destinations differ between
variants, but the semantic inputs do not.

`vf92` has a 28-byte record:

- word 0: float4 quaternion;
- word 4: float3 translation.

Four dynamically indexed records are fetched per vertex. In command 445 the
captured binding is guest address `0x06715000`, size 10192 bytes: exactly 364
records. That is one observed roster state, not a family constant. Live
gameplay also supplies 10248-byte bindings: 366 records.

The observer derives the palette shape from the active fetch:

- `fetch_byte_count` must be divisible by `2 * 28`;
- `record_count_per_half = fetch_byte_count / (2 * 28)` must be 1 through
  256;
- `0x8225C668` must run under the synchronous player owner, bind
  `fetch_base`, and expose current cached `{generation, source}` state for the
  primary half;
- `0x8225C720` must bind the same buffer and expose current cached
  `{generation, source}` state for the alternate half;
- both binders must derive the same record count and cache generation;
- the stable full-fetch copy must decode every quaternion/translation record
  in both halves.

Either half can legitimately skip its packer call when its cache state is
current. A same-generation first-half write fingerprint is checked against
the captured payload when present, but is optional corroboration rather than
a false per-frame requirement. Both halves are proven by the exact title
cache invariants plus full record validation. There is no hardcoded
182/364-record assumption.

Do not substitute the CA9 palette. CA9 uses a distinct nonzero binding at
`0x06709000`, size 10248 bytes (366 records).

### Title-family fence

One XTR boot observed these player-scoped D47 pointer tuples:

| Pass | Program | Vertex shader | Pixel shader |
| --- | --- | --- | --- |
| `0x400E85D8` | `0x400EE810` | `0x400E5E00` | `0x400F3410` |
| `0x400F56E8` | `0x400FA820` | `0x400FF420` | `0x400F24C0` |

They are heap pointers and remain historical telemetry only. A first-dispatch
probe observed the immutable title-side microcode identity:

- pixel `D47C83252CF2B765`;
- vertex template in
  `{F06FA0F9B3C93AEB, 4857D676B0E080E9}`.

Those hashes are learned evidence, not title admission gates. Capture records
the player-owned structural mesh/palette superset. The backend independently
requires the four translated vertex variants
`{20DD150A38FA9949, 05AB26C749FFA8B3, 9013322A360FE8D5,
84D1A8EF1D71CF60}` with the same D47 pixel hash, then selects the title tokens
by exact frame, ordered indexed-draw identity and three identical tile blocks.
Live evidence proves each title template maps to the corresponding same-sized
pair of backend variants; the observer does not guess or reproduce that
transformation.

This backend join positively excludes 6AE without hardcoding its heap objects.
The same
backend D47 block also contains draws cataloged outside the player scope under
the historical pointer `0x401D1D64`; those remain ineligible because they have
no proven player owner. The shader-object creation path and stage-specific
hash proof are in `docs/player_shader_identity_ghidra.md`.

## Vertex constants

All variants consume:

- `c12-c15`: clip transform;
- `c19`: camera position;
- `c29-c36`: two transform matrices, identity in this trace;
- `c255 = (1, 255.00195, 0, 0)`.

`20DD...` and `84D1...` also use `c46-c47`. `05AB...` and `9013...` use
`c46-c48`. Representative command-445 values are
`c46.x = 0.015686275` and `c47.x = 0.0627451`.

## Pixel resources

The disassembly contains 21 texture-fetch instructions but only seven
distinct fetch slots:

| Fetch slot | Trace binding | Representative resource | Role |
| ---: | ---: | --- | --- |
| 0 | 0 | DXT1, 1024x1024, mips 0-8 | per-material map |
| 2 | 1 | DXT1, 1024x1024, mips 0-8 | per-material map |
| 1 | 2 | DXT4/5, 1024x1024, mips 0-8 | per-material normal/material map |
| 3 | 3 | D24S8, 640x480 | screen-depth input, eight taps |
| 4 | 4 | RGBA8, 32x32 | global lookup/noise input, eight taps |
| 6 | 5 | DXT4/5 cube, 256x256x6, mips 0-3 | environment cube |
| 5 | 6 | DXT1, 256x256 | global lookup/mask |

Slots 0-2 change with mesh/material. Slots 3-6 are stable across the
inspected block. Command 445 binds the material triple at guest addresses
`0x0F0D2000`, `0x0F2E2000`, and `0x0F182000`.

The pixel shader consumes `c19`, `c21-c27`, `c46-c77`, and `c252-c255`.
These cover camera/screen sampling, material and lighting parameters, and
shader literals. Command 445 proves the literal block:

- `c252 = (-0.5, 0.5, -1, 4)`;
- `c253 = (1.5, 1, 1/12, 1/8)`;
- `c254 = (0.3, 0.59, 0.11, 1/8)`;
- `c255 = (3, 0, 0, 0)`.

Material constants vary per draw and must be captured, not hardcoded.

## Attachment, depth, blend, and raster state

All 69 draws use the same opaque color-plus-depth state:

- `RB_SURFACE_INFO = 0x14020500`: 1280-pixel pitch, Xenos 4x MSAA field;
- `RB_COLOR_INFO = 0x00000400`: RGBA8 RT0 at EDRAM tile `0x400`;
- `RB_DEPTH_INFO = 0x00000000`: D24S8 at EDRAM tile 0;
- `RB_MODECONTROL = 0x00000004`: color + depth;
- `RB_COLOR_MASK = 0x0000000F`: write RGBA;
- `RB_DEPTHCONTROL = 0x00700736`: depth test and write enabled,
  less-or-equal, stencil disabled;
- `RB_BLENDCONTROL0 = 0x00010001`: one/zero add, host blending disabled;
- `RB_COLORCONTROL = 0x87000005`: alpha test and alpha-to-coverage disabled;
- `PA_SU_SC_MODE_CNTL = 0x00018002`: back-face culling, CCW front face,
  multisampling and vertex window offset enabled.

The primitive-restart register contains `0x0000FFFF`, but restart is not
enabled in `PA_SU_SC_MODE_CNTL`.

## Relationship to CA9

D47 and CA9 share the reusable *decoder shape*: endian-2 guest streams,
U8 weights and indices, packed signed normals/tangents, float UVs, and
28-byte quaternion-plus-translation palette records.

They are not interchangeable render passes:

- D47 is a 23-draw opaque, depth-writing, back-face-culled family with a
  seven-resource material shader.
- CA9 is a 20-draw paired alpha/depth-prepass and blended-color family with
  three material textures and no face culling.
- Their blocks repeat with the same 441-command cadence but use different
  palette addresses and record counts. The strongest current interpretation
  is that they belong to separate player owners.

Safe reuse is limited to semantic vertex decoding, palette math, and generic
guest-resource capture. D47 serving stays observer-only until its zero
palette is explained and a real nonzero palette is verified over multiple
frames.
