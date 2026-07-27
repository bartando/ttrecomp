# Crowd family contract — trace `545407DF_5661`

This document records the observer-side contract for the largest remaining
non-player scene family:

- Pixel shader trace hash: `C6CEFDA3753CF2BA`
- Vertex shader trace hash: `BD4B1DF972B828B7`
- First-tile command range: `247..402`
- Logical title submissions: 156
- Logical submitted indices: 114,819
- Unique geometry payloads: 22

The same logical block is repeated by the GPU at `688..843` and
`1129..1284` for the other EDRAM tiles. Those repetitions are not additional
engine submissions.

## Proven title owner and hook

RTTI identifies vtable `0x8206AF9C` as `fxCrowdGfx`. Its render callback is
`sub_82385AB0` (vtable slot 0).

The callback owns:

| Offset | Meaning |
| --- | --- |
| `+0x20` | crowd resource |
| `+0x24` | crowd state/helper |
| `+0x28`, `+0x2C` | drawable/model-geometry sources |
| `+0x30`, `+0x34` | associated model/material objects |
| `+0x38...` | visible instance pointer array |
| `+0x438` | visible instance count |

`sub_82385C88` receives the owner in `r3` and exactly one of its stored pairs
in `r4/r5`: `{+0x28,+0x30}` or `{+0x2C,+0x34}`. It walks the instance array,
uploads a transform from each entry, and calls `sub_820EE910`. That routine
resolves the primary aggregate, then calls the already-hooked
`sub_820EE6E8` draw helper.

The observer validates `sub_82385C88` against both the active
`sub_82385AB0` owner and the exact stored drawable/model pair. That deeper
scope remains live through `sub_820EE910` -> `sub_820EE6E8` ->
`DrawIndexedPrimitive`, joining each draw to `fxCrowdGfx` without a shader
hash or frame-order guess. Crowd uses this specialized path directly and does
not enter the generic `grmShaderFx::DrawModelGeometry` scope. No guest draw is
suppressed.

## Vertex contract

### Primary stream (`vf95`)

- Stride: 36 bytes
- Fetch endian: 8-in-32 (`2`)
- Position: `float3`, byte offset 0
- Packed palette / atlas selector: `8_8_8_8`, byte offset 16
- Normal: signed integer `2_10_10_10`, byte offset 20
- UV: `float2`, byte offset 24
- Final four bytes remain retained verbatim pending semantic proof

The shader multiplies the packed selector by `c255.z` and uses it to fetch a
record from the secondary stream.

### Secondary stream (`vf92`, fetched by the shader as `vf3`)

- Stride: 28 bytes
- Fetch endian: 8-in-32 (`2`)
- Quaternion: `float4`, byte offset 0
- Translation: `float3`, byte offset 16

An observed 1,232-byte binding contains 44 records. The observer copies and
finite-checks every record rather than walking a guessed title-side skeleton.

Indices are big-endian `uint16_t` and are rejected if any decoded value is
outside the captured primary vertex range.

## Constants

The vertex shader consumes:

- `c32..c35`: per-instance transform; `c32.w` also carries a variation value
- `c36..c39`: shared view-projection transform
- `c136..c141`: lighting sphere positions, scales, and colors
- `c145`: ambient color
- `c255`: decode constants, verified as
  `{0, 1, 255.001953125, 0}`

There are no material pixel float constants in this family.

## Texture and pixel shader

The pixel shader performs one `tfetch3D` from texture slot 0, multiplies the
sampled RGB by the vertex-computed lighting color, saturates, and writes alpha
1.

The texture is a tiled 3D DXT1 volume:

- Width: 128 or 256 depending on crowd geometry
- Height: 128 or 256 depending on crowd geometry
- Layers: 8
- Fetch swizzle: `0x688`

Live fetch telemetry decodes `dimension=k3D`, `stacked=0`, `depth=8`. The
crowd snapshot retains the complete stable-double-read mip-zero guest payload
and its layout metadata rather than forcing it through a 2D-only snapshot.

## Raster contract

Across inspected draws:

- Main color target: RGBA8, EDRAM base `0x400`
- The title's raster state enables depth test/write against its D24S8 EDRAM
  surface at base `0`. The live late C6 borrowed scope exposes the translated
  depth/stencil attachment too: D24S8 on native backends, or D32S8 through the
  MoltenVK fallback (`depth_format=21`, `stencil_format=21`) on macOS.
  A stale no-DSV diagnostic came from an earlier callback that did not yet
  publish the final attachment signature; it must not be used for replacement
  admission.
- `RB_COLORCONTROL = 0x87000005`
- `RB_BLENDCONTROL = 0x00010001` (opaque)
- `RB_DEPTHCONTROL = 0x00700736` (depth test/write)
- Normalized color write mask: `0x00000007` (RGB; alpha is not written)
- `PA_SU_SC_MODE_CNTL = 0x00018002`
- Live late borrowed scope: Vulkan, render-pass key `0x0000000E`,
  surface pitch `1280`, one RGBA8 color attachment, matching D24S8/D32S8
  depth and stencil attachments, 4x samples, full sample mask, and primitive
  restart disabled (`raw index=0xFFFF`).

The ordered backend block contract retains the raw rasterizer mode and validity
bit and requires them to remain identical for a matched block. The native RHI
represents front-face winding explicitly; `0x00018002` decodes to
counter-clockwise front faces with back-face culling. Serving still waits for
the live same-frame block proof before selecting that pipeline state.

## Borrowed-scope prewarm proof

`tabletennis_native_crowd_replacement_prewarm` remains observer-only and
always falls back to the guest draw. Immutable vertex, index, and decoded
volume-texture resources are uploaded in the output callback. When a newer
exact title frame reaches the later borrowed callback, only its host-visible
palette and constant buffers are populated there; this records no Vulkan copy
or barrier inside the guest render pass.

The macOS Vulkan run in
`/tmp/tt_c6_d32s8_exact_prewarm.log` proved both levels:

- an exact current-frame C6 tuple preflighted against the live RGBA8/D32S8
  four-sample render scope;
- one complete ordered tile block preflighted all 252 of 252 candidates with
  zero failures and no order gaps (`complete_block_ready=true`).

The initial cold block can report `device_mismatch` before the first output
callback creates the native device/resources. It remains guest-authoritative;
later complete blocks are the readiness proof. No C6 draw is served yet.

`tabletennis_native_crowd_replacement_serve_draws` is the separate,
default-zero serving flip. It serves only the first N checked candidates from
a later block after the complete-block proof already existed. A one-draw run
recorded the native indexed command successfully and retained correct gameplay
output. A full-family diagnostic run also retained correct output, but stayed
around 8–9 FPS because the late borrowed callback has already paid most of the
guest render-target, shader, binding, and tiled-pass setup cost. This route is
a rendering-parity milestone, not the 30-FPS solution; broad native scene
output must bypass the tiled guest MAIN path earlier.

## Deterministic verifier

The runtime observer admits only draws inside the exact owned
`sub_82385C88` drawable/model scope with the 36-byte primary stream. Runtime
telemetry proved the helper's optional `r5` secondary-stream argument is zero
for every observed crowd draw; vf92 is instead validated from live GPU fetch
slot 92 at draw time. A live frame validates when every owned draw has a
complete payload, its index count belongs to the proven family, and nothing
was dropped. The original trace has this stricter histogram, retained as a
separate parity check:

| Indices | Occurrences | Indices | Occurrences |
| ---: | ---: | ---: | ---: |
| 553 | 6 | 561 | 12 |
| 562 | 5 | 609 | 5 |
| 634 | 7 | 649 | 6 |
| 652 | 9 | 659 | 7 |
| 672 | 5 | 690 | 8 |
| 693 | 8 | 739 | 6 |
| 759 | 8 | 772 | 12 |
| 793 | 8 | 823 | 9 |
| 893 | 5 | 919 | 6 |
| 1003 | 5 | 1004 | 6 |

Enable capture with:

```sh
./run.sh --skip-menu --tabletennis_native_crowd_observer=true
```

The observer reports the live family as `VERIFIED` when every
`fxCrowdGfx`-owned stride-36 draw has the complete vf95/vf92/3D-DXT1 material
contract, an index count from the traced family, and zero dropped draws or
copy failures. Live draw counts vary with visible crowd instances. A separate
`trace_parity=true` bit is emitted only for the original exact 156-draw,
114,819-index, 22-geometry histogram. Live backend telemetry also proves that
the same ordered logical block is replayed for multiple EDRAM tiles, so
serving must correlate whole ordered blocks rather than treating one title
token as one backend draw. Default behavior remains observer-only; the
bounded serving flip above is opt-in and must be expanded only after new
family proofs preserve fallback safety.
