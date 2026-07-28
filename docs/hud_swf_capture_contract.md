# Gameplay HUD/SWF capture contract

This module observes the title's real gameplay HUD output. It does not author
HUD content, render a replacement, or suppress any guest draw.

## Proven ownership boundary

`sub_823F9238` is the SWF draw boundary. The hook accepts a draw only when:

1. `hud = BE32(0x82606610)` is non-zero.
2. `live_context = BE32(hud + 0x5C)` can be read safely.
3. Entry `r3` equals `live_context`.

This deliberately excludes the shell/frontend SWF paths. `hud + 0x58` is not
accepted because it has not been proven to be the gameplay draw context.

## Ordered state and payload capture

- `sub_822EC298` entry records the ordered SWF texture handle. A zero `r3`
  resolves to `BE32(0x825EBA20)`, matching the title's own fallback.
- `sub_822EC298` passes the resolved handle to `sub_820F76F8`, which stores it
  as the slot-0 **texture resource object**. It is not itself a GPU binding.
- The title later invokes either
  `grcTextureReference::GetGpuBinding (sub_8215D510)` or
  `grcTextureXenon::GetGpuBinding (sub_8215FC98)`. Their observed return value
  is joined back to the exact resource pointer and vtable. Only then does the
  observer read the binding twice, locate its embedded six-dword type-2 Xenos
  texture fetch, and use the shared immutable mip-0 snapshot/untile path.
- A cached default `grcTextureXenon` resource can be rebound without another
  virtual call. Ghidra proves `sub_8215FC98` returns the big-endian pointer at
  `this + 0x10`. For the exact `0x820353CC` vtable only, the observer may
  double-read that field as a fallback, require a stable nonzero value, and
  then run the same binding/fetch/payload validation as the hook-fed path.
- `sub_82152A78` entry receives `{batch_kind = r3,
  requested_vertex_count = r4}`. Before the original overwrites the immediate
  globals, the preceding completed batch is copied.
- After `sub_82152A78`, `BE32(0x825EBA74)` is the initial dynamic vertex
  write cursor. Each emitted 36-byte vertex advances this global.
- The emitted count is read from `BE32(0x825EBA88)`.
- The title's inline submission path clears the cursor at `0x825EBA74` to zero
  but leaves the emitted count at `0x825EBA88` intact. A completed batch
  therefore accepts either the live
  `cursor == initial_base + count * 36` state or this exact post-submit
  `cursor == 0` state. Both paths still validate the retained count, original
  base, full byte range, and bounds before copying.
- Every vertex is retained byte-for-byte at the title's `0x24` stride.
- The last batch is copied at `sub_823F9238` exit because there is no following
  batch start to close it.

Guest reads use `GuestTryCopy`. A revoked streaming range rejects that batch
and marks the immutable frame incomplete; it never crashes the title.
Texture telemetry distinguishes binding read faults, a binding that changed
during its two copies, handles without a type-2 texture fetch, and payload
snapshot failures. This keeps non-texture SWF state from being silently
misclassified as a valid texture.

Every completed batch retains the exact order of the texture bind active when
the title emitted it. Publication classifies binds as required or unreferenced
from that ordering. An unreferenced bind may fail capture without invalidating
the replay payload because a later bind replaced it before any batch used it.
Every bind referenced by a batch must still have a complete immutable texture
payload, and every batch must resolve to an observed bind.

## Bounds

Each frame is limited to:

- 8 nested SWF scopes;
- 2,048 completed batches;
- 2,048 texture binds;
- 65,536 vertices per batch;
- 32 MiB of copied vertex payload.

Overflow, invalid counts/base changes, and guest faults are reported separately
in the frame snapshot and telemetry.

## Publication and readiness

`HudSwfCaptureFrameEnd()` publishes
`std::shared_ptr<const HudSwfFrameSnapshot>` before
`NativeFrameSceneFrameEnd()` at the title's Swap boundary. The master
`tabletennis_native_frame_scene_capture` observer enables this module
automatically.

The standalone switch is:

```text
--tabletennis_native_hud_swf_capture=true
```

The first non-empty frame and then every configured interval report:

```text
Table Tennis HUD/SWF observer: ... scopes=... batches=... vertices=...
texture_binds=... complete=... observer_only=true guest_suppressed=false
```

`TableTennisFrameScene::hud_swf` retains the immutable batch and texture
payload capture, and readiness reports `hud_capture_valid`. `hud_complete`
intentionally remains false until exact blend/depth/raster/scissor state and
native replay are independently verified.

## Translated-backend observer and ordered join

`tabletennis_hud_swf_backend_observer` consumes both the pre-gate draw census
and the normal draw-replacement matcher callback stream without returning a
route. It retains only events with the exact gameplay HUD shader pair:

```text
VS F0B85512865B6E5E / PS 391847433E1601A9
```

Events are bucketed by the authoritative backend frame sequence. The pre-gate
`NativeGuestDrawEligibilityContext` observes every translated draw and retains
its submitted/host counts, primitive topology, processed-index shape, and
eligibility result. Ordinary auto-indexed triangle lists and strips do not
need a host index buffer, so they correctly do not reach the selective
replacement matcher.

Authoritative late matcher callbacks are retained separately. They copy
blend/depth/color/raster/restart state, render-pass key, raw and decoded EDRAM
target identity, and the host attachment/sample contract, but cover only
converted primitive draws. Their partial coverage is never presented as a
full replay-state proof.

The reference trace contains one separate 88-vertex quad-list draw from this
shader family followed later by the gameplay SWF block. The gameplay block is
exactly 55 draws and 266 submitted vertices:

- 7 six-vertex triangle lists;
- 16 six-vertex triangle fans;
- 32 four-vertex triangle strips.

The `batch_kind` passed to `sub_82152A78` indexes the verified big-endian table
at `0x825D4C64`. Kinds `0..6` decode to Xenos primitives
`{1, 2, 3, 4, 6, 5, 13}`. At backend-frame close the observer finds both the
earliest and latest ordered mappings of the immutable title
`{decoded primitive, vertex count}` vector into the complete pre-gate family
vector. A frame is accepted only when those mappings are identical. This
proves a unique 55-draw join and excludes the 88-vertex prefix.

On the current Vulkan backend, the triangle fans are converted to a host
triangle-list index buffer (`6` guest vertices become `12` host indices), and
the 88-vertex quad list is converted too. Those 17 draws reach the matcher.
The remaining 39 auto-indexed list/strip draws do not. One backend draw never
consumes multiple SWF batches.

The published `HudSwfBackendFrameSnapshot` owns the title snapshot, one
pre-gate identity per joined batch, and the 17 available late contracts.
`observer_complete()` proves the exact 55/266 identity and topology join plus
the expected partial late-state coverage. It does not claim complete live
state and is not serving approval:
`replay_ready()` is hard-wired false, no draw is suppressed, and no GPU command
is recorded.

The remaining replay inputs are explicit:

- vertex constants `c12..c15`;
- pixel constant `c110`;
- effective slot-0 sampler state;
- effective viewport and scissor;
- the native MAIN resolve / COMP / HUD target handoff.

No value for those inputs is inferred from the old trace.
