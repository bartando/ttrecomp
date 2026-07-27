# Player pass identity from title shader microcode

This replaces boot-local D47/6AE title pointer allowlists with stable
microcode evidence. The evidence is learned after the backend identifies a
draw; it is deliberately not a title-side admission gate. Capture remains
observer-only.

## Render chain

Ghidra 12.1.2 headless analysis of `default.xex` establishes:

1. `rage::grmShaderFx::DrawModelGeometry` (`0x820EFB30`, vtable `+0x0C`)
   invokes `BeginTechnique` (`+0x18`), then for each pass invokes
   `BeginPass` (`+0x20`), the model draw, and `EndPass` (`+0x24`).
2. `rage::grmShaderFx::BeginPass` (`0x820EF928`) has
   `r3 = grmShaderFx*` and `r4 = pass_index`. It loads
   `active_fx_state->pass_table[(pass_index + 3)]`, calls
   `rage::fx::ApplyPass`, then commits dirty state.
3. `rage::fx::ApplyPass` (`0x82158C48`) consumes a plain data descriptor:
   - `pass + 0x08`: program pair;
   - `pass + 0x0C`: device-command list;
   - `pass + 0x10`: sampler/state list;
   - `program + 0x48`: vertex-shader reference;
   - `program + 0x4C`: pixel-shader reference.
4. Dereferencing each shader reference yields the exact shader object later
   passed to `rage_gfx_BindVertexShader` (`0x82357240`) or
   `rage_gfx_BindPixelShader` (`0x82356F50`).

The descriptor, program, reference and shader-object addresses are heap
pointers. They are useful within one frame, but none is a boot-independent
identity.

## Shader object layout

The creation paths prove where the immutable microcode lives.

### Pixel shader

`0x82356E60` reads the source header's word 1 as its metadata byte count and
word 2 as its microcode byte count. It copies the source header to the new
object at `+0x34`, allocates and copies exactly word-2 bytes, and stores that
allocation at object `+0x0C`.

- pixel microcode pointer: `shader + 0x0C`;
- pixel microcode byte count: `shader + 0x3C`.

### Vertex shader

`0x82357168` copies the source header to the new object at `+0x250`, allocates
and copies exactly source-header word-2 bytes, then calls `0x82357098`.
`0x82357098` stores the allocation at object `+0x28`.

- vertex microcode pointer: `shader + 0x28`;
- vertex microcode byte count: `shader + 0x258`.

Both creation routines copy immutable guest-endian shader templates. A live
fresh-boot probe proved all reads and bounds, and found an important
stage-specific distinction:

| Family/stage | Title object XXH3 | Backend XXH3 |
| --- | --- | --- |
| 6AE vertex | `C22A861F30E92845` | `BBB580AA5620D2A6` |
| 6AE pixel | `6AE43640A86B33D8` | `6AE43640A86B33D8` |
| D47 vertex template A | `F06FA0F9B3C93AEB` | `20DD150A38FA9949` or `84D1A8EF1D71CF60` |
| D47 vertex template B | `4857D676B0E080E9` | `05AB26C749FFA8B3` or `9013322A360FE8D5` |
| D47 pixel | `D47C83252CF2B765` | `D47C83252CF2B765` |

Pixel microcode reaches the backend byte-identically. Vertex microcode does
not: the title template becomes one of the backend variants according to the
active vertex contract. The static evidence here does not prove that
transformation well enough to reproduce it, so the observer deliberately
keeps the two identities independent instead of pretending the hashes should
match.

## Stable owner identities

RTTI and vtable contents establish the synchronous ownership chain:

| Object/interface | Stable vtable | Relevant slot |
| --- | --- | --- |
| `rage::grmShaderFx` | `0x8202F2DC` | `+0x0C` → `0x820EFB30`, `+0x20` → `0x820EF928` |
| `pongPlayer` primary | `0x8203A43C` | primary player interface |
| `pongPlayer + 8` renderable | `0x8203A47C` | slots 0/1 → `0x8218E6E8` / `0x8218E860` |
| `pongCreature + 0x10` render interface | `0x82027B08` | slot 6 → `0x820C6378` |
| `pongDrawable` | `0x8204DD9C` | slot 5 → `0x8225C7E0` |

`pongPlayer_Render` subtracts eight from its embedded renderable and
synchronously reaches the creature/drawable chain. `pongCreature_RenderDrawable`
resolves `creature_interface + 0x90 -> holder + 0x14 -> pongDrawable`, and
dispatches its `+0x14` slot. This is stronger ownership evidence than matching
any live shader heap pointer.

## Runtime contract

`SceneCatalogPassIdentity` fault-guards and bounds both microcode reads, then
stores XXH3 hashes plus byte counts. A live gate probe showed why those
first-dispatch title hashes cannot classify the family: every hash-matching
D47/6AE sample arrived outside the catalog's `DrawModelGeometry` scope, and
the vertex object could already contain its translated backend form on the
next frame.

Title capture therefore admits a bounded structural superset:

- a nonzero synchronous `pongPlayer` owner;
- valid program/reference/shader structure;
- the proven indexed triangle-strip mesh, stream, palette and material
  contracts.

The independently translated backend stream then selects the exact title
tokens by frame number and ordered draw identity. Publication requires three
identical EDRAM tile blocks with the family's exact backend shader and
attachment/state contract. Only after that join are the selected title
pass/program pointers and first-dispatch microcode hashes logged as learned
evidence. They are never hardcoded as candidate counts or admission hashes.
No native draw is served and no guest draw is suppressed by this observer.

If pass-index telemetry is needed later, the exact minimal hook is
`rage::grmShaderFx::BeginPass` (`0x820EF928`) entry, preserving only
`{r3 owner, r4 pass_index}` through the nested `ApplyPass`. It is not needed
for the dynamic title/backend join and was not added.
