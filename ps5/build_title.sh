#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Adapted from holdmysocks/mcla-recomp's PS5 title build (b5765a9).
# Uses Mihawk-99's PS5_Vulkan native CRT, RADV link recipe and converter.
set -euo pipefail
port=${1:?usage: build_title.sh PS5_PORT_ROOT [LIBRARY_DIRECTORY]}
libs=${2:-$port/rexglue-sdk/out/ps5-amd64}
sdk="$port/sdk"
driver="$port/reference/PS5_Vulkan"
native="$driver/tooling/native"
tool="$driver/build/host/ps5-native-tool"
archive="$port/arch-radv-build/src/amd/vulkan/libvulkan_radeon.a"
source="$port/game-source/ps5/game"
work="$port/game-title"
title="$port/dist/PPSA99782"
for file in "$tool" "$archive" "$libs/libtabletennis-ps5.a" "$source/param.json"; do
  [[ -f $file ]] || { echo "missing $file" >&2; exit 2; }
done
mkdir -p "$work/obj" "$work/stubs" "$work/ld" "$title/sce_sys" "$title/sce_module"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$driver/tooling/prospero-clang18" "$@"; }

# Large-model game code must be placed in the executable segment, including
# after conversion. An orphan .ltext once caused MCLA to crash the console.
sed -E 's/\*\(\.text \.text\.\*\)/*(.text .text.* .ltext .ltext.* __lcxx_override)/;
        s/\*\(\.rodata \.rodata\.\*\)/*(.rodata .rodata.* .lrodata .lrodata.* .note.dlopen)/;
        s/\*\(\.data \.data\.\*\)/*(.data .data.* .ldata .ldata.*)/;
        s/\*\(\.bss \.bss\.\*\)/*(.bss .bss.* .lbss .lbss.*)/' \
    "$native/ps5-pie.ld" > "$work/ld/ps5-pie.ld"
[[ $(grep -c -E '\.ltext|\.lrodata|\.ldata|\.lbss' "$work/ld/ps5-pie.ld") -eq 4 ]]
cp "$driver/tooling/psbc/ps5-pie-unwind.ld" "$work/ld/ps5-pie-unwind.ld"
for file in app_crt app_cpp_runtime; do
  cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$native/$file.cpp" -o "$work/obj/$file.o"
done
cc -std=c11 -O2 -fPIC -ffunction-sections -c "$source/title_support.c" -o "$work/obj/title_support.o"
stub() {
  local library=$1 file=$2
  cc -std=c11 -O2 -fPIC -c "$file" -o "$work/obj/${library}_stub.o"
  "$sdk/bin/prospero-lld" --shared -soname "${library}.prx" \
    -o "$work/stubs/${library}.so" "$work/obj/${library}_stub.o"
}
stub libSceAgc "$driver/vendor/ps5/sdk/stubs/agc_canary_link_stub.c"
stub libSceAgcDriver "$driver/vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c"
stub libSceSystemService "$port/probe-source/system_service_stub.c"
source "$driver/tools/radv-link.sh"
radv_link_recipe "$driver" "$sdk" "$archive"
{
  printf '{\n local:\n'
  for name in isatty pathconf getresuid getresgid timegm; do
    radv_link_flags+=("--defsym=$name=tt_title_$name")
    printf ' %s;\n' "$name"
  done
  printf '};\n'
} > "$work/title-support-local.map"
# Guest overrides (extern "C" REX_FUNC(sub_...)) replace weak generated
# definitions. Nothing references their objects, so a plain archive link drops
# them silently; desktop links the objects directly.
archives=()
for archive in "$libs"/*.a; do
  [[ $(basename "$archive") == libtabletennis-ps5.a ]] || archives+=("$archive")
done
"$sdk/bin/prospero-lld" -T "$work/ld/ps5-pie-unwind.ld" -L "$work/ld" --eh-frame-hdr \
  "${radv_link_flags[@]}" --version-script "$work/title-support-local.map" \
  --version-script "$native/app-symbols.map" --exclude-libs=ALL --undefined=main \
  -e _start -o "$work/llvm-pie.elf" "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" \
  "$work/obj/title_support.o" --whole-archive "$libs/libtabletennis-ps5.a" --no-whole-archive --start-group "${archives[@]}" "$sdk/target/lib/libc++experimental.a" --end-group \
  "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" "$work/stubs/libSceSystemService.so" \
  "${radv_link_inputs[@]}" --as-needed "$sdk"/target/lib/*.so
python3 "$port/probe-source/check_elf.py" "$work/llvm-pie.elf"
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
  --stub-dir "$sdk/target/lib" --stub "$work/stubs/libSceAgc.so" \
  --stub "$work/stubs/libSceAgcDriver.so" --stub "$work/stubs/libSceSystemService.so" \
  --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
python3 "$port/probe-source/check_elf.py" "$work/eboot.elf"
"$tool" self --sign --in "$work/eboot.elf" --out "$title/eboot.bin" --magic 0x1D3D154F
# Signing also updates an ELF note. Verify uploads against the signed extraction.
"$tool" self --extract --file "$title/eboot.bin" --out "$work/eboot-extracted.elf"
cp "$source/param.json" "$title/sce_sys/param.json"
cp "$source/icon0.png" "$title/sce_sys/icon0.png"
cp "$driver/runtime/libc.prx" "$title/sce_module/libc.prx"
"$tool" self --extract --file "$title/sce_module/libc.prx" --out "$work/libc-extracted.elf"
"$tool" self --inspect --file "$title/eboot.bin"
printf 'Built actual Table Tennis title: %s\n' "$title"
