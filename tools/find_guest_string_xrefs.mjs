#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";

const DEFAULT_IMAGE = "out/xexdump/default_82000000_00770000.bin";
const DEFAULT_INIT = "generated/default/tabletennis_init.cpp";
const DEFAULT_BASE = 0x82000000;

function usage() {
  console.error(
      "Usage: find_guest_string_xrefs.mjs [--image file] [--init file] " +
      "[--base 0x82000000] <literal> [literal ...]");
}

function parseArguments(argv) {
  const options = {
    image: DEFAULT_IMAGE,
    init: DEFAULT_INIT,
    base: DEFAULT_BASE,
    literals: [],
  };

  for (let index = 0; index < argv.length; ++index) {
    const argument = argv[index];
    if (argument === "--image" || argument === "--init" ||
        argument === "--base") {
      if (index + 1 >= argv.length) {
        usage();
        process.exit(2);
      }
      const value = argv[++index];
      if (argument === "--image") {
        options.image = value;
      } else if (argument === "--init") {
        options.init = value;
      } else {
        options.base = Number.parseInt(value, 0);
      }
    } else {
      options.literals.push(argument);
    }
  }

  if (options.literals.length === 0 || !Number.isSafeInteger(options.base)) {
    usage();
    process.exit(2);
  }
  return options;
}

function loadFunctionAddresses(initFile) {
  const source = fs.readFileSync(initFile, "utf8");
  const functions = [];
  const pattern = /\{\s*0x([0-9A-Fa-f]{8}),\s*(sub_[0-9A-Fa-f]{8})\s*\}/g;
  for (const match of source.matchAll(pattern)) {
    functions.push({
      address: Number.parseInt(match[1], 16) >>> 0,
      name: match[2],
    });
  }
  functions.sort((left, right) => left.address - right.address);
  return functions;
}

function findOwner(functions, address) {
  let low = 0;
  let high = functions.length;
  while (low < high) {
    const middle = (low + high) >>> 1;
    if (functions[middle].address <= address) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low === 0 ? null : functions[low - 1];
}

function signed16(value) {
  return value & 0x8000 ? value - 0x10000 : value;
}

function findAddressReferences(image, imageBase, targetAddress) {
  const references = [];
  for (let offset = 0; offset + 4 <= image.length; offset += 4) {
    const upperInstruction = image.readUInt32BE(offset);
    const upperOpcode = upperInstruction >>> 26;
    const upperRegister = (upperInstruction >>> 21) & 31;
    const upperSource = (upperInstruction >>> 16) & 31;
    if (upperOpcode !== 15 || upperSource !== 0) {
      continue;
    }

    const upperValue = (upperInstruction & 0xFFFF) << 16;
    for (let lookAhead = 1; lookAhead <= 8 && offset + lookAhead * 4 + 4 <= image.length;
         ++lookAhead) {
      const instruction = image.readUInt32BE(offset + lookAhead * 4);
      const opcode = instruction >>> 26;
      const register21 = (instruction >>> 21) & 31;
      const register16 = (instruction >>> 16) & 31;
      let resolved = null;

      // addi RT,RA,SIMM uses RA as the high-half source. ori RA,RS,UIMM
      // uses RS as the source. The destination often differs (for example,
      // lis r11,address@h; addi r3,r11,address@l).
      if (opcode === 14 && register16 === upperRegister) {
        resolved = (upperValue + signed16(instruction & 0xFFFF)) >>> 0;
      } else if (opcode === 24 && register21 === upperRegister) {
        resolved = (upperValue | (instruction & 0xFFFF)) >>> 0;
      }

      if (resolved === targetAddress) {
        references.push((imageBase + offset + lookAhead * 4) >>> 0);
      }

      // Stop once another instruction replaces the register value. Stores and
      // comparisons may mention it without clobbering it, so only check common
      // integer-producing instructions here.
      const writesUpperRegister =
          (opcode === 14 || opcode === 15) && register21 === upperRegister ||
          (opcode === 24 || opcode === 25 || opcode === 26 || opcode === 27) &&
              register16 === upperRegister;
      if (lookAhead > 1 && writesUpperRegister) {
        break;
      }
    }
  }
  return [...new Set(references)];
}

const options = parseArguments(process.argv.slice(2));
const imagePath = path.resolve(options.image);
const initPath = path.resolve(options.init);
const image = fs.readFileSync(imagePath);
const functions = loadFunctionAddresses(initPath);

for (const literal of options.literals) {
  const needle = Buffer.from(`${literal}\0`, "utf8");
  let offset = -needle.length;
  let found = false;
  while ((offset = image.indexOf(needle, offset + needle.length)) !== -1) {
    found = true;
    const guestAddress = (options.base + offset) >>> 0;
    console.log(`"${literal}" @ 0x${guestAddress.toString(16).toUpperCase()}`);
    const references = findAddressReferences(image, options.base, guestAddress);
    if (references.length === 0) {
      console.log("  no lis/addi or lis/ori references found");
      continue;
    }
    for (const reference of references) {
      const owner = findOwner(functions, reference);
      const ownerText = owner === null
          ? "outside known functions"
          : `${owner.name}+0x${(reference - owner.address).toString(16).toUpperCase()}`;
      console.log(
          `  0x${reference.toString(16).toUpperCase()}  ${ownerText}`);
    }
  }
  if (!found) {
    console.log(`"${literal}" not found`);
  }
}
