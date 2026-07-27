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
fetch bindings. Bindings 0, 1 and 5 are the changing material images;
bindings 2-4 are the shared mask, screen-depth and lookup inputs. Capture
retains all six descriptors and immutable mip-0 payloads for bindings 0, 1
and 5.

Captured constants are vertex c12-c15, c19, c29-c36, c46-c47 and c255, plus
pixel c19, c21-c27, c46-c70 and c254-c255. Trace-proven literal rows are
validated rather than hardcoded as material values.

## Serving policy

This family is observer-only. Publication requires immutable geometry,
indices, a live referenced palette, material data, exact backend
shader/attachment/state identity, and three identical ordered EDRAM tile
blocks. No 6AE callback serves, replaces or suppresses a guest draw.
