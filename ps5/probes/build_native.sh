#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Uses PS5_Vulkan's GPL native-title tooling (BlackBearReloaded / Mihawk-99).
set -euo pipefail

port=${1:?usage: build_native.sh PS5_PORT_ROOT}
sdk="$port/sdk"
driver="$port/reference/PS5_Vulkan"
native="$driver/tooling/native"
tool="$driver/build/host/ps5-native-tool"
rex="$port/rexglue-sdk"
sources="$port/probe-source"
work="$port/native-probes"
title="$port/dist/PPSA99780"

for file in "$tool" "$driver/runtime/libc.prx" "$port/probe-param.json" \
  "$rex/src/core/exception_handler_ps5.cpp" "$sources/native_main.cpp"; do
  [[ -f $file ]] || { echo "missing $file" >&2; exit 2; }
done
mkdir -p "$work/obj" "$title/sce_sys" "$title/sce_module"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$driver/tooling/prospero-clang18" "$@"; }
flags=(-std=c++23 -O2 -Wall -Wextra -Werror -march=znver2 -ffunction-sections \
  -fdata-sections "-ffile-prefix-map=$port=." -I "$rex/include")
for spec in platform:tt_platform_probe_main simd_context:tt_simd_context_probe_main \
  rex_faults:tt_rex_fault_probe_main; do
  file=${spec%%:*}
  entry=${spec#*:}
  cc "${flags[@]}" "-Dmain=$entry" -c "$sources/$file.cpp" -o "$work/obj/$file.o"
done
cc "${flags[@]}" -c "$sources/native_main.cpp" -o "$work/obj/native_main.o"
cc -c "$sources/fault_sites.S" -o "$work/obj/fault_sites.o"
cc "${flags[@]}" -c "$rex/src/core/exception_handler_ps5.cpp" -o "$work/obj/exception.o"
cc "${flags[@]}" -c "$rex/src/core/math_gcc.cpp" -o "$work/obj/math.o"
for file in app_crt app_cpp_runtime; do
  cc -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -c "$native/$file.cpp" -o "$work/obj/$file.o"
done

# Same native ABI/layout as the driver's title, with no Vulkan archive or GPU use.
builtins="$(clang --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
[[ -f $builtins ]] || { echo "missing compiler builtins: $builtins" >&2; exit 2; }
printf '{ local: __assert; };\n' > "$work/platform-local.map"
"$sdk/bin/prospero-lld" -T "$native/ps5-pie.ld" -L "$native" --eh-frame-hdr \
  --gc-sections --version-script "$native/app-symbols.map" --exclude-libs=ALL \
  --defsym=__assert=ps5___assert --version-script "$work/platform-local.map" \
  -e _start -o "$work/llvm-pie.elf" "$work"/obj/*.o \
  -L "$sdk/target/lib" --start-group "$sdk/target/lib/libps5platform.a" \
  "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" \
  "$sdk/target/lib/libunwind.a" "$builtins" --end-group \
  --as-needed "$sdk"/target/lib/*.so
python3 "$sources/check_elf.py" "$work/llvm-pie.elf"

"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
  --stub-dir "$sdk/target/lib" --module-sdk 0x02000009 \
  --companion-sdk 0x08050001 --file-name eboot.elf
python3 "$sources/check_elf.py" "$work/eboot.elf"
"$tool" self --sign --in "$work/eboot.elf" --out "$title/eboot.bin" --magic 0x1D3D154F
cp "$port/probe-param.json" "$title/sce_sys/param.json"
cp "$driver/runtime/libc.prx" "$title/sce_module/libc.prx"
if [[ -f $sources/icon0.png ]]; then
  cp "$sources/icon0.png" "$title/sce_sys/icon0.png"
fi
"$tool" self --inspect --file "$title/eboot.bin"
printf 'Built installed CPU probe title: %s\n' "$title"
