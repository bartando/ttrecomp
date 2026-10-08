# PS5 port: first hardware probe

The user reports a jailbroken PS5 on firmware **13.60**, with **elfldr**,
etaHEN, Kstuff Lite, ftpsrv, ps5debug-ng, and ShadowMountPlus available.
The supplied console address was reachable through elfldr on **9021**, and
the first diagnostic payload ran successfully. Firmware 13.60 is user-reported;
the probe does not independently identify the firmware.

## Prepared locally

`out/ps5-probe/tt-ps5-probe.elf` is an x86-64 PS5 ELF built with
[ps5-payload-sdk v0.43](https://github.com/ps5-payload-dev/sdk/releases/tag/v0.43)
and LLVM 21. Its source and rebuild notes are in
`out/ps5-probe/probe.cpp` and `out/ps5-probe/README.md`.

The payload logs startup, checks pthread creation/join, reserves 4 GiB of
virtual address space, writes one page, changes its protection, releases the
mapping, and exits. It contains no game data or exploit code and does not
write files or change console settings.

Compilation/linking and ELF architecture/imports were checked. The same
source passed a macOS host self-check and then **ran on the user's PS5**.

## First console result

The payload's stdout returned through elfldr and reported:

```text
TT PS5 probe: started (C++23, x86-64)
PASS: pthread creation/join
PASS: 4 GiB virtual address reservation at 200020000 (not 4 GiB physical RAM)
PASS: page read/write, page size=16384
PASS: read-only page protection
TT PS5 probe: finished with 0 failures
```

The full result is saved in `out/ps5-probe/console.log`. This verifies these
specific operations on the console. It does not verify ReXGlue's complete
memory layout/aliases, deliberate fault recovery, game execution, graphics,
audio, or controller input. The reservation commits only one writable page.

`out/ps5-probe/send_probe.py` sends the ELF over elfldr's TCP protocol and
captures its stdout. Its transfer/response handling passed a localhost mock
test and the actual console test. To repeat with the user's running loader:

```sh
python3 out/ps5-probe/send_probe.py --host PS5_IP --port 9021
```

Success requires the payload's final zero-failures message. Merely sending the
file does not count as a successful hardware test.

## Hardware milestones

1. **Passed:** run the probe through the user's compatible ELF loader and collect stdout.
2. Establish graphics output and controller input with a small PS5 demo.
   Verify which graphics API and shader toolchain are actually available.
3. Port ReXGlue's platform layer: virtual memory/aliases, fault handling,
   threads, timing, storage and input. Exercise these before loading the game.
4. Connect a PS5 graphics backend, then test game boot and rendering.

The current SDK recognizes Windows, Linux and macOS. Its graphics backends
are Vulkan and D3D12, and this project's render paths include Vulkan-specific
code. A PS5 CPU build alone does not supply a usable renderer. There is no
PS5 runtime or graphics backend in this checkout yet.

The checkout's retail game files were deleted at the user's request. The
user subsequently identified a separate installation made by the macOS app;
its extracted `default.xex` was located and its title ID confirmed as Table
Tennis (`545407DF`). It remains outside the checkout and is not bundled or
published. The initial probe does not use game files.

The native PS5 RADV driver and MCLA's ReXGlue PS5 port provide a concrete
graphics and runtime reference. See `docs/ps5_runtime_reference.md` for the
inspected revisions, actual native-title launch path and compatibility gaps.
