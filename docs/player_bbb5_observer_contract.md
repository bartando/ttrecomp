# BBB5 / 2D71 player-auxiliary observer

`tabletennis_player_bbb5_observer` is an observer-only capture for the MAIN
shader pair:

- vertex: `BBB5B0E2C39FB1CE`
- pixel: `2D7133947D0D8685`

Historical gameplay traces show this family as a dynamic player-auxiliary
block. It has appeared as two logical draws / 1,662 indices and as four
logical draws / 5,261 indices. Neither the draw count, index total, nor title
ordinals are part of the contract.

## Admission

A title candidate must carry the exact bound shader pair, a pongPlayer owner,
valid draw scope, pass, selected mesh, draw state, transforms, 16-bit physical
index storage, and an immutable vertex declaration. The vertex stride is read
from the selected guest mesh and retained as data; it is not guessed.

For every candidate the observer copies twice and compares:

- the complete selected vertex buffer;
- the submitted index range, with every decoded index range-checked;
- texture fetch 0 and its complete descriptor-selected mip chain;
- the vertex constant ranges used by the related player programs;
- pixel constants `c46-c49` and `c255`, which the dumped 2D71 microcode reads.

The declaration is probed again during capture and must exactly equal the
candidate declaration. All public snapshots own host memory only.

## Backend proof

The observer consumes the translated Vulkan post-pipeline state tap, after
render-pass and attachment setup but before replacement or the guest draw.
That callback must match the exact shader pair, MAIN target, indexed
triangle-strip state, physical index/vertex identities, attachment contract,
depth/color/blend state, and rasterizer state:

```text
normalized depth control = 00700732
normalized color mask    = 0000000F
color control            = 8700000C
blend control 0          = 00010706
rasterizer mode control  = 00018000
samples / mask           = 4 / all bits
```

Publication requires three byte-for-byte identical backend tile blocks and a
unique ordered title join in both forward and reverse directions.

Successful publications are assigned to `player_bbb5` in the MAIN coverage
ledger using `kSameFrameBackendProof`.

This module has no renderer, replacement matcher, suppression path, or serving
switch. Live validation is still required before any captured field can be
considered a renderer contract.
