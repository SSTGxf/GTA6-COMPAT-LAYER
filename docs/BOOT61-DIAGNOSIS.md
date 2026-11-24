# Boot 61 diagnosis — build 80 (real xg_xs texture computer)

Boot #61 ran build 80 (`scarlett_verify.exe "D:\game\game\game_scarlett_master.exe" 300000`,
log `F:\ScarlettCompat\gta_boot61.log`, 2041 lines, `BOOT_EXIT=1`).

## Result in one line

The boot-#60 crash is gone. The exe now dies at `0x140344196` (`int3`,
`0x80000003`) because the **device's `GetResourceAllocationInfo` returned
`sizeInBytes = 0xFFFFFFFFFFFFFFFF` for a console-layout (tiled) resource**.

## Evidence, in log order

```
[xgxs      ] XGSuggestSwizzleMode(desc 256x128 bpe 1, flags 0) -> S_OK layout 0x101 (this layer's 64 KiB model, tile 256x256)
[xboxdev   ] GetResourceAllocationInfo desc: dim=3 align=4096 w=256 h=128 depth=1 mips=1 fmt=61 sample=1/0 layout=0 flags=0x300000
[xboxdev   ] GetResourceAllocationInfo mask=1 num=1 -> size=32768 align=4096 ret=0x140344145
[xboxdev   ] GetResourceAllocationInfo desc: dim=3 align=4096 w=256 h=128 depth=1 mips=1 fmt=61 sample=1/0 layout=257 flags=0x300000
[xboxdev   ] GetResourceAllocationInfo mask=1 num=1 -> size=18446744073709551615 align=65536 ret=0x140344145
[xboxdev   ] GetResourceAllocationInfo desc: dim=3 align=0 w=256 h=128 depth=1 mips=1 fmt=61 sample=1/0 layout=257 flags=0x300000
[xboxdev   ] GetResourceAllocationInfo mask=1 num=1 -> size=18446744073709551615 align=65536 ret=0x140344188
[loader    ] veh: exception 0x80000003 at 0000000140344196 thread 23548
```

Readings that follow directly from those numbers:

* `layout=257` is `0x101` = `0x100 | 1`, i.e. exactly what this layer's
  `XGSuggestSwizzleMode` returned. The game took the suggestion (stored
  `mode|0x100` in its descriptor) and asked the device to size the resource.
  So the suggest path is now exercised and its output is consumed.
* With `layout=0` the same descriptor sized to `32768` bytes / align `4096`
  (256×128×1 byte per texel — a plain linear surface).
* With `layout=257` the layer returned `0xFFFFFFFFFFFFFFFF` and align `65536`.
  The layer does not size console-layout resources; it reports failure through
  the "invalid" size and the caller asserts.

The assert site (`0x140344190`):

```
0x14034413f:  call [rax + 0xc8]        ; device slot 25 = GetResourceAllocationInfo
0x140344145:  movups [rsi], xmm0       ; store D3D12_RESOURCE_ALLOCATION_INFO
0x14034414d:  cmp qword [rsi+8], 0x1000  ; alignment == 4096 ? -> keep it
0x140344155:  je 0x140344190
0x140344157:  mov qword [rdi+8], 0
0x14034415f:  ... second call [rax+0xc8] with align=0
0x140344188:  movups [rsi], xmm0
0x140344190:  cmp dword [rsi+4], 0     ; high dword of sizeInBytes
0x140344194:  je 0x140344197           ; zero -> continue
0x140344196:  int3                     ; <-- boot #61 died here
```

`[rsi+4]` is the high half of `sizeInBytes`; `0xFFFFFFFF` there is the layer's
`-1`, so the exe breaks. The game treats "the device could not size my resource"
as an internal invariant violation, not as a recoverable error.

## What build 80 proved

* `XGCreateTextureComputer` was **not called** in this boot (no `[xgxs]` line for
  it). The path that crashed at `0x1416F71FB` in boot #60 is not reached before
  the new failure, so slot 8 and the copy path remain unexercised in a real boot.
* `XGSuggestSwizzleMode` is called once, at `0x140C4B3CE` (the 0xFF-mode branch),
  and its answer is used. Nothing else in the boot regressed: the same arena
  (`G=20968dc3040 Gsize=808000`), the same slot-74 page-pool registration
  (`count 99776 -> S_OK index=0`), the same 6,538,919,936-byte reservation.

## The next measured blocker

`GetResourceAllocationInfo` (device slot 25, `call [rax+0xc8]` from
`0x14034413F`) must size a resource described with a console layout
(`0x100|mode`) instead of returning `-1`.

Disproof condition for that statement: if a later boot shows the same `int3`
with a real (non-`-1`) size, then the assert is about something else in the
returned struct and this diagnosis is wrong.

That work is **not** part of build 80's four ordered items and no fix build was
made after this boot.

## Checks actually run for build 80

* `build.bat` in the working clone: exit 0; every guest prints `--- verdict: ALL PASS ---`;
  `era stages: 21/21 pass`; `=== verify exit code: 0 ===`.
* Same build on `F:\ScarlettCompat`: compiles all sources (exit reported 1 only
  because the fixed-timestep guest's `ft_120fps` stage measured 225 steps and
  `max interp err 0.5157` under the wall-clock load of the full build). Rerun
  standalone, the same binary reports `ft stages: 7/7 pass` and
  `--- verdict: ALL PASS ---`. It is a wall-clock-sensitive test in
  `tests/guest_physics.cpp`, untouched by build 80 (`src/fixed_timestep.cpp`
  was not modified).
* `out\ScarlettCompat.dll` rebuilt at 04:30 and contains the new `xgxs` code.
* Boot #61 as above. The window stayed black throughout (no frame was ever
  presented); a 6,177,315,413-byte full-memory dump was written and archived to
  `D:\scarlett-dumps\scarlett_crash_boot61.dmp`.
