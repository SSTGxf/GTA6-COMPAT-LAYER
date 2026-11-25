# Boot 163 diagnosis — slot 25 sizes the typeless-depth allocation

Boots #163 and #164 ran the build in this commit. Both are identical on the
fixed path and on the next frontier.

## Result in one line

**The 2560x1440 typeless-depth allocation is now sized** (19,988,480 bytes,
align 65536), the guest's `VirtualAlloc` of that size succeeds, and the guest
runs ~600 log lines past `0x140344196` — the size assert that killed #162 —
before stopping at a *different* descriptor the layer refuses
(`layout=256` mode 0, 2048x2048, fmt 65).

## The descriptor that failed

Boot #162, verbatim:

```
[xboxdev] alloc-info: layout=0 dim=3 2560x1440 depth=1 mips=1 fmt=19
          flags=0x80300006 sanitized=0x6 bpe=0 ... size=18446744073709551615
          align=65536 path=linear-forward status=not-computed
[loader ] guest crashed (veh-breakpoint): exception 0x80000003 at 0000000140344196
```

fmt 19 is `R32_FLOAT_X8X24_TYPELESS` — a typeless depth-stencil format. Slot 25
forwards every linear (`Layout == 0`) descriptor to the desktop device, and
the desktop answers `UINT64_MAX`, which the guest treats as an invariant
violation (`cmp dword [rsi+4], 0; je ok; int3` at `0x140344190`).

## Evidence measured on this adapter

A direct D3D12 probe (`GetResourceAllocationInfo`, 2560x1440x1) plus the
boot's own requests:

| descriptor | result |
| --- | --- |
| fmt 19, flags 0x2 (DEPTH_STENCIL) | **19,988,480 / align 65536** |
| fmt 19, flags 0x6 (DSV \| UAV) | `UINT64_MAX` |
| fmt 19, flags 0x4 (UAV only) | `UINT64_MAX` |
| fmt 19, flags 0x0 | 19,660,800 / 65536 |
| fmt 17 / 18 (typeless / typed D32S8), any flags | `UINT64_MAX` (not resource formats here) |
| fmt 20, 40, 45 with flags 0x6 | `UINT64_MAX` |
| fmt 40 (D32_FLOAT), flags 0x2 | 16,056,320 |
| fmt 45 (D24S8), flags 0x2 | 19,988,480 |

So the only thing the desktop objects to is `ALLOW_UNORDERED_ACCESS` on a
depth-stencil format — D3D12 has no UAV depth resource. The runtime's depth
sizing is not `width*height*8`: 2560x1440x8 = 29,491,200, while the runtime
returns 19,988,480 for the same shape (and boot #162's own `flags=0x800002`
request for it returned the same number), so the layer must not derive it.

## The fix

In slot 25's linear path, if the first forward returns `UINT64_MAX`, the call
carries a single descriptor, and that descriptor has
`ALLOW_UNORDERED_ACCESS`, the layer retries the same descriptor with only that
bit cleared and reports the runtime's answer (never a size of its own). The
retry is logged with both attempts:

```
[xboxdev] alloc-info: fmt=19 flags=0x6 refused (UINT64_MAX); retried without
          ALLOW_UNORDERED_ACCESS -> size=19988480 align=65536
[xboxdev] alloc-info: layout=0 dim=3 2560x1440 depth=1 mips=1 fmt=19
          flags=0x80300006 sanitized=0x6 ... size=19988480 align=65536
```

The retry fires exactly once in each boot (only the depth descriptor needs
it); every other linear descriptor is unchanged.

## What the guest did next (both boots)

```
[kernelx] VirtualAlloc(0, 19988480, 0x20003000 -> 0x20003000, 0x283) -> 0 (err 87)
[kernelx] VirtualAlloc(0, 19988480, 0x20003000 -> 0x00003000, 0x4) -> 0000017BB3C50000 (err 0)
```

The same 2560x1440 fmt 19 shape with flags 0x2 (`0x4800002`) was then sized at
19,988,480 twice more, and the guest continued through DStorage submissions
and further resource creation.

## The next measured blocker

Both boots stop at the same site (`0x140344196`) for a new descriptor:

```
[xboxdev] alloc-info: layout=256 dim=3 2048x2048 depth=1 mips=1 fmt=65
          flags=0x300000 sanitized=0x0 ... size=18446744073709551615 align=0
          path=tiled-rejected status=unsupported-mode
[xboxdev] alloc-info: layout 256 cannot be sized by this layer
          (unsupported-mode) -> UINT64_MAX (no linear substitute)
```

`layout=256` is the console layout `0x100|0` (mode 0); the layout engine models
the modes used by earlier boots (257, 258, 261) but reports mode 0 as
`unsupported-mode`, so slot 25 fails cleanly and the guest asserts. fmt 65 is
`R8G8B8A8_UNORM_SRGB`, 2048x2048, 1 mip. This is a layout-engine gap, not a
depth-format one, and is outside this change.

A second thing to watch: the same depth descriptor, when it is later created
rather than sized, still carries UAV; slot 133 does not yet apply the same
measured flag rule, so a create of it would be refused by the desktop. No boot
has reached that create yet.

## Checks actually run

* Local `build.bat`: exit 0, `era_detail=21/21`, `ft stages: 7/7 pass`,
  `=== verify exit code: 0 ===`, B1/B2 harness 149/149 PASS.
* Same sources on `F:\ScarlettCompat`: exit 0, the same four gate lines.
* Boots #163 and #164 from the F: build: both size the depth descriptor at
  19,988,480, both reach the same new frontier (`layout=256` 2048x2048 fmt 65,
  `status=unsupported-mode`), no new crash before it.
