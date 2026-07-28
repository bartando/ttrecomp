# Late COMP first-slice contract

`tabletennis_native_late_comp_first_slice_observer` is the first bounded
preparation unit for a real late native replay. It does not author geometry,
record GPU commands, replace a draw, or suppress guest work.

The plan joins three independently published same-frame proofs:

1. the exact post-pipeline state of the
   `FA14ACFDF2DE3ED0 / 5E11FC7AE2F1C2BF` MAIN-to-COMP transition;
2. the complete late-phase order proof, which places the current frame's 2AC
   block after that transition; and
3. the immutable 2AC payload/constant/geometry preparation plan.

It then selects the first real single-stream-32 draw by title kind, not by a
fixed ordinal. Promotion requires its late identity to be
`4761A30F65AA309C / 2AC059EB5C7A942F` and to agree with the independently
joined title proof on primitive, submitted index count, and physical index
base. The retained geometry fingerprint, submitted index count, and referenced
vertex count come only from decoded guest bytes.

## Explicit blockers

A complete observer plan is still not GPU-recording-ready. Promotion remains
blocked, in order, by:

1. an exact native port of vertex program `4761A30F65AA309C`;
2. capture and ownership of the first draw's sampleable resolved-MAIN scene
   fetch and sampler state; and
3. a private ordered one-sample COMP transaction.

The existing 2AC motion-composite pixel port is not enough by itself. A generic
player transform, a shared 4x MAIN attachment, or synthetic geometry would be
visually wrong.

## Validation switches

All prerequisite observers must be armed at startup:

```text
--tabletennis_native_transition_state_observer=true
--tabletennis_native_late_phase_ledger=true
--tabletennis_native_player_2ac_renderer_observer=true
--tabletennis_native_late_comp_first_slice_observer=true
```

The first-slice observer is intentionally not a convenience switch that
silently widens backend capture. Its log reports exact transition/order/draw
proof flags and the first remaining blocker.

## Live validation

`/tmp/tt_late_6ae_live.log` published a complete observer plan for frame 2678:

```text
transition backend index = 1585
first 2AC backend index  = 1586
indices / referenced     = 37 / 33
geometry fingerprint     = 5AB84A6644E89897
transition/order/identity/geometry = exact
```

The plan correctly remains blocked on the missing `4761` vertex-program port.
