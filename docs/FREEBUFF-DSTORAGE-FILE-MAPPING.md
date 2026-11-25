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
