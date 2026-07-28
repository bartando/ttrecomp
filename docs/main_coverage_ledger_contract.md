# Observer-only MAIN coverage ledger

`tabletennis_main_coverage_ledger` is the capture-first inventory for a future
native MAIN compositor. It observes the game's real submissions. It does not
render, replace, suppress, or authorize takeover.

## What it proves

For each authoritative backend frame sequence, the ledger:

1. collects only late translated-backend callbacks whose raw Xenos target
   registers prove the MAIN target identity;
2. retains value-only draw identity:
   `{primitive, guest-submitted index count, physical index base}`;
3. retains shader hashes, raw target registers, their decoded EDRAM identity,
   and the complete available attachment, depth, blend, raster, restart,
   sample-count, and sample-mask contract;
4. retains every phase of the largest contiguous `A,A,A` sequence in the
   backend stream;
5. requires those three non-empty tile blocks to repeat observable identities,
   order, and shader hashes exactly;
6. computes a maximum-cardinality monotone join, with gaps on both backend and
   title, for every maximal phase, including the actual low-level bound pixel
   shader fingerprint;
7. selects exactly one phase only when its match count is strictly greatest
   and its optimal mapping is unique;
8. requires complete, identical backend contracts across all three tiles for
   every title-joined offset; and
9. labels joined ordinals claimed by valid same-sequence family snapshots.

No reference draw count such as 441, 355, 156, 72, 47, or 23 is a live
admission condition. The tile-block count is the title's proven MAIN rendering
shape; the number of draws inside each block remains dynamic.

MAIN may emit callbacks that do not join a title draw. Every unmatched backend
offset is retained across all three tiles, along with callbacks before and
after the selected tiled core. `backend_only_events` keeps the original event
index, identity, shader hashes, captured contract and its completeness flag,
region, tile, and tile-local offset. Only title-joined events must have a
complete draw and attachment contract. Incomplete repeated callbacks stay
unmatched backend-only offsets; their retained identity or shader fields may be
zero when the backend callback itself did not expose them. A cyclic boundary
event remains prefix or suffix
backend-only/uncovered work depending on the uniquely selected phase; it is not
reclassified as a helper.

`backend_tile_event_count` is the raw length of one repeated tile block.
`logical_main_draw_count` is the smaller title-joined match count whenever
interleaved backend-only offsets exist. Backend-only callbacks never become
title ordinals, never increase the logical draw count, and are never counted
as covered.

## Auto-indexed MAIN prefix rectangle

The persistent callback immediately before the three-tile core is not an
indexed scene draw and must not be invented as a title ordinal. Primitive `8`
is `xenos::PrimitiveType::kRectangleList`, not a triangle list. RexGlue runs
the three guest control vertices through the guest vertex shader, synthesizes
the fourth corner, and submits a four-index host triangle strip. Its exact
translated shader pair is:

- VS `0A6D1DD7767FDF27`: fetches vf95 with a 28-byte stride as
  `{float3 position, float4 color}`, writes the position directly, and exports
  the color as interpolator 0;
- PS `2E372EA28CC404B7`: reads interpolator 0 and exports it to color target 0.
  It contains no texture fetch or discard.

These facts come from both the guest microcode disassembly and the independently
translated SPIR-V dump. They prove this shader pair is color-output work, not a
barrier or no-op, but shader identity alone is not enough to classify a live
callback.

For that pair the ledger now performs a bounded, fault-guarded proof:

1. require Vulkan, the exact MAIN raw/decoded target, rectangle-list primitive
   8, exactly three guest control vertices, RexGlue's four-index host expansion,
   auto-indexed submission, and the exact shader hashes;
2. require the complete borrowed RGBA8/depth/stencil 4x attachment contract,
   complete draw/raster state, and an enabled color channel;
3. require vf95 endian 2 with at least three 28-byte records;
4. copy only the first 84 bytes twice through `GuestTryCopy`, reject a revoked
   or changing range, and retain an FNV-1a payload fingerprint;
5. decode and retain the three positions plus both decoded color values and
   their raw bits;
6. require the exact three-corner rectangle geometry and color basis observed
   live: `(1, 0xFFFFFFFF, 0, 0)`, `(1, 0xFFFFFFFF, 1, 0)`, and
   `(1, 0xFFFFFFFF, 0, 1)`.

`0xFFFFFFFF` is an intentional negative quiet-NaN in the green channel, not a
bad guest read. It is preserved as bits because native replay must reproduce
the shader and render-target conversion rather than sanitizing the value.
For the observed 320x180 rectangle the whole 84-byte payload repeats with
fingerprint `48F89B1D4D072A58` even as its guest physical address rotates.

Only then is the event labeled `vertex_color_rectangle_output`. The proof owns
host values only. Classification remains observer-only: it does not render,
suppress, create a title ordinal, or make `all_backend_events_covered()` true.
A future native MAIN transaction must serve this rectangle output explicitly.

The public `MainCoverageFrameSnapshot` owns only host values and containers.
It retains no guest pointer, guest allocation, RHI resource, or command object.

## Exact guest target identity

The backend render-pass cache key is retained for diagnostics and repeated-tile
equality, but it is not a MAIN admission condition. In particular,
`render_pass_key == 0xE` is not treated as proof of guest target identity.

Every candidate retains the raw draw-point values of:

- `RB_COLOR_INFO[0]`;
- `RB_DEPTH_INFO`;
- `RB_SURFACE_INFO`; and
- `RB_MODECONTROL`.

The backend also publishes the decoded color EDRAM base, depth EDRAM base,
surface pitch, and EDRAM mode. Before accepting a callback, the ledger
independently decodes the documented contiguous register fields from the raw
values and requires them to equal the published decode. It then requires the
trace-known MAIN signature:

- color EDRAM base `0x400`;
- depth EDRAM base `0`;
- surface pitch `1280`; and
- EDRAM mode `4` (`kColorDepth`).

The borrowed attachment contract separately proves RGBA8 color, supported
depth/stencil format, and 4x sampling for complete title-joined events. Missing
raw state, a raw/decoded mismatch, or any target-signature mismatch fails
closed. Raw values are part of the retained backend contract, so the existing
three-tile state-equality proof also covers all non-address target bits.

## Unique ordered join

Counts are not identities and a physical mesh may be reused by another pass.
The ledger therefore does not take the first matching title draw. A title token
uses the pixel shader observed at the low-level device bind postcondition when
available, falling back to ApplyPass microcode only when needed.

Vertex shader hashes are deliberately not compared across the title/backend
boundary. The title owns immutable vertex templates while the backend may own
one of several transformed variants for the active vertex contract; this is
documented in `player_shader_identity_ghidra.md`. Same-frame family proofs may
still compare backend VS hashes because both sides of that comparison come
from the translated backend.

For every maximal backend phase, it computes the longest monotonic mapping
between the raw first tile block and the ordered title catalog. Either side may
have gaps. Distinct optimal mappings are counted by matched index pairs, not by
equivalent skip paths, and the count is capped at two:

- a greatest match count of zero rejects the frame as an order/identity
  mismatch;
- a tie for greatest match count across phases rejects the tile phase as
  ambiguous;
- two optimal mappings within the sole greatest phase rejects the title join
  as ambiguous; and
- exactly one optimal mapping in a strictly greatest phase publishes its
  matched original title ordinals as the logical MAIN draws.

Invalid title identities or missing pixel fingerprints are never guessed. A
missing physical alias simply makes an exact mapping impossible. Aggregate
catalog read failures from unrelated, unselected records remain telemetry and
do not invalidate a uniquely selected exact mapping. Dropped ordered records
still reject the whole frame because ordering would be incomplete.

## Family assignments and proof tiers

Assignments are accepted only when:

- the family snapshot sequence equals the archived catalog sequence;
- the family snapshot is valid under its own capture contract;
- its ordinals are strictly increasing before any global merge;
- no ordinal is claimed twice; and
- the family `{primitive, submitted count, physical index base}` equals the
  independently joined MAIN identity for that ordinal.

The snapshot exposes separate proof tiers:

- `kTitlePayloadOnly`: PS328 has an exact immutable title payload but no backend
  proof retained in that snapshot;
- `kBackendEvidenceNotSequenceTied`: CA9 currently has backend evidence, but its
  API does not bind that evidence to the snapshot sequence;
- `kSameFrameBackendProof`: 14D, E33, C6, D47, 6AE, and BB903 retain their own
  same-sequence backend join.

These labels prevent ordinal coverage from being mistaken for renderer
readiness.

BB903 contributes two real indexed content ordinals. Its six primitive-8 trace
helper rows do not reach the indexed draw dispatcher and are not invented as
title ordinals by this ledger. The 441-command trace census and the dynamic
indexed MAIN ledger are intentionally separate measurements.

## Sequence ownership

Every `SceneCatalogDrawOccurrence` now carries the catalog's authoritative
building sequence. PS328, 14D, E33, CA9, D47, 6AE, and BB903 publish that value
instead of resettable parallel counters, so hot-enabling capture cannot join a
current backend frame to an old title sequence.

The coverage module keeps a bounded sequence-keyed archive because family
backend proof is asynchronous. Frame `N` is finalized only after observing a
MAIN event from a newer backend sequence, which proves all events for `N` have
arrived. Every asynchronous family observer advances on that same global
backend sequence boundary, even when its own shader is culled in `N+1`, and
publishes pending proof before the MAIN ledger records the closing event.

## Failure policy

The complete frame fails closed on:

- missing/empty catalog publication, dropped ordered records, or no exact
  selected-token join;
- missing, malformed, or overflowed backend events;
- no maximal exact three-block extraction, or no single phase with a strictly
  greatest title alignment;
- any repeated-tile identity, shader, order, or state mismatch;
- no title mapping or multiple title mappings;
- duplicate/regressing family ordinals;
- overlapping family claims; or
- a family claim whose value identity differs from the joined MAIN draw.

A valid snapshot may still contain unassigned draws or backend-only events.
Unassigned title draws are grouped by
backend `{vertex shader hash, pixel shader hash}` and retain every original
ordinal. `all_ordinals_assigned()` reports label coverage, including weaker
title-only and sequence-untied tiers. `all_draws_same_frame_proven()` requires
every assignment to own a same-frame backend proof and requires zero
backend-only events. `all_backend_events_covered()` is false whenever any
prefix, interleaved-tile, or suffix backend-only event exists. None of these
predicates is a serving gate.

## Integration

- `ObserveMainCoverageBackendDraw` runs before replacement routing.
- `MainCoverageLedgerFrameEnd` runs after `SceneDrawCatalogFrameEnd` and after
  the family observers have had a chance to publish delayed proof.
- Enabling `tabletennis_native_main_coverage_ledger` arms the existing immutable
  frame-scene capture graph.
- `LatestMainCoverageFrameSnapshot()` exposes the newest finalized result for a
  future atomic compositor readiness layer.

The log interval is controlled by
`tabletennis_native_main_coverage_log_interval`. Reports include assigned and
unassigned totals plus grouped shader hashes and ordinal lists.
