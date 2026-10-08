#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Uses PS5_Vulkan's GPL native-title tooling and shared RADV link recipe.
set -euo pipefail

port=${1:?usage: build_native_vulkan.sh PS5_PORT_ROOT}
sdk="$port/sdk"
driver="$port/reference/PS5_Vulkan"
mesa="$port/reference/PS5_Mesa-7b59ef27c1b09b9671bc4153c41940c3155c3af2"
archive="$port/arch-radv-build/src/amd/vulkan/libvulkan_radeon.a"
native="$driver/tooling/native"
tool="$driver/build/host/ps5-native-tool"
sources="$port/probe-source"
work="$port/vulkan-probe"
title="$port/dist/PPSA99781"

for file in "$tool" "$archive" "$driver/runtime/libc.prx" \
  "$sources/vulkan-param.json" "$sources/vulkan_info.cpp"; do
  [[ -f $file ]] || { echo "missing $file" >&2; exit 2; }
done
mkdir -p "$work/obj" "$work/stubs" "$work/gen" "$title/sce_sys" "$title/sce_module"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$driver/tooling/prospero-clang18" "$@"; }
flags=(-std=c++23 -O2 -Wall -Wextra -Werror -march=znver2 -ffunction-sections \
  -fdata-sections "-ffile-prefix-map=$port=." -I "$mesa/include" -I "$work/gen")
glslangValidator -V --target-env vulkan1.1 --vn tt_compute_shader \
  "$sources/compute.comp" -o "$work/gen/compute_shader.h"
glslangValidator -V --target-env vulkan1.1 "$sources/compute.comp" -o "$work/gen/compute.spv"
spirv-val --target-env vulkan1.1 "$work/gen/compute.spv"
cc "${flags[@]}" -c "$sources/compute_probe.cpp" -o "$work/obj/compute_probe.o"
cc "${flags[@]}" -c "$sources/display_probe.cpp" -o "$work/obj/display_probe.o"
cc "${flags[@]}" -c "$sources/vulkan_info.cpp" -o "$work/obj/vulkan_info.o"
cc "${flags[@]}" -Dmain=tt_platform_probe_main -c "$sources/platform.cpp" -o "$work/obj/platform.o"
cc -c "$sources/fault_sites.S" -o "$work/obj/fault_sites.o"
for file in app_crt app_cpp_runtime; do
  cc -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
    -ffunction-sections -fdata-sections -c "$native/$file.cpp" -o "$work/obj/$file.o"
done

# These stubs name system-module imports; they do not provide GPU code.
stub() {
  local library=$1 source=$2
  cc -std=c11 -O2 -fPIC -c "$driver/vendor/ps5/sdk/stubs/$source" -o "$work/${library}_stub.o"
  "$sdk/bin/prospero-lld" --shared -soname "${library}.prx" \
    -o "$work/stubs/${library}.so" "$work/${library}_stub.o"
}
stub libSceAgc agc_canary_link_stub.c
stub libSceAgcDriver agc_driver_canary_link_stub.c
cc -std=c11 -O2 -fPIC -c "$sources/system_service_stub.c" -o "$work/system_service_stub.o"
"$sdk/bin/prospero-lld" --shared -soname libSceSystemService.prx \
  -o "$work/stubs/libSceSystemService.so" "$work/system_service_stub.o"

source "$driver/tools/radv-link.sh"
radv_link_recipe "$driver" "$sdk" "$archive"
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
  --version-script "$native/app-symbols.map" --exclude-libs=ALL \
  -e _start -o "$work/llvm-pie.elf" "$work"/obj/*.o \
  "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" "$work/stubs/libSceSystemService.so" \
  "${radv_link_inputs[@]}" --as-needed "$sdk"/target/lib/*.so
python3 "$sources/check_elf.py" "$work/llvm-pie.elf"
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
  --stub-dir "$sdk/target/lib" --stub "$work/stubs/libSceAgc.so" \
  --stub "$work/stubs/libSceAgcDriver.so" --stub "$work/stubs/libSceSystemService.so" \
  --module-sdk 0x02000009 \
  --companion-sdk 0x08050001 --file-name eboot.elf
python3 "$sources/check_elf.py" "$work/eboot.elf"
"$tool" self --sign --in "$work/eboot.elf" --out "$title/eboot.bin" --magic 0x1D3D154F
cp "$sources/vulkan-param.json" "$title/sce_sys/param.json"
cp "$sources/icon0.png" "$title/sce_sys/icon0.png"
cp "$driver/runtime/libc.prx" "$title/sce_module/libc.prx"
"$tool" self --inspect --file "$title/eboot.bin"
printf 'Built installed Vulkan info probe: %s\n' "$title"
