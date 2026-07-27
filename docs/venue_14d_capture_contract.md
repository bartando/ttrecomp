# Venue `14D6B61CBC3D853C` capture contract

This is an observer contract, not a replacement contract. It never serves or
suppresses a guest draw.

## Trace facts

The reference frame `out/trace/frames/545407DF_5661.xtr` contains 47 logical
MAIN draws (141 tiled backend submissions), 24,057 logical indices, five
texture bindings, and this backend shader set:

- pixel: `14D6B61CBC3D853C`
- vertex: `4EAEC701E97DCDAD`, `08D6210341AD63F6`
- MAIN render pass key `0xE`, pitch 1280, RGBA8 plus D24S8/D32S8, 4x MSAA

The 47 draws occur as three ordered segments per EDRAM tile: 11 at commands
92-102, 30 at 178-207, and six at 407-412. The same sequence repeats at
commands 533-648/848-853 and 974-1089/1289-1294.

## Guest correlation

Existing scene-catalog logs correlate all three segments by exact index
sequence, not by address proximity:

- pass `402E78A8`, program `402EAE80`, VS `402E5510`, PS `402E6500`
- pass `402DD218`, program `402E07B0`, VS `402DB350`, PS `402DC340`
- pass `402F20A8`, program `402F5680`, VS `402EFCE0`, PS `402F0CD0`

Ghidra's `rage_fx_ApplyPass` (`0x82158C48`) confirms these are authoritative
draw-time fields: the pass descriptor owns the program pair at `+0x08`, and
that pair owns the vertex/pixel shader references at `+0x48/+0x4C`. The title
hook observes those pointers before the original function binds the programs
and executes its device command lists.

For example, the first guest program emits
`698,698,698,699,348,348,194,188,190,201,188`, exactly matching trace
commands 92-102. The third program emits late accessory draws including
`15` and `11`, matching trace commands 407 and 409. Omitting that program
made the ordered proof reset at logical draw 42 even when the first 41 title
draws matched. These identities are candidate gates only. They are not enough
to publish a draw.

## Publication gate

`tabletennis_venue_14d_observer` copies every candidate while its guest
buffers are live:

- complete raw vertex and submitted 16-bit index payloads;
- five authoritative texture fetches and immutable full texture mip chains;
- all 256 float4 vertex and pixel constant rows as raw words;
- declaration, owner token, pass/program identity, and ordered ordinal.

The common texture snapshot path exposes an explicit full-fetch-range mode.
For 14D it calculates the guest base and mip storage from the authoritative
Xenos fetch, captures the base/mip pair twice, validates every tiled or linear
read against that layout, unpacks mip tails, endian-swaps blocks, and preserves
every face of a cube at every selected level. Existing generic consumers retain
their mip-0-only default. All caches and individual guest/published payloads
remain bounded.

The translated-backend observer independently filters the exact 14D hash and
MAIN attachment contract. Each event retains the Vulkan command processor's
authoritative frame token. A frame is analyzed only after that backend token
has advanced, then requires:

- exactly three equal-sized tile blocks for that one frame token;
- the exact same ordered identities and backend state on all three tile
  replays, including raw `PA_SU_SC_MODE_CNTL` and its validity bit;
- a one-to-one `{primitive, index count, physical index base}` join from the
  first backend block to valid immutable title snapshots from the same frame;
- zero drops, read/copy/texture failures, or sequence mismatches.

Only then is an immutable `Venue14DFrameSnapshot` published. The master native
frame scene still compares the dynamic live count against the 47-draw
reference census separately; the observer proof itself does not confuse a
trace-specific culling count with a serving invariant.

## Current limitation

Serving remains disabled. Live gameplay proved that culling can produce 50
title program candidates while the backend contains three blocks of 49 real
family draws. The extra title candidate is excluded only by the exact
same-frame backend join; it is never guessed away by count or arrival order.
If either proof fails, the frame is rejected and guest rendering remains
untouched.

## Native observer renderer

`tabletennis_native_venue_14d_renderer=true` enables a default-off comparison
renderer. It consumes only a valid published `Venue14DFrameSnapshot`, keeps
the captured vf95 bytes in their original 8-in-32 guest order, and decodes
both verified layouts in the vertex shader:

- `4EAEC701E97DCDAD`: stride 40, tangent at byte 36;
- `08D6210341AD63F6`: stride 48, tangent at byte 44.

The HLSL ports the captured vertex transforms, tangent basis construction,
DXT5 green/alpha normal reconstruction, reflected ray/sphere intersection,
cube lookup, and the five-texture `14D6B61CBC3D853C` material equation. Raw
vertex/pixel constant words are copied from each immutable title draw. The
renderer preserves the independently verified backend order and overlays the
result after the untouched guest output. It does not install a draw replacer,
serve a field, suppress a draw, or participate in takeover readiness.

The observer creates real 2D/cube resources with every descriptor-selected
level and exposes the complete SRV range. Its shader now uses implicit-gradient
samples rather than forcing level zero. Full-chain capture is mandatory for
14D publication, so a missing, unstable, out-of-bounds, or unsupported mip
rejects the title snapshot instead of degrading silently. This removes mip
payload completeness as a serving blocker; exact per-fetch sampler state and
the broader coherent scene/depth takeover still require independent proof.

The immutable per-draw backend contract now also retains the raw rasterizer
mode. The observer logs its live unique-value census; native culling remains
disabled until that same-frame census proves a supported winding/cull tuple.

## Sampler contract

The five command-92 fetch constants and the other sampled 14D draws collapse
to two effective sampler states:

| fetch slots | coordinates | address U/V/W | fetch min/mag/mip | anisotropy | LOD |
| --- | --- | --- | --- | --- | --- |
| 0-3 | 2D | repeat | linear / linear / point | 2x | computed, bias 0, min 0 |
| 4 | cube | clamp-to-edge | linear / linear / point | 2x | computed, bias 0, min 0 |

The verified 14D disassembly contains plain `tfetch2D` / `tfetchCube`
instructions with no filter, gradient, register-LOD, or bias overrides, so the
fetch constants are authoritative. RexGlue's guest texture cache deliberately
normalizes an anisotropic fetch to linear min, mag, and mip filtering on both
Vulkan and D3D12. The native observer mirrors that behavior with two immutable
2x-anisotropic samplers: repeat for slots 0-3 and clamp-to-edge for slot 4.
Every draw is rejected unless all sampler-affecting fetch fields, format,
dimension, swizzle, numeric mode, and mip range match this proven contract.

NRHI does not currently expose dynamic sampler objects or sampler descriptor
tables. The two proven immutable states fit its static sampler model exactly,
so 14D does not need to approximate them. Supporting a future material whose
fetch state differs would require an SDK extension with:

- a sampler descriptor containing independent min, mag, and mip filters;
- independent U, V, and W address modes, border color, anisotropy, LOD bias,
  and min/max LOD;
- device create/destroy methods plus a bounded descriptor-keyed cache;
- a sampler-table binding parameter and command binding API, implemented as
  separate sampler descriptors on Vulkan and a sampler descriptor heap/table
  on D3D12.

Until that exists, an unseen sampler tuple fails closed instead of being mapped
to the closest static state.

# Backend frame identity

Every backend draw event retains the Vulkan command processor's
`backend_frame_sequence`. This is the authoritative PM4-consumption frame and
prevents an asynchronous backend proof from spanning title frames. Live
telemetry proved direct identity: title frame 2676's delayed backend work
carried token 2676 even though it arrived after later title swaps.
