# Build 87 — real scatter mapping; the streaming fault is independent of it

Build 86 proved the ABI (`BOOL(pages, count, out)`) and returned FALSE, which
stopped the guest at its own `int3`. Build 87 implements the call for real
and re-runs the boot.

## What the guest actually asked for (dumped from the live call)

Boot #70, from the shim:

```
[xmem] XMemMapPhysicalPagesScatter(pages 00000081AACF2340, count 6720,
        out 00000081AACE5140) from 0000000140F2CC43:
        d[0]=0x1ee2bce0000 d[1]=0x1ee2bcf0000 d[6719]=0x1ee460d0000 out[0]=0x0
```

* `count = 6720`, descriptors are **64 KB apart** (`0x1ee2bce0000`,
  `0x1ee2bcf0000`, …, `0x1ee460d0000`): 6720 pages scattered one per
  16-page (64 KB) block across a 440 MB span.
* The second array is zero-filled scratch. After the call the guest only
  frees it (`0x140f2ccd8` -> `0x144d9c130`) and never reads it, so the call
  is fire-and-forget with the mapping placed at the supplied addresses.
* The span was already reserved by the layer arena
  (`state=2000 type=20000 allocBase=000001EA2BCE0000`, `reservedByLayer=0`),
  so nothing was re-reserved; each descriptor was committed inside it.

## Implementation

`HleXMemMapPhysicalPagesScatter` now maps exactly what the guest lists:
one `VirtualAlloc(va, 0x1000, MEM_COMMIT)` per descriptor, filling
`out[i]` with the committed address — the address actually obtained, not a
constant. If the span is free, it reserves it first; if any page fails to
commit it returns FALSE (no partial success). Every successful call records
`{base, span, pages, owned}` in a small table, and `XMemFreePhysicalPages`
releases those reservations exactly once (and logs when a span belongs to
the layer arena and must not be released).

## Boot #70 — the guest gets past 0x140F2CC47 and the fault is unchanged

```
2008:[xmem] XMemMapPhysicalPagesScatter: mapped 6720/6720 pages,
          out[0]=0x1ee2bce0000 out[last]=0x1ee460d0000
2009:[loader] veh: exception 0xc0000005 at 00000001428C9A08 thread 24208
2031:[loader] ctx-probe: fault=00000001428C9A08 rsi(ctx)=0x1f62d5fb780
          rcx(sub-object)=0x0 magic=0x6d736473
2032:[loader] ctx-probe: ctx=0x1f62d5fb780 owner=0x1478f5e80 sub[+0x18]=0x0
          arg8=2 argc=0 aux[+0x20]=0x0 aux[+0x28]=0x0
```

* No `int3` at `0x140F2CC4B`: the status check at `0x140F2CC47` passed with
  a real mapping behind it.
* `0x1428C9A08` is reached again with **`[record+0x18] = 0x0`** and
  `magic = 0x6d736473` ("sdms") — byte-identical to build 85's fault, with
  the same `arg8=2`, `argc=0`, `aux[+0x20]=aux[+0x28]=0`.

So the map call is not the selector and not the missing producer: giving
the guest a real 6720-page mapping changes nothing at the submit. The
difference relative to build 86 is exactly the return value
(`TRUE` instead of `FALSE`) and the consequence that execution continues
past `0x140F2CC4B`.

## Caveat: ft_hook is timing-sensitive on this machine

`build.bat` is green on era (21/21) and verify (exit 0), but the
fixed-timestep guest's `ft_hook` stage currently reports

```
[guest] ft_hook FAIL 5 frame events over 0.318 s: steps 15 (want 12 +/-2)
```

vs `0.256 s / 12 steps` in the 7/7 run earlier today (`build_ga87b.log`).
The stage measures 5 × (`Sleep(50)` + `WaitFrameEventX`); the tabulated
`timeBeginPeriod(1) -> 0` shows the 1 ms resolution is raised, and an
out-of-process probe measured `Sleep(50)` at 52.4 ms, so the extra ~13 ms
per frame comes from the frame-event wait landing a tick late, not from the
scatter change (this test never calls xmem). Recorded as a limitation
rather than papered over.
