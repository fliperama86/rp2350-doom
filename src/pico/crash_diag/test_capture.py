#!/usr/bin/env python3
"""Execute the actual linked capture handlers with mocked SRAM/MMIO.

uv run --with unicorn --with pyelftools src/pico/crash_diag/test_capture.py \
    build/src/doom_tiny_usb.elf

This validates handler instructions and record layout, not hardware exception
entry, real bus faults, multicore operation, or HDMI scanout.
"""
import struct
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_LR,
    UC_ARM_REG_MSP, UC_ARM_REG_PSP, UC_ARM_REG_XPSR, UC_ARM_REG_PRIMASK,
)


with open(sys.argv[1], "rb") as source:
    elf = ELFFile(source)
    symbols = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}
    sections = [(s["sh_addr"], s.data()) for s in elf.iter_sections()
                if s["sh_flags"] & 2 and s["sh_type"] == "SHT_PROGBITS"]

FIELDS = "kind pc lr sp cfsr hfsr mmfar bfar exc_return xpsr detail extra ipsr frame_valid reserved0 reserved1".split()


def run(entry, core=0, args=(0, 0, 0), lr=0x10012345, sp=0x20081700,
        psp=0x20081700, cfsr=0, frame=True):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    for base, size in [(0x10000000, 0x200000), (0x20000000, 0x82000),
                       (0xD0000000, 0x1000), (0xE000E000, 0x2000)]:
        uc.mem_map(base, size)
    for address, data in sections:
        uc.mem_write(address, data)
    uc.mem_write(0xD0000000, struct.pack("<I", core))
    uc.mem_write(0xE000ED28, struct.pack("<5I", cfsr, 0x40000000, 0, 0x12345678, 0x87654321))
    if frame:
        frame_sp = psp if lr & 4 and entry.startswith("isr_") else sp
        uc.mem_write(frame_sp, struct.pack("<8I", 1, 2, 3, 4, 12, 0x10005555, 0x10001234, 0x21000000))
    uc.reg_write(UC_ARM_REG_XPSR, 0x01000003 if entry.startswith("isr_") else 0x01000000)
    uc.reg_write(UC_ARM_REG_MSP, sp)
    uc.reg_write(UC_ARM_REG_PSP, psp)
    uc.reg_write(UC_ARM_REG_LR, lr)
    for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
        uc.reg_write(reg, value)
    record_base = symbols["picodoom_crash"] + 64 * core
    writes = []

    def on_write(_uc, _access, address, size, value, _data):
        assert (record_base <= address and address + size <= record_base + 64) or (
            address == symbols["hdmi_diag_i_error_count"] and size == 4
        ), f"Unexpected write at {address:08x}"
        writes.append(address)

    def on_code(_uc, address, _size, _data):
        if address == symbols["picodoom_crash_halted"]:
            _uc.emu_stop()

    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    uc.emu_start(symbols[entry] | 1, 0, count=300)
    values = struct.unpack("<16I", uc.mem_read(record_base, 64))
    record = dict(zip(FIELDS, values))
    assert record["kind"] != 0, "Capture did not complete"
    assert writes[-1] == record_base, "Record must be published last"
    assert bytes(uc.mem_read(symbols["picodoom_crash"] + 64 * (1-core), 64)) == bytes(64)
    assert uc.reg_read(UC_ARM_REG_PRIMASK) == 1
    assert record["cfsr"] == cfsr and record["hfsr"] == 0x40000000
    assert record["mmfar"] == 0x12345678 and record["bfar"] == 0x87654321
    er = struct.unpack("<I", uc.mem_read(symbols["hdmi_diag_i_error_count"], 4))[0]
    assert er == (record["kind"] == 2)
    return record


tests = 0
for core in (0, 1):
    for kind in (2, 3, 6):
        r = run("picodoom_crash_stop", core=core, args=(kind, 123, 456))
        assert (r["kind"], r["detail"], r["extra"], r["pc"]) == (kind, 123, 456, 0x10012345)
        tests += 1
    for entry, kind, detail in [("__wrap_panic", 4, 0), ("__wrap___assert_func", 5, 789)]:
        r = run(entry, core=core, args=(0x10044444, 789, 0))
        assert (r["kind"], r["detail"], r["extra"], r["pc"]) == (kind, detail, 0x10044444, 0x10012345)
        tests += 1
    for lr in (0xFFFFFFF9, 0xFFFFFFFD, 0xFFFFFFE9, 0xFFFFFFED):
        r = run("isr_hardfault", core=core, lr=lr, psp=0x20011000)
        assert r["kind"] == 1 and r["frame_valid"] == 1
        assert (r["pc"], r["lr"], r["xpsr"]) == (0x10001234, 0x10005555, 0x21000000)
        assert r["sp"] == (0x20011000 if lr & 4 else 0x20081700)
        assert r["exc_return"] == lr and r["ipsr"] == 3
        tests += 1

for kwargs in [dict(sp=0x10000000), dict(sp=0x20082000), dict(sp=0x20081FF0),
               dict(cfsr=0x10), dict(cfsr=0x1000), dict(cfsr=0x100000),
               dict(lr=0xFFFFFFB9), dict(lr=0xFFFFFFD9)]:
    r = run("isr_hardfault", frame=False, **(dict(lr=0xFFFFFFF9) | kwargs))
    assert r["frame_valid"] == 0 and r["pc"] == 0 and r["lr"] == 0
    tests += 1

# Link-time vector substitution is part of the test, not just symbol presence.
vector = next(data for address, data in sections if address == 0x10000000)
for number in (3, 4, 5, 6):
    actual = struct.unpack_from("<I", vector, number * 4)[0]
    assert actual == symbols["isr_hardfault"] | 1, f"Wrong fault vector {number}"

# Text must fit 53 six-pixel glyph cells; include every needed hexadecimal digit.
video = Path(__file__).parents[1].joinpath("i_video.c").read_text()
for letter in "0123456789ABCDEFHKLMNPRSTVX":
    assert "{'" + letter + "'," in video, f"Missing glyph {letter}"
lines = ["C1 K6 PC FFFFFFFF LR FFFFFFFF", "FS FFFFFFFF HS FFFFFFFF SP FFFFFFFF",
         "BF FFFFFFFF MM FFFFFFFF EX FFFFFFFF V1",
         "D 4294967295 X FFFFFFFF TC -2147483648 CP 4294967295",
         "LP 4294967295 TC -2147483648 CP 4294967295",
         "FZ 4294967295 HC 4294967295 C1 4294967295",
         "DM 6 PB 4294967295 CN 4294967295 RS 4294967295"]
assert max(map(len, lines)) <= 53
print(f"PASS: {tests} emulated captures, both cores, publication ordering, wrappers, fault vectors, overlay capacity/glyphs")
