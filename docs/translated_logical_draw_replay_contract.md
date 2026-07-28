# Translated logical-draw replay contract

This is the shortest credible path from the current capture graph to a
complete native frame. It reuses RexGlue's translated guest shaders as a
bootstrap, renders one copy of every logical MAIN draw, and keeps hand-written
HLSL only for shaders that cannot be replayed correctly or cheaply.

It is not a serving contract yet. No draw may be suppressed until a complete
private frame has been compared against guest output.

## What already exists

The pieces needed to select and order replay work are real:

- the MAIN coverage ledger uniquely joins the first of three equal backend
  tile blocks to title ordinals and retains complete fixed state;
- immutable family snapshots own geometry, constants, and many texture
  payloads;
- the private scene transaction owns an output-sized four-sample RGBA8 target,
  records original-ordinal work, and aborts/discards without touching guest
  output;
- the late-phase ledger proves transition, 2AC, post, gameplay HUD, and
  conditional final-compositor order; and
- the transition observer owns exact post-pipeline viewport, scissor, texture,
  attachment, and fixed-state values for its shader pair.

The translated programs also exist inside RexGlue. Vulkan shader translations
retain `translated_binary()`, and pipeline descriptions retain the exact
vertex/pixel shader modifications used by a live draw.

That does **not** make the SPIR-V directly usable through NRHI. The translated
programs depend on RexGlue's private pipeline layout:

- system, vertex-float, pixel-float, bool/loop, and fetch constant buffers;
- shared-memory/EDRAM descriptors used for vertex pulling and guest access;
- transient texture and sampler descriptor sets;
- the translated pipeline modification and render-target path; and
- converted or guest-DMA index-buffer state plus dynamic Vulkan state.

NRHI's application binding layout is intentionally different. Copying only
the SPIR-V blob into `nrhi::ShaderDesc` would create a shader whose descriptor
contract cannot be satisfied.

## Backend-owned replay packet

The Vulkan command processor should capture an opaque packet immediately
after `UpdateBindings`, residency, pipeline, viewport, and scissor preparation.
Title code receives only an immutable token and value identity; raw Vulkan
objects stay in RexGlue.

One packet owns or safely retains, until the native output callback for that
backend frame:

1. backend frame sequence, backend draw index, guest identity, shader hashes,
   and exact shader modification keys;
2. the pipeline-cache handle and pipeline layout;
3. copies of all constant-buffer descriptor ranges used by the draw;
4. stable shared-memory/EDRAM and texture/sampler descriptor bindings;
5. index buffer, offset, type, guest and host counts, and converted topology;
6. viewport, scissor, blend constants, stencil reference/masks, depth bias,
   sample mask, and primitive-restart state; and
7. attachment formats/sample count required by pipeline compatibility.

The public token is valid only for its owning output callback. RexGlue rejects
another frame sequence, an expired transient descriptor generation, a
placeholder pipeline, an incomplete binding set, memexport, tessellation, or
an unsupported render-target path.

### Deferred-replay safety gate

The current phase-zero token is an **identity observer**, not a replay-safe
resource snapshot. `valid` proves that the pipeline identity, required
descriptor sets, all five constant-buffer ranges, dynamic state, index
binding, and attachment signature were present when the callback ran.
`resources_stable_for_deferred_replay` remains false because object lifetime
alone does not freeze resource contents or layouts:

- transient descriptor sets are not recycled until their owning frame
  completes, and uniform-buffer pool ranges are append-only for that frame;
  those two pieces are safe to retain within the frame;
- set 0 still points at mutable shared memory/EDRAM, so vertex-pulled bytes may
  be uploaded or changed after capture;
- texture descriptors retain views and samplers, not the image layout or a
  frozen texel version; later guest draws may transition or update them;
- guest-DMA indices may point into mutable shared memory, while converted
  indices are only guaranteed by the primitive processor's current-frame
  allocation lifetime; and
- a compatible render-pass signature does not make the guest attachments the
  private replay attachments, nor permit opening a second scope while the
  guest scope is active.

The grouped replay API must therefore either record immediately at the capture
point or create backend-owned immutable snapshots/pins for shared-memory
ranges, texture views plus image/layout state, constant ranges, and index
bytes. It must reject every token whose
`resources_stable_for_deferred_replay` is false. Descriptor handles, buffer
handles, and same-frame sequence checks are necessary but insufficient.

The private token also needs to retain the value-owned viewport, scissor,
depth-bias, blend/stencil state, topology/counts, and primitive-restart state
before a replay entry point is added. Those values currently exist only in the
public observer context and must not be reconstructed from whatever backend
state happens to be current at replay time.

The replay API should be a grouped transaction rather than one public
`VkPipeline`:

```text
BeginTranslatedReplay(output_context, private_target_contract)
ReplayTranslatedDraw(token, normalized_untiled_state)
EndTranslatedReplay()
```

RexGlue unwraps the Vulkan NRHI command/attachments internally, creates or
reuses an attachment-compatible private render scope, binds its private
translated layout, and restores NRHI state on exit. Failure aborts the
discarded private transaction; it never falls back after partially recording
into guest output.

## Exact private target

The existing private target is RGBA8 plus D32, four samples. A live MAIN guest
pipeline uses RGBA8 plus the backend's exact packed D24S8 representation
(`D24_UNORM_S8_UINT` or the MoltenVK `D32_FLOAT_S8_UINT` fallback), four
samples. Vulkan pipeline compatibility includes depth/stencil format and
sample count.

Translated replay therefore uses the separate preparation-only
`tabletennis_exact_main_targets` owner:

```text
color   = R8G8B8A8_UNORM, 4x, 1280x720
depth   = the captured MAIN depth/stencil format, 4x, 1280x720
samples = captured sample mask
```

Do not silently reuse the current D32 custom-HLSL target. Both target types may
share `OffscreenTargetOwner` allocation, validation, abort, and resolve-state
machinery, but their resources and format contracts remain distinct. Exact
MAIN preparation records no commands and retains the captured sample mask for
the future translated pipeline.

## Three-tile elimination gate

Selecting the first repeated backend block is necessary but not sufficient.
The three instances may differ in state that the current MAIN ledger does not
retain. Before replaying one copy, capture and compare, for each uniquely
joined ordinal:

- complete `SystemConstants` bytes;
- usage-masked vertex and pixel float constants;
- bool/loop and fetch constant buffers;
- texture/sampler bindings and resource identities;
- viewport, NDC scale/offset, and scissor; and
- index conversion and dynamic draw state.

Classify every difference as one of:

- **invariant**: byte-identical and replayable as captured;
- **mechanical tile state**: viewport/scissor/NDC or translated target fields
  with a proven output-sized untiled normalization; or
- **semantic tile state**: title shader constants, resources, or control flow
  differ.

Only the first two classes may collapse to one draw. A semantic difference
fails closed and sends that shader family to the existing custom capture
renderer. This diagnostic is the first real proof that generic replay saves
the three tiled submissions rather than merely drawing the first third of the
screen.

## Tracer bullets

### Phase 0: translated pipeline plumbing

Use the persistent backend-only rectangle:

```text
VS 0A6D1DD7767FDF27
PS 2E372EA28CC404B7
primitive 8, three guest control vertices -> four-index host strip
payload fingerprint 48F89B1D4D072A58
```

It has no texture fetch, no indexed guest geometry, and a fully proven
position/color payload. It is the smallest test of shader translation lookup,
private compatible render scope, translated descriptor layout, auto-index
expansion, and output readback. Compare its private pixels with the same guest
rectangle. It does not prove tiled logical-draw collapse because it lives
outside the indexed three-tile core.

The title-side phase-zero coordinator is
`tabletennis_phase0_rectangle_replay.{h,cpp}`. It fails closed while joining:

- the stable, double-read 84-byte guest rectangle payload and fingerprint;
- the current-backend-frame opaque token;
- the exact `{hash, modification, stage}` VS and PS artifacts;
- the token-derived RGBA8 + packed depth/stencil, 4x MAIN contract; and
- private exact-MAIN attachments validated against an output context.

Target preparation and replay happen together in the output callback. The
rectangle token is executable only when the backend's bounded vf95 payload
watch proves the exact 84 bytes survived unchanged; all other mutable-resource
tokens remain fail-closed. Attachment pointers never escape the callback.

After successful replay, `tabletennis_phase0_rectangle_readback.{h,cpp}`
shader-resolves the exact private 4x RGBA8 attachment into a separate private
one-sample texture. It queues a texture-to-readback copy, waits for
`CompletedSubmission()` before CPU access, and publishes a bounded diagnostic:
visible-byte FNV-1a hash, nonzero-pixel count and bounds, and the first nonzero
RGBA pixel. The resolve and copy never touch the presenter or guest resources,
and no guest draw is replaced or suppressed.

### Phase 1: first indexed logical MAIN draw

Use a PS328 draw with:

```text
VS 0E9982BE6B1E99A1
PS 328FA02B07C392DC
```

Choose it dynamically from the unique first-tile MAIN join, not by a hardcoded
ordinal or index count. PS328 is preferable to A406 for the first dedupe proof:
the project already owns its immutable geometry, two textures, constants, and
a hand-written native reference renderer. The same captured draw can therefore
be rendered three ways:

1. guest translated output;
2. backend-owned translated replay; and
3. the existing custom PS328 native path.

The experiment proves all three tile instances are invariant or mechanically
normalizable, records exactly one private translated draw, and compares a
cropped image/hash against both references. A406 remains a useful second
generic packet because it exercises a complex declaration and material, but
its seven descriptors and missing owned payloads in slots 3, 4, and 5 make it
a poor first pipeline test.

The private PS328 coordinator consumes only
`PrepareLatestPs328LogicalReplayJoin()`. It does not repeat title selection or
resource equivalence logic. Replay is attempted only when that public join
reports a complete packet, exact token, invariant three-tile proof, stable
backend resources, and no required mechanical normalization. Otherwise it
logs the explicit join blocker and records nothing. A successful draw uses the
same exact private MAIN target and asynchronous diagnostic resolve/readback as
the rectangle, tagged separately as `kind=ps328`; guest rendering remains
untouched.

The first executable PS328 diagnostic deliberately remains narrower than that
future normalized packet path. `NativeGuestOutputRenderContext` carries the
authoritative command-processor frame sequence. At the output callback, the
coordinator queries the PS328 observer for the first guarded token from exactly
that frame whose shader and local strip shape match a prior finalized,
non-semantic, resource-invariant PS328 proof. It replays that live tile-1 token
unchanged and tags its private readback `kind=ps328`. The prior proof's expired
Vulkan token is authorization evidence only and is never submitted. Canonical
viewport/NDC/SystemConstants normalization remains a later packet milestone.

The next private diagnostic widens only the same-frame tile-1 scissor. It
requires the prior proof's canonical normalization, exact live
viewport/NDC/depth transform, local scissor origin and strip height, and
valid live SystemConstants offsets/mask whose bounded NDC bytes equal both the
live token arrays and prior canonical NDC. Other SystemConstants bytes may
legitimately vary between frames: the helper copies the guarded current token
and changes only its scissor extent to 1280x720. Opaque resources, constant
buffers, viewport, and every other dynamic field remain untouched. Failure of
any comparison records no replay.

The full-family diagnostic extends that proof as one fail-closed private batch.
It obtains the backend's complete ordered current-frame tile-1 token vector and
preflights its count, family offsets, shader/modification identity, topology,
index conversion, submitted counts, guarded resources, canonical scissor-only
normalization, and one common attachment signature against the prior
authorization snapshot. Every token must pass before target preparation or
the first replay command.

The coordinator then replays the normalized tokens in exact vector order into
one exact private MAIN target. Only the first draw clears color and
depth/stencil; every later draw loads and preserves earlier results. One
asynchronous readback is queued after the complete batch succeeds and is
tagged `kind=ps328_batch`. A count, order, identity, normalization, attachment,
target, replay, or readback failure never presents, replaces, suppresses, or
writes guest rendering.

### Post-proof PS328 retirement

Once the native output callback observes a retained, private-batch-ready PS328
family authorization, it retires only PS328 title/artifact proof construction.
Full PS328 title payload copies, redundant PS328 native invariance
publication, and repeated PS328 artifact requests stop. MAIN coverage, scene
catalog publication, frame-scene capture, and other family learners remain
active so the next family can be proven without restarting the title. The 9E
venue observer is armed directly by MAIN coverage or translated-artifact
capture; E33 continues through the frame-scene full-capture path.

Retirement does not change any cvar, shut down the translated artifact store,
or reset the retained proof. The PS328 replay-token filter remains installed,
so RexGlue continues producing and guarding the complete ordered current-frame
family used by the private query and replay. Explicit PS328 observer or overlay
cvars still override retirement for diagnostics. The latch is one-way until
normal renderer shutdown resets the translated diagnostic state. While
latched, a CPU-only heartbeat reports elapsed time and effective native-output
callback rate every 120 callbacks without querying or waiting on the GPU.

## Generic MAIN capture

The family-at-a-time route is retired as the primary path. Proving PS328, 9E,
C6 and 14D individually established the mechanism - three-tile matching,
untiled normalization, global draw order, resource lifetime, crash-free
private replay - but repeating it per material does not scale and is not where
the measured cost lives.

`vulkan_generic_main_guarded_replay` (default off, diagnostic only) replaces
the fixed family whitelist with a runtime family table:

- any shader pair on the host render-target path is discovered at run time and
  guarded under `kGuardedGenericFamilySpec`, seeded alongside the four tuned
  families;
- discovered families share one frame-wide byte budget, so admitting more of
  the pass cannot multiply capture cost by family count;
- a family that submits draws but never proves for
  `kGuardedGenericFamilyAbandonFrames` frames is dropped from capture, which
  retires non-MAIN work without a per-draw tax; and
- `TryGetCurrentFrameGuardedMainReplayBatch` merges every proven family into
  one globally token-ordered plan and reports the rest in `batch.families`
  with a reject mask.

An unproven family is excluded and reported rather than failing the query.
`batch.valid` means the plan is internally consistent and replay-safe, never
that the MAIN pass is completely covered. Coverage remains a separate serving
gate.

### Measured

Live gameplay with generic capture enabled (`/tmp/tt_generic_main4.log`,
`/tmp/tt_generic_main5.log`) records one private transaction of up to
**309 of 314** observed logical draws in exact global token order, from ~18-23
proven families across ~40-50 discovered ones, with a single render scope, a
single clear, 1280x720 scissor normalization, and guest rendering untouched.
The previous hand-built path reached 202 draws across four families.

Remaining uncovered families are now a short, named list with reject masks
rather than an open-ended reverse-engineering queue. That list - not the next
material - is what deserves bespoke attention.

## MAIN frame construction

After both tracer bullets pass:

1. retain one replay token for every event in the selected first tile block;
2. map title-joined events to the ledger's original ordinals;
3. retain backend-only prefix/interleaved/suffix events in their observed
   relative positions rather than inventing title ordinals;
4. merge custom-family draw records and translated tokens into one ordered
   private MAIN plan;
5. normalize only tile fields proven mechanical;
6. record every logical item once into the exact-MAIN target; and
7. resolve to a sampleable one-sample 1280x720 MAIN image.

The merge must reject duplicate claims, missing backend-only work, expired
tokens, state incompatibility, or a family that is neither translated-replay
ready nor custom-renderer ready.

## Late compositor status

MAIN replay alone cannot present a correct frame:

- transition: exact state exists for the known FA14/5E11 pair, but no private
  translated replay token exists yet;
- 2AC: immutable geometry and constants plus a native pixel port exist, but
  translated replay still needs the exact slot-0 resolved-MAIN binding and all
  four vertex variants;
- post: the late ledger owns identity/order only, not full state or resources;
- HUD: title geometry/texture and a partial indexed backend contract exist,
  while constants `c12..c15`, pixel `c110`, sampler, viewport, and scissor are
  still missing for all 55 draws; and
- final compositor: identity/conditional ordering exists, but full replay
  state and resources do not.

Once MAIN works, broaden the same filtered post-pipeline packet capture from
the transition through the final compositor. Auto-indexed draws are mandatory;
the selective indexed replacement callback cannot provide complete late
coverage.

## Promotion gates

Translated replay may resolve only to a private diagnostic image until:

1. rectangle pipeline/readback parity passes;
2. one PS328 logical draw matches guest and custom references;
3. tile-state comparison proves one-copy correctness;
4. a complete MAIN plan contains every joined and backend-only event;
5. the resolved MAIN image passes image comparison;
6. transition, 2AC, post, HUD, and final composition are complete in order;
7. a whole private frame is stable under gameplay/camera changes; and
8. measured private-frame cost supports the 30 FPS budget.

Only after those gates does suppression become a separate all-or-nothing
serving decision.
