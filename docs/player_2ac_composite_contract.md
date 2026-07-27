# 2AC player motion-composite contract

This is capture evidence for pixel shader `2AC059EB5C7A942F`. It describes
real title work; it is not a replacement scene or a serving claim.

## Evidence

- Gameplay trace: `out/trace/frames/545407DF_5661.xtr`
- Pixel microcode:
  `/tmp/tt_2ac_shaders/shader_2AC059EB5C7A942F.ucode.frag`
- Targeted draw-state dumps:
  `/tmp/tt_2ac_dump.log`, `/tmp/tt_2ac_detail_1427.log`, and
  `/tmp/tt_2ac_sampler.log`

The trace contains 75 indexed 2AC draws at commands 1415 through 1489,
totalling 109,982 indices. They use four vertex shaders:

- `4761A30F65AA309C`
- `3E233105507CB75F`
- `633DEDEA0081898C`
- `435F65388E61D4B5`

The first two vertex variants cover small/special player meshes. The latter
two use the same two-stream player contract as the main skinned materials:
geometry in fetch 95 and the current player palette in fetch 92. The trace
palette is 28-byte records and the geometry variants observed so far use
36-byte or 44-byte vertex strides.

The complete command 1415..1489 detail dump
(`/tmp/tt_2ac_all_detail.log`) resolves the 75 draws into:

| Vertex shader | Topology | Raster | Draws | Indices | Vertex contract |
| --- | --- | --- | ---: | ---: | --- |
| `4761A30F65AA309C` | strip | `0x00018002` | 8 | 821 | fetch 95, 32-byte stride |
| `4761A30F65AA309C` | strip | `0x00018006` | 8 | 821 | fetch 95, 32-byte stride |
| `3E233105507CB75F` | list | `0x00018002` | 3 | 16,056 | fetch 95, 96-byte stride |
| `3E233105507CB75F` | list | `0x00018006` | 3 | 16,056 | fetch 95, 96-byte stride |
| `633DEDEA0081898C` | strip | `0x00018002` | 36 | 55,834 | fetch 95 at 36 bytes plus fetch 92 palette |
| `435F65388E61D4B5` | strip | `0x00018002` | 17 | 20,394 | fetch 95 at 44 bytes plus fetch 92 palette |

The 32- and 96-byte single-stream draws are emitted in paired raster states.
The 53 skinned draws are split by the two live player palettes: commands
1427..1456 use `0x06709000` with 10,248 bytes, while commands 1467..1489 use
`0x06715000` with 10,192 bytes. These addresses are trace evidence only; the
live observer must join by current palette ownership and geometry identity.

## Render pass

Every detailed draw has:

- RGBA8 color target at EDRAM base `0x2D0`
- normalized depth control `0x00700736`
- color control `0x8700000C`
- blend control `0x07060706`
- no primitive restart
- scene color in texture fetch 0: tiled RGBA8, 1280x720, one level
- point minification and magnification, base-map-only mip filtering
- clamp-to-edge on U, V, and W, no anisotropy, no LOD bias, mip range 0..0

The raw trace describes the COMP EDRAM surface as 4x. In the live macOS
Vulkan borrowed callback, the translated COMP pass is render-pass key
`0x0000000C` and exposes a one-sample RGBA8 plus D32S8 attachment tuple. This
is not the tiled MAIN key `0x0000000E`. The observer requires both layers of
evidence: the exact raw Xenos depth/color/blend/raster fields and the exact
late host attachment contract that a native replacement would actually
borrow.

The trace alternates `PA_SU_SC_MODE_CNTL` `0x00018002` and `0x00018006` for
some paired meshes, so culling/winding must remain per-draw state. It is not
safe to collapse those pairs by index-buffer identity alone.

Although the translated shader advertises 13 texture-binding instructions,
all instructions reference the same fetch constant. The only live image is
texture 0.

## Pixel shader behavior

The guest microcode is an object-restricted motion composite:

1. Project interpolator 1 to screen UV and add the half-texel center from
   pixel constant 37.
2. If the motion vector in interpolator 0 is sub-pixel, sample scene color
   once.
3. Otherwise sample 12 positions from 0/12 through 11/12 along the motion
   vector.
4. Keep only samples whose alpha is at least the interpolated coverage
   threshold.
5. Output the average kept RGB and `kept_sample_count / 12` as alpha.

The observed pixel constants are:

```text
c37  = (1/1280, 1/720, 0.5 + 0.5/1280, 0.5 + 0.5/720)
c253 = (10/12, 11/12, 0, 0)
c254 = (6/12, 7/12, 8/12, 9/12)
c255 = (1/12, 3/12, 4/12, 5/12)
```

This is not a general material pass. It redraws the real player silhouettes
over resolved scene color to preserve their motion/coverage treatment.

The isolated pixel port is
`src/native/shaders/tabletennis_player_motion_composite.hlsl`. Its generated
SPIR-V is validated for the native binding layout, but it is not connected to
a renderer or used to suppress guest work.

## Native serving requirements

The eventual native transaction must:

1. finish and resolve the shared 4x MAIN scene to a sampleable 1280x720 RGBA8
   image;
2. retain the exact current-frame player geometry, skinning palette,
   transforms, interpolated motion vector, and coverage threshold;
3. replay the 75 dynamic 2AC draws in captured order with per-draw topology
   and raster state;
4. use the proven sampler state and the ported 12-tap shader; and
5. compose HUD/post-processing only after this pass.

Until the title-side identities and the translated backend sequence are
joined for a live frame, 2AC stays observer-only and cannot participate in
takeover readiness.

## Live observer

`tabletennis_player_2ac_observer` implements that join without serving:

- title admission requires the actual low-level bound pixel shader fingerprint
  `2AC059EB5C7A942F`, then validates 32/96-byte single-stream geometry or
  36/44-byte geometry in vertex fetch 95 plus a 28-byte-record palette in
  fetch 92, with 16-bit indexed strip/list topology as appropriate;
- the translated-backend tap requires pixel hash `2AC059EB5C7A942F`, one of
  the four traced vertex hashes, the exact gameplay attachment/draw state,
  and the traced per-family raster state;
- ordered backend events are joined to one unique monotonic title subsequence
  by primitive type, submitted index count, current physical index base,
  current vertex/palette fetch identities, and the actual low-level bound
  pixel-shader fingerprint;
  unrelated structural candidates may be interleaved, so uniqueness is
  proven by identical earliest and latest mappings rather than assumed
  contiguity;
- the single-stream back/front raster pairs must be adjacent and otherwise
  state-identical; and
- after the actual low-level bound-PS gate and scalar fetch/index admission,
  `tabletennis_player_2ac_payload` takes fault-guarded stable copies of the
  complete vf95 stream and exact submitted 16-bit index range; every decoded
  index must fit the captured vertex count;
- the 36/44-byte skinned variants also retain a frame-owned stable vf92 copy,
  decode all 28-byte quaternion/translation records, and reject non-finite
  components; immutable vertex/index payloads use bounded reusable caches,
  revalidate cached physical ranges once per live frame, and share a hard
  outstanding-byte ceiling;
- publication retains shared immutable real guest payloads beside title
  program addresses, fetch descriptors, ownership metadata, and backend
  contracts. Unmatched title candidates are never published.
  Dynamic visibility is accepted: a live frame may contain only a subset of
  the four traced vertex variants or one player palette. Every draw still must
  join uniquely and every present single-stream pair and skinned palette group
  must validate exactly.

The hot cvar is `tabletennis_native_player_2ac_observer`. Ambiguous windows,
missing raster pairs, guest-read failures in the selected window, or an
inconsistent live palette grouping fail closed. No fixed draw count, fixed
palette-group count, cached ApplyPass shader hash, heap pointer, replacement,
suppression, or fake draw is part of admission.

## Observer renderer preparation

`tabletennis_player_2ac_renderer` is the next capture-first slice. It still
records no GPU commands:

- every admitted draw now retains the exact finite constant-bank spans consumed
  by the traced programs: VS rows 0..20, 37..47 and 254..255, plus PS row 37
  and rows 253..255;
- the preparation pass walks the exact submitted 16-bit indices in title
  order and decodes only their referenced real vf95 vertices;
- stride 32 decodes float3 position plus packed signed 10:10:10 normal,
  stride 96 decodes float3 position plus the float normal at byte 16, and
  strides 36/44 decode float3 position, byte weights/indices and their packed
  normal at byte 20; the stride-36 index bytes follow the traced `.zwyx`
  fetch swizzle rather than the `.zyxw` weight/stride-44 order;
- skinned preparation validates both palette record selectors used by the
  Xenos normalized-byte programs, `(bone/255)*c254.y` and
  `(bone/255+c20.w)*c254.y`, against the retained vf92 record count; and
- the resulting immutable plan keeps exact draw order, topology, raster state,
  decoded bounds and a content fingerprint derived from guest bytes.

The hot cvar `tabletennis_native_player_2ac_renderer_observer` arms the 2AC
capture and publishes this decoded plan. It reports
`gpu_recording_ready=false` and fails closed at
`missing vertex program ports`: the common 2AC pixel port exists, but exact
ports of all four bound vertex programs do not. Two later blockers are also
explicit in the readiness contract: a sampleable resolved MAIN scene handoff
and a distinct ordered one-sample COMP transaction. A generic player vertex
shader or the shared 4x MAIN pass would be visually wrong, so neither is used
as a placeholder.
