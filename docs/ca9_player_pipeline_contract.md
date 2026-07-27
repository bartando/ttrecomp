# CA9 visible-player native pipeline contract

Evidence: `out/trace/frames/545407DF_5661.xtr`, replayed by
`/tmp/tt_trace_5661_ca9_nonzero.log`. The contract below applies to the
20-draw block at trace commands 500 through 519 using vertex shader
`CA9BBF96B0928616` and pixel shader `77AF85E1AF823D02`.

## Draw and pass ordering

All draws are indexed 16-bit triangle strips (`VGT_DRAW_INITIATOR.prim_type =
6`, DMA source, no 32-bit index flag).

| Commands | Pass descriptor | Index counts | Role |
| --- | --- | --- | --- |
| 500-501 | `0x40106638` | 1663, 460 | depth/alpha prepass |
| 502-503 | `0x4010664C` | 1663, 460 | blended color |
| 504-505 | `0x4010D638` | 1518, 460 | depth/alpha prepass |
| 506-507 | `0x4010D64C` | 1518, 460 | blended color |
| 508-513 | `0x401145B8` | 1623, 110, 185, 207, 211, 131 | depth/alpha prepass |
| 514-519 | `0x401145CC` | 1623, 110, 185, 207, 211, 131 | blended color |

The prepass is not pure depth-only: it writes RT0 alpha as well as depth.

The DMA index address is an exact per-mesh identity in this block. Each
address occurs once in the prepass and once in the color pass:

| Index base | Index count | Commands |
| --- | ---: | --- |
| `0x0D035540` | 1663 | 500, 502 |
| `0x0D0469E0` | 460 | 501, 503 |
| `0x0D02BDE0` | 1518 | 504, 506 |
| `0x0D044210` | 460 | 505, 507 |
| `0x0D0241C0` | 1623 | 508, 514 |
| `0x0D03C6C0` | 110 | 509, 515 |
| `0x0D03DC50` | 185 | 510, 516 |
| `0x0D03F330` | 207 | 511, 517 |
| `0x0D040A30` | 211 | 512, 518 |
| `0x0D041CD0` | 131 | 513, 519 |

This proves the 20 submissions are ten phase-complete geometry groups and
gives the in-order replacement matcher a stronger identity than index count
alone.

## Shared attachment and raster state

- `RB_MODECONTROL = 0x00000004`: color + depth mode.
- `RB_COLOR_INFO = 0x00000400`: RT0 at EDRAM tile `0x400`, RGBA8.
- `RB_DEPTH_INFO = 0x00000000`: depth at EDRAM tile 0, D24S8.
- `PA_SU_SC_MODE_CNTL = 0x00018000`: front and back culling disabled,
  filled polygons, multisampling enabled, vertex window offset enabled.
- Stencil is disabled in both passes.

## Depth/alpha prepass state

Passes: `0x40106638`, `0x4010D638`, `0x401145B8`.

- `RB_COLOR_MASK = 0x00000008`: write RT0 alpha only.
- `RB_DEPTHCONTROL = 0x00700736`: depth test enabled, compare less-or-equal,
  depth write enabled.
- `RB_BLENDCONTROL0 = 0x00010001`: one/zero add for color and alpha, so host
  blending is disabled.
- `RB_COLORCONTROL = 0x87000015`: alpha-to-coverage enabled with offsets
  3, 1, 0, 2; alpha test disabled.

## Blended-color state

Passes: `0x4010664C`, `0x4010D64C`, `0x401145CC`.

- `RB_COLOR_MASK = 0x0000000F`: write RT0 RGBA.
- `RB_DEPTHCONTROL = 0x00700732`: depth test enabled, compare less-or-equal,
  depth write disabled.
- `RB_BLENDCONTROL0 = 0x07060706`: add with source-alpha and
  one-minus-source-alpha for both color and alpha; host blending is enabled.
- `RB_COLORCONTROL = 0x8700000C`: alpha test enabled with `GREATER`,
  alpha-to-coverage disabled.
- `RB_ALPHA_REF = 0x3D808081` (`0.062745101749897`).

## Pixel constants common to all 20 draws

Raw float words were reconstructed at the pixel constant base, not inferred
from rounded log text.

- `c254 = { 0x40000000, 0xBF800000, 0xBF000000, 0x3E99999A }`
  = `(2.0, -1.0, -0.5, 0.30000001192092896)`.
- `c255 = { 0x40400000, 0x3F800000, 0x3F44EC4F, 0x40800000 }`
  = `(3.0, 1.0, 0.7692307829856873, 4.0)`.

These values are unchanged from command 500 through command 519.

## Observer-overlay limitations

The first native observer preserves the six pass descriptors, draw order,
alpha-only depth-writing prepass, exact color-pass alpha reference, and
source-alpha blending. It deliberately does not claim serving parity yet:

- The observer still allocates a private `D32_FLOAT` depth attachment instead
  of borrowing the guest pass's live `D24S8` attachment.
- The comparison overlay is single-sample, so the prepass cannot reproduce
  the guest's alpha-to-coverage state.

These are observer-only differences. They must be resolved or proven
irrelevant before any player draw is eligible for in-order replacement.

## Incremental observer selection

`tabletennis_native_player_observer_geometry_group` narrows rendering without
narrowing capture or verification:

- `-1` (default) renders every verified player draw, preserving the original
  observer behavior.
- `N >= 0` renders the Nth unique immutable mesh group in first-seen guest
  order. Its identity is the player owner plus the physical base, size/count,
  element size, and payload fingerprint of both the `vf95` vertex stream and
  index stream. The title's `model` and `geometry_index` scope fields are not
  identities here: both were zero across a live 20-draw player block.
- Every draw belonging to the selected tuple remains in guest order. The
  renderer requires at least one verified depth/alpha-prepass draw and one
  verified blended-color draw; it refuses to render an incomplete group.

This is a comparison control, not suppression. The guest frame remains
untouched, and the full captured frame must still pass all validity checks.

## In-order replacement candidate gate

`tabletennis_player_replacement_candidates` builds a diagnostic token ledger
after a complete immutable player frame is published. Its observer tap is
registered with the draw-replacement dispatcher, but it never selects a
route. The separate default-off prewarm route is readiness-only and always
falls back to the guest draw.

Each eligible mesh must contain exactly two submissions in guest order:

1. one depth/alpha prepass descriptor;
2. its exact paired blended-color descriptor from the same descriptor family.

Both phases must share the player owner, vf95 and index physical
base/size/count/fingerprints, vf92 base/size/fingerprint, texture fetches,
texture payload fingerprints, and texture-view swizzles. Partial groups,
duplicate/ambiguous phases, mismatched descriptor pairs, and payload
mismatches publish no tokens. If two distinct immutable meshes collapse to
the same backend-visible `{primitive, index count, physical index base}`
identity, both are rejected because the backend context cannot distinguish
their vf95 payloads.

Every accepted observed submission receives a unique one-shot generation.
The default-off prewarm matcher additionally requires the exact
`CA9BBF96B0928616` / `77AF85E1AF823D02` hashes, indexed triangle-strip
topology, submitted count, and physical guest index base. A match consumes
its generation immediately so a failed borrowed-scope acquisition cannot
retarget a later coincidental draw.

The backend context also carries the effective normalized depth control and
color mask, raw `RB_COLORCONTROL` and `RB_BLENDCONTROL0`, and processed
primitive-restart state for both early and late probes. Candidate phases are
not inferred from token order: the prepass must match
`{0x00700736, 0x8, 0x87000015, 0x00010001}` and the color pass must match
`{0x00700732, 0xF, 0x8700000C, 0x07060706}`. Primitive restart must be
disabled, as proven by `PA_SU_SC_MODE_CNTL.multi_prim_ib_ena = 0`. The raw
`VGT_MULTI_PRIM_IB_RESET_INDX` value is retained for diagnostics but is not
compared while restart is disabled because it has no draw semantics in that
state.

### Live borrowed-scope proof

Native RHI can now describe `D24S8`, create D24S8 resources and pipelines on
Vulkan and D3D12. MoltenVK substitutes the guest D24S8 attachment with
`VK_FORMAT_D32_SFLOAT_S8_UINT`; the backend-independent RHI therefore also
describes `D32_FLOAT_S8_UINT`, and the CA9 gate accepts either exact
depth/stencil pair. The player prepass shader emits the exact Xenos 4x
sample mask directly: fragment parity selects packed offsets `3,1,0,2`, and
guest samples `0,1,2,3` map to Vulkan samples `0,2,1,3` at thresholds
`0.75,0.25,0.5,1.0 - offset/16`. Fixed-function host alpha-to-coverage is
explicitly disabled for this pipeline so it cannot AND an
implementation-defined second mask with the shader result. The Vulkan
replacement callback also receives the color/depth/stencil formats, sample
count, and sample mask translated from the actual borrowed render scope. The
dormant CA9 matcher requires exactly one RGBA8 color attachment, D24S8
or D32S8 depth/stencil, four samples, and the full four-sample mask; its early
pre-render-target probe therefore cannot consume a token.

Live gameplay has now proven this contract rather than reconstructing it:

- render-pass key `0x0000000E`, surface pitch 1280;
- one RGBA8 color attachment plus D32S8 depth/stencil on macOS;
- four samples with the full sample mask;
- 58 immutable title tokens / 29 complete phase pairs in the current arena;
- three complete, identically ordered backend tile replays (174 events);
- exact phase and attachment parity for every accepted event.

The observer prewarm also resolves both real borrowed Vulkan pipeline
variants and every buffer/texture descriptor tuple. Output post-processing
first uploads immutable meshes and textures. On the next exact callback,
lookup-only static resources are reused while the current frame's
host-visible palette and constants buffers are filled without recording
copies or barriers inside the borrowed scope. This reached
`succeeded_exact_frame` in live gameplay. The first candidate of a cold run
may still yield with `device_mismatch`; the guest remains authoritative while
the RHI and caches are initialized.

### Remaining no-serve blocker

Serving either phase alone is forbidden: the color pass depends on the
paired depth/coverage result, while suppressing only the prepass changes the
guest frame's depth and alpha semantics.

The current selective callback commits one draw at a time and command
recording cannot be rolled back. CA9 phases are interleaved across mesh
groups, so even a two-draw reservation is not a safe transaction boundary.
Before a player claiming route is registered, the renderer needs a complete
ordered CA9 block plan: all tokens and phase pairs must be present, all exact
resources and both pipeline variants must preflight, and the backend must
commit the whole block or fall back without suppressing any member. Until
that block transaction exists, prewarm always returns `false`.

`tabletennis_native_player_replacement_prewarm` is a default-off,
observer-only readiness probe. It keeps player capture/resource preparation
active, enters only an exact late borrowed CA9 scope, resolves the selected
phase's borrowed Vulkan pipeline and complete buffer/texture descriptor
tuple through `PreflightDraw`, and then always returns `false`. No native draw
is recorded and the original guest draw always executes. The checked serving
API reports success only after `DrawIndexedChecked` actually records an
indexed command; D3D12 fails these readiness calls closed because selective
borrowed replacement is currently Vulkan-only.

The readiness path also maintains bounded ordered asynchronous ledgers: 512
title tokens across at most 16 title frames, plus 512 late backend events.
Each verified title draw publishes a value-only token before its backend
submission, carrying a monotonic title generation, frame generation, phase,
CA9 hashes, primitive/index identity, immutable mesh identity, and separate
palette, constants, and material fingerprints. At the title swap boundary
those tokens are first compared bijectively with the immutable frame
candidates. Successfully verified batches remain live for up to 12 title
generations because Vulkan translation and EDRAM replay may arrive after the
title boundary.

The backend observer tap runs at dispatcher entry, before any replacement
route is selected, even while prewarming is disabled. This ordering matters:
an earlier route may legitimately stop matcher traversal, but it must not hide
a CA9 callback from an observer. Early callbacks are counted and ignored;
only late borrowed-scope callbacks can enter the proof ledger. The tap never
selects a route and the default-off prewarm matcher remains the only route
that can select a player readiness probe.

Bounded diagnostics distinguish four failure shapes without logging every
draw: no CA9 callbacks, early callbacks that never reach the late path, exact
late callbacks rejected by the phase/attachment contract, and callbacks that
arrive before any title token exists. A cumulative summary is emitted when
the first verified title frame expires without a complete proof.

Backend admission preserves the exact phase and borrowed attachment gates
above. Events are consumed only when they reproduce the complete current
immutable title order for each of the three EDRAM tile blocks. The original
trace proved the 20-draw order at commands `500..519`, `941..960`, and
`1382..1401`; the current live arena expands the same contract to 58 ordered
draws per tile.
Mismatched prefixes, interrupted blocks, expired batches, and capacity
pressure are reported without manufacturing a match. This proof channel
retains no guest pointers and never suppresses a guest draw.
