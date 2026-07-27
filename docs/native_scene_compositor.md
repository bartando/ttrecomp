# Native scene compositor foundation

This is an observer-only bridge from the current per-family renderers to a
Skate 3-shaped native scene pass. It never enables output takeover, guest-draw
suppression, or authored replacement content.

## Implemented now

`BuildNativeSceneCompositionPlan` joins only the four families that already
have real immutable payload renderers:

- PS328 static venue
- 14D five-texture static venue
- C6 crowd
- CA9 skinned players

The planner refuses a frame unless:

1. gameplay is active;
2. the generic ordered catalog has no dropped occurrences and belongs to the
   exact title frame; aggregate read failures from unrelated, unselected draws
   remain telemetry;
3. all four family snapshots are valid and belong to that same frame;
4. every family draw still matches the catalog record at its captured title
   ordinal; and
5. no two families claim the same ordinal.

The result owns the immutable `TableTennisFrameScene` and contains one
strictly increasing list of `{family, ordinal, family_draw_index}` records.
Dynamic family counts are allowed. The trace census counts remain useful
coverage evidence, but are not a render-order invariant.

This plan is deliberately only "observer composition ready." It is not whole
scene readiness and cannot make `takeover_ready` true.

## Shared-pass renderer contract

Phase one implements the caller-owned target contract and PS328 adapter:

- family recorders consume a caller-owned color/depth attachment pair and
  never assume ownership of its state;
- `PrepareVenueFullFamilyNativeScene` creates the dedicated depth-enabled
  PS328 pipeline and prepares immutable geometry/textures before the pass.
- `RecordPreparedVenueNativeSceneDraw` consumes exactly one
  `NativeSceneDrawRef`, requires the exact prepared frame, and records the
  real `ps_replace` material path.
- the record function never binds, clears, or transitions its attachments.

The default-off private transaction observer now calls this API. It discards
the completed offscreen pass, so it cannot display a partial scene or trigger
suppression.

Phase two adds the equivalent C6 crowd adapter:

- `PrepareCrowdNativeScene` prepares every exact immutable C6 draw and its
  current-frame palette/constants against the same caller-owned target
  contract.
- `RecordPreparedCrowdNativeSceneDraw` requires the exact prepared frame and
  records one original-ordinal triangle-strip draw without touching target
  state.
- the overlay and borrowed 4x replacement-prewarm paths keep their existing
  private-depth and exact borrowed-pipeline behavior.

One C6 raster difference remains an explicit observer blocker:

- captured PA_SU state requests counter-clockwise-front back-face culling.
  NRHI represents front-face winding explicitly on Vulkan and D3D12. Live C6
  backend-block proof confirmed uniform `0x00018002`, so the shared native
  scene pipeline now uses back-face culling with counter-clockwise fronts;

That difference is not allowed to unlock serving.

Phase three adds the CA9 skinned-player adapter:

- `PreparePlayerNativeScene` accepts only a complete exact frame in which
  every immutable mesh has one traced prepass followed by its paired color
  descriptor. Palette, texture payloads, phase identity, and original draw
  ordinals must agree before resources are published.
- `RecordPreparedPlayerNativeSceneDraw` records exactly one planned ordinal.
  It chooses the depth-writing alpha-only prepass or the alpha-blended
  depth-read color pipeline from that draw's captured descriptor.
- both shared pipelines use the traced less-equal depth function and
  cull-none raster state. The prepass writes only alpha and the color pass
  writes RGBA with source-alpha/inverse-source-alpha blending.
- the record function never binds, clears, or transitions attachments. The
  existing overlay, private D32, and borrowed replacement-prewarm behavior is
  unchanged.

The CA9 adapter is still observer-only. Its remaining ordering blocker is:

- phase ordering is exact only when the caller records the already validated
  composition plan in ascending ordinal order. A family recorder cannot
  transactionally enforce the order of calls made by a future compositor.

This is not allowed to unlock serving.

Phase four adds the independently backend-verified 14D venue family:

- the composition planner requires the same immutable 14D frame as the
  catalog and validates each dynamic live draw count by original ordinal,
  title pass/program/shaders, guest shader fingerprints, owner, vertex/index
  binding, declaration, and the catalog's captured texture fetches;
- `PrepareVenue14DNativeScene` rejects any frame outside the proven opaque
  RGB-write, less-equal depth-test/write backend state, then uploads all mesh
  buffers and all five complete descriptor-selected mip chains before
  recording is possible;
- preparation uses the exact proven 2x-anisotropic repeat/clamp samplers and
  creates separate identity opacity/gamma constants for native-scene
  rendering, so the comparison overlay retains its own controls;
- `RecordPreparedVenue14DNativeSceneDraw` accepts only the exact prepared
  frame and one matching original ordinal. It binds the real five-texture
  shader resources and records one triangle strip without binding, clearing,
  transitioning, allocating, or uploading anything.

The remaining 14D shared-pass differences are explicit blockers:

- the backend proof retains the title's face-culling register and checks it
  across all three tile replays. Live 14D frames proved one uniform
  `0x00018002` value, and the shared pipeline selects back-face culling with
  counter-clockwise fronts only for that exact contract.

Stencil is disabled by the verified 14D depth-control state, so the absence of
stencil writes in shared D32 does not itself change this family's behavior.
The D32 representation and unproven culling still prevent serving.

Phase five adds reusable shared attachment and resolve ownership:

- `PrepareNativeSceneRenderTargets` creates one output-sized 4x RGBA8 color
  target, one matching 4x D32 depth target, a `Texture2DMS` resolve view, and
  presenter-format resolve pipeline. It rejects devices that cannot support
  four samples for both formats.
- `NativeScenePassTargets` now validates the actual attachment formats,
  extents, and sample counts through the RHI. It rejects the presenter itself
  as a family render target.
- `BeginNativeSceneRenderPass` binds and clears only the offscreen pair using
  caller-provided clear values.
- `AbortNativeSceneRenderPass` closes and discards an open offscreen pass
  after any family-record failure. It returns both owned attachments to their
  steady states without transitioning or writing the presenter.
- `ResolveNativeSceneRenderPass` averages all four RGBA8 samples in a
  fullscreen shader, writes the result through the presenter's actual
  one-sample format, then restores the offscreen target to render-target state
  and the presenter to guest-output state.
- the D3D12 and Vulkan texture interfaces expose their real sample count, so
  callers cannot claim a 4x contract for a 1x allocation.
- every shared-pass family draw now uses `DrawIndexedChecked` and returns an
  explicit `rhi_draw_state_rejected` result if backend state preparation or
  descriptor binding fails. D3D12 latches texture-table allocation failures;
  Vulkan reports its existing lazy pipeline/render-pass preparation result.

The target owner is now called only by the private transaction observer. That
path always aborts/discards the target, including after a completely successful
record, so guest rendering remains authoritative, serving remains disabled,
and no family draw is suppressed. The shader resolve follows the Skate 3
native renderer because NRHI has no native resolve command, but is not invoked
by the observer transaction.

## Required compositor integration before drawing

Each family now exposes the preparation/recording split needed by the shared
pass. Every record function assumes its caller already bound the render
targets and viewport. It does not transition the color target, bind a new
depth target, clear depth, allocate, upload, or silently select another
frame.

The private observer transaction records one pass:

```text
prepare every family against the exact immutable frame
prepare/reuse shared RGBA8+D32 4x targets
begin shared pass and clear once
for draw in composition_plan (ascending original ordinal):
    if RecordPreparedFamilyDraw(...) fails:
        abort shared pass without touching guest_output
        keep guest frame authoritative
discard the complete private target without touching guest_output
```

PS328 uses its real `ps_replace` material path, CA9 retains the captured
prepass/color phase per individual draw, and C6 retains its captured sampler
and material constants. Existing overlay entry points remain comparison
tools, while these explicit scene adapters share their prepared resource
caches.

Runtime serving is still blocked on complete family coverage, exact
per-family raster/material contracts, and visual/telemetry verification of
the merged ordinal stream. Checked draw failure and pass abort/recovery are
now exercised without resolving: serving stays false until every title family
is covered and the complete transaction is independently proven safe to
resolve. Suppression remains a separate, later serving decision. See
`native_scene_transaction_contract.md`.
