"""Synthetic ARM64 ELF fixture: no DJI object or binary is stored in the repo."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from patch_psdk_usb_padding import EH, SH, SYM, RELA, patch_object


def fixture():
    names = b"\0.text\0.symtab\0.strtab\0.rela.text\0.shstrtab\0"
    strings = b"\0DjiMopChannel_UsbBulkSendData\0memcpy\0"
    text = bytearray(0x3400)
    struct.pack_into("<I", text, 0x32b8, 0xaa0103e2)
    struct.pack_into("<I", text, 0x32c0, 0x94000000)
    symbols = SYM.pack(0, 0, 0, 0, 0, 0) + SYM.pack(1, 2, 0, 1, 0x325c, 392)
    symbols += SYM.pack(strings.index(b"memcpy"), 0x10, 0, 0, 0, 0)
    relocs = RELA.pack(0x100, (2 << 32) | 283, 0) + RELA.pack(0x32c0, (2 << 32) | 283, 0)
    chunks = [b"", text, symbols, strings, relocs, names]
    definitions = [(0, 0, 0, 0, 0, 0, 0), (1, 1, 6, 0, 0, 4, 0),
                   (names.index(b".symtab"), 2, 0, 3, 2, 8, 24),
                   (names.index(b".strtab"), 3, 0, 0, 0, 1, 0),
                   (names.index(b".rela.text"), 4, 0, 2, 1, 8, 24),
                   (names.index(b".shstrtab"), 3, 0, 0, 0, 1, 0)]
    result = bytearray(64)
    sections = []
    for i, (name, kind, flags, link, info, align, entsize) in enumerate(definitions):
        result.extend(b"\0" * ((-len(result)) % max(align, 1)))
        offset = len(result) if i else 0
        sections.append([name, kind, flags, 0, offset, len(chunks[i]), link, info, align, entsize])
        result.extend(chunks[i])
    result.extend(b"\0" * ((-len(result)) % 8))
    shoff = len(result)
    for section in sections:
        result.extend(SH.pack(*section))
    ident = b"\x7fELF\x02\x01\x01" + b"\0" * 9
    EH.pack_into(result, 0, ident, 1, 183, 1, 0, 0, shoff, 0, 64, 0, 0, 64, len(sections), 5)
    return bytes(result)


def sections(data):
    h = EH.unpack_from(data)
    result = []
    for i in range(h[12]):
        s = SH.unpack_from(data, h[6] + i * SH.size)
        result.append((s, data[s[4]:s[4] + s[5]]))
    return result


class PatchTests(unittest.TestCase):
    def test_exact_call_site_and_original_length_only(self):
        before = fixture()
        after = patch_object(before, verify_hash=False)
        old, new = sections(before), sections(after)
        text = bytearray(old[1][1])
        struct.pack_into("<I", text, 0x32b8, 0x53003c42)
        self.assertEqual(new[1][1], text)
        self.assertEqual(new[2][1][:-SYM.size], old[2][1])
        self.assertEqual(new[2][0][7], old[2][0][7])  # Local-symbol boundary.
        self.assertEqual(new[4][1][:RELA.size], old[4][1][:RELA.size])  # Other memcpy stays untouched.
        self.assertEqual(RELA.unpack_from(new[4][1], RELA.size), (0x32c0, (3 << 32) | 283, 0))
        self.assertEqual(new[5][1], old[5][1])
        self.assertTrue(new[3][1].endswith(b"L3Usb_CopyPadded\0"))

    def test_unknown_archive_object_rejected(self):
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            patch_object(fixture())

    def test_changed_instruction_and_relocation_rejected(self):
        original = fixture()
        table = sections(original)
        altered = bytearray(original)
        struct.pack_into("<I", altered, table[1][0][4] + 0x32b8, 0)
        with self.assertRaisesRegex(ValueError, "instruction mismatch"):
            patch_object(altered, verify_hash=False)
        altered = bytearray(original)
        RELA.pack_into(altered, table[4][0][4] + RELA.size, 0x32c0, (2 << 32) | 282, 0)
        with self.assertRaisesRegex(ValueError, "CALL26"):
            patch_object(altered, verify_hash=False)


if __name__ == "__main__":
    unittest.main()
