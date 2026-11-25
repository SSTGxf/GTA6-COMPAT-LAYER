
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
