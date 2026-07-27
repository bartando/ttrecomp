#!/usr/bin/env node

import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

const packedOffsets = 0x87;
const hostSampleBits = [1 << 2, 1 << 1, 1 << 0, 1 << 3];

// Independent golden transitions for fragment parity (x, y). Every entry is
// [alpha threshold, cumulative Vulkan sample mask] at the inclusive boundary.
// These are the exact 4x thresholds after the Xenos 3,1,0,2 offsets.
const golden = [
  {
    x: 0,
    y: 0,
    offset: 3,
    transitions: [
      [1 / 16, 0x4],
      [5 / 16, 0x6],
      [9 / 16, 0x7],
      [13 / 16, 0xf],
    ],
  },
  {
    x: 1,
    y: 0,
    offset: 1,
    transitions: [
      [3 / 16, 0x4],
      [7 / 16, 0x6],
      [11 / 16, 0x7],
      [15 / 16, 0xf],
    ],
  },
  {
    x: 0,
    y: 1,
    offset: 0,
    transitions: [
      [4 / 16, 0x4],
      [8 / 16, 0x6],
      [12 / 16, 0x7],
      [16 / 16, 0xf],
    ],
  },
  {
    x: 1,
    y: 1,
    offset: 2,
    transitions: [
      [2 / 16, 0x4],
      [6 / 16, 0x6],
      [10 / 16, 0x7],
      [14 / 16, 0xf],
    ],
  },
];

const floatBits = new DataView(new ArrayBuffer(4));

function previousFloat32(value) {
  const rounded = Math.fround(value);
  if (!(rounded > 0)) {
    throw new Error(`previousFloat32 expects a positive value, got ${value}`);
  }
  floatBits.setFloat32(0, rounded, true);
  floatBits.setUint32(0, floatBits.getUint32(0, true) - 1, true);
  return floatBits.getFloat32(0, true);
}

function xenos4xCoverageMask(alpha, x, y) {
  const offsetIndex = (x & 1) | ((y & 1) << 1);
  const offset = (packedOffsets >>> (offsetIndex << 1)) & 3;
  const thresholdOffset = Math.fround(offset * (1 / 16));
  const thresholds = [
    Math.fround(0.25 - thresholdOffset),
    Math.fround(0.50 - thresholdOffset),
    Math.fround(0.75 - thresholdOffset),
    Math.fround(1.00 - thresholdOffset),
  ];
  const roundedAlpha = Math.fround(alpha);
  let coverage = 0;
  for (let index = 0; index < thresholds.length; ++index) {
    if (roundedAlpha >= thresholds[index]) {
      coverage |= hostSampleBits[index];
    }
  }
  return coverage;
}

function assertEqual(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(
      `${message}: got 0x${actual.toString(16)}, ` +
        `expected 0x${expected.toString(16)}`,
    );
  }
}

export function verifyPlayerPrepassCoverage() {
  let checkedCases = 0;
  for (const pixel of golden) {
    const offsetIndex = (pixel.x & 1) | ((pixel.y & 1) << 1);
    const actualOffset =
      (packedOffsets >>> (offsetIndex << 1)) & 3;
    assertEqual(
      actualOffset,
      pixel.offset,
      `offset at parity ${pixel.x},${pixel.y}`,
    );
    ++checkedCases;

    let previousMask = 0;
    assertEqual(
      xenos4xCoverageMask(0, pixel.x, pixel.y),
      previousMask,
      `transparent mask at parity ${pixel.x},${pixel.y}`,
    );
    ++checkedCases;
    for (const [threshold, inclusiveMask] of pixel.transitions) {
      assertEqual(
        xenos4xCoverageMask(
          previousFloat32(threshold),
          pixel.x,
          pixel.y,
        ),
        previousMask,
        `mask below ${threshold} at parity ${pixel.x},${pixel.y}`,
      );
      assertEqual(
        xenos4xCoverageMask(threshold, pixel.x, pixel.y),
        inclusiveMask,
        `mask at ${threshold} at parity ${pixel.x},${pixel.y}`,
      );
      checkedCases += 2;
      previousMask = inclusiveMask;
    }
    assertEqual(
      xenos4xCoverageMask(1, pixel.x, pixel.y),
      0xf,
      `opaque mask at parity ${pixel.x},${pixel.y}`,
    );
    ++checkedCases;
  }
  return checkedCases;
}

const invokedPath = process.argv[1] ? resolve(process.argv[1]) : "";
if (invokedPath === fileURLToPath(import.meta.url)) {
  const checkedCases = verifyPlayerPrepassCoverage();
  process.stdout.write(
    `verified ${checkedCases} Xenos 4x coverage golden cases\n`,
  );
}
