#!/usr/bin/env node

import fs from "node:fs";

const IMAGE_BASE = 0x82000000;
const IMAGE_PATH = "out/xexdump/default_82000000_00770000.bin";
const INIT_PATH = "generated/default/tabletennis_init.cpp";

function guestOffset(address, imageLength) {
  const offset = (address >>> 0) - IMAGE_BASE;
  if (offset < 0 || offset >= imageLength) {
    throw new Error(`guest address 0x${address.toString(16)} is outside image`);
  }
  return offset;
}

function readGuestU32(image, address) {
  return image.readUInt32BE(guestOffset(address, image.length));
}

function readGuestString(image, address) {
  const offset = guestOffset(address, image.length);
  const end = image.indexOf(0, offset);
  return image.toString("utf8", offset, end === -1 ? image.length : end);
}

function loadFunctionNames(source) {
  const functions = new Map();
  const pattern = /\{\s*0x([0-9A-Fa-f]{8}),\s*(sub_[0-9A-Fa-f]{8})\s*\}/g;
  for (const match of source.matchAll(pattern)) {
    functions.set(Number.parseInt(match[1], 16) >>> 0, match[2]);
  }
  return functions;
}

function findAlignedU32(image, value) {
  const matches = [];
  for (let offset = 0; offset + 4 <= image.length; offset += 4) {
    if (image.readUInt32BE(offset) === value) {
      matches.push(offset);
    }
  }
  return matches;
}

function findVtablesForType(image, decoratedName) {
  const typeNeedle = Buffer.from(`${decoratedName}\0`, "utf8");
  const typeNameOffset = image.indexOf(typeNeedle);
  if (typeNameOffset < 8) {
    return [];
  }

  const typeDescriptor = (IMAGE_BASE + typeNameOffset - 8) >>> 0;
  const completeObjectLocators = findAlignedU32(image, typeDescriptor)
      .filter((offset) => offset >= 12)
      .map((offset) => (IMAGE_BASE + offset - 12) >>> 0);

  const vtables = [];
  for (const locator of completeObjectLocators) {
    for (const offset of findAlignedU32(image, locator)) {
      const vtable = (IMAGE_BASE + offset + 4) >>> 0;
      if (vtable >= IMAGE_BASE + 4 &&
          vtable < IMAGE_BASE + image.length) {
        vtables.push(vtable);
      }
    }
  }
  return [...new Set(vtables)];
}

if (process.argv.length < 3) {
  console.error(
      "Usage: describe_guest_vtables.mjs [--slots=N] " +
      "<vtable-address|RTTI-type> [...]");
  process.exit(2);
}

const image = fs.readFileSync(IMAGE_PATH);
const functionNames = loadFunctionNames(fs.readFileSync(INIT_PATH, "utf8"));
let slotCount = 4;
const targets = [];
for (const argument of process.argv.slice(2)) {
  if (argument.startsWith("--slots=")) {
    slotCount = Number.parseInt(argument.slice("--slots=".length), 10);
    if (!Number.isInteger(slotCount) || slotCount < 1 || slotCount > 128) {
      throw new Error(`invalid slot count: ${argument}`);
    }
  } else {
    targets.push(argument);
  }
}

function describeVtable(vtable) {
  try {
    // Xbox 360 MSVC RTTI: vtable[-1] -> CompleteObjectLocator. Its fourth
    // field points at a TypeDescriptor whose decorated name starts at +8.
    const locator = readGuestU32(image, vtable - 4);
    const typeDescriptor = readGuestU32(image, locator + 12);
    const decoratedName = readGuestString(image, typeDescriptor + 8);
    console.log(
        `0x${vtable.toString(16).toUpperCase()}  ${decoratedName} ` +
        `(COL=0x${locator.toString(16).toUpperCase()})`);
    for (let slot = 0; slot < slotCount; ++slot) {
      const target = readGuestU32(image, vtable + slot * 4);
      const name = functionNames.get(target) ?? "data/import/unknown";
      console.log(
          `  [${slot}] 0x${target.toString(16).toUpperCase()} ${name}`);
    }
  } catch (error) {
    console.log(`0x${vtable.toString(16).toUpperCase()}  ${error.message}`);
  }
}

for (const text of targets) {
  const parsedAddress = Number.parseInt(text, 0);
  if (/^0x[0-9a-f]+$/i.test(text) && Number.isSafeInteger(parsedAddress)) {
    describeVtable(parsedAddress >>> 0);
    continue;
  }

  const decoratedName = text.startsWith(".?A") ? text : `.?AV${text}@@`;
  const vtables = findVtablesForType(image, decoratedName);
  if (vtables.length === 0) {
    console.log(`${decoratedName}  no vtables found`);
    continue;
  }
  for (const vtable of vtables) {
    describeVtable(vtable);
  }
}
