# Net `BB90345BFEEE544B` observer contract

This module is observer-only. It does not match a replacement route, record
native draw commands, suppress a guest draw, or claim serving readiness.

## Reference census and live identity

The historic gameplay MAIN trace groups these command rows under the last
bound BB903 pixel shader:

- pixel shader `BB90345BFEEE544B`;
- indexed content vertex shader `20EA5AF4BA4E164B`;
- auto-indexed helper vertex shader `72CBCAA6A7984111`;
- logical event sequence `[4680, 4680, 3, 3, 3, 3, 3, 3]`;
- two triangle-list content submissions and six primitive-8 helpers;
- 8 logical draws and 9,378 logical indices;
- the same sequence in three EDRAM tile blocks, 441 commands apart.

Those six helper rows are census-only. Fresh live instrumentation in
`/tmp/tt_dynamic_hash_net.log` proves that the Vulkan eligibility callback,
which is emitted once for each guest draw after primitive processing, sees
exactly six identical-shape BB903 events per gameplay backend frame:

- VS `20EA5AF4BA4E164B`, PS `BB90345BFEEE544B`;
- triangle list, `guest_count=host_count=4680`;
- processed 16-bit guest index buffer present;
- replacement-eligible;
- two content draws repeated across three EDRAM tile blocks.

It never sees VS `72CBCAA6A7984111`, primitive 8, or a three-vertex BB903 draw.
The trace helper rows inherit command-state labels but are not draw-dispatcher
events. They must not participate in backend proof and a future compositor
must not draw them. The live renderable family is two draws and 9,360 submitted
indices.

The shared MAIN attachment identity is render-pass key `0xE`, pitch 1280, one
RGBA8 color attachment, a combined D24S8/D32S8 host depth-stencil attachment,
4x MSAA, and a full sample mask. The observer requires this exact attachment
shape for both content submissions and equality across all three tile blocks.

## Payload promotion

`tabletennis_net_bb903_observer` does not copy vertex or index bytes. The title
API retains the already immutable mesh returned by:

- `LatestTableMeshSnapshot()`;

The material-parameter cache does not contain this family's live scope shader,
so it cannot authoritatively join the two textures after the draw. Once the
strict title identity below admits a draw, the observer instead passes that
draw's two synchronous six-dword fetches directly to
`CaptureTextureSnapshot()`. The capture function fault-guards two identical
guest copies, untiles mip 0, and publishes only an immutable payload. The
result must still match the expected handle, size, BC3 format, owner shader,
and exact fetch words before the title snapshot retains it.

The old `TableMeshSnapshot` is admitted only when the live title draw proves
the full net signature:

- triangle list, 4,680 16-bit indices;
- selector 2, 96-byte stride;
- geometry 0, LOD 0, alternate pass;
- valid draw state and world/WVP transforms;
- a physical index-buffer alias.

The two title submissions are then identified without relying on
`CurrentTableVisibleMaterialPass()`, which live evidence proved does not
describe this family:

1. The first exact-geometry draw must have the direct semantic `lvlTable`
   owner, table-renderable role, and vtable `0x8204F6D4`. It learns the
   frame-local shader/model, pass descriptor, program pair, title shader
   pointers, and vertex/index aliases.
2. The later submission must be ownerless and match that learned identity
   byte-for-byte.

This admits the observed selector-2 pair while rejecting nearby selector-3
draws and unrelated ownerless shapes. Runtime heap addresses are learned
again every title frame; none are promoted to boot-independent constants.

The retained mesh itself must additionally prove:

- the exact captured vertex/index aliases;
- 948 decoded vertices and 4,680 indices.

The two retained BC3 textures are:

| Handle | Size | Draw slot |
| --- | --- | --- |
| `00080002` | 512x512 | 0 |
| `00100006` | 512x256 | 1 |

Each immutable texture's owner and six fetch words must equal the synchronous
draw-time catalog state. The observer retains both title submissions
separately, including their world/WVP matrices and c12-c15 values. The two
live net phases may intentionally share the same WVP; phase identity comes
from ordered title/backend identity and the independently captured draw state,
not an invented transform-inequality requirement.

Pass, program, and title shader pointers are recorded as telemetry, not
hard-coded identities. They are runtime heap addresses and may change between
boots. Backend ucode hashes provide the stable independent identity.

## Backend taps

The two backend feeds are intentionally separate:

1. `ObserveNetBB903BackendDraw` consumes the normal draw-replacer dispatcher
   stream. It ignores the early probe and records only late indexed content
   contexts with complete borrowed attachment and draw-state contracts.
2. `ObserveNetBB903BackendEligibility` consumes the Vulkan pre-gate stream.
   This is now census telemetry only; it independently confirms the six live
   events are indexed content submissions.

The eligibility contract exposes both the guest submission count and the
post-conversion host draw count. Bounded BB903 tuple samples print both values
plus the processed-index and eligibility flags. Counters retain PS, content
shader-pair, content shape, indexed-contract, and eligible matches. The
eligibility tap does not publish proof events because it lacks the exact
borrowed attachment and raster contracts available at the normal late draw
tap.

Every accepted backend event retains the SDK's authoritative
`backend_frame_sequence`. The title frame-end counter advances with the same
swap-boundary convention used by the 14D/E33 observers. Events join only the
pending title ledger whose sequence is exactly equal; there is no
oldest-pending-frame fallback. Events for a completed title sequence with no
exact pending ledger are discarded and counted.

The observer waits until a newer authoritative backend frame token proves the
bucket is complete. It then derives `draws_per_tile = event_count / 3`, requires
that value to equal the exact title candidate count, and treats the first live
tile as the contract source. Both later tile blocks must repeat every content
identity, attachment, and raster field byte-for-byte. The first block must
also join the two ordered title draws by physical index-buffer base. This
avoids hard-coding trace helper rows the live dispatcher never emits.

A published snapshot repeats the authoritative sequence in
`backend_frame_sequence`; `observer_complete()` requires six backend events,
two draws per tile, three equal tile blocks, and equality with the title
`sequence`.

## Raster limitation

The live backend context exposes these values for each content submission:

- normalized depth control;
- normalized color mask;
- raw color control;
- raw blend control 0;
- primitive-restart value and effective state.

They are captured verbatim and compared across tiled repetitions. They remain
marked `observed=true, proven=false`. The historic BB903 trace predates
`TRACE_RASTER`, so repetition only proves stability, not correctness against
the title's intended net state.

Consequently:

- `NetBB903FrameSnapshot::observer_complete()` may become true;
- `NetBB903FrameSnapshot::ready_to_serve()` always remains false;
- telemetry always reports `raster_tuple_proven=false` and
  `serving_enabled=false`.

A new trace containing the BB903 raster tuple must be checked before any
serving or suppression work.

## Shared integration

The observer is integrated through these exact calls:

1. Add `src/native/tabletennis_net_bb903_observer.cpp` to
   `TABLETENNIS_SOURCES`.
2. Arm the existing mesh payload producer for the observer without enabling
   the diagnostic overlay:

   - in `ObserveTableModelSubmission`, include
     `NetBB903ObserverEnabled() && !HasTableMeshSnapshot()` in the condition
     which calls `ProbeModelGeometry`.

   This is a gate around the existing mesh copy path. Texture copies occur
   synchronously from the admitted draw as described above; material texture
   capture remains independent observer telemetry, not a BB903 dependency.
3. In `ObserveSceneDrawCatalogIndexedDraw`, after the local `draw` value is
   fully populated and after the catalog mutex is released, call:

   ```cpp
   ObserveNetBB903TitleDraw(guest_base, draw);
   ```

   This must remain inside the synchronous indexed-draw hook so the direct
   owner anchor is observed before its ownerless sibling.
4. In `DrawReplacerDispatcher::Match`, beside the other non-claiming observer
   taps and before route selection, call:

   ```cpp
   ObserveNetBB903BackendDraw(context);
   ```

5. Multiplex the single SDK eligibility callback rather than replacing the
   existing CA9 diagnostic. Its callback body must call:

   ```cpp
   ObserveNetBB903BackendEligibility(context);
   if (PlayerReplacementGateDiagnosticEnabled()) {
     ObservePlayerReplacementDrawEligibility(context, nullptr);
   }
   ```

   Install this multiplexer whenever the BB903 observer or the CA9 gate
   diagnostic is enabled. For BB903 it remains independent census telemetry;
   the late normal draw tap owns the attachment-complete frame proof.
6. At the title swap boundary, call:

   ```cpp
   NetBB903ObserverFrameEnd();
   ```

   Place it with the other family frame-end publishers and before
   `NativeFrameSceneFrameEnd()`.
7. A later frame-scene integration may retain
   `LatestNetBB903FrameSnapshot()`, but must not add its two renderable draws to
   serving readiness while `ready_to_serve()` is false.

No renderer or suppression route belongs in this integration.
