# Boot #171/#172 Diagnosis — CBV fix PROVEN, new stop at 0x1425A087D

Date: 2026-11-25 ~01:10 EST. Builder: Muse (Seth: "freebuff isnt working anymore. you are").
Fix: CBV unaligned-location copy (hle_registry.cpp, build 171). Build: BUILD_EXIT=0,
winrt 9/9, B1 149/149, verify 0. DLL 369,152 bytes (was 365,568).

## Boot #171 — flaky early crash, NOT the fix

- Only 236 lines. Crashed at 0x142215C35 (0xc0000005, write to 0x0), right after the
  appdata0.rpf path-rewrite (line 211), during mount/pack init. No CBV fix lines ran.
- The mount-probe at crash time shows zeros (table=0, tblCount=4294967295, init42/43
  all zero) — that is a CONSEQUENCE of crashing early, before the pack manager is set
  up, not the cause.
- None of the new code executes before this point (no resource or CBV creation yet),
  so the fix did not cause it. The dump (5,056,057,266 bytes) was archived to
  D:\scarlett-dumps\scarlett_crash_boot171.dmp (size-verified) and the F: original removed.
- Boot #172 (same DLL) sailed past this point, confirming #171 was a flaky/racy early
  init crash, not a regression from the fix.

## Boot #172 — THE FIX WORKS

- **16,079 lines** (vs ~4,500 for boots #169/#170). The game got nearly 4x further.
- **The CBV fix ran: 8 unique unaligned CBVs were caught and repointed to aligned
  scratch buffers.** All 8 sources were DEFAULT-heap (not CPU-readable), so all 8 got
  the zero-filled scratch fallback (0 copied). Log lines:
  - `CBV unaligned loc=0x2ee591bff20 size=256: source not CPU-readable, zero-filled scratch`
  - `CBV unaligned loc=0x2ee591bff20 size=256 -> aligned scratch 0x315767000 (zero-filled)`
  - (same for loc=0x2ee591bdc40 size=512, loc=0x2ee591dff20 size=256, and 5 more)
- **0x142474606 appears ZERO times in the boot #172 log.** The game definitively passed
  the DEVICE_REMOVED assert that killed boots #165–#170. The CBV unaligned-location
  fix is PROVEN.
- DStorage kept working: batches=20, copied=4199, file=4261 in the tail. New unresolvable
  hash: 0x000000f5b62fe5b2 (Submit refused, E_NOTIMPL) — a second hash-gate entry after
  0xd4a5bde7c2 (boot #169). The hash gate is still open.

## New stop: 0xc0000005 at 0x1425A087D (line 15960 of 16079)

- Exception 0xc0000005, params[0]=0x0 (read), params[1]=0x2e1e44e8694 (a guest VA in
  the 0x2e1e... heap range), thread 860. The game tried to READ an unmapped guest address.
- Immediate context (lines 15952–15959): UNIMPLEMENTED slots 67 (vtable +0x218) and 62
  (vtable +0x1f0) firing repeatedly (call #704, #768, E_NOTIMPL), then a DStorage Submit
  refusal (hash 0xf5b62fe5b2), then the crash.
- Three candidate causes (not yet distinguished):
  1. **Zero-filled CBVs**: the 8 constants read zero. If a constant holds a pointer or an
     offset the game uses to compute an address, zero gives a bad address and a read AV.
     This is the leading suspect — the fix let the game past the assert, but the game
     needs the REAL constant data. The GPU-copy path for DEFAULT-heap sources is the
     real fix (copy DEFAULT-heap source -> scratch via CopyBufferRegion on a command
     list, or read back via a staging buffer).
  2. **Unimplemented slots 67/62**: if the game depends on what they do, E_NOTIMPL could
     leave state unset that the later read depends on. Identify slots 67/62 from the
     D3D12Xbox device vtable and implement the ones the game needs.
  3. **Hash gate**: the refused hash 0xf5b62fe5b2 means some data was not loaded; the read
     address could point into unloaded data.
- Boot #172's dump is GONE (scarlett_crash.dmp is 0 bytes — the known dump-vanishing
  issue; the dump disappears ~2 min after the boot). BOOT_EXIT=1. F: free 8.7 GB.
- The log (scarlett_boot172.log, 16,079 lines) is the evidence; the dump is not available.

## Next steps

1. **GPU-copy path for DEFAULT-heap CBV sources** (the real CBV fix): when the source is
   DEFAULT-heap, copy it to the aligned scratch buffer with a GPU copy instead of
   zero-filling. This is the highest-value next step — it removes the leading suspect
   for the 0x1425A087D crash.
2. Identify slots 67 and 62 (vtable +0x218, +0x1f0) and implement if the game needs them.
3. Hash gate: resolve 0xf5b62fe5b2 (and 0xd4a5bde7c2) — find which RPF holds each.
4. Dump-archive fix (still open): the dump vanishes ~2 min after each boot; archive
   immediately post-boot or write dumps to D: directly.
5. Push the CBV fix + this diagnosis as entry 31 (README/STATUS: boots through #172,
   CBV fix proven, new stop 0x1425A087D).
