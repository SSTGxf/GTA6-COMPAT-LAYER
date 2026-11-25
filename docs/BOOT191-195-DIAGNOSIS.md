# Boots #191–#195 — AcpHal object, APU arena, raw DStorage, PSIN wall

All five boots used the same harness (`out\scarlett_verify.exe`, 300 s budget,
`SCARLETT_NOPROBE=1`) against `D:\game\game\game_scarlett_master.exe`.

| boot | binary | lines | stop |
|------|--------|-------|------|
| #191 | AcpHalCreate publishes an object | 16,680 | `0xc0000005` write at `0x217820` (VCRUNTIME140+0x128a0) |
| #192 | + APU arena above the heap | 16,852 | `0x80000003` (assert) at `0x1409DFC18` |
| #193 | + DStorage return-address logging | 16,946 | same assert (different stream ids raced) |
| #194 | + raw requests inherit the last resolved pack | 16,931 | same assert |
| #195 | + guest stack walk on int3 | 16,933 | same assert, caller `0x141621CA0` |

Gates green on every build that produced these binaries: `BUILD_EXIT=0`, all
verdicts ALL PASS, `B1 result: 149 checks, 0 failed -> PASS`,
`=== verify exit code: 0 ===`.

## 1. AcpHalCreate must publish an object at `ctx+0` (resolved)

Guest evidence (boots #187–#190 disassembly): `0x1443AEDA5 mov rcx,[ctx]`,
`0x1443AEDB7 mov rax,[rcx]`, `0x1443AEDBA call [rax]` with
`(this, 0x1000, 0x1000)`, then `0x1443AEE1F/2F call [r10+0x10]` with
`(this, 2, 0xF0F0F0F0, 0xFFFFFFFF, &0xC0, 0)`. Both results are only
sign-checked. A context-reference scan over `0x149AE98B8..0x149AE9948`
(background `acp_scan.py`) added three more consumers:

- `0x140078509`: `mov rcx,[ctx]; mov r10,[rcx]; mov edx,0x12; …; call [r10+0x10]`
  — slot 2 is a **generic command** `(kind, r8d, r9d, struct*, 0)` whose
  register arguments are filler (`0xF1F1F1F1/0xFFFFFFFF`, `0xF6F6F6F6/…`) and
  whose return value the callers ignore. Kinds seen: `0x2` (init), `0x5`,
  `0x12`.
- `0x1443A73FF`: same slot 2, kind `0x5`, struct = `{esi}`.
- `0x1443AE845`-region teardown: reads `[ctx]`, `[ctx+8]` (the guest's own
  `CreateEventA` handle), `[ctx+0x10]` (status dword the guest polls with
  `Sleep(10)` while negative — it is zeroed before the poll, so a stub that
  never sets it negative is consistent), then clears `[ctx]`.

Implemented in `src/era_to_gdk.cpp`: `EraAcpHalObject` with a 16-slot vtable,
`slot0`/`slot2` logging `S_OK`, `EraAcpHalCreate` writing `&g_acpHalObject`
into `ctx+0`. Boot #191 log:

```
[era-acphal] AcpHalCreate(ctx=0000000149AE98B8, 0x20, 0xE0, 0xE0) -> S_OK object=… vtable=…
[era-acphal] AcpHalObject::slot0(this=…, 0x1000, 0x1000) -> S_OK
[era-acphal] AcpHalObject::slot2(this=…, 0x2, 0xf0f0f0f0, 0xffffffff, out=…, …) -> S_OK
```

The old stub returned `S_OK` without publishing, which is why boots #187/#188
died at `0x1443AEDB7` with `rcx=0`. `tests/guest_era.cpp` now asserts the
measured contract (slot must be non-null and word 0 must be a vtable).

## 2. ApuAlloc must serve requests larger than the declared heap (resolved)

Boot #191 stopped immediately after the ACP object with:

```
[era-acphal] ApuAlloc(out=0000000149AB7E60 size=0x1913afa0 align=0x800) -> E_OUTOFMEMORY (used=0x222e00 of 0x226e00)
[loader    ] veh: exception 0xc0000005 at … write at 0x217820
```

The title asks for **421 MB** from a heap it declared as 2.2 MB, then memsets
through the returned pointer. The allocator now keeps the guest's heap
`0x1C700000..0x1C922E00` and, for anything larger, commits a 64 KiB-aligned
arena contiguously above the heap, so every pointer stays inside the title's
APU VA neighbourhood:

```
ApuAlloc(out=…, size=0x1913afa0, align=0x800) -> S_OK arena block=000000001C930000
```

## 3. Raw DStorage requests (`compSize == destSize`) need pack attribution

Boot #192 refused 139 sequential 64 KiB reads (`id 0x307c02cc9eb90000`,
offsets `0x106609510 + i*0x10000`). `ScDsResolvePack` probes for a zlib stream
and can never resolve a raw page. Measured context: the eleven ids that did
resolve in these boots all resolved to `audio.rpf`, and the title streams one
pack at a time, so a raw request for an unknown id now inherits the pack most
recently resolved in the boot, is pinned per id, and is only used when that
pack covers `offset+size` (`ScDsContextPackForRaw`). Boot #194: **0 refused
requests**, all 139 pages copied, the title moved on to further streams
(`0x96b7afe9e2` at `0x2784d2000`).

## 4. Current wall: a PSIN blob that the fourcc dispatcher rejects

The assert is `int3 0x80000003` at `0x1409DFC18`, raised when
`0x1409DFC30` returns null, i.e. when the BE fourcc dispatcher `0x14407E700`
returns false. New instrumentation (boot #195) walks the faulting stack:

```
veh: exception 0x80000003 at 00000001409DFC18 thread 24096
[watch] stack: 0x14667d140,0x141621ca0
```

`0x141621CA0` is a "size → allocate → read whole blob → parse"
helper (`0x140048A50` size, `0x144F97130` new, `0x1451D2260` read,
`0x1409DF9D0` parse). The faulting registers were read back out of the
full-memory minidump's exception stream (`r14` = buffer, `rbx` = size):

```
buffer=0x221ce321a50 size=9168400 (0x8be610)
head: 5053494e 003340c0 5c000000 30d50600 aaaaaaaa aaaaaaaa 01010000 00002200 …
```

- It is a **PSIN** resource, 9,168,400 bytes, fully populated to the last byte
  (no `0xaa` holes except the 8-byte sentinels that are part of the data).
- It is not present verbatim in `data.rpf`, `shaders.rpf`, `common.rpf`,
  `packs.rpf`, `textures.rpf`, `script.rpf` or `appdata0.rpf`.
- `0x14407E700`'s PSIN arm (`0x14407EADF`) only records the buffer and falls
  into the chunk walker at `0x14407EB4A`, which reads `[fourcc][BE length]`
  records (`0x70707070` = "pppp" padding), bounds-checks each length against
  the buffer end, allows at most 99 chunks, and dispatches on the chunk fourcc.
  The reject path returns `edi = 0` at `0x14407EBB6`.

Next step: decide which chunk fourcc rejects the blob. The blob is saved at
`D:\scarlett-dumps\psin_blob-195.bin` (extracted from
`scarlett_crash_boot195.dmp`), and the extraction tools are kept in
`C:\Users\seths\AppData\Local\Temp\{dmpregs,dmpexc2,extract}.py`.

## Offline tooling added this session (not in the repo build)

- `dmpregs.py` — parses a full-memory minidump: stream directory, thread list,
  CONTEXT records, memory map (`Memory64ListStream`).
- `dmpexc2.py` — reads the exception stream's faulting CONTEXT and dumps the
  registers plus the memory at `r14`.
- `extract.py` — writes the register-referenced buffer out as a file.
- `findblob.py <hex> <packs…>` — chunked literal search across RPFs.

These made "what was the guest parsing when it asserted" answerable in one
boot instead of a manual dump hunt, and are the recommended first move on
every future assert.

## §5 The PSIN blob is structurally valid (rules out wrong-pack data)

Walked offline with `[fourcc][BE length]` at each offset, the buffer from boot
#195 is a **complete, exact chunk stream**:

| # | offset | fourcc | length | ends |
|---|--------|--------|--------|------|
| 0 | 0x0 | `PSIN` | 0x3340C0 (3,358,912) | 0x3340C0 |
| 1 | 0x3340C0 | `LSCH` | 0x4C8 | 0x334588 |
| 2 | 0x334588 | `LENM` | 0xB0 | 0x334638 |
| 3 | 0x334638 | `LSNS` | 0x589F64 (5,807,972) | 0x8BE59C |
| — | 0x8BE59C | zeros | — | 0x8BE610 (buffer end) |

Every length lands exactly on the next chunk and the stream terminates in the
buffer's own trailing zero padding, so the data is what the title expects.
That kills the "wrong pack / short read" hypotheses: `r14` (buffer) and `rbx`
(size 0x8BE610) hold a legitimate resource.

The header field at `+4` is a size, not a hash: valid PSIN blobs from
`data.rpf` (inflated offline, offsets 0x1C2767F0 → 472 B and 0x1AF39910 →
1008 B) carry `0x00000080` and `0x00000160` there, and ours carries
`0x003340C0` — the first chunk's length. The `0xaaaaaaaa` qwords seen in the
head are format sentinels, not unwritten memory (they occur in the known-good
blobs too).

All four fourccs are handled by the dispatcher (`PSIN` → `0x14407EADF`,
`LSNS` → `0x14407EB04`, `LENM` → `0x14407EAC8`, `LSCH` → `0x14407E...`), and
the chunk walker's bounds test passes for each, so the reject is **after** the
chunk walk, in the post-walk gate at `0x14407EC98`:

```
0x14407EC98: cmp qword ptr [rsi+0x90], 0   ; STRE/STRF stream chunk present?
0x14407ECA0: je  0x14407ECDB
0x14407ECDB: test r12,r12 / test rdi,rdi / test r14,r14
0x14407ECF0: je  0x14407F228               ; -> returns edi = 0 (reject)
```

`[rsi+0x90]` is only ever written by the `STRE`/`STRF` chunk arms
(`0x14407EA93`: `[rsi+0x90] = buf+8`, `[rsi+0x98] = len-8`), and this resource
has no such chunk. The other branch needs `r12`, `rdi` and `r14` (the values
the walker keeps for the chunk it just consumed) to be non-zero. So the next
question is exactly: which of `r12`/`rdi`/`r14` is zero at `0x14407ECDB`, and
which chunk arm was supposed to set it — the four candidate arms are
`0x14407EA93` (STRE/STRF), `0x14407EAAA`, `0x14407E968` (LSNS) and
`0x14407E9B4`.

## §6 What the 0x14407EC98 gate actually requires (from the arms)

The dispatcher's chunk arms write these slots in the parse struct (`rsi`):

| fourcc | arm | writes |
|---|---|---|
| `PSIN` (nested) | `0x14407F0B3` | `[rsi+0x20]=chunk`, `[rsi+0x28]=len`, `[rsp+0x30]=chunk`, `[rsp+0x48]=1` |
| `LSCH` | `0x14407EEA9` | `[rsi+0xB0]=chunk+8`, `[rsi+0xB8]=len-8` |
| `LENM` | `0x14407F09B` | `[rsi+0xC0]=chunk+8`, `[rsi+0xC8]=len-8` |
| `LSNS` | `0x14407F0E2` | `[rsi+0xD0]=chunk+8`, `[rsi+0xD8]=len-8` |
| `STRF` | `0x14407F05A` | `[rsi+0x90]=chunk+8`, `[rsi+0x98]=len-8` |
| **`PSCG`** | **`0x14407F02D`** | `[rsi+0x40]=chunk`, `[rsi+0x48]=len`, **`r12 = chunk`**, `[rsp+0x38]=1` |

`r12` is written **only** by the `PSCG` arm. `rdi`/`r14` are written only in the
`0x14407EBD2` block, which is entered only when `r11` (the walker's saved
previous-chunk pointer, `[rsp+0x50]`) is non-zero. The walk's natural end
(`0x14407F1BC: mov r10,r15; jmp 0x14407EB78`) comes back through that block with
`r11 = [rsp+0x50]`, which the top-level `PSIN` entry arm zeroed
(`0x14407EAEC xor r10d,r10d`), so for a resource whose **first** chunk is
`PSIN`, `r11 == 0` and control falls to `0x14407EC98`.

From there the required state is:

```
0x14407EC98: cmp qword ptr [rsi+0x90], 0   ; needs a STRF/VSTR chunk …
0x14407ECDB: test r12, r12                 ; … or a PSCG chunk (r12)
0x14407ECE4: test rdi, rdi                 ; … and rdi
0x14407ECED: test r14, r14                 ; … and r14
```

With `r14 = [rsp+0x30]` set by the `PSIN` arm (non-null), the two that are null
for a `PSIN`/`LSCH`/`LENM`/`LSNS` stream are **`r12` and `rdi`** — i.e. the
resource class this parse is being called for must carry a **`PSCG` chunk**
(`0x50534348`), and a resource without one is rejected with status 3 at
`0x14407F228`.

That is a property of the **data**, not of a struct field the layer can
initialise: nothing in the layer writes `rsi`'s chunk slots (they are filled by
the arms from the buffer the guest is parsing). So the fix has to be on the feed
side: either this resource is the wrong member of its container (the
`PSCG`-bearing sibling exists and was never served — the pack feed answered a
first-chunk-`PSIN` resource for a request whose consumer expects the
pipeline-carrying one), or the container has a sibling chunk the loader has not
requested yet.

Status: not cleared. The blob is complete and self-consistent (from chunk offsets)
but carries no `PSCG`/`STRF` chunk, so the guest's own parser is entitled to
reject it. The next concrete step is to identify the file the guest opened for
this parse (the helper `0x141621C30` reads via `0x1451D2260` after a size query
`0x140048A50`) and confirm which container member should carry `PSCG`, rather
than fabricating one.

## §7 File identification for the rejected parse (boot #195 dump, offline)

Recovered from the dump without a rebuild. The faulting frame's `rdi` is the
caller's parse context, and it is a stack struct whose bytes hold the resource
path (little-endian qwords at `0x9673DFE638+0x18`):

```
platform:/anim/clip_sets/runtime/clip_sets.zmt
s_secondary_motion
rpf
```

So the rejected buffer (`r14 = 0x221CE321A50`, `rbx = 0x9168400`) is a member of
**`clip_sets.zmt`**, the animation clip-set container reached through the
`platform:/` mount, and the container exposes at least the members
`s_secondary_motion` and `rpf`.

Data-side checks performed on disk (`D:\game\game`):

- `clip_sets.zmt` is not a file on disk; the `platform:/anim/...` path is served
  from the packs.
- The container is **not stored raw**: a full scan of `anim.rpf` (4.4 GB) and of
  `data.rpf`, `common.rpf`, `packs.rpf`, `textures.rpf`, `script.rpf`,
  `appdata0.rpf`, `navmeshes.rpf` finds neither `clip_sets` nor
  `s_secondary_motion`. Its bytes only exist after the layer's DStorage inflate,
  which is why the buffer's content is exact and self-consistent (see §5).
- The served member is a complete `PSIN`/`LSCH`/`LENM`/`LSNS` chunk stream that
  ends 116 bytes before the buffer end (zeros to `0x8BE610`), and it carries no
  `PSCG` and no `STRF` chunk — the two chunk types that satisfy the
  `0x14407EC98` gate (§6).

Consequence for the fix, stated as evidence rather than guess: the parse is for a
**clip-set member**, and the code path demands a chunk (`PSCG` → `r12`, or
`STRF` → `[rsi+0x90]`) that this member does not contain. The two candidates that
remain are (a) the member handed to this parse is not the one the descriptor
belongs to (the container has an `rpf` member and others), or (b) the member's
decompressed size was clipped at `0x9168400`, dropping trailing chunks.

Discriminating test (next step, cheap): log the DStorage request that produced a
`destSize` of `0x9168400` (id, offset, compSize, pack) and the inflated length
returned by the inflater. If `outLen > destSize`, the member was clipped and the
fix is in the inflate/descriptor path (serve the full length); if `outLen ==
destSize`, the member really ends there and the descriptor/member pairing is
wrong.

No reboot was spent on this pass: the finding is offline evidence only, so the
verified boot state is still #195/#196 (§ earlier) and the int3 at
`0x1409DFC18` is **not** cleared.

## §8 MEASURED: the PSIN int3 no longer fires (boots #197/#198)

`ScDsReadInflateInto` and the `DsqSubmit` inflate path now log any case where
the inflated stream is **longer** than the descriptor's `destSize`
(`inflate length mismatch: … destSize=… outLen=…`).

Boots #197 and #198 of that build (same binary):

| boot | lines | `inflate length mismatch` | stop |
|---|---|---|---|
| #197 | 17,060 | **0** | `0xc0000005` at `0x140004B5E`, `params[0]=0 params[1]=0` (read of address 0) |
| #198 | 16,842 | **0** | same: `0xc0000005` at `0x140004B5E` |

So the discriminating measurement answers itself: **`outLen == destSize` for every
request in the boot** (zero mismatches) — the member was *not* clipped, and the
`PSIN` parse now returns non-null. `0x80000003` at `0x1409DFC18` does not appear in
either log, and the fault moves past `0x14407ECF0`/`0x14407F228` to `0x140004B5E`.

`ctx-probe` at the new fault:

```
fault=0000000140004B5E rsi(ctx)=0x149a62470 rcx(sub-object)=0x149a62470
magic=0xffffffff00000000 registered=0x2050191b780
exception 0xc0000005 params[0]=0x0 params[1]=0x0 thread 11868
```

`params[0]=0` means a **read**, `params[1]=0` the address — the guest dereferenced
null at `0x140004B5E` (RVA `0x4B5E`, early `.text`), with its context object
`0x149A62470` and `magic=0xffffffff00000000`. That is the new frontier.

Caveat stated honestly: the only code delta between the boot #195/#196 binary and
this one is the mismatch logging, which changes no data flow, so the PSIN member
now being accepted is attributable to the DStorage pack resolution differing
between boots (the raw-request context heuristic is order-dependent), not to a
deterministic fix. Two consecutive boots now agree on the new stop, but the pack
attribution should be made deterministic rather than left to the last-resolved
pack ordering.
