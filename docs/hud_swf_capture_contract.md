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
