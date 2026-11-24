# Boot 62 diagnosis — build 81 (slot 25 tiled allocation sizing)

Boot #62 ran build 81 (`scarlett_verify.exe "D:\game\game\game_scarlett_master.exe" 300000`,
log `F:\ScarlettCompat\gta_boot62.log`, 2040 lines, `BOOT_EXIT=1`).

## Result in one line

**The slot-25 fix works**: layout 0 still sizes to 32768/4096, layout 257 now
sizes to 65536/65536, the game passes `0x140344196` with no allocation-info
assert, creates the texture computer, and runs the slot-8 copy successfully.
The boot then dies later, in a different place: `0xC0000005` at `0x1428C9A08`
reading address `0x0`.

## Slot-25 evidence (verbatim log records)

```
[xboxdev] alloc-info: layout=0 dim=3 256x128 depth=1 mips=1 fmt=61 flags=0x300000 sanitized=0x0 bpe=0 tile=0x0 tiles=0 subs=[] packed=0 size=32768 align=4096 path=linear-forward status=not-computed
[xgxs   ] XGSuggestSwizzleMode(desc 256x128 bpe 1, flags 0) -> S_OK layout 0x101 (this layer's 64 KiB model, tile 256x256)
[xboxdev] alloc-info: layout=257 dim=3 256x128 depth=1 mips=1 fmt=61 flags=0x300000 sanitized=0x0 bpe=1 tile=256x256 tiles=1 subs=[0+65536] packed=32768 size=65536 align=65536 path=tiled status=SUCCESS
[xboxdev] alloc-info: layout=257 dim=3 256x128 depth=1 mips=1 fmt=61 flags=0x300000 sanitized=0x0 bpe=1 tile=256x256 tiles=1 subs=[0+65536] packed=32768 size=65536 align=65536 path=tiled status=SUCCESS
```

* Layout 0 (linear) is unchanged: the record still comes from the desktop
  device forward, `size=32768 align=4096`, same as boot #61.
* Layout 257 (`0x100|1`) is now sized by the shared engine:
  `bpe=1 tile=256x256 tiles=1 subs=[0+65536] packed=32768 -> size=65536
  align=65536 status=SUCCESS`. The 64 KiB is *derived* (one tile covers
  256×128 at 1 byte per texel), not hardcoded — the engine check below shows
  other shapes producing 262144/4 tiles, 4194304/64 tiles, mip chains and array
  slices.
* The earlier 3840×2160 R8G8B8A8 records are also unchanged
  (`layout=0 ... size=33423360 align=65536`).

## What the boot did next

```
[xgxs   ] XGCreateTextureComputer(desc ...: dim 3 256x128 depth 1 mips 1 fmt 61 layout 0x101 flags 0x300000 bpe 1) -> supported (status 0)
[xgxs   ] XGCreateTextureComputer -> S_OK object 0000022475C86F30 (1 subresources, 65536 bytes, tile 256x256 bpe 1)
[xgxs   ] copy: dst 000002238A000000 dstRowPitch 0 sub 0 src 00000223454AD620 srcRowPitch 256 srcSlicePitch 32768 (desc 256x128 fmt 61 bpe 1 mips 1 slices 1, layout 65536 bytes)
[xgxs   ] copy: tiled 256x128 bpe 1 into sub 0 at 000002238A000000 (tile 256x256) -> S_OK
[xgxs   ] computer Release -> 0, freeing (256x128 fmt 61 mips 1 slices 1)
```

This is the first boot in which the texture-computer path completes: object
created, slot 8 called with `dstRowPitch 0` (the measured linear→tiled shape),
source row pitch 256 and slice pitch 32768 (the packed linear extents of a
256×128 R8 image), the destination span validated as 65536 committed/writable
bytes, the tiling performed, and the object released through slot 1. No
`E_INVALIDARG` from any of the copy guards.

## The new blocker

```
[xgxs   ] XGSuggestSwizzleMode(desc 256x128 bpe 1, flags 0) -> S_OK layout 0x101 (...)
[xboxdev] UNIMPLEMENTED Xbox device slot called from game 0000000144ECC821 -> E_NOTIMPL
[xboxdev] UNIMPLEMENTED Xbox device slot called from game 0000000142481B31 -> E_NOTIMPL
[xmem   ] XMemMapPhysicalPagesScatter(pages 0000004B8ECF22B0, count 6720, attrs 0x8ece50b0, protect 0x5afe0000) -> 000002262FF10000 (27525120 bytes, ordinary memory)
[loader ] veh: exception 0xc0000005 at 00000001428C9A08 thread 1532
...
[loader ] guest crashed (veh-breakpoint): exception 0xc0000005 at 00000001428C9A08 (record ..., params[0]=0x0 params[1]=0x0) thread 1532
```

The faulting instruction, disassembled from the exe:

```
0x1428c9960:  function start (push r15/r14/rsi/rdi/rbx; sub rsp,0x80), rcx = context -> rsi
0x1428c99ce:  call 0x14550db40            ; builds something into [rsp+0x24]
0x1428c99f3:  mov [rdx], r14              ; stack descriptor: flags/address
0x1428c99f6:  mov [rdx+8], rdi            ; +0x08 address
0x1428c99fa:  mov [rdx+0x18], eax         ; +0x18 format/size field
0x1428c99fd:  mov dword [rdx+0x34], 0
0x1428c9a04:  mov rcx, [rsi+0x18]         ; <-- sub-object pointer, NULL
0x1428c9a08:  mov rax, [rcx]              ; <-- fault: read address 0x0
0x1428c9a0b:  call [rax+0x18]             ; would have been vtable slot 3 on it
0x1428c9a0f:  epilogue
```

`[context+0x18]` is a sub-object pointer that was never created; the code
releases it unconditionally through its vtable slot 3. The function is reached
from exactly one call site, `0x1428C9713`, and runs immediately after the
scatter page mapping and two `E_NOTIMPL` Xbox device-slot calls from
`0x144ECC821` and `0x142481B31`.

Not yet measured (the next investigation, not part of build 81): which device
slots those two call sites are, and which of them is supposed to create the
`[context+0x18]` object.

## Build-81 changes (slot 25 only)

* `include/xg_texture.h` / `src/xg_texture.cpp`: the layout calculation was
  factored out of the texture-computer object into
  `ScXg_ComputeLayout(const void* desc, ScXgLayoutInfo*)` +
  `ScXg_BytesPerTexel(fmt)`, so slot 25 and the copy share one calculator.
  The engine reports `status` (`SUCCESS`, `unsupported-mode`,
  `unsupported-format`, `invalid-dims`, `overflow`, `too-many-subresources`)
  and rejects anything it cannot model instead of returning a number.
* `src/hle_registry.cpp` slot 25 (`XboxDevice_GetResourceAllocationInfo`):
  layout 0 keeps the existing desktop forward untouched; a non-zero layout is
  sized by `ScXg_ComputeLayout` (or fails cleanly with `UINT64_MAX` + reason);
  one structured `alloc-info:` record per call carries layout, dimensions,
  format, mips, dimension, original + sanitized flags, bytes per texel, tile
  geometry, tile count, subresource offsets+extents, packed extent, final size
  and alignment, path and status.
* **Descriptor field-map correction.** Build 80 read the descriptor as
  `+0x24 = bytes per texel` and `+0x30 = layout`. The struct is a
  `D3D12_RESOURCE_DESC`: `+0x24` is `SampleDesc.Count`, `+0x2C` is `Layout`,
  `+0x30` is `Flags` (measured: the game writes `0x105` to `+0x2C` at
  `0x1403440F7`, and the boot-#61 slot-25 record reads layout 257 / flags
  `0x300000` at those offsets). Bytes per texel is now derived from the DXGI
  format (format 61 = R8_UNORM = 1 byte).
* Slot 8's code path is unchanged. The only thing that changed under it is
  that its layout now comes from the shared engine with a format-derived bytes
  per texel; for the boot descriptor that is the same `bpe 1`, `tile 256x256`,
  65536-byte layout as before (proved by the boot record above).

## Verification actually run

* Engine check (throwaway program compiled from `src/xg_texture.cpp` and
  `src/opt_detile.cpp`, not added to the repo), 14/14 PASS:
  * `256x128 fmt61 mip1 layout 0x101` → `size=65536 align=65536 tiles=1
    bpe=1 tile=256x256 subs=1 packed=32768` (the boot descriptor);
  * `512x512 fmt61` → `262144 / 4 tiles`; `1024x1024 fmt28` →
    `4194304 / 64 tiles`; `256x128 mip3` → `196608 / 3 subs`;
    `256x128 depth2` → `131072 / 2 subs`;
  * rejects: layout `0x102`, layout `0x1FF` (`unsupported-mode`), format 0 and
    95 (`unsupported-format`), width 0 / mips 0 / mips 32 / depth 0 /
    dimension 2 (`invalid-dims`), each returning `E_INVALIDARG` with a zeroed
    result.
* `build.bat` in the working clone: exit 0, every guest `ALL PASS`,
  `era stages: 21/21 pass`, `ft stages: 7/7 pass`,
  `=== verify exit code: 0 ===`.
* Same sources built on `F:\ScarlettCompat`; the only failure in that run is
  the known wall-clock flake in `tests/guest_physics.cpp` (`ft_hook` measured
  15 steps against 12±2); rerun standalone it reports `ft stages: 7/7 pass`.
  `src/fixed_timestep.cpp` is untouched by build 81.
* Boot #62 as above; window black, no frame presented.
  A 6,205,020,757-byte dump was written, archived to
  `D:\scarlett-dumps\scarlett_crash_boot62.dmp` (head + tail byte-identical,
  source deleted; F: back to 8.2 GB free).
