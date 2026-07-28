# 6AE player material pipeline contract

Evidence: deterministic gameplay trace
`out/trace/frames/545407DF_5661.xtr`, dumped with
`--trace_dump_shader=6AE43640A86B33D8`, and the title scene-catalog capture in
`/tmp/tt_owner_pass_catalog.log`.

## Exact family identity

The first MAIN tile contains 14 draws at commands 455-461 and 481-487.
Commands 896-902/922-928 and 1337-1343/1363-1369 repeat the same ordered
sequence for the other two EDRAM tiles. The logical block submits 14,764
16-bit indices as triangle strips.

Every backend draw uses vertex shader `BBB580AA5620D2A6` and pixel shader
`6AE43640A86B33D8`. One captured boot exposed this synchronous title-side
pointer tuple:

- pass descriptor `0x4008E110`;
- program pair `0x40093C30`;
- vertex shader object `0x4008C240`;
- pixel shader object `0x40098890`.

Those addresses are retained only as historical telemetry. Ghidra proves the
objects are heap allocations, so they are not a boot-independent identity.
A first-dispatch probe observed these immutable guest-template hashes:

- vertex template `C22A861F30E92845`;
- pixel `6AE43640A86B33D8`.

They are learned evidence, not a title gate. The observer captures the
player-owned structural superset, then the backend independently requires
translated vertex
`BBB580AA5620D2A6` with the same pixel hash. The vertex template and backend
bytes are not identical; this was live-proven rather than guessed away. The
backend's frame-tagged ordered sequence selects the exact title tokens and
must repeat as three identical EDRAM tile blocks.
Each join identity includes primitive and index count, guest index base, and
the primary vertex fetch physical base, byte count and endian mode. Publication
requires the forward-earliest and reverse-latest ordered joins to select the
same title tokens; an ambiguous subsequence is rejected.
The exact object layout and creation-path proof are documented in
`docs/player_shader_identity_ghidra.md`.

## Geometry and skinning

The mesh stream is `vf95`, endian mode 2, with a 36-byte stride:

- float3 position at byte 0;
- UNORM8x4 weights at byte 12, `zyxw`;
- U8x4 palette indices at byte 16, `zyxw`;
- signed 2_10_10_10 normal at byte 20;
- float2 UV at byte 24;
- signed 2_10_10_10 tangent at byte 32.

The second stream is `vf92`, endian mode 2, with 28-byte records containing a
float4 quaternion and float3 translation. The trace binds 364 records for the
first player and 366 for the second. Runtime counts may change, so capture
validates every palette record actually referenced by a nonzero vertex
weight instead of hardcoding either trace count.

## Material state

The pixel shader has 20 texture-fetch instructions but six distinct live
Xenos fetch slots. Fetch slots 0-2 are the changing material images; fetch
slots 5, 3 and 4 are the shared mask, resolved screen-depth and lookup inputs.
Capture retains all six exact descriptors and the complete descriptor-selected
mip range for every fetch slot. Shared slots are frame-keyed in the texture
snapshot cache. This is required for fetch slot 3: its address and descriptor may
remain stable while the resolved depth contents change between frames. All
draws in one frame share the same immutable snapshots, so the payload is
copied once rather than once per draw.

The trace-proven effective sampler contracts are:

| Fetch slot | Trace unique binding | Resource | Addressing | Min/mag/mip | Anisotropy | Swizzle |
| ---: | ---: | --- | --- | --- | --- | --- |
| 0 | 5 | DXT1 material, mipmapped | repeat | linear/linear/linear | 2:1 | `688` |
| 1 | 0 | DXT5 material, mipmapped | repeat | linear/linear/point | 2:1 | `688` |
| 2 | 1 | DXT1 or DXT5 material, mipmapped | repeat | linear/linear/point | 2:1 | `688` |
| 3 | 3 | D24S8 resolved depth, 640x480 or 1120x704 | clamp-to-edge | point/point/base | disabled | `B48` |
| 4 | 4 | RGBA8 32x32 lookup or D24S8 1120x704 resolved depth | clamp-to-edge | point/point/base | disabled | `60A` or `B48` |
| 5 | 2 | DXT1 256x256 shared mask | clamp-to-edge | linear/linear/point | 2:1 | `688` |

Admission also validates unsigned component interpretation, 2D non-stacked
dimension, tiling, endian mode, packed-mip ownership, LOD range/bias,
anisotropic walk flags, gradient adjustments, border state, `tri_clamp=3`,
and the otherwise-unused fetch bits. A renderer may only choose immutable
host samplers after this per-fetch-slot proof succeeds. The alternatives above
are not broad format admission: each was observed on backend-joined 6AE draws
in consecutive gameplay frames, and any other format, shape or sampler state
still rejects the frame.

Captured constants are vertex c12-c15, c19, c29-c36, c46-c47 and c255, plus
pixel c19, c21-c27, c46-c70 and c254-c255. Trace-proven literal rows are
validated rather than hardcoded as material values.

## Serving policy

This family is observer-only. Publication requires immutable geometry,
indices, a live referenced palette, material data, exact backend
shader/attachment/state identity, and three identical ordered EDRAM tile
blocks. No 6AE callback serves, replaces or suppresses a guest draw.

The backend proof also requires the raw render-target registers to agree with
their decoded fields and identify the 4x MAIN target
(`color_base=0x400`, `depth_base=0`, `pitch=1280`, `mode=4`,
render-pass key `0xE`). The primary fetch must be endian mode 2 and a nonempty
multiple of the 36-byte layout. Rasterizer mode is captured and required to be
the live-proven `0x00018002`. These fields are compared across all three tile
blocks rather than inferred from attachment formats.

`tabletennis_player_6ae_geometry_probe.hlsl` is the first native tracer bullet:
it decodes the real 36-byte vf95 stream, performs the four-weight vf92
quaternion skinning, and applies the captured c12-c15 clip transform. The clip
transform is the algebraic result of guest instructions 132-137:
`world.x*c14 + world.y*c12 + world.z*c13 + c15`.
The palette rotation also preserves the guest's cross-product reconstruction
of its third basis column instead of substituting a generic quaternion helper.
The companion `tabletennis_player_6ae_bbb580_vertex.hlsli` now ports the
remaining BBB580 outputs required by the material: both packed direction
fields, the normalized skinned frame and handed cross-axis, the conditional
material selector, world position, and all c29-c36 projections in the exact
o0-o6 slots.

`tabletennis_player_6ae_material_stage.hlsli` now ports pixel instructions
19-59 as a second tracer bullet. It preserves the guest register write masks,
samples tf1/tf2/tf5 in source, reconstructs the perturbed normal in the
skinned tangent frame, and evaluates the exact six-term c64-c70 diffuse
sum. The overlay displays the real instruction-59 RGB intermediate directly;
it does not invent a final material equation. Because tf2 and tf5 values are
only consumed after instruction 59, the optimized probe binary truthfully
eliminates those two reads and retains tf1. All captured pixel constant blocks
are uploaded verbatim rather than hardcoded.

This is still explicitly an observer material-stage probe, not a claim that
the full 185-instruction 6AE pixel material has been ported. Instructions
60-185 still contain the conditional light construction, two eight-tap
tf3/tf4 depth tests, tf0 albedo modulation, specular lobes and final
composite. The overlay must remain non-suppressing until those slices are
ported and its output is compared against the guest draw.

Live validation in `/tmp/tt_late_6ae_live.log` repeatedly recorded the
instruction-59 diffuse intermediate over the untouched guest output using
immutable vf95/vf92/index payloads and real texture fingerprint
`B2A763C328D2D114`. No draw was suppressed.
