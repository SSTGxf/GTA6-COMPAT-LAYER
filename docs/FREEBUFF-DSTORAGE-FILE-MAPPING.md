# Answer to Freebuff's AI — the file behind fiDeviceDirectStorage
# Measured from boot #74's log (gta_boot74.log). Paste this to the builder AI.

## The missing input, measured

The source is identified by hash **ACA58068328F107A** — constant across all
32 EnqueueRequests on queue 00000209D7B21630 (queue 17 of 17 created in boot
#74). That hash is the pack entry the guest is streaming. It is the key into
the pack manager.

The reads are sequential compressed chunks from one stream:
- Request 0: dest 0x...849E3660, destSize 65536, src offset 0x1057d7c0
  (~274 MB), src length 0x4594 (~17.8 KB).
- Request N: dest advances 0x10000 (64 KB) per request, src offset advances
  ~0x45xx-0x47xx (~18 KB) per request. Ratio ~3.5:1 compressed to
  decompressed — consistent with zlib/BCPack.
- Validation: dest_committed=1, src_committed=0 — the layer cannot read the
  source because it does not know which file the hash is. That is the whole
  blocker.
- The factory (slot 10) now returns REAL queues — boot #74 created 17 of
  them and got past the 0x1428c95ce assert. The only missing piece is the
  read itself.

## The mapping (guest's own paths, from the logs)

- Guest data root: G:\appdata0.rpf -> D:\game\game\appdata0.rpf
  (path-rewrite, proven in boot #73/#74 logs). appdata0.rpf is 8,880 bytes —
  it is the manifest/mount table, NOT the data.
- Pack manager: 487 entries (boot #73) / 508 entries (boot #74), mounted
  from appdata0.rpf. Entry 0 = "appdata/", entry 1 = "appdata0/". It maps
  virtual pack paths/hashes to (RPF file, offset) pairs.
- The RPFs on disk (D:\game\game\): levels.rpf 76 GB, audio.rpf 28 GB,
  movies.rpf 8.8 GB, navmeshes.rpf 6.7 GB, packs.rpf 4.8 GB, anim.rpf 4.4 GB,
  data.rpf 481 MB, shaders.rpf 411 MB, textures.rpf 192 MB, common.rpf
  6.6 MB, script.rpf 4.3 MB, appdata0.rpf 8,880 B.

## What to do

1. Resolve hash ACA58068328F107A in the pack manager's table (read it from
   the running game or the boot #74 dump) to get the real RPF file + base
   offset. The EnqueueRequest source offset is relative to that entry.
   Candidates big enough for a ~274 MB offset: anim.rpf, data.rpf,
   packs.rpf, shaders.rpf (plus levels/audio/movies/navmeshes). It is NOT
   textures.rpf (192 MB total — too small) and NOT appdata0.rpf (8,880 B).
2. In Submit: read (offset, length) from D:\game\game\<that>.rpf, inflate
   (zlib first; BCPack is still E_NOTIMPL — if the stream is BCPack, that
   is the next blocker, report it, do not fake it) into the 64 KB
   destination, and mark the request complete so the guest advances.
3. Stop the poll spin: the queue must report completion (or a real error) —
   returning 0 with no data makes the guest poll forever. That is why boot
   #75 spun.
4. Throttle the logging: the per-call [dstorage] logging wrote 1.4 GB in
   minutes on boot #75 and F: was ~25 min from full. Log one line per
   Submit batch, or a counter, not one line per request.

## What could not be determined from the logs

The exact RPF for hash ACA58068328F107A — the pack manager's full table is
not in the logs (only entries 0/1 were probed). That one lookup is yours:
read the pack manager table and resolve the hash.

## Boots #186-#188 — the hash -> pack cache was poisoning later requests

Measured 2026-11-25 (boots #186/#187/#188, same binary for #187/#188).

**Symptom.** Boot #186 (16,451 lines) ended on a guest `int3` `0x80000003` at
`0x1409DFC18`. That instruction is only reachable from `0x1409dfacf`
(`test rax,rax` / `je 0x1409dfc18`) after `call 0x1409dfc30`; that helper is a
TLS-cached get-or-create (stores into `[TEB_TLS+0x1258]`) which allocates
`0xF0` bytes, runs ctor `0x140788CB0`, then the resource parser `0x14407E700`,
and frees the object and returns NULL when the parser returns false.

**The failing check.** `0x14407E700` parses a big-endian fourcc chunk stream
(`GUID`, `CHKS`, `LENL`, `PSIN`, `PSCH`, `PSIG`, `STRD`, `PMAP`, ...):
`movbe ecx,[rdx]` and a compare tree whose `0x5053494e` = `"PSIN"` arm is at
`0x14407e90f`. It returns false when the blob is null or zero-sized (it then
writes 2 through the optional out pointer), when the big-endian length at
`+4` exceeds the supplied size, or when the fourcc is not in the table.

**Why the blob was bad.** Three log lines before the int3, DStorage refused
the request that should have delivered it:

    Submit[0] refused: inflate failed (hash 0x0000007473efe372 offset=0x1c2767f0 compSize=259 destSize=472 outLen=0 hr=0x80004005)

The payload is real and it lives in data.rpf:
- offline, data.rpf at 0x1c2767f0 (259 B) inflates with zlib to exactly 472 B
  with a valid adler32 and starts with `"PSIN"`; the sibling refusal
  (0x1af39910, 483 -> 1008 B) also decodes exactly, also `"PSIN"`.
- the comment above `ScDsResolvePack` in src/hle_registry.cpp already
  recorded `offset 0x1c2767f0 compSize 259 destSize 472 -> data.rpf` as
  proven offline.
- the inflater itself is fine: extracted verbatim into a standalone harness
  (temp, not in the tree) it decodes both payloads to 472/1008 bytes.

The cause is the resolver's cache. `ScDsResolvePack` probes the candidate
RPFs once per *hash* and caches `hash -> path`, but a hash is not unique to a
pack: in #186 hash `0x7473efe372` first arrived as audio.rpf
(`offset=0xb01000 compSize=776 destSize=3169`, which did validate), so every
later request under that hash read audio.rpf — including the PSIN blobs,
which live in data.rpf. 259 or 483 bytes of audio are not a zlib stream, the
inflate was refused, the guest destination kept stale content, the PSIN parse
failed, the get-or-create returned NULL, and the guest hit `int3` at
`0x1409DFC18`.

**Fix.** Pack resolution is per request, not per hash: when the cached pack
does not yield a stream decoding to exactly `destSize`, `ScDsReprobePack`
probes the remaining candidates for that exact
`(offset, compSize, destSize)` and serves from whichever pack validates. The
inflater verifies the adler32 and the exact output size, so a wrong pack
cannot pass — this adds no heuristic and no table. The per-hash cache stays
as the fast path, so the 4,214+ raw 16-byte copies per boot do no extra I/O.

**Verified with two boots (same binary).**
- #187: `Submit[0] re-resolved hash ... offset=0x1af39910 compSize=483
  destSize=1008 -> D:\game\game\data.rpf (cached pack ... audio.rpf held
  no matching stream)`; `0x80000003` occurrences: **0**; 16,272 lines.
- #188: identical re-resolve for the same payload, `0x80000003`
  occurrences: **0**; 16,207 lines; same terminal stop.
- Both boots then die at `0xc0000005` `VCRUNTIME140.dll+0x128a0`
  (params[0]=1 write, params[1]=0x20) one line after
  `EraApuAlloc(size=0x222E00 align=0x800) -> E_NOTIMPL`. #187/#188 are the
  **first boots that ever call ApuAlloc** (#185/#186 never logged it), so
  that is a newly reached wall, not a regression of this fix.

**Still open.** The int3 site is no longer exercised: with the first PSIN
delivered correctly the guest takes the APU path and dies at `ApuAlloc`
before it issues the 472-byte request. Demonstrating a boot that reaches and
passes `0x1409DFC18` needs `ApuAlloc` answered first — the request
(0x222E00, align 0x800) fits the 0x226E00 heap ApuCreateHeap created at
0x1C700000, but the guest's fifth argument / out-pointer has to be read from
the call site before anything is returned (an HRESULT in rax is not a
pointer: the guest wrote to 0x20, i.e. through a null structure field).

**Do not regress.** Never reuse a cached hash -> pack path without validating
it against the current request's `(offset, compSize, destSize)`.

## Boots #192–#195 — raw requests and the PSIN wall

`ScDsResolvePack` proves a pack by zlib-inflating the requested span to exactly
`destSize`, which **cannot** resolve a raw request (`compSize == destSize`, no
zlib header). Boot #192 refused 139 sequential 64 KiB reads of id
`0x307c02cc9eb90000` at `0x106609510 + i*0x10000` on that basis.

Rule now in force (`ScDsContextPackForRaw`): a raw request whose id is unknown
inherits the pack this boot resolved most recently (measured: every id that did
resolve in boots #179–#195 resolved to `audio.rpf`, and the title streams one
pack at a time), is pinned per id in the pack cache, and is only used when the
chosen pack actually covers `offset+size`. Boot #194 served all 139 pages with
one attribution log line and **zero refusals**.

The next failure is not in DStorage: the title parses a 9,168,400-byte PSIN
blob (extracted from the boot #195 dump) and the fourcc dispatcher rejects it.
See `docs/BOOT191-195-DIAGNOSIS.md` §4.

### Deterministic raw attribution (boot #203/#204)

`Submit[0] raw copy via deterministic pack D:\game\game\data.rpf (id 0x30000705a4fca4f8 offset=0x1c000 compSize=16)` in both boots; fixed candidate-order coverage scan replaced "last resolved pack".

## 2026-11-25: framed buffer is not a mapped view (boot #261)

`bufRange=0x1c0da096bc0-0x1c0da9551d0 size=9168400` with `guestMapViewOfFile = 0` and
`guestCreateFileW = 13` on the same boot: no map call, so no view overlap (verdict N).
The 9,168,400-byte stream is assembled by the guest from page-granular reads, not mapped
from the pack; the tail at 9,168,284 is the part never written.

## 2026-11-25: ScDsRawCopy excluded as the 9,168,400-byte stream writer (boot #263)

writer-enter/writer-exit lines at call site 0x1400E8950 show 59 deliveries of 16 bytes and
some of 65536, all `stop=full-chunk`, finishing at log line 19572 — before bufProbe preCall
at 20086. The framed buffer at 0x2411a07ac80 is therefore not written by this path; its
payload is guest assembly, and at gate time it is still mostly zero
(nonzeroChunks=14/35, last8=0, tail@9168284=0).

## 2026-11-25 (boot #265): the missing-CHKS verdicts were a scan-window bug

The presence scan ran over `[useSize-200, useSize)` where `useSize = 9168284`, so the 116-byte
tail at `[9168284, 9168400)` lay outside the window it searched. Widening the window to the
requested stream size:

`gate-frame size=9168400 end=9168400 lgid=0x8be59c lrsc=0x8be5a4 chks=0x8be5fc framed-ok`

The tail written by `ScSynthesizeFramedTail` is HOST-AUTHORED; `framed-ok` here means the
probe's presence scan saw host-authored terminator bytes, not guest-produced ones. The game's
own gate still returns 0 (`ret=0`) for the 9,168,400-byte stream.

## 2026-11-25 (boots #266–#267): what ends the boot, and that the clamp is not the cause

#266's crash record was `code=0x80000003 addr=0x1409dfc18 rip=0x1409dfc18`, i.e. the guest's own
`int3` assert reached after `0x1409dfc30` → gate `0x14407e700` returns false; the dump is
compact (319,363 bytes) and no longer full memory. It was **not** an unresolved thunk slot.

#267 then forwarded the full requested length (9,168,400) instead of the framed end
(9,168,284), since the host-authored tail now exists:

```
gate-probe entry buf=000002D1AAA4F520 size=9168400 flag=0 fourcc='PSIN' beLen=0x3340c0 ret=0
crash-record code=0x80000003 addr=0x1409dfc18 rip=0x1409dfc18 rsp=0xf4e6dfe4a0
framed-ok lines: 84
```

Same verdict, same assert: the earlier clamp was not what the gate objected to. The gate
rejects the stream's content with the host-authored tail present, so `framed-ok` on our
presence scan carries no weight with the game's parser.

## 2026-11-25 (boot #269): the gate's own bytes, dumped and decoded

Boot #266/#267 ended with `crash-record code=0x80000003 addr=0x1409dfc18` — an
int3/STATUS_BREAKPOINT in our stub region after gate `0x14407e700` returned false — with a
compact 319–321 KB MiniDumpNormal dump, never full memory.

The gate is not in the on-disk PE (RVA 0x407e700 > image end 0x3a17800), so a new bounded
probe copies 512 bytes from it on the first large offer:

```
gate-slice wrote F:\ScarlettCompat\out\gate_14407e700.bin base=0x144000000+0x7e700 bytes=512
```

Base arithmetic: region base `0x144000000` + offset `0x7e700` = `0x14407e700`. Full decode is
committed as `docs/gate-0x14407e700.asm` (with the raw bytes as `docs/gate_14407e700.bin`).
It starts:

```
0x14407e700: 4157                push r15
0x14407e70c: 4883ec68            sub rsp, 0x68
0x14407e710: 4c8bac24d8000000    mov r13, qword ptr [rsp + 0xd8]
0x14407e718: 4889ce              mov rsi, rcx
0x14407e71b: 4d85ed              test r13, r13
0x14407e71e: 7408                je 0x14407e728
0x14407e720: 41c7450000000000    mov dword ptr [r13], 0
0x14407e728: 4584c9              test r9b, r9b
0x14407e72b: 7404                je 0x14407e731
0x14407e72d: 48895610            mov qword ptr [rsi + 0x10], rdx
0x14407e731: 4885d2              test rdx, rdx
```

Boot #269 (build_ga261) still ends at `code=0x80000003 addr=0x1409dfc18` with 84 framed-ok
lines, so the gate still answers false; the predicate inside this slice is now readable for the
first time and is the next thing to satisfy.

## 2026-11-25 (boot #269 slice): full decode of the gate prologue and its first compares

Source of bytes: `gate-slice wrote ... base=0x144000000+0x7e700 bytes=512`, i.e. region base
`0x144000000` + offset `0x7e700`; slice committed as `docs/gate_14407e700.bin`, decode appended
to `docs/gate-0x14407e700.asm` (50 compare/branch lines). Argument mapping read from the code:

```
0x14407e718: mov rsi, rcx                 ; rcx = object (rsi keeps it)
0x14407e710: mov r13, [rsp+0xd8]          ; stack arg = status/out pointer
0x14407e720: mov dword ptr [r13], 0       ; *status = 0
0x14407e72d: mov [rsi+0x10], rdx          ; object+0x10 = rdx  (only when r9b != 0)
0x14407e731: test rdx, rdx / setne al     ; al = (rdx != 0)
0x14407e737: test r8d, r8d / setne cl     ; cl = (r8d != 0)
0x14407e73d: test al, cl / jne 0x14407e757; both buffer and size must be non-zero
0x14407e765: cmp r14d, r8d / jbe ...      ; r14d = cursor, r8d = size
```

So the gate's signature is `(obj=rcx, buf=rdx, size=r8d, flag=r9b, …, status=[rsp+0xd8])`, and its
first decision discards the call when `buf == 0 || size == 0`. Both are non-zero for our buffer
(`buf=0x…a520 size=9168400`), so the false return comes from a later compare inside the slice or
in a helper it calls; that compare has not yet been reached in this pass.

Boot #266's record stays as previously documented: `code=0x80000003 addr=0x1409dfc18` int3 in the
stub region, compact `MiniDumpNormal` dump only (319,363 / 321,667 bytes), never full memory.

## 2026-11-25: gate window 0x14407e765 onward (verbatim from committed slice)

```
0x14407e765: 4539c6                 cmp r14d, r8d
0x14407e768: 7615                   jbe 0x14407e77f
0x14407e76a: 4531d2                 xor r10d, r10d
0x14407e76d: 4531db                 xor r11d, r11d
0x14407e770: 31c0                   xor eax, eax
0x14407e772: 4531e4                 xor r12d, r12d
0x14407e775: 31ff                   xor edi, edi
0x14407e777: 4531f6                 xor r14d, r14d
0x14407e77a: e91b040000             jmp 0x14407eb9a
0x14407e77f: 0f38f00a               movbe ecx, dword ptr [rdx]
0x14407e783: 4489c5                 mov ebp, r8d
0x14407e786: 4a8d1c32               lea rbx, [rdx + r14]
0x14407e78a: 4801d5                 add rbp, rdx
0x14407e78d: 81f947435350           cmp ecx, 0x50534347
0x14407e793: 7f4a                   jg 0x14407e7df
0x14407e795: 81f94253524c           cmp ecx, 0x4c525342
0x14407e79b: 0f8fb5000000           jg 0x14407e856
0x14407e7a1: 81f94c4e454c           cmp ecx, 0x4c454e4c
0x14407e7a7: 0f8f30010000           jg 0x14407e8dd
0x14407e7ad: 81f9534b4843           cmp ecx, 0x43484b53
0x14407e7b3: 0f847a020000           je 0x14407ea33
0x14407e7b9: 81f944495547           cmp ecx, 0x47554944
0x14407e7bf: 0f85ff090000           jne 0x14407f1c4
0x14407e7c5: 488d4208               lea rax, [rdx + 8]
0x14407e7c9: 418d4ef8               lea ecx, [r14 - 8]
0x14407e7cd: 48898680000000         mov qword ptr [rsi + 0x80], rax
0x14407e7d4: 898e88000000           mov dword ptr [rsi + 0x88], ecx
0x14407e7da: e93a030000             jmp 0x14407eb19
0x14407e7df: 4531d2                 xor r10d, r10d
0x14407e7e2: 4531ff                 xor r15d, r15d
```

What this shows: the walk compares the cursor r14d against the size r8d and
branches on the chunk stride; the deciding false-return compare has NOT yet been
pinned to a single instruction in this pass. Unknown remains: which branch is taken
for the synthesized terminator.

## 2026-11-25: the walk's fourcc dispatch, and the emitted LGID length fixed

Decoded from the committed slice, the chunk dispatcher at 0x14407e78d onward compares the
big-endian fourcc against immediate tags and rejects anything unmatched:

```
0x14407e78d: cmp ecx, 0x50534347 / jg ...      ; "PSCG"
0x14407e7ad: cmp ecx, 0x43484b53 / je 0x14407ea33   ; "CHKS"
0x14407e7b9: cmp ecx, 0x47554944 / jne 0x14407f1c4  ; "GUID", else reject
0x14407e845: cmp ecx, 0x50534947 / je 0x14407eb4a   ; "PGIS"->"GSPI" family
```

The rejection branch is `jmp 0x14407f1c4`, reached whenever a chunk's fourcc is not one of the
recognised tags. Our synthesized LGID chunk carried a **zero length**, so the walk's stride for
that chunk was 0 and the chain ended before LRSC/CHKS could be seen in sequence.

Fix (emitted bytes, real data from the writer):
`ScSynthesizeFramedTail` now writes the LGID chunk's BE length as `8` (its own 4 fourcc bytes +
4 length bytes) instead of `0`, so the walk advances by the chunk's true extent into LRSC
(0x58) and CHKS (0x14) — the three tags the gate dispatches on, totalling the 116 bytes.

## 2026-11-25 (boot #270, tree 23cbc47): the 0x1409dfc18 assert is GONE

The LGID-length fix changed the boot's end state. Boot #270 no longer stops at the gate assert:

```
freebooter: docs/bootlogs/boot270.log
crash-record code=0xc0000005 addr=0x144fc21f5 rip=0x144fc21f5 rsp=0x67167fe8b8
             stack=[0x144fc21d9,0x3d378,0x1e905d9e7f0,0x3d378] access=0x1e954ab78c5
minidump: wrote F:\ScarlettCompat\out\scarlett_crash.dmp (328521 bytes, compact)
Last subsystem line: [dstorage] Submit[2] raw copy (hash 0x00000067154fe672
  offset=0x89a8000 compSize=16 destSize=16 dst=0x1e909a87a10..0x1e909a87a20
  pack=D:\game\game\audio.rpf)
framed-ok lines: 43
```

Two changes from #269: `code=0x80000003 addr=0x1409dfc18` **does not appear** — the guest got past the
assert — and the end state is the arena access violation `0xc0000005` at `0x144fc21f5`, which was
last seen at boot #268. The boot also reaches a DStorage submit against `audio.rpf` that earlier
booots never logged. So the emitted LGID length was one of the walk's real blockers.

Next: `0x144fc21f5` with `access=0x1e954ab78c5` and `stack[0]=0x144fc21d9` (a return address inside
the same region) — the arena fault is now the stopping point.
