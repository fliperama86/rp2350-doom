#!/usr/bin/env python3
"""Regression tests executing the linked RP2350 thinker AND zone allocators.

uv run --with unicorn --with pyelftools src/doom/tests/test_thinker_pool.py firmware.elf

Only I_ZoneBase (platform startup) is stubbed. Allocation/free, compressed
pointers, bitsets, memset, and zone heap validation are the real ARM code.
This does not emulate Doom's demo, interrupts, HDMI, or other hardware.
"""
import random
import struct
import sys
from collections import defaultdict

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_MSP, UC_ARM_REG_XPSR

with open(sys.argv[1], "rb") as source:
    elf = ELFFile(source)
    SYMBOLS = {s.name: (s["st_value"] & ~1 if s["st_info"]["type"] == "STT_FUNC" else s["st_value"])
               for s in elf.get_section_by_name(".symtab").iter_symbols()}
    SECTIONS = [(s["sh_addr"], s.data()) for s in elf.iter_sections()
                if s["sh_flags"] & 2 and s["sh_type"] == "SHT_PROGBITS"]
    sizes = {}
    for unit in elf.get_dwarf_info().iter_CUs():
        for die in unit.iter_DIEs():
            name = die.attributes.get("DW_AT_name")
            size = die.attributes.get("DW_AT_byte_size")
            if die.tag == "DW_TAG_structure_type" and name and size:
                if name.value in (b"mobj_s", b"mobjfull_s", b"thinker_s"):
                    sizes[name.value] = size.value
        if len(sizes) == 3:
            break
    assert sizes == {b"mobj_s": 32, b"mobjfull_s": 84, b"thinker_s": 4}, sizes


class Device:
    RETURN = 0x10000000

    def __init__(self):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.mem_map(0x10000000, 0x200000)
        self.uc.mem_map(0x20000000, 0x82000)
        for address, data in SECTIONS:
            self.uc.mem_write(address, data)
        self.uc.reg_write(UC_ARM_REG_XPSR, 0x01000000)
        self.uc.hook_add(UC_HOOK_CODE, self.on_code)
        self.call("Z_Init")
        self.call("P_InitThinkers")
        self.initial_free = self.call("Z_FreeMemory")

    def on_code(self, uc, address, _size, _data):
        if address == self.RETURN:
            self.returned = True
            uc.emu_stop()
        elif address == SYMBOLS["I_ZoneBase"]:
            size_ptr = uc.reg_read(UC_ARM_REG_R0)
            uc.mem_write(size_ptr, struct.pack("<I", 0x20070000 - SYMBOLS["__end__"]))
            uc.reg_write(UC_ARM_REG_R0, SYMBOLS["__end__"])
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
        elif address == SYMBOLS.get("picodoom_crash_stop"):
            raise AssertionError(f"Firmware fatal stop: kind={uc.reg_read(UC_ARM_REG_R0)}, detail={uc.reg_read(UC_ARM_REG_R1)}")

    def call(self, name, *args):
        uc = self.uc
        self.returned = False
        uc.reg_write(UC_ARM_REG_MSP, 0x20081E00)
        uc.reg_write(UC_ARM_REG_LR, self.RETURN | 1)
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), args):
            uc.reg_write(reg, value)
        uc.emu_start(SYMBOLS[name] | 1, 0, count=100000)
        assert self.returned, f"{name} did not return"
        return uc.reg_read(UC_ARM_REG_R0)

    def alloc(self, size):
        ptr = self.call("Z_ThinkMallocImpl", size)
        data = bytearray(self.uc.mem_read(ptr, size))
        data[3] = 0  # pool_info is intentionally set after zeroing the object
        assert data == bytes(size), "Object was not zero-initialized"
        return ptr

    def free(self, ptr):
        self.call("Z_ThinkFree", ptr)

    def validate(self, live):
        assert self.call("Z_ValidateHeap") == 0
        groups = defaultdict(list)
        for ptr, size in live.items():
            info = self.uc.mem_read(ptr + 3, 1)[0]
            assert info & 8
            assert info >> 4 == (0 if size == 32 else 1)
            groups[(info >> 4, ptr - (info & 7) * size)].append(info & 7)
        for type_, size in enumerate((32, 84)):
            expected = set()
            for (t, block), slots in groups.items():
                if t != type_:
                    continue
                assert len(slots) == len(set(slots))
                bitmap = 0xFF ^ sum(1 << slot for slot in slots)
                assert self.uc.mem_read(block - 1, 1)[0] == bitmap
                if bitmap:
                    expected.add(block)
            link = self.short(SYMBOLS["thinker_pool"] + 2*type_)
            seen = set()
            while link:
                assert link not in seen, "Cycle in partial-pool chain"
                seen.add(link)
                bitmap = self.uc.mem_read(link - 1, 1)[0]
                assert 0 < bitmap < 255
                link = self.short(link + (bitmap.bit_length()-1)*size)
            assert seen == expected, f"Lost partial pools: {expected - seen}; unexpected: {seen - expected}"

    def short(self, address):
        value = struct.unpack("<H", self.uc.mem_read(address, 2))[0]
        return 0x20030000 + value*4 if value else 0


def regression(size, slot):
    device = Device()
    blocks = [[device.alloc(size) for _ in range(8)] for _ in range(3)]
    live = {p: size for block in blocks for p in block}
    # Free one object in each FULL block. The list is now C -> B -> A.
    for block in blocks:
        device.free(block[slot])
        del live[block[slot]]
    device.validate(live)
    before = device.call("Z_FreeMemory")
    for block in reversed(blocks):
        ptr = device.alloc(size)
        assert ptr == block[slot], f"Expected reuse of {block[slot]:08X}; got {ptr:08X}"
        live[ptr] = size
        # Original code fails here: memset destroyed C's link to B -> A.
        device.validate(live)
    assert device.call("Z_FreeMemory") == before, "Allocated a new block despite reusable slots"
    for ptr in live:
        device.free(ptr)
    device.validate({})
    assert device.call("Z_FreeMemory") == device.initial_free


for size in (32, 84):
    for slot in range(8):
        regression(size, slot)
print("PASS: 16 last-free-slot regressions across both actor sizes")

device = Device()
live = {}
rng = random.Random(0xD00D)
for step in range(3000):
    if not live or (len(live) < 96 and rng.random() < 0.55):
        size = rng.choice((32, 84))
        ptr = device.alloc(size)
        assert ptr not in live, "Aliased two live objects"
        live[ptr] = size
        # Model the caller populating the thinker link and all payload bytes.
        device.uc.mem_write(ptr, b"\xA5\x5A\x01")
        device.uc.mem_write(ptr + 4, b"\xCC" * (size - 4))
    else:
        ptr = rng.choice(list(live))
        device.free(ptr)
        del live[ptr]
    device.validate(live)
for ptr in live:
    device.free(ptr)
device.validate({})
assert device.call("Z_FreeMemory") == device.initial_free
print("PASS: 3000 mixed allocate/free operations, exact pool reachability, heap integrity and full reclamation")

for size in (12, 20, 44):
    ptr = device.alloc(size)
    assert device.uc.mem_read(ptr + 3, 1)[0] == 0
    device.free(ptr)
assert device.call("Z_FreeMemory") == device.initial_free
for _ in range(3):
    for size in (32, 84):
        for _ in range(24):
            device.alloc(size)
    device.call("Z_FreeTags", 5, 6)
    device.call("P_InitThinkers")
    device.validate({})
    assert device.call("Z_FreeMemory") == device.initial_free
print("PASS: non-pooled sizes and three level teardown/reinitialization cycles")
