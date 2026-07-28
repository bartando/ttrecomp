# Gameplay shader-family census: `545407DF_5661.xtr`

Evidence:

- GPU trace: `out/trace/frames/545407DF_5661.xtr`
- Trace draw dump: `/private/tmp/tt_trace_5661_dump.log`
- Scene-catalog run: `/private/tmp/tt_repro.log`
- CA9 pass contract: `docs/ca9_player_pipeline_contract.md`

This is a one-frame census, not a claim that every family has been
semantically reversed. Roles marked `H` are directly supported by ordering,
geometry reuse, vertex state, or texture state; `M` roles are strong
phase-based classifications; `L` roles still need an isolated draw or render
target dump.

## Counting model

Trace commands 91-531 are the first EDRAM tile. Commands 532-972 and 973-1413
repeat the same 441-command sequence exactly. Counts in the `logical` column
therefore divide tiled GPU submissions by three. Commands outside that range
are not divided.

| Metric | Whole frame | PS328 + CA9/77AF covered | Remaining |
| --- | ---: | ---: | ---: |
| GPU submissions | 1,598 | 276 (17.27%) | 1,322 |
| Logical draws | 716 | 92 (12.85%) | 624 |
| Logical indices | 468,945 | 29,922 (6.38%) | 439,023 |

The scene catalog reports 523 ordered draw hooks at gameplay frame 2730.
The additional GPU-only work is expected: clears, resolves, offscreen passes,
the 75-draw scene-color composite, and HUD/post-processing do not all pass
through the title indexed-draw hook.

## Render-target keys

- `MAIN`: RT0 RGBA8 at EDRAM `0x400`, D24S8 at EDRAM `0`, tiled three
  times. Pitch is approximately 1280 pixels, inferred from the resolved
  1280x720 scene texture; `RB_SURFACE_INFO` was not emitted by this trace
  logger, so the pitch is not yet a captured register fact.
- `COMP`: RT0 RGBA8 at EDRAM `0x2D0`. The first 2AC draw samples a
  1280x720 RGBA8 scene texture. Output pitch is therefore likely 1280, but
  remains inferred.
- `PRE-B8`: offscreen RT0 at EDRAM `0xB8`. Pitch is unknown in the current
  log.
- `PRE/MIXED` and `POST/MIXED`: the family crosses target changes or has not
  had its exact target isolated. Pitch is unknown.

## Priority census

This table contains every family with at least eight logical draws, ordered
by logical draw count.

| Pixel shader | GPU | Logical | Logical indices | Target | Likely role | Confidence |
| --- | ---: | ---: | ---: | --- | --- | :---: |
| `C6CEFDA3753CF2BA` | 468 | 156 | 114,819 | MAIN | Crowd/spectator geometry. One 256x256x8 DXT1 atlas, skinned-looking BD4 VS state, and one contiguous block immediately before players. | H |
| `2AC059EB5C7A942F` | 75 | 75 | 109,982 | COMP | Late player scene-color composite/translucency pass. It redraws the same player index sequences through four VS variants while sampling the resolved 1280x720 scene. | H |
| `328FA02B07C392DC` | 216 | 72 | 16,786 | MAIN | Covered native venue family. | H |
| `2E372EA28CC404B7` | 60 | 60 | 84 | PRE/POST MIXED | Clear, resolve, and synchronization helper draws; not scene geometry. | H |
| `391847433E1601A9` | 57 | 57 | 357 | COMP | HUD/UI sprite and strip draws from commands 1490-1571. | H |
| `14D6B61CBC3D853C` | 141 | 47 | 24,057 | MAIN | Lit arena/venue material family with five textures including a cubemap. | H |
| `D47C83252CF2B765` | 69 | 23 | 48,484 | MAIN | Major player material family: 21 bindings, multiple skinned VS variants, large body/clothing meshes. | H |
| `E33DEAA20A98FCEF` | 69 | 23 | 13,598 | MAIN | Static venue/arena props with three textures and the common 37F2 VS. | H |
| `77AF85E1AF823D02` | 60 | 20 | 13,136 | MAIN | Covered CA9 visible-player family. | H |
| `6AE43640A86B33D8` | 42 | 14 | 14,764 | MAIN | Player skin/clothing material family with 20 bindings and a two-stream skinned VS. | H |
| `2D7133947D0D8685` | 33 | 11 | 4,297 | MAIN | Player auxiliary material, likely hair/eyes/accessories. | M |
| `BCFD62344925BA44` | 10 | 10 | 9,749 | PRE/MIXED | Venue offscreen/reflection or lighting prepass using the same 4EAE geometry class as 14D. | H |
| `F17EBE6C26907F7A` | 10 | 10 | 30 | PRE/POST MIXED | Fullscreen resolve/post-effect helper. | H |
| `1E2A0DEE4F646065` | 27 | 9 | 20,573 | MAIN | High-cost player material/depth family with 21 bindings. | H |
| `9E1AF02A96682354` | 27 | 9 | 1,663 | MAIN | Opaque RGB-only textured/fogged static venue props; alpha is computed but masked at the target. | H |
| `BB90345BFEEE544B` | 24 | 8 | 9,378 | MAIN | Table-net family: two visible 4,680-index net submissions plus six 3-index helper draws. | H |
| `E23F5FEA5FD5399E` | 24 | 8 | 7,293 | MAIN | Player material family with 20 bindings and multiple skinned VS variants. | H |
| `F67534A48B71FF64` | 8 | 8 | 228 | PRE/MIXED | Player-effect/offscreen material followed by resolve helpers. | M |
| `0FFBFA898A141C27` | 8 | 8 | 24 | POST/MIXED | Fullscreen post-processing chain. | H |
| `9567C79307ACC6F5` | 8 | 8 | 24 | PRE/MIXED | Fullscreen offscreen-pass resolve/copy helper. | H |

## Remaining main-scene families

All rows below use `MAIN` and inherit the inferred 1280-pixel pitch.

| Pixel shader | GPU | Logical | Logical indices | Likely role | Confidence |
| --- | ---: | ---: | ---: | --- | :---: |
| `526A35DC94475116` | 21 | 7 | 1,290 | Small static venue props. | H |
| `E24ABECDA1844474` | 15 | 5 | 3,778 | Player material family with 19 bindings. | H |
| `9E20D262C45DE1E5` | 15 | 5 | 231 | Player auxiliary/helper family at the end of the player block. | H |
| `3B8782A9763F4318` | 12 | 4 | 280 | Table, ball, or nearby gameplay accessory; four small draws immediately before the player-material block. | M |
| `A90F5B73F73CB789` | 12 | 4 | 673 | Small player material family with 18 bindings. | H |
| `73427E652A334EC7` | 12 | 4 | 148 | Small player material family with 19 bindings. | H |
| `4B8E0EEF4D762BB8` | 9 | 3 | 12 | Venue environment/sky special using four-index primitives and seven textures. | M |
| `7673413F6418894E` | 9 | 3 | 5,320 | Player geometry family at the start of the player block. | H |
| `0F9CCE179F32DA36` | 9 | 3 | 16,056 | Expensive player material family with 21 bindings. | H |
| `49A6A8DC1D15FD29` | 3 | 1 | 1,392 | Single player auxiliary material. | H |
| `BC4D92BE9E3B4EFB` | 3 | 1 | 22 | Venue special at the start of each tile. | M |
| `E06E999710740ADB` | 3 | 1 | 50 | Venue special immediately before the crowd block. | M |

## Low-count prepass, post-processing, and presentation families

| Pixel shader | GPU/logical | Logical indices | Target | Likely role | Confidence |
| --- | ---: | ---: | --- | --- | :---: |
| `1F510BB662548417` | 5 | 1,357 | PRE/MIXED | Venue offscreen/reflection prepass, including the PS328 geometry class. | H |
| `CCA58AB779028CF6` | 3 | 9,213 | PRE/MIXED | Large offscreen geometry/light-depth pass. | M |
| `E9BB6149C455B5A4` | 6 | 18 | POST/MIXED | Fullscreen post-processing. | H |
| `E786087400784505` | 6 | 1,033 | PRE/MIXED | Player-effect/offscreen material. | M |
| `5378160B0DBCF24F` | 3 | 9 | POST/MIXED | Fullscreen post-processing. | H |
| `8FD900BD69B62CE3` | 2 | 6,493 | PRE-B8 | Static venue offscreen/reflection or lighting prepass. | H |
| `966DBDF8ADFF5F4B` | 2 | 84 | PRE/MIXED | Small static offscreen material. | M |
| `A6B501F3F726D137` | 2 | 1,826 | PRE/MIXED | Static offscreen material. | M |
| `1E70EB9513D670C9` | 2 | 6 | POST/MIXED | Fullscreen post-processing. | H |
| `C2BDF1485839FECF` | 2 | 6 | POST/MIXED | Fullscreen post-processing. | H |
| `8486288CA5F6867C` | 1 | 6,918 | PRE-B8 | Large offscreen geometry/reflection or light-depth pass. | H |
| `13233A08BBBE7F6A` | 1 | 453 | PRE/MIXED | Offscreen special geometry. | L |
| `1BD17548BD04902C` | 1 | 2,941 | PRE/MIXED | Offscreen special geometry. | L |
| `5E11FC7AE2F1C2BF` | 1 | 3 | POST/MIXED | MAIN-to-COMP transition/fullscreen setup. | M |
| `120029A08B10A915` | 1 | 4 | POST/MIXED | Final post-processing primitive. | M |
| `300BD18022137D26` | 1 | 3 | POST/MIXED | Final compositor/present helper. | M |

## Scene coverage and next family

The 441 logical tiled MAIN draws break down as:

| Block | Logical draws | Logical indices | Current native family coverage |
| --- | ---: | ---: | --- |
| Environment/venue, commands 91-246 | 156 | 57,325 | PS328: 72 draws / 16,786 indices |
| Crowd, commands 247-402 | 156 | 114,819 | None |
| Gameplay accessory transition, commands 403-413 | 11 | 433 | None |
| Players, commands 414-531 | 118 | 145,523 | CA9/77AF: 20 draws / 13,136 indices |

The next family should be `C6CEFDA3753CF2BA`.

- It is the largest uncovered family by both logical draw count (156) and
  logical index work (114,819).
- It is a self-contained contiguous block.
- It is visually important: without it, the arena crowd is absent.
- Its pixel side is comparatively tractable: 18 microcode dwords and one
  texture binding. The hard part is verifying the BD4 vertex decode and
  per-draw transforms/palette.

Recommended sequence:

1. Observe all 156 first-tile C6 draws without serving.
2. Prove vertex/index bounds, BD4 input decode, texture slice selection, and
   transforms against trace commands 247-402.
3. Serve one C6 draw in order, then one repeated batch, then the whole family.
4. Keep `2AC059EB5C7A942F` in observation until the underlying player families
   are covered; it depends on resolved scene color and redraws player geometry.

After C6, the next visual/cost candidates are the D47 player family, the 14D
venue family, and then the 2AC player composite. D47 is more central to image
correctness; 14D is the easier broad venue win.
