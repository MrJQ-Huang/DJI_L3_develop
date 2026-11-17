#!/usr/bin/env python3
"""Correct one USB-copy call site in a BUILD COPY of pinned DJI PSDK 3.18 ARM64.

No vendor archive is distributed or overwritten. The entire input archive and
object must match known hashes. Preserve the original source length in x2 and
retarget ONLY this memcpy relocation to L3Usb_CopyPadded. The SDK still pads
USB transmissions to 4096, with zeroes supplied by the application adapter.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ARCHIVE_SHA256 = "5725cc72824d915410be59e98cb31b4d269510c4615c1033da8d5fffbffdb5e7"
OBJECT_SHA256 = "65a5663a37d24014e0b34fb1ae45fc74b543bc72540b964fcb7f8f3051d46ede"
OBJECT_NAME = "dji_mop_channel.c.o"
EH = struct.Struct("<16sHHIQQQIHHHHHH")
SH = struct.Struct("<IIQQQQIIQQ")
SYM = struct.Struct("<IBBHQQ")
RELA = struct.Struct("<QQq")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def patch_object(data: bytes, *, verify_hash=True) -> bytes:
    if verify_hash:
        require(hashlib.sha256(data).hexdigest() == OBJECT_SHA256, "SDK object hash mismatch")
    header = list(EH.unpack_from(data))
    require(header[0][:6] == b"\x7fELF\x02\x01" and header[1:3] == [1, 183], "expected ARM64 LE relocatable ELF")
    require(header[10] == 0 and header[11] == SH.size, "unexpected program/section headers")
    sections = [list(SH.unpack_from(data, header[6] + i * SH.size)) for i in range(header[12])]
    payloads = [bytearray(data[s[4]:s[4] + s[5]]) if s[1] != 8 else bytearray() for s in sections]

    def string(table, offset):
        return bytes(table[offset:]).split(b"\0", 1)[0].decode("ascii")

    names = payloads[header[13]]
    by_name = {string(names, s[0]): i for i, s in enumerate(sections)}
    text_i, sym_i = by_name[".text"], by_name[".symtab"]
    str_i = sections[sym_i][6]
    sym_data, strings = payloads[sym_i], payloads[str_i]
    symbols = [SYM.unpack_from(sym_data, i) for i in range(0, len(sym_data), SYM.size)]
    symbol_ids = {string(strings, s[0]): i for i, s in enumerate(symbols)}
    function = symbols[symbol_ids["DjiMopChannel_UsbBulkSendData"]]
    require(function[3:] == (text_i, 0x325c, 392), "USB function layout mismatch")
    text = payloads[text_i]
    require(struct.unpack_from("<I", text, 0x32b8)[0] == 0xaa0103e2, "copy length instruction mismatch")
    require(struct.unpack_from("<I", text, 0x32c0)[0] == 0x94000000, "copy call instruction mismatch")
    # Until this instruction, w2 still holds the original uint16 third argument.
    # UXTH w2,w2 replaces MOV x2,x1 (which overwrites it with padded length).
    struct.pack_into("<I", text, 0x32b8, 0x53003c42)
    new_symbol = len(symbols)
    sym_data.extend(SYM.pack(len(strings), 0x12, 0, 0, 0, 0))  # GLOBAL FUNC UND
    strings.extend(b"L3Usb_CopyPadded\0")
    replacements = 0
    for i, section in enumerate(sections):
        if section[1] != 4 or section[7] != text_i:
            continue
        require(section[6] == sym_i and section[9] == RELA.size, "unexpected relocation table")
        for offset in range(0, len(payloads[i]), RELA.size):
            address, info, addend = RELA.unpack_from(payloads[i], offset)
            if address != 0x32c0:
                continue
            require(info >> 32 == symbol_ids["memcpy"] and (info & 0xffffffff) == 283 and addend == 0,
                    "expected memcpy CALL26 relocation")
            RELA.pack_into(payloads[i], offset, address, (new_symbol << 32) | 283, 0)
            replacements += 1
    require(replacements == 1, "expected exactly one USB memcpy relocation")

    # Rebuild section storage while retaining every section index, symbol value,
    # local-symbol boundary and relocation except the two deliberate edits.
    output = bytearray(data[:EH.size])
    for i, section in enumerate(sections):
        if i == 0:
            continue
        alignment = max(1, section[8])
        require(alignment <= 65536 and alignment & (alignment - 1) == 0, "invalid section alignment")
        output.extend(b"\0" * ((-len(output)) % alignment))
        section[4] = len(output)
        if section[1] != 8:
            section[5] = len(payloads[i])
            output.extend(payloads[i])
    output.extend(b"\0" * ((-len(output)) % 8))
    header[6] = len(output)
    for section in sections:
        output.extend(SH.pack(*section))
    EH.pack_into(output, 0, *header)
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ar", default="ar")
    args = parser.parse_args()
    require(args.input.resolve() != args.output.resolve(), "never overwrite the official archive")
    require(hashlib.sha256(args.input.read_bytes()).hexdigest() == ARCHIVE_SHA256, "unsupported official archive hash")
    original = subprocess.check_output([args.ar, "p", str(args.input), OBJECT_NAME])
    patched = patch_object(original)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="l3-usb-copy-", dir=args.output.parent) as directory:
        directory = Path(directory)
        obj = directory / OBJECT_NAME
        obj.write_bytes(patched)
        archive = directory / "libpayloadsdk.a"
        shutil.copyfile(args.input, archive)
        subprocess.run([args.ar, "rcs", str(archive), str(obj)], check=True)
        require(subprocess.check_output([args.ar, "p", str(archive), OBJECT_NAME]) == patched, "archive replacement failed")
        os.replace(archive, args.output)
    print(json.dumps({"sdk_usb_padding_fix": True, "official_input_sha256": ARCHIVE_SHA256,
                      "fixed_copy_sha256": hashlib.sha256(args.output.read_bytes()).hexdigest()}))


if __name__ == "__main__":
    main()
