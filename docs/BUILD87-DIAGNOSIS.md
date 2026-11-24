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

## Build 88 — why `ft_hook` failed, and the fix

`ft_hook` (5 × `Sleep(50)` + `WaitFrameEventX`) was failing with
`0.318 s / 15 steps (want 12 ±2)`. The frame-event wait was not the
culprit: it logs `already past deadline (62.6 ms elapsed)`, i.e. the
guest's own 50 ms sleep was taking ~62.5 ms — one 15.625 ms tick late.

Measured cause, in this order:

1. `timeBeginPeriod(1)` returned 0 (success) yet `Sleep(20)` inside the
   layer's own thread took **31.2 ms** — the process timer stayed coarse.
2. `NtQueryTimerResolution` reported the current resolution as
   **15.625 ms** (fine 0.5 ms available).
3. Calling `timeBeginPeriod` through `GetProcAddress` changed nothing, so
   it was not the loader's import rewriting.
4. `NtSetTimerResolution(5000, TRUE, &got)` returned success but
   **granted only 156250 (15.625 ms)** — the OS was clamping the process.
5. The clamp was `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION`
   (0x4), which Windows applies to background processes. The layer was
   clearing only `PROCESS_POWER_THROTTLING_EXECUTION_SPEED`, so the
   timer-resolution policy stayed in force.

Fix (`HleSetFrameIntervalX`, once): clear **both** throttle bits with
`SetProcessInformation(ProcessPowerThrottling)` (`ControlMask =
EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION`, `StateMask = 0`), then
`NtSetTimerResolution(5000, TRUE, &got)`. Result:

```
[present] power throttling off -> ok=1 err=0
[present] NtSetTimerResolution(5000) -> 0x0, granted 5000 (0.500 ms)
[present] timer resolution now: status 0x0 coarse 15.625 ms fine 0.500 ms current 0.500 ms
[guest] ft_hook PASS 5 frame events over 0.252 s: steps 12 (want 12 +/-2), x=2.000
```

The logger was also made non-blocking (build 88): every line used to do
an unbuffered `fputs(stderr)` plus `fopen/fputs/fclose("scarlett.log")`,
which cost 10-15 ms under disk load and was charged to the guest because
`ScheduleFrameEventX` stamps the frame QPC before logging. Both streams
are now buffered and flushed on a 200 ms cadence, and `ScLogFlush()` is
called from the crash path before the minidump, so a fault still loses
nothing. `SetFrameIntervalX` now also logs the worst post-stamp cost of
`ScheduleFrameEventX` every 32 frames as a standing check that logging
stays out of the frame budget.

No test tolerance, step limit or stage was changed.

## Post-fix boot (green binaries)

```
[xmem  ] XMemMapPhysicalPagesScatter(pages 000000EFCEDF2500, count 6720, out 000000EFCEDE5300)
         from 0000000140F2CC43: d[0]=0x2f17bd10000 d[1]=0x2f17bd20000 d[6719]=0x2f196100000
[xmem  ] XMemMapPhysicalPagesScatter: target 000002F17BD10000 span 440340480 state=2000
         type=20000 allocBase=000002ED7BD10000 reservedByLayer=0
[xmem  ] XMemMapPhysicalPagesScatter: mapped 6720/6720 pages,
         out[0]=0x2f17bd10000 out[last]=0x2f196100000
[loader] ctx-probe: ctx=0x2ed0191b780 owner=0x1478f5e80 sub[+0x18]=0x0 arg8=2 argc=0
         aux[+0x20]=0x0 aux[+0x28]=0x0
[loader] guest crashed (veh-breakpoint): exception 0xc0000005 at 00000001428C9A08
```

No `int3` at `0x140F2CC4B`; the streaming fault is unchanged.
