#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Usage: python3 test_dmt_data.py <audited-en7512.ko> <audited-en7516.ko>."""

import hashlib
import io
from pathlib import Path
import struct
import sys
import unittest
import zlib

from elftools.elf.elffile import ELFFile

from extract_dmt_data import TARGETS, decode_defaults, extract, extract_map


class ExtractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = [Path(path).read_bytes() for path in MODULES]
        cls.images = [extract(blob) for blob in cls.sources]

    def test_golden_images(self):
        expected = ["3dbc7fdc08d1545951675aac3900a3d77c018123dd461b3870ed5a89ec764100",
                    "1c2a7f8e28a04d6aeb2a7b5045dbc99d4ca52b133b82fe61812607a890c45081"]
        for (image, manifest), digest in zip(self.images, expected):
            self.assertEqual(hashlib.sha256(image).hexdigest(), digest)
            self.assertEqual(manifest["firmware_sha256"], digest)

    def test_header_and_crc(self):
        for image, manifest in self.images:
            fields = struct.unpack_from("<8s6H3I", image)
            self.assertEqual(fields[0], b"AIRODMT\0")
            self.assertEqual(fields[2], manifest["target"])
            self.assertEqual(fields[7], len(image) - 32)
            self.assertEqual(fields[8], zlib.crc32(image[32:]))
            self.assertEqual(fields[9], 0)

    def test_defaults_and_dynamic_values(self):
        for index, (image, manifest) in enumerate(self.images):
            digital = struct.unpack_from(f'<{manifest["digital_regs"]}I', image, 32)
            self.assertEqual(digital[0], 0x00060409)
            self.assertEqual(digital[1], [0x019413F0, 0x008F3644][index])
            self.assertEqual(digital[6], [0, 0x01000010][index])
            self.assertEqual(digital[16], 0xFFFFFFFF)
            if index:
                self.assertEqual(digital[34:36], (0, 0))
                self.assertEqual(manifest["dynamic_registers"],
                                 {34: 0x1FA201C0, 35: 0x1FA201C4})
            analog = image[32 + len(digital) * 4:32 + len(digital) * 4 + 52]
            self.assertEqual(analog[1:3], b"\x15\x0d")
            self.assertEqual(analog[46], 0)
            self.assertEqual(analog[51], 4)

    def test_preserve_vendor_map_defects(self):
        for image, manifest in self.images:
            offset = 32 + manifest["digital_regs"] * 4 + 52
            offset += manifest["digital_maps"] * 6
            for index in (149, 150, 151):
                self.assertEqual(struct.unpack_from("<H", image, offset + index * 6)[0], 148)

    def test_reject_unknown_and_modified_module(self):
        for blob in (b"", self.sources[0][:-1], self.sources[1][:-1]):
            with self.assertRaisesRegex(ValueError, "allowlist"):
                extract(blob)

    def test_reject_relocated_table(self):
        for blob in self.sources:
            elf = ELFFile(io.BytesIO(blob))
            spec = TARGETS[hashlib.sha256(blob).hexdigest()]
            name = f'afe{spec["afe"]}_register_map'
            symbol = elf.get_section_by_name(".symtab").get_symbol_by_name(name)[0]
            reloc = elf.get_section_by_name(".rel.data")
            modified = bytearray(blob)
            struct.pack_into(">I", modified, reloc["sh_offset"], symbol["st_value"])
            with self.assertRaisesRegex(ValueError, "relocated"):
                extract_map(ELFFile(io.BytesIO(modified)), name, spec["digital_maps"],
                            spec["stride"], spec["digital_regs"], 32)

    def test_reject_unreviewed_instruction(self):
        for blob in self.sources:
            elf = ELFFile(io.BytesIO(blob))
            spec = TARGETS[hashlib.sha256(blob).hexdigest()]
            start, end = spec["digital_range"]
            modified = bytearray(blob)
            struct.pack_into(">I", modified, elf.get_section_by_name(".text")["sh_offset"]
                             + start, 0xFFFFFFFF)
            with self.assertRaisesRegex(ValueError, "unreviewed instruction"):
                decode_defaults(ELFFile(io.BytesIO(modified)), (start, end),
                                f'AFE{spec["afe"]}WriteValueToSwReg')

    def test_reject_truncated_delay_slot(self):
        for blob in self.sources:
            spec = TARGETS[hashlib.sha256(blob).hexdigest()]
            start, end = spec["analog_range"]
            with self.assertRaisesRegex(ValueError, "truncated delay slot"):
                decode_defaults(ELFFile(io.BytesIO(blob)), (start, end - 4),
                                "AFEEN7556WriteValueToSwReg")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    MODULES = sys.argv[1:]
    unittest.main(argv=[sys.argv[0]], verbosity=2)
