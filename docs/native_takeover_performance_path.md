# Native takeover performance path

This note records the measured performance boundary and the shortest
correctness-preserving route from the current observer renderer to a stable
30 FPS native output. It does not authorize guest suppression.

## What the measurements prove

The clean gameplay run in `/tmp/tt_clean_fps.log` reaches the match and then
settles at roughly:

- 9-11 FPS;
- 87-108 ms host frame time;
- 79-114 ms reported GPU time;
- 65-99 ms of host wait time.

A fresh muted baseline in `/tmp/tt_muted_baseline.log`, with every native
observer disabled and presenter telemetry sampled every five guest swaps,
settles at 8.6-10.4 FPS / 96-117 ms after gameplay begins. Reported GPU time
is approximately 98-124 ms. Muting output therefore removes sound without
materially changing the renderer bottleneck.

The frontend is approximately 60 FPS / 17 ms in the same run. Gameplay is
therefore not limited by the title's general PPC execution or by frame pacing.
The translated gameplay renderer is the dominant cost.

The flat native-output diagnostic in
`/tmp/tt_native_suppression_ceiling.log` holds approximately 60 FPS / 16.7 ms.
That run is not content-correct, but it proves that the recompiled game,
presentation path, and host can exceed the 30 FPS target once the translated
gameplay draw stream is suppressed.

An early PS328 diagnostic suppression run appeared to improve gameplay to
roughly 14-15 FPS / 66-69 ms. The later exact selective benchmark in
`/tmp/tt_ps328_suppress_bench.log` suppresses roughly 105 PS328 guest
submissions per frame and still measures about 10.25 FPS. PS328 alone is
therefore not a demonstrated performance win; broad pass retirement is
required.

The C6 in-order replacement runs remain roughly 8-9 FPS while capture and the
rest of the guest frame are active. They are useful correctness experiments,
not a viable final performance architecture.

The timestamped no-audio gameplay run in `/tmp/tt_gpu_profile.log` identifies
where that translated cost is spent. Settled 120-frame samples report
101.7-116.7 ms of GPU span. Only 7.1-11.0 ms is attributed to MAIN draw
shading. The dominant buckets are:

- texture upload: 15.4-15.6 ms;
- shared-memory upload: 20.1-21.5 ms;
- texture upload copy: 30.5-38.3 ms;
- render-pass entry/setup: 10.7-15.4 ms.

Those four buckets total 78.4-89.1 ms. Optimizing the individual translated
material shaders cannot plausibly reach 30 FPS while this upload/setup path
still runs. A complete native transaction that reuses promoted resources and
retires the tiled guest MAIN pass is the measured performance path.

The guarded private replay in `/tmp/tt_guarded_crowd_live.log` now proves a
202-draw transaction in exact global token order:

- 37 PS328 venue draws;
- 9 Venue9E draws;
- all 156 CrowdC6 draws.

It records one private render scope and one clear, produces a nonempty
1280x720 readback across the full output, survives the match, and leaves guest
rendering untouched. Its 10-12 FPS heartbeat is expected because this
comparison phase duplicates the guest work. It is correctness progress, not
an optimization benchmark.

The private native transaction has already recorded complete four-family
plans of 310, 383-385, and 515 draws in different live frames. This variation
is real camera/content variation. A fixed `441` reference census is useful as
an old trace comparison, but it cannot be a serving predicate. The live
ordered catalog and backend join must define each frame's denominator.

The combined D47 + A406 observer run in `/tmp/tt_main_a406_d47.log` proves
same-frame assignments for up to 453 of 508 current-frame logical MAIN draws,
leaving 53-55 unassigned in its settled snapshots. This is capture coverage,
not renderer readiness: A406 has no native material renderer yet, and the
6AE/D47 paths are not both serving-faithful.

The A406 family has since published exact immutable same-frame snapshots from
both the existing replacement callback and the post-pipeline state tap. The
BBB5 family exposed a different live state contract
(`00700732 / 0000000F / 8700000C / 00010706 / 00018000`) and correctly
rejected the original guessed values.

Full post-pipeline state capture must not run indiscriminately. An unfiltered
diagnostic copied render-scope, viewport, scissor, and texture state for every
translated draw and reduced gameplay to roughly 2.5 FPS. The SDK tap now has a
cheap shader-identity prefilter, so expensive value capture is restricted to
the explicitly observed families. Runs with these diagnostics enabled are
correctness evidence, not acceptance benchmarks.

## The MAIN-pass ceiling, measured directly

`native_render_main_pass_suppression_benchmark` drops framebuffer-sized
(pitch >= 1280) emulated passes and their resolves *without* requiring a
serving native renderer, which is what made this measurable before the
takeover work exists. Output is deliberately wrong; it is a timing probe only,
default off, and `..._delay_seconds` holds it back until automation has
navigated to gameplay.

Settled gameplay, same scene, audio muted:

| | baseline | MAIN suppressed |
|---|---|---|
| FPS | 9.0-9.4 | **53.4-58.4** |
| host frame | 107-111 ms | 17.1-18.7 ms |
| reported GPU | 118-122 ms | 17.2-19.1 ms |

The MAIN pass is therefore roughly **100 ms of a 110 ms frame**. Removing it
leaves about 18 ms, against a 33.3 ms budget for 30 FPS: roughly 15 ms of
headroom for a native MAIN replacement.

The GPU bucket profile (`vulkan_gpu_timestamp_buckets`) shows the upload and
setup cost is largely MAIN's own, not a fixed tax from other passes:

| bucket | baseline | suppressed |
|---|---|---|
| Main | 36.3 ms | 1.0 ms |
| TexUpCopy | 18.1 ms | 2.5 ms |
| PassEntry | 20.4 ms | 5.5 ms |
| SMUpload | 17.9 ms | 5.7 ms |
| TexUpload | 15.7 ms | 2.4 ms |

### This corrects the PS328 reading

The earlier conclusion that "PS328 alone is not a demonstrated performance win,
broad pass retirement is required" was right about the remedy but was
mis-generalized into a claim that MAIN takeover could not reach 30 FPS because
shading is only 7-11 ms of the frame. It can. Suppressing 105 of ~440 draws
left the pass, its render-pass entries, its residency and its texture uploads
all still running, so it removed almost nothing. Retiring the whole pass
removes all of it. Draw shading was never the cost; the per-frame preparation
around those draws was, and that preparation dies with the pass.

## Cross-frame resource stability, measured

`UpdateGuardedReplayFamilyPromotions` retains each proven family's pipeline,
draw count and per-draw resource fingerprints, and compares the next frame
against them. Geometry, texture identity, texture content generation and
constants are fingerprinted separately so "stable but re-uploaded" is
distinguishable from "genuinely changed". Measured over 60-240 gameplay frames
per family (`/tmp/tt_promotion3.log`):

- **`pipeline_changed=0` for every family.** Pipeline and pipeline-layout
  identity are stable across frames and can be promoted.
- **`count_changed` 0-2.** Per-family draw counts are stable.
- **`texture_identity_changed` 0-2, while `textures_changed` is ~100%.** The
  same textures are bound every frame and re-uploaded every frame anyway.
- geometry splits cleanly: static venue families report `geometry_changed=0`,
  animated families (players, ball, crowd) report ~100%.
- constants change on only a fraction of frames, and are cheap regardless.

### The texture upload is redundant, not required

This is the important one. Texture identity is stable on ~99% of frames while
the backend's content generation advances on nearly all of them, so
`TexUpload` + `TexUpCopy` - about **34 ms of the ~110 ms frame** - is spent
re-uploading texels that did not change.

That cost is addressable by a persistent, identity-keyed texture cache, and
unlike everything else in this document it does **not** depend on native
takeover, suppression, or the late-phase work. It is a guest-path optimization
that the native path later inherits.

## Why whole-frame takeover is the shortest path

RexGlue already implements the Skate 3-shaped performance mechanism.
After a registered native output renderer successfully returns `true`,
`g_native_output_active` becomes true. On following frames,
`ShouldSuppressEmulatedDraws()` skips eligible emulated draw and resolve work
while PM4 parsing, fences, queries, memory exports, and title execution keep
running.

Selective draw replacement is still valuable for proving one family in the
real borrowed guest render scope. It cannot remove the whole translated
pipeline cost until every expensive family has an exact in-order replacement.
It also repeatedly crosses the borrowed-scope boundary. The final Table
Tennis renderer should instead follow Skate 3:

1. Observe title-owned draw submissions and copy or cache immutable payloads.
2. Verify each family against translated-backend identity and state.
3. Build one ordered native frame transaction in a private target.
4. Keep the guest frame authoritative until the complete transaction and
   late presentation sequence are proven over consecutive frames.
5. Atomically render the complete transaction to the presenter and return
   `true`.
6. Yield (`false`) immediately for frontend, loading, invalid, stale, or
   incomplete frames.

The current `tabletennis_native_render` flat clear proves only step 5's
performance ceiling. It must not become the content renderer.

### Avoid a backend-proof deadlock

Same-frame translated-backend proof is a **promotion** requirement, not a
permanent post-takeover input. Once native output succeeds, RexGlue suppresses
the translated draws that generated those backend callbacks. Requiring a new
backend callback every served frame would therefore make takeover alternate
between native and fallback or fail immediately.

Before promotion, observe the complete contract for consecutive guest-rendered
frames. Promotion freezes the verified shader/state schema. While native
output is active, admit each new title-owned payload only if its value identity
still matches that promoted schema. A new shader, layout, pass, render-target
contract, or unsupported payload invalidates promotion and yields back to the
guest path for re-observation. Do not treat the expected absence of suppressed
backend callbacks as stale proof.

## Serving gate

Whole-frame serving must remain fail-closed. A frame may be served only when
all of these are true:

- gameplay is active from title-owned player submissions;
- the ordered title catalog is current and has no dropped occurrences;
- the MAIN coverage ledger has an exact three-tile join;
- every logical MAIN ordinal is assigned to a renderer-ready family;
- every required backend-only MAIN event has an explicit native equivalent or
  a proven no-output classification;
- every served family owns an immutable same-frame title payload and matches
  an independently verified promoted backend contract;
- the native composition plan preserves the original title ordinal exactly;
- every family prepares all resources before the render pass opens;
- the complete private transaction records successfully;
- the MAIN-to-COMP transition, 2AC composite work, post chain, gameplay HUD,
  and final compositor have exact replay implementations;
- the same complete contract remains stable for multiple consecutive frames.

`MainCoverageFrameSnapshot::all_ordinals_assigned()` is not sufficient by
itself. Assignment labels may still be title-only or sequence-untied.
`all_draws_same_frame_proven()` and `all_backend_events_covered()` are the
minimum MAIN evidence, and neither proves that a renderer exists.

Do not compute `main_complete` from fixed per-family draw counts or the old
441-draw census. Those counts vary in live gameplay. Completion is equality
between the exact current frame's ordered/backend-joined draw set and the
exact current frame's renderer-ready draw set.

Likewise, `LatePhaseFrameSnapshot::observer_complete()` proves sequence shape
only. `replay_ready()` must remain false until those draws have exact native
replay.

## Promotion order

The most direct implementation order is:

1. Keep PS328, 14D, C6, and CA9 in the existing private transaction.
2. Add E33 and 6AE only after their observer overlays match the guest output.
3. Turn the MAIN coverage ledger's remaining unassigned shader groups into
   bounded family observers, largest draw-count group first.
4. Account for backend-only MAIN work explicitly.
5. Replay the late sequence in its observed order: transition, 2AC, post,
   HUD, final compositor.
6. Add a default-off presenter comparison mode that renders the complete
   transaction but still returns `false`.
7. Promote one all-or-nothing serving cvar only after the comparison mode
   stays complete and stable.

Do not promote PS328 or C6 suppression merely because they improve a benchmark.
Partial suppression changes depth, blending, and later composite inputs.

## 30 FPS acceptance

The target is achieved only when a content-correct gameplay run:

- remains at or above 30 FPS for at least 300 consecutive presented gameplay
  frames;
- has no native fallback or incomplete-transaction frame during that window;
- has no stale guest pointer or payload read;
- preserves normal title clock speed and input;
- yields correctly back to emulated frontend/loading output;
- runs with audio muted through `audio_mute`, without removing audio timing
  services.
