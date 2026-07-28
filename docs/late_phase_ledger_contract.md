# Late presentation-phase observer ledger

`tabletennis_native_late_phase_ledger` is an observer-only proof of the real
translated draw order after MAIN. It records no GPU commands, returns no draw
replacement route, never writes the presenter, and never suppresses guest
work.

## Reference anchors, dynamic payload

The reference trace establishes this ordering:

1. the `FA14ACFD / 5E11FC7A` three-vertex MAIN-to-COMP transition;
2. the current frame's complete `2AC059EB` player composite block;
3. an `F0B85512 / 39184743` 88-vertex quad-list prefix and the intervening
   post-processing chain;
4. the current frame's uniquely joined gameplay HUD block; and
5. when present, the `648EED93 / 300BD180` three-vertex final compositor.

Only the anchors are reference identities. The ledger does not assume a fixed
backend draw ordinal, a fixed number of 2AC draws, or a fixed post-chain
length. It consumes the independently published current-frame 2AC and HUD
proof vectors, finds both their earliest and latest ordered mappings in the
complete all-draw pre-gate stream, and accepts only identical mappings.

The admitted 2AC vector is joined independently and must be contiguous after
exactly one MAIN-to-COMP marker. Up to 16 intervening backend draws are
retained and labeled as transition setup rather than silently skipped; a
larger gap rejects the frame. The gameplay HUD vector must be contiguous, the
post range must begin with exactly one 88-vertex HUD-family prefix, and exactly
one reference compositor, when present, must immediately follow the gameplay
HUD vector. Missing compositor identity is also admitted: live frames prove
that this draw is conditional because the frame loses exactly one draw while
the joined HUD endpoint and the following 24-draw tail remain unchanged. The
ledger never promotes the next tail draw by ordinal. Duplicate, displaced,
reordered, dropped, or ambiguous evidence rejects the frame.

For rejected frames, telemetry reports the exact transition-marker candidate
count, the first and last independently joined 2AC backend indices, and at
most four draw identities immediately preceding 2AC. It also retains and logs
at most four identities immediately following the joined HUD plus the exact
reference-compositor candidate count. This bounded evidence distinguishes a
changed marker hash from a valid marker followed by setup draws, and a
conditional compositor omission from an alternate identity, without falling
back to ordinal guesses.

## Publication

The immutable `LatePhaseFrameSnapshot` owns value-only copies of every backend
draw from the transition through the final compositor. It labels the
transition, transition setup, 2AC, post, gameplay HUD, and final-compositor
ranges. When the conditional compositor is absent, the immutable window ends
at the final HUD draw and records the untouched backend-tail count. No backend
object or guest pointer survives publication.

The observer is bounded to 4,096 draw identities per backend frame and eight
retained frames. Overflow is explicit and makes the frame incomplete.

The switch is:

```text
--tabletennis_native_late_phase_ledger=true
```

It automatically arms the underlying 2AC and gameplay-HUD captures. The
backend eligibility callback still must be installed at application startup,
so command-line or configuration activation is the reliable validation path.

`observer_complete()` is sequence evidence only. `replay_ready()` is
hard-wired false because render-target state, the post programs and resources,
and the HUD constants/sampler/viewport/scissor contract are not captured by
the pre-gate API yet.
