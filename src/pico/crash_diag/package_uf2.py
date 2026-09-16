#!/usr/bin/env python3
"""Package firmware + its advertised WHX into one verified 2 MB-board UF2.

Usage: package_uf2.py firmware.uf2 doom1.whx output.uf2
Requires the matching firmware.bin beside firmware.uf2 and picotool on PATH.
No device access. The output BIN is retained beside output.uf2 for verification.
"""
import re
import struct
import subprocess
import sys
from pathlib import Path

FLASH_BASE = 0x10000000
FLASH_SIZE = 0x200000
ARM_S_FAMILY = 0xE48BFF59
ABSOLUTE_FAMILY = 0xE48BFF57


def flash_pages(path):
    raw = Path(path).read_bytes()
    assert len(raw) % 512 == 0, "Partial UF2 block"
    pages = {}
    for offset in range(0, len(raw), 512):
        block = raw[offset:offset+512]
        m0, m1, flags, addr, size, _, _, family = struct.unpack_from("<8I", block)
        assert (m0, m1, struct.unpack_from("<I", block, 508)[0]) == (0x0A324655, 0x9E5D5157, 0x0AB16F30)
        # picotool's RP2350-E10 workaround block is not application flash data.
        if family == ABSOLUTE_FAMILY:
            assert flags == 0xA000 and addr == 0x10FFFF00 and size == 256
            continue
        assert flags == 0x2000 and family == ARM_S_FAMILY and size == 256
        assert FLASH_BASE <= addr and addr + size <= FLASH_BASE + FLASH_SIZE
        assert addr % 256 == 0 and addr not in pages
        pages[addr] = block[32:32+size]
    assert pages, "No RP2350 ARM Secure flash blocks"
    return pages


def main():
    firmware, whx, output = map(Path, sys.argv[1:])
    assert output.resolve() not in (firmware.resolve(), whx.resolve())
    assert output.with_suffix(".bin").resolve() != firmware.with_suffix(".bin").resolve()
    info = subprocess.check_output(["picotool", "info", "-a", str(firmware)], text=True)
    wad_addr = int(re.search(r"WHX at (0x[0-9a-fA-F]+)", info)[1], 16)
    code_end = int(re.search(r"binary end:\s+(0x[0-9a-fA-F]+)", info)[1], 16)
    code = firmware.with_suffix(".bin").read_bytes()
    wad = whx.read_bytes()
    assert len(code) == code_end - FLASH_BASE
    assert wad_addr % 4096 == 0, "WHX must start on an erase-sector boundary"
    pages = flash_pages(firmware)
    code_padded = code + bytes((-len(code)) % 256)
    assert set(pages) == set(range(FLASH_BASE, FLASH_BASE+len(code_padded), 256))
    for addr, data in pages.items():
        offset = addr - FLASH_BASE
        assert data == code_padded[offset:offset+256], "BIN/UF2 do not match"
    assert FLASH_BASE + len(code_padded) <= wad_addr, "Firmware overlaps WHX"
    assert wad_addr + len(wad) <= FLASH_BASE + FLASH_SIZE, "WHX exceeds physical flash"
    combined = code + b"\xff" * (wad_addr-code_end) + wad
    output.parent.mkdir(parents=True, exist_ok=True)
    binary = output.with_suffix(".bin")
    binary.write_bytes(combined)
    subprocess.run(["picotool", "uf2", "convert", str(binary), str(output),
                    "--family", "rp2350-arm-s", "--abs-block"], check=True)
    expected = combined + bytes((-len(combined)) % 256)
    packaged = flash_pages(output)
    assert set(packaged) == set(range(FLASH_BASE, FLASH_BASE+len(expected), 256))
    for addr, data in packaged.items():
        offset = addr - FLASH_BASE
        assert data == expected[offset:offset+256], "Packaged bytes differ"
    print(f"VERIFIED {output}: code end {code_end:#x}, WHX {wad_addr:#x}..{wad_addr+len(wad):#x}, "
          f"{FLASH_SIZE-len(expected)} bytes clear of 2 MB ceiling")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main()
