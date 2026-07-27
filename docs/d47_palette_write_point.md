# D47 packed-palette write point

This follows up the zero `vf92` payload reported for the first D47 trace
block in `545407DF_5661.xtr`. The title does populate this buffer. The zero
payload is caused by late XTR memory capture relative to the first draw block.

## Verified title chain

Ghidra decompilation and instruction/xref output establish this sequence:

1. `pongCreature_RenderDrawable` (`0x820C6378`) calls
   `0x8225C668` at `0x820C63E4`, before invoking the drawable's render vslot.
2. `pongDrawable_SubmitModels` (`0x8225C7E0`) also calls `0x8225C668` at
   `0x8225C804`, immediately before the model-list walk at `0x8225D8A0`.
3. `0x8225C668` selects the drawable's current ring-buffer entry, binds it as
   vertex stream 3 with stride `0x1C` through `0x82356118` at `0x8225C6D8`,
   then calls `0x8225C510` at `0x8225C714`.
4. At that call, the ABI arguments are exact:
   - `r3 = *(buffer_object + 0x0C) & ~3`: packed-palette destination;
   - `r4`: source array of 64-byte matrices;
   - `r5`: matrix count.
5. `0x8225C510` converts each 64-byte source matrix into one 28-byte record:
   normalized float4 quaternion followed by float3 translation. Stores are at
   `0x8225C5DC-0x8225C618`; the loop advances source by `0x40` and destination
   by `0x1C`. It flushes every touched 128-byte cache block starting at
   `0x8225C648` and returns at `0x8225C664`.

`0x8225C510` has only two direct call sites: `0x8225C714` above and
`0x8225C7D4` in `0x8225C720`. The latter selects the same drawable ring
buffer and writes another matrix set at `base + matrix_count * 28`. Its two
gameplay call sites pass the two different player drawables stored at
`manager + 0x974` and `manager + 0x978`; they do not combine one half from
each player. This explains why each observed fetch buffer contains two
equal-sized halves owned by one player.

Both binders conditionally reuse their cached payload. `0x8225C668` stores
the active generation/source at drawable `+0xAC/+0xB0`; `0x8225C720` uses
`+0xB4/+0xB8`. Each skips its packer call when both values still match, while
still selecting and binding the current ring buffer. Runtime observed the
initial primary pack in generation 2676, then the exact D47 backend/title
block in generation 2677 with no new pack. Any per-frame write requirement is
therefore false.

The observer models the actual title invariant after both binders return:

- current generation from `0x825C9010`;
- ring divisor from `0x825C9A6C`;
- buffer slot `(generation % divisor + 35)`;
- record count from `*(drawable + 8) + 0x0C`;
- primary cached generation/source from drawable `+0xAC/+0xB0`;
- alternate cached generation/source from drawable `+0xB4/+0xB8`;
- physical buffer base from the selected buffer object's handle.

The proof is admitted only when both cached value pairs equal their calls'
current generation/sources, both derived buffer/count contracts exactly match
the live `vf92` fetch, and the primary binder carries the exact player owner.
The stable full-fetch copy validates all records in both halves. A packer
fingerprint is checked when a same-generation write exists.

The packer destination is a high-heap virtual alias. For example, observed
destination `E6714000` maps to physical `06715000`, including the recomp
runtime's `0x1000` high-heap host-page offset. Masking it directly produces
`06714000` and can never match the active `vf92` binding. Palette-write
correlation must use the same alias-to-physical conversion as vertex and
index capture.

## D47 ownership and sizes

Existing observer logs from the trace run identify:

| Family | Player | Creature | Drawable | Source matrices | Count | `vf92` |
| --- | --- | --- | --- | --- | ---: | --- |
| D47 | `EF08F020` | `F1147790` | `EF090AC0` | `F114B390` | 182 | `06715000`, 10192 bytes |
| CA9 | `EBB8F020` | `EE399770` | `EBB90AC0` | `EE39D3C0` | 183 | `06709000`, 10248 bytes |

The sizes close exactly:

- D47: `182 * 28 * 2 = 10192`;
- CA9: `183 * 28 * 2 = 10248`.

This proves `0x06715000` is the packed output owned by the 182-matrix
`EF08F020` player, not an unrelated zero buffer and not the CA9 player's
palette.

These counts describe that captured trace, not fixed shader-family sizes.
Live gameplay may assign 183 matrices to the player later rendered by D47,
producing a 10248-byte fetch. The runtime proof therefore derives the
per-half count as `fetch_byte_count / (2 * 28)`, accepts 1 through 256
records, and requires completed writes at both derived half addresses.

## Why the first trace block is zero

`tools/audit_xtr_memory.py` scans the XTR command stream without replay and
reports when an overlapping memory command is recorded.

For D47:

```text
command=29640 packet=14209 draws_seen=882 type=read
base=0x06714000 size=16384 overlap=0x06715000+10192
zero=38/10192 fnv=0x04994EE8BBBB901B
```

The first D47 block begins at trace draw command 445. At that point replay has
not received any memory command covering `0x06715000`, so the detailed dump
correctly reports replay's zero-initialized memory. The nonzero D47 memory
read is not recorded until 882 draws have already started, four draws before
the repeated D47 block at command 886.

The recorded first D47 record is valid packed skin data:

```text
quaternion  = (0.00865886, -0.11146807, 0.02453215, 0.99342746)
translation = (0.04825731, -0.04881340, 5.43280792)
```

Both 5096-byte halves of the recorded buffer have the same fingerprint.

CA9 does not show the same ordering failure: its nonzero `0x06709000` memory
read is recorded after only 63 draws, well before CA9 draw command 500.

Therefore the zero D47 result does **not** justify routing the CA9 palette or
changing skin decoding. The XTR writer/backend fails to serialize the
already-live D47 shared-memory range before its first recorded consumer. It
serializes the correct bytes later.

## Safest observer hook

The precise observer point is `sub_8225C510` **exit**:

1. At wrapper entry preserve `ctx.r3`, `ctx.r4`, and `ctx.r5`.
2. Call `__imp__sub_8225C510`.
3. At exit copy and fingerprint exactly `count * 28` bytes from the saved
   destination.
4. Record raw destination, `destination & 0x1FFFFFFF`, source, count, current
   player/creature/drawable scope, and a monotonically increasing write
   sequence.
5. At the existing indexed-draw observer, match `vf92.physical_address`
   against the recorded physical destination and require that the write
   precede the draw.

The current `0x8218E6E8` player scope and `0x820C6378` creature scope bracket
the primary binder. The second half is joined through the exact matching
`0x8225C720` cache/buffer postcondition above.

Hooking `0x8225C510` entry would sample before the write. Hooking
`0x8225C7E0` exit would sample after all nested model draws and lose exact
write-to-draw ordering. Hooking `0x82356118` sees only stream binding, before
the conditional pack at `0x8225C714`.

The observer implements this as a value-only correlation; it does not mutate
guest memory or change either packer call.
