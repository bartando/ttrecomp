# Generic logical-draw replay packet

`tabletennis_logical_draw_replay_packet` is the first renderer-independent
adapter from a verified title/backend join to a native logical draw. It does
not render, route, suppress, retain guest pointers, or invent missing state.

The first adapter consumes an immutable
`A406367569E5F6F8 / 0F9CCE179F32DA36` MAIN draw. That family is useful as the
tracer bullet because the existing observer has already proven the title draw
against three identical translated-backend tile blocks.

## Owned packet

Each packet copies:

- the title frame sequence and authoritative ordered-draw ordinal;
- backend vertex and pixel shader hashes;
- the complete decoded eight-element vertex declaration;
- the complete raw stride-96 vertex buffer and submitted big-endian uint16
  index range, including their stable-copy fingerprints and index bounds;
- all verified VS and PS constant ranges;
- all seven six-word texture fetch descriptors;
- immutable full-fetch-range texture snapshots for slots 0, 1, 2, and 6;
- world and world-view-projection matrices;
- the proven backend MAIN render-pass, EDRAM target, depth, blend, color mask,
  rasterizer, attachment, sample, and primitive-restart state.

VB and IB bytes are deep copies owned directly by the packet. Texture payloads
are shared immutable owners because `TextureSnapshot` already owns copied,
untiled texels and mip metadata.

`LogicalReplayFrame` preserves title ordinal order and rejects an invalid or
cross-frame packet.

## Honest readiness boundary

`valid()` means the packet contains complete immutable preparation evidence.
It does **not** mean independent submission is safe.

`submission_ready()` currently remains false and `missing` names the exact
blockers:

1. The backend now exposes value-owned translated artifacts, and the Table
   Tennis store retains them by exact `{hash, modification, stage}`. The A406
   draw contract does not yet expose its selected vertex/pixel modification
   identities, so no hash-only join is permitted.
2. A406 slots 3, 4, and 5 are descriptor-only evidence. Their
   descriptor-selected texels have not yet been promoted to owned snapshots.

The translated-artifact store is default-off and installed only when:

```text
tabletennis_native_translated_shader_artifacts=true
```

It requires a restart because the backend callback is registered during
native-renderer installation. Even while enabled, its cheap prefilter rejects
every translation except exact `{hash, modification, stage}` keys explicitly
requested by packet preparation. Captured artifacts own SPIR-V and translated
binding metadata; they never retain a backend shader module or pipeline-cache
pointer.

The next generic-replay step is therefore narrow: acquire immutable translated
modification identity at the draw boundary, join the already-supported
artifacts, and promote only the texture payloads proven necessary.
Once those resources exist, the same packet can drive one native logical draw
instead of adding an A406-specific renderer.

## First indexed venue adapter

PS328 (`0E9982BE6B1E99A1 / 328FA02B07C392DC`) is the first indexed adapter.
Venue capture now retains the exact guest-endian VB/IB bytes and the guarded
decoded title vertex declaration in addition to the existing decoded
reference-renderer streams. The adapter accepts one immutable full-family title
draw plus one exact translated replay token and copies:

- original 40-byte-stride VB and big-endian 16-bit IB payloads;
- the complete declaration identity;
- VS c0-6 and c12-15, PS c20 and c46;
- descriptor-selected texture payloads for slots 0 and 1;
- translated VS/PS modification identities, attachment signature, viewport,
  scissor, depth bias, blend/stencil values and opaque pipeline identity.

Admission fails closed on a shader, topology, count, index-address, attachment,
title-transform, or token-lifetime mismatch. Even after exact shader artifacts
arrive, `kTranslatedReplayState` prevents `submission_ready()` while the
backend reports mutable deferred resources. The adapter never suppresses,
records, resolves, or presents a draw.
