#!/usr/bin/env node

import { execFileSync } from "node:child_process";
import {
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { verifyPlayerPrepassCoverage } from "./verify_player_prepass_coverage.mjs";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const shaderDirectory = join(root, "src/native/shaders");
const shader = join(
  shaderDirectory,
  "tabletennis_player_observer.hlsl",
);
const includes = [
  "tabletennis_player_common.hlsli",
  "tabletennis_player_skinning.hlsli",
  "tabletennis_player_material.hlsli",
  "tabletennis_player_coverage.hlsli",
].map((name) => join(shaderDirectory, name));
const output = join(
  shaderDirectory,
  "tabletennis_player_observer_spirv.h",
);
const temporary = mkdtempSync(join(tmpdir(), "tt-player-spv-"));

function readDescriptorBindings(binary) {
  const disassembly = execFileSync("spirv-dis", [binary], {
    encoding: "utf8",
  });
  const names = new Map();
  const descriptors = new Map();
  for (const line of disassembly.split("\n")) {
    const name = line.match(/OpName %(\S+) "([^"]+)"/);
    if (name !== null) {
      names.set(name[1], name[2]);
      continue;
    }
    const decoration = line.match(
      /OpDecorate %(\S+) (DescriptorSet|Binding) (\d+)/,
    );
    if (decoration === null) {
      continue;
    }
    const descriptor = descriptors.get(decoration[1]) ?? {};
    descriptor[decoration[2]] = Number(decoration[3]);
    descriptors.set(decoration[1], descriptor);
  }
  return { names, descriptors };
}

function assertDescriptorBindings(stage, binary) {
  const { names, descriptors } = readDescriptorBindings(binary);
  const expected =
    stage === "vert"
      ? new Map([
          ["player_vertex_words", { DescriptorSet: 0, Binding: 1 }],
          ["player_palette_word_pairs", { DescriptorSet: 0, Binding: 2 }],
        ])
      : new Map([
          ["player_texture0", { DescriptorSet: 1, Binding: 0 }],
          ["player_texture1", { DescriptorSet: 1, Binding: 1 }],
          ["player_texture2", { DescriptorSet: 1, Binding: 2 }],
          ["player_sampler0", { DescriptorSet: 0, Binding: 3 }],
          ["player_sampler1", { DescriptorSet: 0, Binding: 4 }],
          ["player_sampler2", { DescriptorSet: 0, Binding: 5 }],
        ]);

  for (const [resourceName, wanted] of expected) {
    const actual = [...names]
      .filter(([, name]) => name === resourceName)
      .map(([id]) => descriptors.get(id))
      .find((descriptor) => descriptor !== undefined);
    if (
      actual?.DescriptorSet !== wanted.DescriptorSet ||
      actual?.Binding !== wanted.Binding
    ) {
      throw new Error(
        `${stage}: ${resourceName} descriptor is ` +
          `${actual?.DescriptorSet ?? "missing"}/` +
          `${actual?.Binding ?? "missing"}, expected ` +
          `${wanted.DescriptorSet}/${wanted.Binding}`,
      );
    }
  }

  const hasConstants = [...descriptors.values()].some(
    (descriptor) =>
      descriptor.DescriptorSet === 0 && descriptor.Binding === 0,
  );
  if (!hasConstants) {
    throw new Error(`${stage}: constants are not set 0 binding 0`);
  }
}

function assertFragmentCoverage(entry, binary) {
  const disassembly = execFileSync("spirv-dis", [binary], {
    encoding: "utf8",
  });
  const hasSampleMask = /BuiltIn SampleMask/.test(disassembly);
  const hasKill = /\bOpKill\b/.test(disassembly);
  if (entry === "ps_prepass") {
    if (!hasSampleMask) {
      throw new Error(
        "ps_prepass: SPIR-V has no BuiltIn SampleMask output",
      );
    }
    if (!hasKill) {
      throw new Error(
        "ps_prepass: SPIR-V has no zero-coverage OpKill",
      );
    }
  } else if (hasSampleMask) {
    throw new Error(`${entry}: unexpected BuiltIn SampleMask output`);
  }
}

function compile(stage, entry) {
  const binary = join(temporary, `${entry}.spv`);
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
      "-I" + shaderDirectory,
      "-S",
      stage,
      "-e",
      entry,
      "-o",
      binary,
      shader,
    ],
    { stdio: "inherit" },
  );
  execFileSync("spirv-val", ["--target-env", "vulkan1.1", binary], {
    stdio: "inherit",
  });
  assertDescriptorBindings(stage, binary);
  if (stage === "frag") {
    assertFragmentCoverage(entry, binary);
  }
  return readFileSync(binary);
}

function words(name, binary) {
  if (binary.length % 4 !== 0) {
    throw new Error(`${name}: SPIR-V byte count is not word-aligned`);
  }
  const values = [];
  for (let offset = 0; offset < binary.length; offset += 4) {
    values.push(
      `0x${binary.readUInt32LE(offset).toString(16).padStart(8, "0")}`,
    );
  }
  const lines = [];
  for (let index = 0; index < values.length; index += 8) {
    lines.push(`    ${values.slice(index, index + 8).join(", ")},`);
  }
  return (
    `inline constexpr uint32_t ${name}[] = {\n` +
    `${lines.join("\n")}\n};\n`
  );
}

function embeddedSource(path) {
  const name = path.slice(shaderDirectory.length + 1);
  const source = readFileSync(path, "utf8");
  if (source.includes(")tt_hlsl\"")) {
    throw new Error(`${name} collides with raw-string delimiter`);
  }
  return `// ---- ${name} ----\n${source}`;
}

try {
  verifyPlayerPrepassCoverage();
  const vertex = compile("vert", "vs_main");
  const prepassPixel = compile("frag", "ps_prepass");
  const colorPixel = compile("frag", "ps_color");
  const source = [shader, ...includes]
    .map(embeddedSource)
    .join("\n");
  const generated =
    `// Generated by tools/generate_player_observer_spirv.mjs.\n` +
    `// DO NOT EDIT. Regenerate after changing the HLSL source.\n` +
    `#pragma once\n\n#include <cstddef>\n#include <cstdint>\n\n` +
    `namespace tabletennis::native::player_observer_shader {\n\n` +
    `inline constexpr char kHlsl[] = R"tt_hlsl(\n` +
    `${source})tt_hlsl";\n\n` +
    `${words("kVertexSpirv", vertex)}` +
    `inline constexpr size_t kVertexSpirvBytes = ` +
    `sizeof(kVertexSpirv);\n\n` +
    `${words("kPrepassPixelSpirv", prepassPixel)}` +
    `inline constexpr size_t kPrepassPixelSpirvBytes = ` +
    `sizeof(kPrepassPixelSpirv);\n\n` +
    `${words("kColorPixelSpirv", colorPixel)}` +
    `inline constexpr size_t kColorPixelSpirvBytes = ` +
    `sizeof(kColorPixelSpirv);\n\n` +
    `}  // namespace tabletennis::native::player_observer_shader\n`;
  writeFileSync(output, generated);
  process.stdout.write(`wrote ${output}\n`);
} finally {
  rmSync(temporary, { recursive: true, force: true });
}
