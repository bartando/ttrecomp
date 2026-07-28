# MAIN-to-COMP transition state observer

This observer captures the translated backend state of the
`FA14ACFDF2DE3ED0 / 5E11FC7AE2F1C2BF` three-vertex fullscreen transition.
It is evidence for a future private COMP replay, not a renderer or serving
route.

## Generic SDK tap

`NativeGuestDrawStateObserver` runs after Vulkan has prepared the guest
pipeline, render targets, textures, viewport, and scissor, immediately before
draw submission. Unlike the earlier eligibility callback, it observes
auto-indexed and fullscreen draws too.

The context owns value copies of:

- the shader, topology, count, index, render-pass and vertex-fetch identity;
- normalized depth/color state plus raw blend and raster state;
- raw and independently decoded guest render-target registers;
- the exact host attachment formats, sample count and sample mask;
- the effective host viewport, NDC adjustment and scissor;
- the active texture-fetch mask and six raw fetch words for every active
  slot.

It exposes no command list, backend object, or guest pointer. Registration is
atomic and optional. When no observer is registered, the draw path performs
only the existing atomic presence check and constructs no context.

## Table Tennis admission

The title observer is enabled by either:

```text
--tabletennis_native_transition_state_observer=true
--tabletennis_native_late_phase_ledger=true
```

It retains at most eight candidate transitions per backend frame and eight
frames. A newer backend sequence closes the older frame. Publication requires
exactly one candidate, a valid post-pipeline context, raw/decoded target
agreement, complete host attachment state, and valid viewport/scissor and
texture-fetch copies.

The published snapshot sanitizes the embedded draw context so both `device`
and `cmd` are null. It cannot record GPU commands, select a replacement,
suppress the guest transition, or serve output. The first accepted frame logs
the complete contract, including slot-zero raw fetch words, for promotion only
after live evidence is reviewed.
