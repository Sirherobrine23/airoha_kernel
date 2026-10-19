#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract data-only AFE images from two explicitly audited MIPS modules.

Requires pyelftools. Does not execute/emulate the module or export .data as a
whole. A tiny constant propagator decodes audited straight-line initializers;
external calls are recorded, never followed. Source licenses remain separate
from this extractor's license. See Documentation/networking/airoha-dmt.rst.
"""

import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import zlib

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection


TARGETS = {
    "2beeeed730ee9600809e56c92cc1dc53ee65b1e8b03851df8b006a90497a3169": {
        "target": 0x7512, "afe": "A60928", "version": "5.10.3.0",
        "digital_regs": 34, "digital_maps": 208, "stride": 5,
        "digital_range": (0x1568F0, 0x156A90),
        "analog_range": (0x156A90, 0x156CE4),
    },
    "23854f587633d292c67f5877b5355432787ca87cc984e233ff0e5c3c7882ce89": {
        "target": 0x7516, "afe": "A10627", "version": "5.12.10.0",
        "digital_regs": 44, "digital_maps": 262, "stride": 6,
        "digital_range": (0x16E3A8, 0x16E5D4),
        "analog_range": (0x16E5D4, 0x16E828),
    },
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def extract_map(elf, name, count, stride, regs, bits):
    symbols = elf.get_section_by_name(".symtab").get_symbol_by_name(name)
    require(symbols and len(symbols) == 1, f"missing/ambiguous {name}")
    symbol = symbols[0]
    section_index = symbol["st_shndx"]
    section = elf.get_section(section_index)
    start, size = symbol["st_value"], symbol["st_size"]
    require(section["sh_type"] == "SHT_PROGBITS" and
            symbol["st_info"]["type"] == "STT_OBJECT", "not initialized data")
    require(not section["sh_flags"] & 4, "executable map section")
    require(size == count * stride, f"unexpected {name} size")
    require(start + size <= section["sh_size"], "map outside section")
    for rel in elf.iter_sections():
        if isinstance(rel, RelocationSection) and rel["sh_info"] == section_index:
            for entry in rel.iter_relocations():
                off = entry["r_offset"]
                require(not (off < start + size and off + 4 > start),
                        f"relocated/pointer-bearing map {name}")
    raw = section.data()[start:start + size]
    output = bytearray()
    for i in range(count):
        row = raw[i * stride:(i + 1) * stride]
        logical = int.from_bytes(row[:stride - 4], "big")
        physical, width, shift, reserved = row[-4:]
        require(logical == i or (bits == 8 and 149 <= i <= 151 and logical == 148),
                f"unexpected logical ID in {name}[{i}]")
        require(physical < regs and 0 < width <= bits and shift + width <= bits
                and reserved == 0, f"invalid map entry {i}")
        if i < regs:
            require(logical == physical == i and width == bits and shift == 0,
                    "invalid full-register mapping")
        output.extend(struct.pack("<HBBBB", logical, physical, width, shift, 0))
    return bytes(output)


def decode_defaults(elf, span, callee):
    """Propagate LUI/ADDIU/ORI/ADDU constants across audited JALR delay slots.

    Loads are accepted only for the two known EN7516 CHIP_SCU words. Unknown
    values cannot become static defaults. All branches/other calls are errors.
    """
    text = elf.get_section_by_name(".text").data()
    symtab = elf.get_section_by_name(".symtab")
    relocs = {r["r_offset"]: symtab.get_symbol(r["r_info_sym"]).name
              for r in elf.get_section_by_name(".rel.text").iter_relocations()}
    registers = [None] * 32
    registers[0] = 0
    results, dynamic = {}, {}
    pending = False

    def calculate(address):
        nonlocal pending
        word = struct.unpack_from(">I", text, address)[0]
        op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
        rd, funct, imm = (word >> 11) & 31, word & 63, word & 0xFFFF
        signed = imm if imm < 0x8000 else imm - 0x10000
        if op == 15:  # LUI
            registers[rt] = imm << 16
        elif op in (9, 13):  # ADDIU / ORI
            value = registers[rs]
            registers[rt] = None if value is None else (
                (value + signed) & 0xFFFFFFFF if op == 9 else value | imm)
        elif op == 0 and funct == 33:  # ADDU / MOVE
            left, right = registers[rs], registers[rt]
            registers[rd] = None if left is None or right is None else (
                left + right) & 0xFFFFFFFF
        elif op == 35:  # LW -- these values must be read on the actual SoC
            base = registers[rs]
            require(base is not None, "unknown load base")
            effective = (base + signed) & 0xFFFFFFFF
            require(rt == 5 and effective in (0xBFA201C0, 0xBFA201C4),
                    "unreviewed dynamic load")
            registers[rt] = ("chip_scu", effective & 0x1FFFFFFF)
        elif op == 0 and funct == 9:  # JALR; don't execute the callee
            require(rs == 16 and not pending, "unreviewed indirect call")
            pending = True
        else:
            raise ValueError(f"unreviewed instruction {word:08x} at {address:x}")
        if address in relocs:
            require(relocs[address] == callee and rt == 16,
                    "unexpected relocation in initializer")
        registers[0] = 0

    for address in range(*span, 4):
        in_delay_slot = pending
        calculate(address)
        if in_delay_slot:
            index, value = registers[4], registers[5]
            require(isinstance(index, int) and index not in results,
                    "unknown or duplicate default register")
            if isinstance(value, tuple):
                dynamic[index] = value[1]
                value = 0  # Explicit placeholder; the driver overwrites it.
            require(isinstance(value, int), "unknown default value")
            results[index] = value
            pending = False
            # o32 caller-saved state is no longer known after a call.
            for reg in list(range(2, 16)) + [24, 25, 31]:
                registers[reg] = None
    require(not pending, "truncated delay slot")
    return results, dynamic


def extract(blob):
    digest = hashlib.sha256(blob).hexdigest()
    require(digest in TARGETS, "module SHA-256 is not in the audited allowlist")
    spec = TARGETS[digest]
    elf = ELFFile(io.BytesIO(blob))
    require(elf.elfclass == 32 and not elf.little_endian and
            elf["e_machine"] == "EM_MIPS" and elf["e_type"] == "ET_REL",
            "expected relocatable big-endian MIPS32 ELF")
    digital, dynamic = decode_defaults(elf, spec["digital_range"],
                                       f'AFE{spec["afe"]}WriteValueToSwReg')
    analog, analog_dynamic = decode_defaults(elf, spec["analog_range"],
                                             "AFEEN7556WriteValueToSwReg")
    nregs = spec["digital_regs"]
    require(set(digital) == set(range(nregs)) - {7, 8, 17, 25, 26},
            "unexpected digital default set")
    require(set(analog) == set(range(52)) - {15, 46, 48} and not analog_dynamic,
            "unexpected analog default set")
    require(dynamic == ({34: 0x1FA201C0, 35: 0x1FA201C4} if nregs == 44 else {}),
            "unexpected dynamic defaults")
    require(all(0 <= v <= 255 for v in analog.values()), "invalid analog value")
    # Zero-filled .bss shadow slots: skipped digital registers are never written;
    # analog 0x2e is preserved from hardware by the driver, not calibrated here.
    payload = b"".join(struct.pack("<I", digital.get(i, 0)) for i in range(nregs))
    payload += bytes(analog.get(i, 0) for i in range(52))
    payload += extract_map(elf, f'afe{spec["afe"]}_register_map',
                           spec["digital_maps"], spec["stride"], nregs, 32)
    payload += extract_map(elf, "en7556_register_map", 201, spec["stride"], 52, 8)
    header = struct.pack("<8s6H3I", b"AIRODMT\0", 1, spec["target"], nregs, 52,
                         spec["digital_maps"], 201, len(payload),
                         zlib.crc32(payload), 0)
    image = header + payload
    manifest = dict(spec, source_sha256=digest,
                    firmware_sha256=hashlib.sha256(image).hexdigest(),
                    firmware_size=len(image), dynamic_registers=dynamic,
                    analog_uninitialized_slots=[15, 46, 48],
                    inaccessible_vendor_map_ids=[149, 150, 151],
                    scope="AFE initial defaults/maps only; no training firmware")
    return image, manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        image, manifest = extract(args.module.read_bytes())
    except (ValueError, OSError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Refuse accidental replacement of a previously generated artifact.
    with args.output.open("xb") as stream:
        stream.write(image)
    with args.output.with_suffix(args.output.suffix + ".json").open("x") as stream:
        json.dump(manifest, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(f'{args.output}: {len(image)} bytes, {manifest["firmware_sha256"]}')


if __name__ == "__main__":
    main()
