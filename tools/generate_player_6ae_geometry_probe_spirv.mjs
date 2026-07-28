#!/usr/bin/env node

import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const shader = join(
  root,
  "src/native/shaders/tabletennis_player_6ae_geometry_probe.hlsl",
);
const vertexHelper = join(
  root,
  "src/native/shaders/tabletennis_player_6ae_bbb580_vertex.hlsli",
);
const materialHelper = join(
  root,
  "src/native/shaders/tabletennis_player_6ae_material_stage.hlsli",
);
const output = join(
  root,
  "src/native/shaders/tabletennis_player_6ae_geometry_probe_spirv.h",
);
const temporary = mkdtempSync(join(tmpdir(), "tt-player-6ae-probe-spv-"));

function flattenedShaderSource() {
  const source = readFileSync(shader, "utf8");
  const helper = readFileSync(vertexHelper, "utf8");
  const material = readFileSync(materialHelper, "utf8");
  return source.replace(
    '#include "tabletennis_player_6ae_bbb580_vertex.hlsli"',
    helper,
  ).replace(
    '#include "tabletennis_player_6ae_material_stage.hlsli"',
    material,
  );
}

function disassemble(binary) {
  return execFileSync("spirv-dis", [binary], { encoding: "utf8" });
}

function assertBinding(disassembly, name, set, binding) {
  const ids = [...disassembly.matchAll(
    new RegExp(`OpName %(\\S+) "${name}"`, "g"),
  )].map((match) => match[1]);
  const found = ids.some(
    (id) =>
      new RegExp(`OpDecorate %${id} DescriptorSet ${set}`).test(disassembly) &&
      new RegExp(`OpDecorate %${id} Binding ${binding}`).test(disassembly),
  );
  if (!found) {
    throw new Error(`${name} is not descriptor ${set}/${binding}`);
  }
}

function assertLocation(disassembly, name, location) {
  const ids = [...disassembly.matchAll(
    new RegExp(`OpName %(\\S+) "[^"]*${name}"`, "g"),
  )].map((match) => match[1]);
  const found = ids.some((id) =>
    new RegExp(`OpDecorate %${id} Location ${location}`).test(disassembly)
  );
  if (!found) {
    throw new Error(`${name} is not output location ${location}`);
  }
}

function compile(stage, entry) {
  const binary = join(temporary, `${entry}.${stage}.spv`);
  const flattenedShader = join(temporary, "tabletennis_player_6ae_probe.hlsl");
  writeFileSync(flattenedShader, flattenedShaderSource());
  execFileSync(
    "glslangValidator",
    [
      "-D",
      "--D",
      "TABLETENNIS_VULKAN=1",
      "-V",
      "--target-env",
      "vulkan1.1",
      "-Os",
      "-S",
      stage,
      "-e",
      entry,
      "-o",
      binary,
      flattenedShader,
    ],
    { stdio: "inherit" },
  );
  const text = disassemble(binary);
  if (stage === "vert") {
    assertBinding(text, "player_6ae_vertex_words", 0, 1);
    assertBinding(text, "player_6ae_palette_word_pairs", 0, 2);
    for (let location = 0; location < 7; ++location) {
      assertLocation(text, `interpolator${location}`, location);
    }
  } else {
    // The source-level instruction-19..59 slice reads tf1, tf2 and tf5, but
    // values from tf2/tf5 are only consumed by later guest instructions.
    // Optimized SPIR-V therefore retains only tf1 for this displayed
    // intermediate. Do not fake a dependency merely to keep descriptors live.
    assertBinding(text, "player_6ae_texture_tf1", 1, 1);
    assertBinding(text, "player_6ae_wrap_sampler", 0, 3);
  }
  return readFileSync(binary);
}

function byteArray(name, bytes) {
  const rows = [];
  for (let offset = 0; offset < bytes.length; offset += 16) {
    rows.push(
      `  ${[...bytes.subarray(offset, offset + 16)]
        .map((value) => `0x${value.toString(16).padStart(2, "0")}`)
        .join(", ")},`,
    );
  }
  return `inline constexpr uint8_t ${name}[] = {\n${rows.join("\n")}\n};`;
}

try {
  const vertex = compile("vert", "vs_main");
  const pixel = compile("frag", "ps_main");
  const hlsl = flattenedShaderSource();
  writeFileSync(
    output,
    `#pragma once

#include <cstddef>
#include <cstdint>

namespace tabletennis::native::player_6ae_geometry_probe_shader {

inline constexpr char kHlsl[] = R"TT6AE(${hlsl})TT6AE";

${byteArray("kVertexSpirv", vertex)}
inline constexpr size_t kVertexSpirvBytes = sizeof(kVertexSpirv);

${byteArray("kPixelSpirv", pixel)}
inline constexpr size_t kPixelSpirvBytes = sizeof(kPixelSpirv);

}  // namespace tabletennis::native::player_6ae_geometry_probe_shader
`,
  );
} finally {
  rmSync(temporary, { recursive: true, force: true });
}
