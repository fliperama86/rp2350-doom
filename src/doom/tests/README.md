# Thinker-pool regression

Run the real linked ARM allocator code, including the real Doom zone allocator:

```sh
uv run --with unicorn --with pyelftools src/doom/tests/test_thinker_pool.py \
  build/src/doom_tiny_usb.elf
```

The fixture replaces only `I_ZoneBase` to supply the linked image's actual
zone address/capacity. It does not emulate game logic or hardware. The target
contract is this RP2350 tiny build; DWARF verifies 32-byte small actors,
84-byte full actors, and the 4-byte thinker header.

## Bug reproduced from the crashed firmware

The pool's highest-numbered free slot stores the link to the next partially
free pool. When allocating the LAST free slot, `Z_ThinkMallocImpl` used to
zero that slot before following its link. The head therefore became null,
orphaning all remaining partial pools. Their objects were still alive, but
their unused slots became unreachable for allocation. The main zone heap
remained structurally valid, so `Z_ValidateHeap` did not detect this bug.

Minimal reproduction: fill three pools A/B/C, free one slot in each (forming
C -> B -> A), then allocate C's slot. The broken code loses B/A immediately.
The regression fails on the exact pre-fix ELF saved with the user's OOM photo:
`build/diagnostics/oom-capture-20260915/doom_crash_full.elf`.

The fix advances the pool-list head before zeroing the object. Tests cover all
eight last-free-slot positions for both actor sizes, three-pool reachability,
3000 deterministic mixed allocations/frees, zero-initialization, no live-object
aliasing, real zone integrity, full reclamation, non-pooled objects, and level
teardown/reinitialization.

The user's captured OOM requested 680 bytes tagged PU_LEVEL. A full-actor pool
needs exactly `8 * 84 + 8 = 680` bytes including its zone header. This is strong
support for the bug's relevance, but the patched attract-mode hardware soak is
still needed to establish that it resolves the reported freeze.
