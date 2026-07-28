# Venue `E33DEAA20A98FCEF` observer/learner contract

This module observes and copies guest-owned data. It has no replacement
matcher, renderer, fast path, or suppression callback.

## Reference census

`out/trace/frames/545407DF_5661.xtr` contains this exact MAIN family:

- vertex shader `37F2AEC8A23E44E0`;
- pixel shader `E33DEAA20A98FCEF`;
- 23 logical indexed triangle-strip draws;
- 13,598 logical indices;
- three identical EDRAM tile submissions, producing 69 backend events.

The reference count sequence is:

```text
613,499,541,1083,966,540,872,896,442,541,367,721,
724,276,32,721,718,58,687,736,687,736,142
```

The first block is at commands 214-236, followed by identical blocks at
655-677 and 1096-1118.

Those draw and index counts describe that trace only. Live visibility and
venue culling change the family size. The observer never uses 23, 13,598, or
the reference count sequence as a readiness condition.

Backend admission requires Vulkan, MAIN render-pass key `0xE`, pitch 1280,
the decoded MAIN EDRAM target `{color base 0x400, depth base 0, mode 4}`, one
RGBA8 color attachment, matching D24S8/D32S8 depth and stencil, 4x MSAA, the
full sample mask, no primitive restart, and the trace-proven draw state:

```text
normalized depth control = 00700736
normalized color mask    = 00000007
color control            = 87000005
blend control 0          = 00010001
```

The backend contract retains raw `RB_COLOR_INFO[0]`, `RB_DEPTH_INFO`,
`RB_SURFACE_INFO`, and `RB_MODECONTROL` beside their decoded target values.
Every raw register must decode back to the captured target, and the full raw
and decoded contract must repeat across all three tile submissions. It also
retains raw `PA_SU_SC_MODE_CNTL`. This register contains cull selection,
front-face winding, polygon mode, polygon offsets, MSAA raster enable,
window-offset behavior, and multi-primitive enable. Availability is mandatory
for an E33 backend event. These values are observed, not guessed.

Every event is joined to a title occurrence by
`{primitive, submitted index count, physical index base, physical vertex base,
vertex byte count, vertex endian}`. The backend primary vertex fetch must be a
nonempty stride-32 range using endian mode 2. Counts alone are not identities:
unrelated title programs submit counts such as 966 and 58.

Backend evidence carries the command processor's authoritative
`backend_frame_sequence`. A live probe proved that this token equals the title
Swap sequence even when the asynchronous backend callback arrives one to
three title swaps later. The observer accepts a nonzero token only and joins
it to the retained title ledger with exactly the same sequence. Missing,
rejected, finalized, or expired ledgers make their tagged events stale; an
event is never inferred from arrival time or retried against a later frame.

## Dynamic same-frame proof

The title observer records cheap metadata for structurally compatible
non-player draws in the current frame:

- valid title pass/program identity;
- triangle-strip primitive 6;
- 32-byte, 8-in-32 vertex stream;
- a guarded five-element declaration probe with the live-observed
  format/offset layout;
- big-endian 16-bit indices;
- valid physical vertex and index aliases.

Metadata admission is not classification. The authoritative backend frame
token later selects which title candidates belong to E33:

1. Collect every exact E33 backend event tagged with frame `N`.
2. Wait until the backend reports a sequence greater than `N`.
3. Require the event count to divide into exactly three non-empty tile blocks.
4. Derive the live draw count, index sum, and order from the first block.
5. Require blocks two and three to repeat every full draw identity and its
   backend contract exactly.
6. Join the first block one-to-one, in order, to frame `N` title candidates by
   the complete physical index and primary-vertex identity above. Compute both
   the earliest forward and latest backward ordered mappings and require them
   to be identical; otherwise the mapping is ambiguous and the frame is
   rejected.
7. Learn the selected ordered program and declaration identities. This first
   proof frame remains metadata-only and cannot publish a valid payload frame.
8. In later title frames, copy payloads only while candidates match that
   learned order, then independently repeat the full same-frame backend proof
   before publication.

Events never fall back to the oldest pending frame. A token without the exact
same-sequence title ledger is stale. A frame with no events, a partial tile
block, reordered draws, mismatched identities, or a payload failure fails
closed.

The learned identity includes the complete declaration signature for each
live ordered draw: stream masks plus every element's full `packed_type`,
stream, offset, method, usage, and usage index. Its vectors are dynamically
sized to the proven live tile block. The builder's documented uninitialized
padding byte and object-specific address/cache ID are excluded. The identity
is frozen at the first eligible candidate of each title frame. Any generation
drift rejects that frame; backend proof failure clears the learner and returns
capture to metadata-only discovery.

## Immutable payload

Each selected title draw owns:

- the complete raw vertex fetch range;
- the submitted big-endian 16-bit indices and decoded host indices;
- three authoritative texture fetch constants;
- immutable untiled payloads for every descriptor-selected mip and layer;
- all 256 float4 VS constants and all 256 float4 PS constants as raw words;
- title program identity, owner metadata, ordinal, and backend contract.

The published draw also preserves the complete backend draw identity,
including the physical vertex range and endian mode, that independently
proved its title identity. Publication revalidates those fields against the
immutable vertex and index payloads.

The vertex layout is fixed by the common `37F2` vertex shader:

| Byte | Xenos format | Use |
| ---: | --- | --- |
| 0 | 57, `32_32_32_FLOAT` | position |
| 12 | 7, signed `2_10_10_10` | packed normal |
| 16 | 6, `8_8_8_8`, `zyxw` | packed color |
| 20 | 37, `32_32_FLOAT` | UV |
| 28 | 7, signed `2_10_10_10` | packed tangent |

The guest declaration at `0x40014590` was read successfully in live gameplay:
it reports five inline elements, stream 0, low stream mask
`FF00000000000000`, and high stream mask 0. The exact packed types are
`002A23B9`, `001A2387`, `00182886`, `002C23A5`, and `001A2387`; the matching
usages are 0, 3, 10, 5, and 6. Padding/cache bytes are not part of identity.

The reference trace's material layout is:

- slot 0: tiled 2D DXT1;
- slot 1: tiled 2D DXT1;
- slot 2: tiled DXT5 cube.

The trace binds 128x128 textures with mip levels 0-5 in slots 0 and 1, and a
256x256x6 cube with mip levels 0-3 in slot 2. Live captures using the same
learned E33 program bind other valid texture formats, dimensions, layouts and
mip counts. All of those material-owned fields are therefore preserved in
each snapshot and validated by the shared texture decoder; none is mistaken
for shader identity. The E33 gate requires three valid texture fetches and
three valid immutable texture snapshots with complete mip chains.

All guest reads are fault guarded. Geometry and texture payloads use bounded
immutable caches. The fetch and complete constant-bank span is accepted only
after two identical reads. Geometry is re-read and compared once per unique
resource per title frame; later draws sharing that resource reuse the validated
immutable payload. Thus a streaming allocation reusing the same physical range
cannot silently return stale vertex or index bytes without duplicating stable
copies within one frame.

### Index payload diagnostic

Payload capture deliberately has a one-proof bootstrap. The first complete
backend/title join learns generation 1 from metadata and publishes no valid
payload frame. Later frames capture only the ordered generation-1 identities.
If an asynchronous proof changes the generation while a title frame is being
built, that frame's payloads are discarded and
`capture_generation_mismatches` rejects publication.

The reference trace's physical index ranges were also decoded independently:
all 23 are big-endian 16-bit, and every decoded maximum is in bounds. For the
first nine detailed draws, each maximum is exactly one less than the traced
vertex-fetch count (for example, 403 in 404 vertices and 792 in 793 vertices).
This rules out the index byte order and the reference buffers themselves.

The observer now emits at most eight payload diagnostics. A failure reports
the selected stream, resource aliases and physical addresses, declared vertex
bytes/count, decoded big- and little-endian maxima, and the first out-of-range
index. This remains observer-only and is intended to prove whether the live
title mesh walk selected a smaller or different vertex fetch than the backend
before changing any capture or serving rule.

### E33 pixel-constant write-point audit

The exact translated E33 pixel shader multiplies its final RGB by
`c255.x + c20.z * c20.z`. The reference capture contains
`c20=(1000,-1000,1,0)` and `c255=(-1,-36,1.5,1)`, making that multiplier
exactly zero. This was verified independently in the guest shader
disassembly and translated SPIR-V; the native shader must not fudge either
constant to hide the result.

Static Ghidra evidence narrows the producer path:

- `rage_gfx_SetPixelConstants20` at `0x82356A70` copies exactly 20 float4
  rows to `GfxDevice + 0x1780`. Its three direct callers are `0x82152EC0`,
  `0x8214A948`, and `0x82312B44`, so this helper can write only `c0-c19`.
- `rage_gfx_BindPixelShader` at `0x82356F50` parses shader-object metadata
  and updates device state beginning at `GfxDevice + 0x480`.
- `rage_fx_ApplyPass` at `0x82158C48` executes the pass's indirect command
  lists. The pass contains the program pair at `+0x08`, device-command list
  at `+0x0C`, and sampler/state-command list at `+0x10`; the runtime state
  contains the graphics device at `+0x2BC`.
- The device list stores its count at `+0x10` and eight-byte
  `{u32 device_subobject_offset, u32 argument}` entries at `+0x14`. The
  sampler list stores its count at `+0x80` and eight-byte
  `{u16 argument, u16 device_subobject_offset, u32 value}` entries at
  `+0x84`.

The exact `rage_fx_ApplyPass` hook now wraps the untouched guest call with a
bounded observer probe. After the E33 identity is learned, it recognizes the
boot-local pass and program pointers, records both command lists and reads
pixel `c20` and `c255` immediately before and after the call. It samples once
per learned generation, retries after a failed post-call guest read, and never
mutates guest memory. This is the next live proof for whether ApplyPass or its
bind metadata supplies the high constants.

### Observer renderer boundary

The captured data is the game's scene, not authored substitute content. The
common vertex program reads the exact five-element stride-32 stream above and
uses VS constants `c0-c6` and `c12-c15`. The E33 pixel program samples slot 0
as 2D, slot 1 as 2D, and slot 2 as a cube; it combines their results with
interpolated color plus PS constants including `c19`, `c20`, `c46`, `c47`,
`c48`, `c254`, and `c255`. Its output alpha is the slot-0 alpha multiplied by
the fourth interpolator alpha. These are ports-of-game inputs, not a proposed
replacement approximation.

An observer-only E33 renderer now ports those exact programs and consumes only
proof-published immutable payloads. It remains off by default, does not enter
the native scene transaction or shared compositor, and never suppresses guest
draws. Texture mip upload follows each descriptor's complete selected range
rather than a hardcoded reference count.

A fresh live observer run must still prove, on backend-selected E33 draws:

1. all three texture snapshots contain the complete descriptor-selected mip
   chain;
2. the raw clamp, min/mag/mip, anisotropy/walk, LOD-bias, swizzle, border, and
   tri-clamp states normalize to samplers that the shared NRHI compositor can
   reproduce exactly;
3. raw `PA_SU_SC_MODE_CNTL` is stable across the three repeated tile blocks and
   its cull/front-face/polygon/offset behavior is implemented by that pipeline.

The observer now emits bounded candidate material diagnostics so a failed mip
capture remains inspectable, then emits a separate bounded material contract
only for draws selected by a successful backend/title proof. It counts
full-mip and renderer-shape-compatible title captures and emits bounded
decoded raster diagnostics while retaining the raw register in every backend
contract. `RB_COLORCONTROL = 0x87000005` has alpha test disabled, so an
alpha-reference register is not a missing E33 field.

No shared-compositor adapter or serving route is authorized until those live
contracts and the constant producer are proven. Draw count and order remain
dynamically derived from the snapshot vector; promotion must never assume the
reference count of 23 or keep culled draws alive.

### Observer-only serving boundary

`snapshot->valid()` proves the dynamic backend/title join and the immutable
payload invariants implemented by the shared texture decoder. It does **not**
authorize rendering, replacement, or guest suppression. Shader parity,
resource lifetime, render ordering, and a complete offscreen comparison remain
separate serving gates.

## Shared integration calls

The implementation is isolated in:

- `src/native/tabletennis_venue_e33_snapshot.{h,cpp}`
- `src/native/tabletennis_venue_e33_observer.{h,cpp}`
- `src/native/tabletennis_venue_e33_renderer.{h,cpp}`
- `src/native/shaders/tabletennis_venue_e33_observer.hlsl`

Shared wiring should contain only these calls:

1. Add both `.cpp` files to the executable source list.
2. Call `ObserveVenueE33TitleDraw(guest_base, draw)` beside the other title
   observers after `DrawIndexedPrimitive` has committed its fetch state.
3. Call `ObserveVenueE33BackendDraw(context)` before replacement route
   selection so an earlier matcher cannot hide the callback. The context must
   carry the nonzero command-processor `backend_frame_sequence`.
4. Call `VenueE33ObserverFrameEnd()` at title-side Swap before the master
   frame-scene publication.
5. Read `LatestVenueE33FrameSnapshot()` and count the family only when
   `snapshot->valid()` is true.
6. Expose `LatestVenueE33LearnedIdentitySnapshot()` and
   `LatestVenueE33ObserverTelemetry()` for proof diagnostics.
7. Wrap the untouched `rage_fx_ApplyPass` call at `0x82158C48` with
   `BeginVenueE33ApplyPassProbe()` and `EndVenueE33ApplyPassProbe()`.
8. Draw the optional observer renderer only from the observer-overlay path.

None of those integration calls authorizes serving or suppressing E33 draws.
