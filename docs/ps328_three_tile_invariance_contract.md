# PS328 three-tile invariance observer

This observer is the fail-closed gate between the MAIN coverage ledger and
collapsing three tiled guest submissions into one logical translated replay.
It never renders, replaces, suppresses, or normalizes a draw.

## Dynamic selection

For each finalized, uniquely joined MAIN coverage frame, the observer collects
every logical draw with the exact shader pair:

```text
VS 0E9982BE6B1E99A1
PS 328FA02B07C392DC
```

It does not hardcode an ordinal for the venue payload. The ledger supplies the
exact number and order of this shader pair in one logical tile. Every venue
member must have the authoritative `kVenuePs328` title assignment. Live
evidence also proves exactly one non-venue draw sharing this translated shader
pair: it is the trailing four-index MAIN-ledger title/backend join after the
non-empty assigned venue prefix. That member remains deliberately unassigned
by the venue selector and is recorded with separate
`kMainCoverageTrailingShaderPair` authority. It is accepted only in that final
position and shape; an unassigned member anywhere else, a second tail, or any
partially assigned draw rejects the entire family.

The observer requires exactly three equal-sized token blocks, selects the same
family-local offset from each, then verifies backend frame, shader pair, guest
primitive, submitted index count, and physical guest index base. Missing,
extra, stale, reordered, or ambiguous matches reject the entire family.

The published family snapshot preserves each member's family offset, title
ordinal, catalog index, complete draw identity, normalized state, and tile-1
token reference, plus whether its title authority came from the venue payload
or the narrowly classified trailing MAIN coverage join. Its authorization
vector is prior-frame observer evidence only. Partial results are diagnostic
and cannot authorize serving:
`valid()` requires every logical member to be safe.

`valid()` rechecks the producer's counters against the vector rather than
trusting them. It requires exactly `3 * logical_draw_count` observed tokens,
contiguous family offsets, strictly increasing title ordinals and catalog
indices, exact PS328 shaders, same-frame tile-1 tokens, and bit-exact agreement
between every copied identity/normalization and its token.

The private-batch boundary is stricter. `private_batch_ready()` additionally
requires every tile-1 token to own guarded resources stable for deferred
replay. `private_batch_draw_count()` returns zero and
`private_batch_draw(offset)` exposes no member unless the whole ordered family
passes that gate. A coordinator must first match that count to the immutable
title frame, then use `matches_identity()` to join each member by ordinal,
primitive, submitted index count, and physical index base without weakening
the contract to an ordinal-only lookup.

The latest diagnostic snapshot and latest valid authorization are published
separately. Every finalized frame, including a rejection, replaces
`LatestPs328FamilyAuthorizationSnapshot()` so telemetry never hides current
failure. A rejection does not erase
`LatestValidPs328FamilyAuthorizationSnapshot()`: the private coordinator may
use that older complete proof to authorize a later frame.

Cross-frame retention is safe only because the coordinator never submits an
opaque resource from the retained frame. It requires the proof sequence to be
strictly older than the output frame, asks the backend for a newly guarded
current-frame tile-1 vector whose complete three-block repetition was proven,
matches the exact vector count and every ordered shader modification,
primitive/index identity and physical index base, revalidates the current
viewport/NDC/SystemConstants normalization, and requires one common current
attachment signature. Any mismatch fails before target allocation or command
recording. Removing or weakening one of those gates invalidates the retention
contract.

The original single-snapshot API remains available and publishes the first
family member's proof for existing diagnostics.

## Value-owned evidence

The translated replay token now owns the constant values used by the exact
draw, in addition to its backend object identities:

- every byte of `SpirvShaderTranslator::SystemConstants`;
- the four-word VS and PS float usage masks and their bit-exact packed values;
- all 40 bool/loop words;
- all 192 fetch-constant words;
- explicit NDC scale and offset;
- pipeline and pipeline-layout generations;
- for every shader-used texture: its six guest fetch words, translated
  descriptor binding/dimension/signedness, normalized texture key/layout,
  guest base and mip ranges, image-view identity, residency, and per-texture
  content invalidation generation;
- exact translated sampler binding and immutable sampler parameters;
- viewport and scissor;
- primitive/index conversion and all captured dynamic state; and
- attachment formats, sample count, and sample mask.

The float values use the translated shader's ascending used-register packing,
so unused title constants cannot create a false mismatch.

## Classification

Each domain is classified as:

- `invariant`: bit-identical across all three submissions;
- `mechanical_tile`: differences are limited to viewport/scissor/NDC or the
  conservative SystemConstants byte mask for NDC and translated EDRAM target
  addresses; or
- `semantic`: shader constants, resources, depth range, index conversion,
  dynamic state, or attachments differ.

SystemConstants differences outside the explicit mechanical byte mask are
semantic. Transient descriptor-set handles are deliberately ignored: they are
allocation identity, not binding identity. Resources are invariant only when
the exact shader-used fetch, view, texture-key, sampler, residency, and content
generation evidence matches. Missing or outdated resources remain semantic.
The persistent shared-memory/EDRAM descriptor identity must also match.

The log includes `resource_mismatch_mask` when this proof fails:

- bit 0: missing or non-resident evidence;
- bit 1: pipeline or layout;
- bit 2: shader modification;
- bit 3: descriptor-set presence;
- bit 4: texture evidence;
- bit 5: sampler evidence; and
- bit 6: persistent shared-memory/EDRAM set identity.

## Mechanical normalization

Live evidence shows three stacked local EDRAM strips rather than three global
scissors. The observer derives and proves that model:

- every local scissor starts at `(0,0)`, is 1280 pixels wide, and the three
  local heights sum exactly to 720;
- the cumulative heights are the derived global Y origins with no gaps;
- every viewport starts at `(0,0)`, is 1280 pixels wide, and its remaining
  height is exactly `720 - global_y_origin`;
- guest-to-screen affine slopes and X intercepts are invariant;
- `(first_y_intercept - tile_y_intercept) / 2` equals the cumulative global Y
  origin within two IEEE-754 ULPs;
- identical NDC Z scale and offset; and
- every differing SystemConstants byte to be inside NDC scale/offset XY.

Tile 1 is already the full 1280x720 viewport/NDC transform. The resulting
`normalized_state` therefore copies tile 1's viewport, NDC, and complete
SystemConstants bytes exactly, widening only its local scissor to 1280x720.
Any EDRAM-address or unexplained masked byte change fails closed.

`one_copy_replay_ready` requires no semantic difference, invariant positively
proven resources, and a valid normalized state. Per-frame logging is bounded
to one family summary plus the first unsafe member's rejection details; the
published snapshots retain the complete per-member evidence.

The diagnostic is active with:

```text
--tabletennis_native_main_coverage_ledger=true
--tabletennis_native_translated_shader_artifacts=true
```

The second flag installs the shared translated replay-token observer. Logs use
the prefix `Table Tennis PS328 tile invariance`.
