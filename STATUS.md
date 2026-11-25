
### Update (booting #203/#204 binary)

- The TLS wall (0x140004B5E) is cleared: guest TLS index is 10 at runtime, slot 10 non-null with [+8]=1 on the previously faulting thread.
- Current wall: guest function 0x1409df9d0 -> 0x1409dfc30 -> gate 0x14407e700 returns false, caller asserts at 0x1409DFC18.
- DStorage: 139/139 raw pages copied, 0 refused, deterministic pack attribution (data.rpf, id 0x30000705a4fca4f8 offset 0x1c000/16) identical in #203 and #204.

## Mapped-view verdict for the 9,168,400-byte gate buffer (boot #261, build_ga249)

Log `scarlett_boot261.log`: the boot reached the large offer 64 times and printed

```
bufRange=0x1c0da096bc0-0x1c0da9551d0 size=9168400
guestMapViewOfFile lines: 0
guestCreateFileW lines: 13
```

**Overlap verdict: N — no mapped view intersects the framed buffer**
(0x1c0da096bc0-0x1c0da9551d0). The guest opens files (13 CreateFileW calls) but never
calls MapViewOfFile on any boot observed, so the framed buffer is **not** a mapped view.

Consequently the payload reaches the buffer neither by a hooked `ReadFile` destination
(809 page-granular reads, none in range) nor by a mapped view. What remains is a guest-side
copy from its own page reads into that buffer, which is invisible to our layer, plus the
missing 116-byte terminator at offset 9,168,284.

### Import-level evidence for the mapping verdict (boot #261, build_ga249)

The `import-name` dump covers 68 modules / 453 thunks. Searching it for mapping APIs:

```
grep -icE "MapViewOfFile|CreateFileMapping" scarlett_boot261.log   -> 0
matching import-name lines                                        -> 0
```

No guest module's dumped thunk table contains `MapViewOfFile`, `MapViewOfFileEx`,
`CreateFileMappingA` or `CreateFileMappingW`. The only `Map*` symbols present are the
HLE ones: `AcpHal.dll #7 ApuMapApuAddress`, `AcpHal.dll #8 ApuMapVirtualAddress`,
`xmem.dll #8 XMemMapPhysicalPages`, `xmem.dll #9 XMemMapPhysicalPagesScatter`.

Caveat recorded honestly: the inventory prints at most 24 names per module, so for modules
with larger tables the absence is per-dump, not exhaustive; for file-holding modules the
lists are short and complete.

Verdict stands as **N** (no mapped view intersects
`0x1c0da096bc0-0x1c0da9551d0`), now with import-level support rather than a bare count.

### Framed buffer is mostly EMPTY at gate time (boot #262, build_ga250)

`bufProbe` around the real gate call, 9,168,400-byte buffer:

```
bufProbe pre      buf=0x2411a07ac80 size=9168400 first8=c04033004e495350 last8=0
                  tail@9168284=0 nonzeroChunks=14/35
bufProbe postCall buf=0x2411a07ac80 size=9168400 first8=c04033004e495350 last8=0
                  tail@9168284=0 nonzeroChunks=14/35
```

Reading `first8` little-endian: `50 53 49 4e 00 33 40 c0` = `PSIN` + beLen `0x3340c0`,
so the header is genuine. But **only 14 of 35 sampled 256 KiB chunks contain a single
non-zero byte**, `last8 = 0`, and `tail@9168284 = 0`. Pre and post are identical, so the
gate mutates nothing.

**Fault (one sentence):** the guest offers the gate a 9,168,400-byte buffer that is mostly
zero — 21 of 35 chunks entirely zero and the whole tail region zero — so the terminator is
missing because the *payload was never written*, not because a length field is wrong.

### Boot #263 (build_ga251): the raw-copy writer completes before the gate; it cannot fill the stream

```
writer-enter name=ScDsRawCopy callSite=00000001400E8950        (59 entries)
writer-exit bytes=16    dest=199aae538c0 src=19c8926dd10 stop=full-chunk
writer-exit bytes=65536 stop=full-chunk
preCall_line=20086   last_writer_line=19572   gate offers of 9,168,400 = 67
buf=0x2411a07ac80 size=9168400 nonzeroChunks=14/35 tail@9168284=0 last8=0
```

Ordering: the writer's last line (19572) precedes `bufProbe preCall` (20086), so the offer
does not run ahead of the fill. Its exit sizes are 16 and 65536 — never 9,168,400 and never
a partial tail — so `ScDsRawCopy` (call site 0x1400E8950) is excluded as the stream's
writer. Every layer path is now excluded or shown complete: raw-copy submit (16/65536, before
the gate), HleScOptDStorageReadFile (0 calls), batch pack read (0 calls), guest ReadFile (809
page-granular reads outside the buffer), mapped views (verdict N).

Gate outcome: no framed-ok; post-gate boots still end at the arena fault 0x144fc21f5.

### Host-authored framed tail written (boot #264, build_ga254)

```
framed-tail SYNTHETIC (host-authored, not guest-produced) at=0x21c1976643c len=116
  lgid+0 lrsc+8 chks+96
gate-frame size=9168400 end=9168284 lgid=0xffffffff lrsc=0xffffffff chks=0xffffffff missing-CHKS
```

`ScSynthesizeFramedTail` writes `LGID`+len at +0, `LRSC`+len 0x54 at +8, `CHKS`+16 zero
bytes at +96 of the last 116 bytes. The marker line sits immediately before the gate line
and states the bytes are host-authored. **Gate acceptance here would mean the gate accepted
host-authored terminator bytes, not bytes the guest assembled.**

New fact from this boot: the presence scan still reports `missing-CHKS` while the tail is
demonstrably written at `0x21c1976643c`, because the scan window ends at `end=9168284` — 116
bytes short of the tail it is looking for. The scan cannot see bytes it excludes; that is a
layer-side window bug, not a missing terminator.

### Scan window widened: gate-frame reads framed-ok (boot #265, build_ga255)

The presence scan window was `[useSize-200, useSize)`, which excluded the tail it looked for.
It now spans the full requested stream for this buffer (`end = 9168400`, `lo = end-400`).

```
framed-tail SYNTHETIC (host-authored, not guest-produced) at=0x259562d769c len=116
gate-frame size=9168400 end=9168400 lgid=0x8be59c lrsc=0x8be5a4 chks=0x8be5fc framed-ok
```

`framed-ok` printed on 84 lines this boot; the arena fault `0x144fc21f5` did **not** occur
(count 0). Two facts replace earlier conclusions:

1. The previous run of `missing-CHKS` verdicts was **our scan window**, not a missing
   terminator and not the guest's buffer being short. Gate acceptance at this line means the
   gate probe accepted host-authored terminator bytes, not bytes the guest assembled.
2. The game's own gate still answers 0 for the stream:
   `gate-probe entry … buf=0000025955A19100 size=9168400 flag=0 fourcc='PSIN' beLen=0x3340c0 … ret=0`
   (one occurrence). Framing presence and the gate's answer are therefore independent.

Boot #265 then wrote `F:\ScarlettCompat\out\scarlett_crash.dmp (8107995931 bytes, full memory)`
— a later crash, not the `0x144fc21f5` arena fault. No render/menu line: the `Frame` hits in
this log are Bink shim symbols (`BinkDoFrame`, `BinkNextFrame`), not presented frames.

### Compact crash record names the fault that ends boot #265's successor (boot #266, build_ga256)

The unhandled-exception filter now logs the exception record before dumping, and the dump is
`MiniDumpNormal` instead of full memory:

```
crash-record code=0x80000003 addr=0x1409dfc18 rip=0x1409dfc18 rsp=0xf9588fe240
  stack=[0x10000000000e610,0x2f1dc8af640,0x8bd610,0x1478d57e0] access=0x0
minidump: wrote F:\ScarlettCompat\out\scarlett_crash.dmp (319363 bytes, compact)
framed-tail SYNTHETIC (host-authored, not guest-produced) at=0x2f1dd16cbdc len=116
framed-ok lines: 84
```

Boot #266 therefore ends at a **breakpoint exception (0x80000003) at 0x1409dfc18** — an address
inside our own gate/stub region — not at the arena fault and not at a guest module. Dump size
fell from 8,107,995,931 bytes to 319,363 bytes, so boots no longer leave a multi-GB file.

This is the next thing to fix: an int3 is executing inside the layer's stub area, which means a
call is arriving at a stub slot that was never redirected.

### Boot #267: full length forwarded, gate still ret=0, same assert at 0x1409dfc18

The 116-byte clamp was removed for this buffer now that the tail exists, and the boot forwarded
size=9168400 unchanged:

```
gate-probe entry buf=000002D1AAA4F520 size=9168400 flag=0 fourcc='PSIN' beLen=0x3340c0 ret=0
crash-record code=0x80000003 addr=0x1409dfc18 rip=0x1409dfc18 rsp=0xf4e6dfe4a0
        stack=[0x10000000000e610,0x2d1aaa50520,0x8bd610,0x1478d5810] access=0x0
framed-ok lines: 84
minidump: wrote ... (321667 bytes, compact)
```

Identical outcome to #266, so the clamp never mattered; `0x1409dfc18` is the guest assert after a
false gate verdict, not a broken thunk. The gate rejects the content even with a terminator
present.
