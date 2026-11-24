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

## Build 89 — the producer of [record+0x18] is our own stub COM object

The streaming record constructor (`0x1428C9460`) ends with:

```
0x1428c95a6  add r9,0x18                 ; r9 = &record[+0x18]
0x1428c95ad  mov rcx,[global 0x1478F5E78] ; cached interface object
0x1428c95bc  mov qword ptr [rdx+0x18],1
0x1428c95c4  mov rax,[rcx]
0x1428c95c7  call qword ptr [rax+0x50]   ; slot 10
0x1428c95ca  test eax,eax / jns ok / int3
```

The earlier call in the same constructor (`0x1428c9522`, `[r10+0x28]`, slot 5)
stores its result at `+0x30`. So **slot 10 of the cached object is the
operation that must populate `[record+0x18]`**, with the out pointer as its
fifth argument — no other writer exists, which is why the DR0 watchpoint
never trapped.

What that object is, from the boot #71 dump:

```
global 0x1478F5E78 -> 0x000002ed7626c1e0
[0x2ed7626c1e0]    -> vtable 0x00007ffbcf6a6db0   (inside ScarlettCompat.dll)
vtable[0] = 0x7ffbcf66a150   QueryInterface
vtable[1] = 0x7ffbcf66a140   AddRef
vtable[2] = 0x7ffbcf66a160   Release
vtable[3..11] = 0x7ffbcf6577c0  (the same function in every slot)
```

Three real entries and one repeated handler is `g_stubComVtable` in
`src/hle_registry.cpp`: `MakeStubComObject()` hands the guest a fake COM
object for interfaces the layer does not implement (`IGameInput`,
`IDStorageFactory`), and **`StubComGeneric()` returns S_OK for every
method**. So the constructor's request to create the submission backend
into `+0x18` is answered with fake success and no write; the field stays
null, the submit at `0x1428C9A08` dereferences it, and the guest dies.

That is the "upload looked complete" divergence: the record was never
meant to be submitted with a hypothetical backend — it was submitted with
our stub's lie.

### What was tried and what it showed

A dedicated slot-10 handler was added to the texture computer's vtable
(returning E_NOTIMPL with a log naming the out pointer). Boot #72 shows it
is **not reached** on this path — no `computer slot 10` line appears and
the fault is unchanged:

```
[xmem  ] mapped 6720/6720 pages, out[0]=0x2137b220000 out[last]=0x21395610000
[loader] ctx-probe: ctx=0x20f0191b780 owner=0x1478f5e80 sub[+0x18]=0x0 arg8=2
[loader] guest crashed (veh-breakpoint): exception 0xc0000005 at 00000001428C9A08
```

So the constructor's cached object is the *stub COM* object, not the
texture computer, and the fix belongs in what `MakeStubComObject()` hands
out for the interface the streamer asks for:

* either the layer implements that interface for real (its slot 10 must
  return a backend object whose slot 3 accepts the submit request and
  whose slot 8 destroys it), or
* `StubComGeneric` must stop returning S_OK for methods whose contract is
  unknown, so the guest's own `jns`/`int3` at `0x1428c95ca` reports a named
  failure instead of dereferencing a null that our fake success created.

### The interface is identified: IDStorageFactory

Boot #71's stub-creation lines name both objects the layer handed out:

```
[stub] IGameInput         stub COM object 000002ED7626C280   (line 207)
[stub] IDStorageFactory    stub COM object 000002ED7626C1E0   (line 209)
```

and the global the record constructor reads holds **exactly the second
one**: `[0x1478F5E78] = 0x2ED7626C1E0`. So the guest cached the layer's
fake **IDStorageFactory** and uses its **slot 10** as the operation that
creates the submission backend for the streaming record — i.e. it is the
DStorage factory's queue-creation path, called with
`(this, desc*, guid*, void** out)` where `out = &record[+0x18]`.

Because `StubComGeneric` answers `S_OK` and writes nothing, the guest
believes it created a queue, stores the returned null, and later submits
through it at `0x1428C9A08` — the null dereference is directly caused by
that false success.

### The landed fix: a scoped, honest refusal at that one site

The stub layer now gives the `IDStorageFactory` object its **own vtable**
(`g_stubComVtableStorageFactory`, a copy of the shared one with slot 10
replaced). Slot 10 logs the call and returns `E_NOTIMPL` without touching
the out-parameter. Every other stub-backed interface (`IGameInput`,
`XAudio2`, everything else on `g_stubComVtable`) keeps `StubComGeneric`'s
behaviour untouched — this is a per-interface override, not a change of
the default. The factory object itself stays a valid non-null pointer, as
the registry note requires.

Boot #73 measures the result (binaries with the override, `BOOT_EXIT=1`):

```
[stub] IGameInput            stub COM object 000001DB9217D4B0 (vtable 00007FFBCF6A6DB0)
[stub] IDStorageFactory      stub COM object 000001DB9217BCB0 (vtable 00007FFBCF6A6FB0)
[stub] IDStorageFactory slot 10 (create upload backend)
       this=000001DB9217BCB0 desc=0000006F5C9FF960 iid=00000001462FC7F8
       out=000001E79964AE98 return-to=00000001428C95CA
       -> E_NOTIMPL (no DStorage queue contract in the layer; refused instead of faking success)
[loader] veh: exception 0x80000003 at 00000001428C95CE thread 9300
[loader] ctx-probe: ctx=0x1e79964ae80 owner=0x1478f5e80 sub[+0x18]=0x0 arg8=2 argc=0
[loader] ctx-probe: fault-kind=sub-object-null
[loader] guest crashed (veh-breakpoint): exception 0x80000003 at 00000001428C95CE
```

`out = 0x1E79964AE98` is exactly `ctx + 0x18` (`ctx = 0x1E79964AE80`), so
the refusal landed on the proven site. The exception is the guest's own
`int3` at `0x1428c95ce` — the failure branch of the check at
`0x1428c95ca` — and **there is no `0xc0000005` anywhere in the log**
(`grep -c 0xc0000005 gta_boot73.log` → 0). The streamer never receives a
backend it was told was valid.

The only refusal in the whole boot is that one line, and the two stub
objects carry different vtables (`…6DB0` vs `…6FB0`), which shows the
override is scoped to this interface. This boot stops before the
`XMemMapPhysicalPagesScatter` stage, so it has no 6720-page map line; the
last mapping in it is
`XMemMapPhysicalPages(pages 000001E8F1AE0000, count 99776, …) -> 000001EA776E0000`.

What this does **not** do: the streaming path does not complete. The
guest stops at an identified assertion instead of faulting on a null it
was told was valid. Making it proceed needs a real DStorage queue
(`IDStorageFactory` slot 10 must return a queue whose slot 3 accepts the
submit request and whose slot 8 closes it), which is a contract the layer
does not have.

### Why no real implementation is landed here (needs a decision)

The two compliant routes both need something this repository does not have:

1. **Implement the real interface.** Slot 10 must return a genuine DStorage
   queue object whose slot 3 accepts the submit request and whose slot 8
   closes it. The GDK `IDStorageFactory`/`IDStorageQueue` layout is not in
   the image (the guest only ever calls slot 10 once and slot 3/8 on the
   result), and building it would mean synthesising the interface the
   mission forbids. Our `opt_dstorage.cpp` implements file reads and
   decompression (`ScOpt_DStorage*`), not queues.
2. **Refuse at the factory.** Returning a failure from slot 10 is checked
   by the guest at `0x1428c95ca` (`test eax,eax / jns`), which trips its own
   `int3` at `0x1428c95ce`; and `DStorageGetFactory` itself cannot return
   null or a sentinel — the layer's own note (src/hle_registry.cpp, "Boot
   #9 / Leonida retail chain stop #5") records that the game faults when it
   does. Both outcomes are crashes, not clean refusals.

So the crash is fully explained and the responsible surface is named, but
a compliant fix requires the DStorage queue contract (or a decision to
accept the guest's `int3` as the refusal point). The dead texture-computer
slot-10 experiment was removed again; `src/xg_texture.cpp` matches the
state the green builds were produced from.

## Build inputs, HEAD equality and the final commit (HEAD `439b257`)

Enumerated from `build.bat` itself (every `%ROOT%` path it passes to `cl`,
`lib` or `/link`), then followed through every in-repo `#include "…"`:
**48 build inputs** — 21 `src/*.cpp|.h`, 18 `include/*.h`, 8 `tests/*.cpp`,
1 `defs/*.def`. For each, `git hash-object <path>` equals
`git rev-parse HEAD:<path>`:

```
build_inputs=48
by_dir={'defs': 1, 'include': 18, 'src': 21, 'tests': 8}
all_equal=True checked=48
$ git status --porcelain     -> (empty)
```

That is the evidence the previous note lacked. The `M` entries it explained
away were **line endings, not content**: `git diff --ignore-cr-at-eol` was
empty and the fix was to restore the files from HEAD (`git checkout -- .`),
not to commit a renormalisation. One file did carry real content drift:
**`build.bat`**, whose working copy passed `src\xg_texture.cpp` to `cl`
while the HEAD blob did not — so a checkout of the old HEAD could not
produce the green binaries even though the source was tracked. That
one-line difference (saved as
`D:\scarlett-dumps\buildbat-xg_texture-20261124.patch`) was committed.
The remaining untracked entries were prior-campaign boot drivers
(`build_ga*.bat`, `run_boot_ga*.bat`, `build_dump_read2.bat`) and three
older per-boot notes (`docs/BOOT6*.md`); they are not build inputs, so they
are excluded by pattern in `.gitignore` and left on disk untouched rather
than committed or deleted.

Green gate from HEAD `439b257`, both trees, pre-build identity check
`identical=yes` over `src include defs tests build.bat`:

```
local  build.bat -> 0   era 21/21   ft 7/7   verify 0
F:     build.bat -> 0   era 21/21   ft 7/7   verify 0
ft on F: x2 -> PASS 0.251 s / 12 steps, PASS 0.251 s / 12 steps
```

Commits (local only, not pushed): `8c3103e` scoped `IDStorageFactory`
slot-10 refusal + note + boot script; `934017b` `src/xg_texture.cpp`;
`cef5c2a` note update; `38c8c7b` `include/xg_texture.h` + note;
`a0029f4` `build.bat` (compile the tracked source); `439b257` `.gitignore`
(local scratch only). No dump, log, binary or extra boot script is in any
of them.

Still true and not claimed otherwise: the guest stops at its own `int3` at
`0x1428c95ce` rather than faulting on the null it was told was valid, and
the streaming path does not complete.

## Commit contents and the `src/xg_texture.cpp` tracked-ness question

`src/xg_texture.cpp` participates in the build (`build.bat` line 25 passes
`%ROOT%src\xg_texture.cpp` to `cl`) but was never in the repository:

```
$ git ls-files | grep -i texture          -> (no output, rc=1)
$ git log --oneline -- src/xg_texture.cpp -> (no output: never committed)
$ git check-ignore -v src/xg_texture.cpp  -> (no output, rc=1: not ignored)
$ ls src | wc -l  -> 21      $ git ls-files src | wc -l -> 20
```

The same check found one more build input in the same state:
`include/xg_texture.h` (18 headers, 17 tracked) is `#include`d by both
`src/xg_texture.cpp` and `src/hle_registry.cpp`, so it participates in the
build too.

So the green binaries were not reproducible from the commit: one compiled
source and one header it needs existed only on disk. Both are now
committed as they stood — the dead slot-10 experiment is not in the
source (`grep -c 'VtblCreateSubmitBackend'` → 0) and its content hash
equals the file the green build used. Commit `934017b` adds the source;
the header follows in the note commit, source only. Every untracked file
left in the tree is a boot script (`build_ga*.bat`), a doc, or a dump —
none is invoked by `build.bat`, and `defs/` (14/14) and all other
includes are tracked.

**The two trees had also drifted.** Comparing them for this check found
`F:\ScarlettCompat` carrying source differences that were never committed
and were not in this campaign's work: a `SCARLETT_LOG_PATH` log-routing
change in `hle_registry.cpp`, a diagnostic TLS-initialiser probe in
`pe_loader.cpp`, and harness report paths in `verify.cpp` (198 diff lines
total). Those F:-only versions are preserved as a patch at
`D:\scarlett-dumps\f-drift-20261124.patch`, and F: was then synced to the
committed sources. Every file `git ls-files src` lists now has an
identical MD5 in both trees (`pre_build_src_identical=yes`), and both were
rebuilt from them:

```
local build.bat -> 0   era 21/21   ft 7/7   verify 0
F:    build.bat -> 0   era 21/21   ft 7/7   verify 0
ft on F: x2 -> PASS 0.252 s / 12 steps, PASS 0.251 s / 12 steps
```

Delivered commits (local only, not pushed): `8c3103e` — the scoped
`IDStorageFactory` slot-10 refusal, the note and the boot script;
`934017b` — `src/xg_texture.cpp`, the compiled source that was missing
from the repository. `git status --porcelain src/xg_texture.cpp` is now
empty. The only remaining `src/` entries git reports as modified are
CRLF-only differences (`git diff --ignore-cr-at-eol` is empty for them,
with `core.autocrlf=true`), so their content equals `HEAD`.

Note: `build.bat` itself and `defs/*` are tracked but carry pre-existing
modifications from before this campaign and are deliberately not swept
into these commits.

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
