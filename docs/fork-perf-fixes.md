# jessperler88 fork: crash fix and performance work (October 2026)

This fork builds on upstream v1.1 (`452-v1.1-rc29`). Everything below was
tested on a real PS Vita. The game data requirements are unchanged.

## Fixes

### `use_action` crash
The game's `use_action` handler used a weak pointer without checking whether
it had expired. When the object was already gone, the game crashed.
`source/utils/use_action_452.c` patches the check at libPVZ2 offsets
`0x64432c` / `0x644330` (`movs` / `blne`) so a missing object is skipped
instead of dereferenced. The patch is checked against the original bytes
first and is skipped (with a log line) if they don't match.

### Touch check overwrote a saved register (`reimpl/controls.c`)
`controls_tick` passed the address of a `SceTouchSamplingState` local to
`sceTouchGetSamplingState`. The arm-vita-eabi GCC uses 1-byte enums, but the
system call writes 4 bytes, so once a second it overwrote 3 bytes of the
caller's saved `r4`. This caused the bogus FRAME stats seen in every build
(input ~300 ms, max ~16.7 s) and, in some builds, reset the frame counter.
The local is now a `SceUInt32`.

Rule for this toolchain: never pass `&enum_variable` to a `sce*` API.

## Performance

Busy waves (for example Egypt day 12) are CPU-bound in game logic. Measured
at 500 MHz, these changes together save roughly 5–8 ms per busy frame.

### Fast integer division (`reimpl/fastdiv.c`)
The game calls `__aeabi_uidiv`/`__aeabi_idiv`/`__umodsi3`/`__modsi3` between
3,000 and 26,000 times per frame (mostly hash-bucket math). libPVZ2 defines
these helpers itself, so they are bound inside the library and the loader's
import table can't replace them. `pvz2_fastdiv_install` repoints their GOT
slots after `so_relocate`. The new versions cache the reciprocal per divisor
and check every result. On device: 236 ns → 97 ns per call (about 2–3.6 ms
saved per heavy frame). Host test: 126 million checks, 0 failures.

### Fast object lookup (`reimpl/lookup_fast.c`)
`sub_b8e2a0` (game object handle lookup, about 6,500 call sites) is called
2,000–16,000 times per frame. It is replaced by a read-only C version:
- about 78% of calls pass a handle that is already direct and only need a copy;
- named handles go through the type map, the object map and the slot table
  directly, skipping the interned-string refcount++/-- pair that cancels out.

It falls back to the original function for anything unusual (refcount 0, a
named stored handle, a bad slot, a missing pool). An FNV fingerprint
(`0x7039e16b`) over every mirrored code range is checked at boot, and the
patch is skipped on any mismatch. A/B test: 0 mismatches in about 25 million
verified calls, and game time about 8–9% lower (about 2–3 ms in a 30 ms frame).

### Fast sprite atlas UV (`reimpl/sprite_uv_fast.c`)
A C version of the per-sprite atlas UV function at `0xdaa598` (2 idiv, 2 mod
and 5 float divides per call). Fingerprint `0x6ef06d00`. 0 mismatches in
about 4 million verified calls. The gain is small (about 0.5 ms).

### Tried and dropped
- Hand-written assembly for the division hit path: 64 ns vs 68 ns for the C
  version. Not worth keeping.
- A custom allocator: allocation, memcpy and locks cost only about 2 ms per frame.

## Measurement build options (all OFF by default)

None of these are meant for release builds.

| CMake option | Purpose |
| :-- | :-- |
| `PVZ2_PERF_PROBE` | Allocation/copy/lock counters in the frame log |
| `PVZ2_FASTDIV_STATS` | Fast-division call/miss counts plus a startup benchmark |
| `PVZ2_LOOKUP_PROBE` | Times the original object lookup (disables the fast one) |
| `PVZ2_LOOKUP_AB` | Switches between original, fast and verify lookup every 300 frames |
| `PVZ2_UV_AB` | Same A/B switching for the sprite UV function |
| `PVZ2_SAMPLER` | CPU sampler (needs kubridge 0.3+; uses the SDK's newer stub) |
| `PVZ2_ABI_CHECK` | Logs game calls that change callee-saved registers |
| `PVZ2_STACK_SENTINEL` | Detects stray writes into the old main-thread stack region |

Sampler note: its per-function "self" list is biased toward PLT and
cross-page branches. Only the call counts and stack context are reliable.

## Remaining hot spots (ideas, not done)
- Sprite/render batching around `0xda7130` (the `glDrawArrays` caller):
  redundant `glVertexAttribPointer`/`glDisableVertexAttribArray` per draw (`0xda7bc0`).
- Game logic `0xb4d270` → `0xb298e0` (division-heavy).
- Caching named → direct lookup results. This is only safe with insert/erase hooks.
- The `setjmp`-wrapped call at `0xd6a780` and the `wcscmp` lookups at `0xb92908`.

With these changes, busy Egypt 12 runs at about 25–33 ms of game time per
frame at 500 MHz, with occasional spikes to 41–48 ms. It does not yet stay
under 30 ms.
