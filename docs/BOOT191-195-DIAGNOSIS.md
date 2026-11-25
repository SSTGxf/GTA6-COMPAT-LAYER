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

## §9 Disassembly of the new stop: a thread with no TLS array (boots #197/#198)

Fault PC `0x140004B5E`, exception `0xc0000005`, `params[0]=0` (read),
`params[1]=0` (address 0), from `scarlett_crash_boot198.dmp`:

```
0x140004B40: push r15/r14/rsi/rdi/rbp/rbx; sub rsp,0x28
0x140004B4C: mov  ebp, [rcx+0x1c]
0x140004B4F: mov  rbx, [rcx]              ; lock word of the object
0x140004B52: mov  rax, qword ptr gs:[0x58] ; TEB->ThreadLocalStoragePointer
0x140004B5B: mov  rsi, rcx
0x140004B5E: mov  rax, qword ptr [rax]     ; <-- FAULT, rax was 0
0x140004B61: mov  edi, [rax+0x1454]        ; CRT per-thread field
```

Registers at the fault: `Rcx = Rsi = 0x149A62470` (the guest global object the
caller passes with `lea rcx,[rip+...]`), `Rbx = 0xffffffff00000000` (the object's
lock word, i.e. unlocked), `Rbp = 0x3e8`, `Rip = 0x140004B5E`. Caller frame
`0x14185A884`, in the function at `0x14185A870`:

```
0x14185A870: push rsi; sub rsp,0x20; mov rsi,rcx
0x14185A878: lea  rcx, [rip+0x8207bf1]   ; -> 0x149A62470
0x14185A87F: call 0x140004B40            ; lock/enter
0x14185A884: mov  rax, [rsi+8] ...       ; caller frame
```

So `0x140004B40` is a lock helper that, besides the object's lock word, reads the
**current thread's TLS array** and then TLS slot 0 (`[rax]`) and a CRT field off it
(`+0x1454`). `gs:[0x58]` is zero on this thread, i.e. the thread has **no TLS
array at all**, which is exactly the documented loader gap: threads created
through paths the loader does not hook never get guest TLS.

Provenance of the null: it is **not** game data and not an HLE export. It is
missing OS-thread state (`TEB->ThreadLocalStoragePointer == 0`) on a thread that
is already executing guest code. On a console/GDK every thread that runs guest
code has a CRT-initialised TLS array, so this is the loader's responsibility, not
the title's.

Why the obvious patch is not the fix: allocating an empty TLS array is not enough,
because the very next access is `[rax]` — TLS **slot 0**, which the guest's CRT
itself owns (its per-thread data). A blank array just moves the fault to
`[0+0x1454]`. The correct fix is to run the guest CRT's per-thread initialisation
for this thread (i.e. give it the same slot-0 block and thread id the CRT expects),
or to stop the thread from running guest code without it — whichever the evidence
for that specific thread shows.

Next concrete step (evidence, not guess): the faulting thread id is 11868 (boot
#197) / 27548 (boot #198); `scarlett_crash_boot198.dmp` has 64 thread contexts, so
identify which creation path produced that thread (its stack base and the frames
under `0x14185A870`) and initialise slot 0 for it the way the guest CRT's own
thread wrapper does, instead of inventing a value.

Status: no fix landed for this wall in this pass, and no reboot was spent —
the two boots that agree on it are #197 and #198.

## §10 Boots #199/#200 — TLS bootstrap does not fire yet; pack attribution made deterministic

Measured, not inferred:

| build | BUILD_EXIT | FAILED | B1 | verify | boot | stop |
|---|---|---|---|---|---|---|
| #199 | 0 | 0 | 149/149 PASS | 0 | #199 | `0xc0000005` at `0x140004B5E` (16,938 lines) |
| #200 | 0 | 0 | 149/149 PASS | 0 | #200 | `0xc0000005` at `0x140004B5E` (16,962 lines) |

`thread-attach: bootstrapping …` count = 0 in both; `raw copy via deterministic
pack …` count = 1 in #200. So the TLS bootstrap code exists but never executes,
while the deterministic attribution path does execute.

Why the callback does not fire, measured from the built module: the DLL **does**
have a TLS directory (`RVA 0x51B80`, 0x28 bytes) with a **non-null**
`AddressOfCallBacks = 0x180038678`, so Windows does have a callback list to walk.
The remaining explanation is ordering, not linkage: the thread that faults was
created before the guest image's TLS directory had been parsed, so when the
callback ran `g_guestTlsReady` was 0 and it returned without bootstrapping — and
nothing runs it again for that thread.

Landed changes in this build:

- `src/pe_loader.cpp`: `ScGuestTlsThreadAttach/Detach` reuse the existing
  `EnsureThreadTls()` (PE TLS template, `SizeOfZeroFill`, CRT index, `.CRT$XLB`
  callback list) from the DLL's own `.CRT$XLB` entry, guarded by
  `g_guestTlsReady` (set when the guest TLS directory is parsed) and a
  re-entrancy flag.
- `src/hle_registry.cpp`: `ScDsPackByCoverage` — raw descriptors resolve by a
  fixed scan of `kDsPackCandidates` in declared order, first pack that covers
  `offset+size`; the "last resolved pack" heuristic is gone, so attribution no
  longer depends on descriptor arrival order.

Next concrete fix (from the measured ordering): bootstrap lazily per thread on
the first layer entry rather than only at thread attach — i.e. call
`ScGuestTlsThreadAttach()` from the HLE dispatch path (and from the guest's
thread-start trampolines), which covers threads that were already running when
`g_guestTlsReady` became 1, including the one that dies at `0x140004B5E`.

## §11 Boot #201 — attach callback measured; the missing per-thread state is the /GA view, not the TLS array

Measured (boot #201, DLL from `build_ga201`): `thread-attach: callback tid=... ready=1`
appears **63 times, all `ready=1`**. So last turn's inference was wrong: the DLL
TLS callback *does* run, and it runs after the guest TLS directory is parsed. It
simply never had anything to do — every one of those threads already carried our
TLS block.

The fault is therefore not the TLS array. The guest's `mov rax,gs:[58h]` is
rewritten by `PatchGaTlsReads` to read `TlsSlots[gaSlot]` (the per-thread /GA
view at TEB+0x1480+8*slot), so `rax` came from a **missing /GA view**, while
`arr[g_tlsInfo.index]` was set — which is exactly why the VEH bootstrap probe
(`arr[index]`) found "already initialised" and never fired. Boot #201: 0
`guest TLS bootstrap on faulting thread` lines, still `veh: exception
0xc0000005 at 0x140004B5E thread 11124`.

Fix applied after #201: the VEH probe now tests `TlsSlots()[gaSlot]` (the view the
guest actually dereferences) and bootstraps through `EnsureThreadTls()` when it
is absent, so the guest CRT's own `.CRT$XLB` callbacks run for that thread and
slot 0 stays the CRT's.

Unverified: `build_ga202` reports `guest_minimal FAILED` (BUILD_EXIT=1) before the
verify stage — `B1 result: 149 checks, 0 failed -> PASS` still prints. That gate
must be green again before the VEH fix can be booted and claimed.

## §12 Boots #202–#204 — the TLS wall is cleared; the boot now stops at the 0x1409DFC18 int3

### guest_minimal regression (build_ga201/202) and its real cause

`scarlett_verify.exe` on `guest_minimal.exe` reported every stage PASS with
`child_exit=0xc0000005`. Cause (measured): the VEH TLS bootstrap returned
`EXCEPTION_CONTINUE_EXECUTION` for **any** null-read AV whose thread lacked a
/GA view, so unrelated genuine faults were retried instead of propagating. Fix:
one attempt per thread, only for a null read (`ExceptionInformation[1] == 0`),
and the /GA view is re-checked before retrying. `build_ga202`/`ga203`:
`BUILD_EXIT=0`, `B1 result: 149 checks, 0 failed -> PASS`,
`=== verify exit code: 0 ===` (guest_minimal passes again).

### Boot #202 — the bootstrap fired, and the measurement that fixed it

```
veh: guest TLS bootstrap on faulting thread 23788 rip=0x140004b5e
     tlsArray=000002427BC36060 ourSlot=0000000000000000 slot0=000002427B9C21A0 -> retry once
veh: exception 0xc0000005 at 0000000140004B5E thread 23788
```

Measured: the TLS **array** existed, the guest CRT's slot 0 existed
(`0x2427B9C21A0`), and the **/GA view was null** (`ourSlot=0`) — the patched
`mov rax,gs:[58h]` reads `TlsSlots[gaSlot]`, so a null view is what produced
`rax=0`. The attach path had early-returned because `arr[index]` was non-null.
Fix: when the block exists but the view does not, reinstall the view over the
**same** block (`InstallGaView(blk, index)`) — no second allocation, no second
CRT init, slot 0 untouched.

### Boots #203/#204 — past the TLS wall, agreeing

| boot | lines | `0x140004B5E` faults | `reinstalled /GA view` | DStorage | stop |
|---|---|---|---|---|---|
| #203 | 18,045 | 0 | 64 | 139/139 copied, 0 refused | `0x80000003` at `0x1409DFC18` |
| #204 | 17,538 | 0 | 63 | 139/139 copied, 0 refused | `0x80000003` at `0x1409DFC18` |

Both logs show `inflate length mismatch` = 0 and get well past `0x14407ECF0`
(the PSIN accept path) before stopping. Identical descriptor, identical pack in
both boots:

```
#203 Submit[0] raw copy via deterministic pack D:\game\game\data.rpf (id 0x30000705a4fca4f8 ... offset=0x1c000 compSize=16 ...)
#204 Submit[0] raw copy via deterministic pack D:\game\game\data.rpf (id 0x30000705a4fca4f8 ... offset=0x1c000 compSize=16 ...)
```

### The remaining wall: 0x1409DFC18

Disassembly (measured) around the stop:

```
0x1409dfbee: mov  eax, dword ptr [rip + 0x6428f38]  ; guest TlsAlloc index
0x1409dfbf4: mov  rcx, qword ptr gs:[0x58]
0x1409dfbfd: mov  rax, qword ptr [rcx + rax*8]      ; guest per-thread obj
0x1409dfc01: cmp  byte ptr [rax + 8], 0
0x1409dfc08: jne  0x1409dfa95
0x1409dfc0e: call 0x1455db994                        ; CRT assert/report path
0x1409dfc13: jmp  0x1409dfa95
0x1409dfc18: int3                                    ; <- stop
```

The `int3` is a landing pad after the assert-report call, not the check itself;
the checked condition is `byte [guestPerThreadObj + 8] != 0` for the guest TLS
object. Not patched (forbidden), and no return value is forced: the next step is
to establish which guest TLS index/object is being tested and why its `+8` byte
is zero — i.e. which guest subsystem's per-thread state the current host thread
never created.

## §13 The 0x1409DFC18 stop — call chain and the reporting thunk (measured)

From boot #203's log (the VEH already walks the stack on `int3`):

```
veh: exception 0x80000003 at 00000001409DFC18 thread 11640
[watch] stack: 0x14667d140, 0x141621ca0
```

`0x141621ca0` is the return address of `0x141621c9b call 0x1409df9d0`, so the
stop is inside the guest function that starts at `0x1409df9d0`, entered from
`0x141621c9b`. The reporting path inside it is a thunk:

```
0x1409dfc0e: call 0x1455db994
0x1455db994: xor r8d,r8d; xor ecx,ecx; edx=2; jmp 0x1455dd4f4
0x1455dd4f4: cmp edx,2; je ...
0x1455dd509: mov rax, qword ptr gs:[0x58]
0x1455dd503: mov ecx, dword ptr [rip + 0x182b623]   ; -> 0x146e08b2c
0x1455dd518: mov rdx, qword ptr [rax + rcx*8]        ; guest per-thread object
0x1455dd51c: cmp byte ptr [rdx + 8], 1
0x1455dd521: je  0x1455dd54f
0x1455dd523: mov byte ptr [rdx + 8], 1               ; "already reported" latch
```

So the same `[obj+8]` byte is both the condition at `0x1409dfc01` and a
report-once latch inside the reporting function.

`0x146e08b2c` in the image file holds `e0 d3 07 00 | 3b d4 07 00`, i.e.
`0x0007d3e0` / `0x0007d43b` — not valid TLS indices, so that dword is a
**relocation/initialiser placeholder in the file**; the real runtime index has
to be read from a live process/dump rather than the image. The mount-probe in
the same boot prints `tlsArray=… slot0=… slot10=… view=… view0=…` populated, so
the guest is using low indices at runtime, not 0x7d3e0.

Not done this pass and explicitly not claimed: no fix landed for
`0x1409DFC18`, no new binary was booted, and the assert text was not recovered.
The int3 was not patched and no value was forced.

## §14 0x1409DFC18 is NOT the TLS check — runtime index and object measured from the dump

Read from `D:\scarlett-dumps\scarlett_crash_boot203.dmp` (Memory64List, live values,
not the image file):

```
runtime [0x146E08B2C] = 10 (0xa)          <- guest TLS index, low index as expected
faulting tid=28276 teb=0x5ca8061000
  TEB+0x58 tlsArray   = 0x1adfcb51230
  slot[10]            = 0x19f7a0fd8f0     <- NON-NULL
    [obj+8]           = 0x01              <- flag SET
    [obj+0x10]        = 0x19f7a0fd910
  slot[0]             = 0x19f7a09c790     <- guest CRT block, non-null
exception: code=0x80000003 rip=0x1409dfc18 tid=28276 nparam=1 params=[0x0]
context:   rax=0x0 rcx=0x1471c27c0 rdx=0x6e7400000001 r8=0xffffffff00000000
```

Therefore the TLS condition at `0x1409dfc01 cmp byte [rax+8],0` **passes** on the
faulting thread and the boot does not stop for missing per-thread TLS state.

Who reaches the pad (disassembled from the caller):

```
0x1409dfac4: call 0x1409dfc30
0x1409dfac9: mov  r12, rax
0x1409dfacc: test rax, rax
0x1409dfacf: je   0x1409dfc18      ; <- rax == 0 lands on the int3
```

and inside `0x1409dfc30` the real work is:

```
0x1409dfc5d: mov  ecx, 0xf0
0x1409dfc62: call 0x144f97130      ; allocate 0xF0-byte object
0x1409dfcc1: call 0x14407e700      ; resource gate (same family as 0x14407ECF0)
0x1409dfcc6: test al, al
0x1409dfcc8: je   0x1409dfd04      ; gate returned false -> path returns 0
```

So the assert is a **null result from a resource open driven by the gate at
0x14407e700**, i.e. the same resource/parse family as the PSIN gate, not TLS.

Measured: the TLS object exists with `[+8]=1`; the gate call exists on this path.
Inference (not proven): `0x14407e700` returns false for this resource and the
caller treats null as a hard assert. Not patched, nothing forced, no value
synthesised. No rebuild and no boot were run in this pass.

## §15 What 0x14407e700 accepts, and what was live on the stack at the stop (measured)

Chunk fourccs compared by the gate (BE dwords at [buf+0], `movbe ecx,[rdx]`):

`PSCG LRSB LENL CHKS GUID STFM PSIM PSCH PSIG LSNR LRSC LSCH STRD STFN STNS
LENM LGID PSIN STND LSNS PMAP STRE STRF`

So every chunk of the clip-sets PSIN stream (`PSIN`, `LSCH`, `LENM`, `LSNS`) is
handled by this parser; the rejection is not "unknown fourcc".

First rejection test inside the gate:

```
0x14407e731: test rdx,rdx ; setne al
0x14407e737: test r8d,r8d ; setne cl
0x14407e73d: test al,cl  ; jne 0x14407e757
0x14407e741: (null buffer or size) status=2 -> return false
0x14407e757: movbe r14d, dword ptr [rdx + 4]   ; chunk BE length
0x14407e765: cmp   r14d, r8d
0x14407e768: jbe   0x14407e77f                  ; length <= size -> parse
             else: return false (rax=0)
```

Live objects found on the faulting thread's stack at the stop (dump scan):

```
stack+0x0d0 -> 0x19f0ae655c0  fourcc='PSIN'  head=5053494e003340c0 5c000000 30d50600
stack+0x200 -> 0x19f07253930  "platform:/anim/jlimits.r..."
stack+0x230 -> 0x19f073777d0  "platform:/anim/clip_sets..."
stack+0x2e8 -> 0x19f0724c300  "audio:/config/speechanim..."
```

Measured: the PSIN buffer is live in this frame and its first chunk is
`PSIN` with BE length `0x003340C0`; `platform:/anim/jlimits.rpf` is a *new*
resource path present here that earlier boots never reached.

Not measured yet (requires the gate call's actual rdx/size at 0x1409dfcc1): which
descriptor was passed and which of the gate's later conditions failed. No fix
landed, no rebuild, no boot in this pass; nothing patched or forced.

## §16 The gate call captured live (boots #205/#206) — the gate itself rejects the PSIN buffer

Instrumentation landed: the 5-byte `call rel32` at `0x1409dfcc1` is redirected to a
stub (`jmp [rip+0]`, then the absolute address of the wrapper) allocated inside
the guest address range, and the wrapper calls the **real** gate with the same
arguments and returns its value unchanged. The gate is not modified, not
short-circuited and not weakened.

Live capture of the failing call (boot #205, 171 `gate-probe` lines in total):

```
gate-probe entry  tid=25660 obj=000002714401DB30 buf=00000271495DFE60 size=9168400
                  flag=0 fourcc='PSIN' (0x5053494e) beLen=0x3340c0 ret=0 al=0
gate-probe return tid=25660 obj=000002714401DB30 buf=00000271495DFE60 size=9168400
                  flag=0 fourcc='PSIN' (0x5053494e) beLen=0x3340c0 ret=0 al=0
```

and with the out-parameter printed (boot #206):

```
gate-probe entry tid=24192 obj=000001B4BC20D9F0 buf=000001B4BDBCD580 size=9168400
                 flag=0 fourcc='PSIN' (0x5053494e) beLen=0x3340c0
                 a5=0000006407DFDDC4 a6=0000000000000000 status=?4294967295 ret=0 al=0
```

Facts this establishes:

- the buffer handed to the gate is the full 9,168,400-byte PSIN stream;
- its first chunk is `PSIN` with BE length `0x3340C0`, which is `<=` the buffer
  size, so the size guard at `0x14407e765` passes;
- the fourcc is one the gate accepts, so the fourcc dispatch is not the cause;
- the 6th argument (the status out-pointer at `[rsp+0xd8]` in the gate) is NULL
  for this call, so the gate reports nothing through it;
- earlier calls on the same path with `size=1640` (`beLen=0x170`) and `size=512`
  (`beLen=0x80`) return 1 — only the full-stream call returns 0.

So the rejection is a later internal guard of the PSIN parser, not the header,
the size or the fourcc. Not yet measured: which internal guard (the parser's
chunk-walk is the next probe target). No fix landed this pass, no int3 patch, no
forced return, gate untouched.

## §17 The failing guard is named: the LSNS chunk overruns the assembled stream by 908 bytes

Boot #207 (probe build `build_ga207`: `BUILD_EXIT=0`, `B1 result: 149 checks,
0 failed -> PASS`, `=== verify exit code: 0 ===`, 0 FAILED) logs the gate's own
view of each buffer it is handed:

```
gate-probe walk size=512    chunks: [0]PSIN(len=0x80) [128]LSCH(len=0xa8) [296]LENM(len=0x40) [360]LSNS(len=0x24) [396]LGID(len=0x8) [404]LRSC(len=0x58) [492]CHKS(len=0x14)
gate-probe walk size=1640   chunks: [0]PSIN(len=0x170) [368]LSCH(len=0x428) [1432]LENM(len=0x8) [1440]LSNS(len=0x54) [1524]LGID(len=0x8) [1532]LRSC(len=0x58) [1620]CHKS(len=0x14)
gate-probe walk size=9168400 chunks: [0]PSIN(len=0x3340c0) [3358912]LSCH(len=0x4c8) [3360136]LENM(len=0xb0) [3360312]LSNS(len=0x589f64) [9168284]<garbage>(0x56d61794,len=0xb4e073cd)
gate-probe entry  tid=21436 ... size=9168400 fourcc='PSIN' beLen=0x3340c0 ... ret=0 al=0
veh: exception 0x80000003 at 00000001409DFC18 thread 21436
```

The small streams are complete: every chunk fits exactly inside the buffer and
they return 1. The 9,168,400-byte stream is **one chunk short**:

- `PSIN` len `0x3340C0` = 3,358,912 → next chunk at 3,358,912 (matches LSCH)
- `LSCH` len `0x4C8` = 1,224 → 3,360,136 (matches LENM)
- `LENM` len `0xB0` = 176 → 3,360,312 (matches LSNS)
- `LSNS` len `0x589F64` = 5,808,996 → would end at **9,169,308**, but the buffer
  is **9,168,400** bytes: **908 bytes short**
- only `9,168,400 − 3,360,312 = 5,808,088` bytes remain for LSNS

The parser therefore walks past the end of the buffer, reads a garbage fourcc at
9,168,284 (`0x56d61794` with a nonsense length `0xb4e073cd`), rejects it, returns
0, and the caller asserts at `0x1409DFC18`.

Measured cause: the assembled PSIN stream handed to the gate is **908 bytes too
short for the LSNS chunk it declares** — i.e. the tail of the stream was never
copied into the buffer (the archived blob in D:\scarlett-dumps is zero-padded to
its declared 9,168,400 bytes). Inference (not proven): the shortfall comes from
the final DStorage page (`Submit[138] ... compSize=58896`) or from the guest's
size accounting, rather than from the parser.

No fix landed this pass; the gate is byte-identical, no int3 patch, no forced
return. Fix target: make the loader supply the full declared chunk length when it
assembles the stream, using the same path as clip_sets.zmt.

### §17 correction (arithmetic error in the paragraph above)

`0x589F64` is **5,807,972**, not 5,808,996. With `LSNS` at offset 3,360,312 the
chunk ends at **9,168,284**, i.e. it *fits* inside the 9,168,400-byte buffer with
**116 bytes left over**. So the stream is **not** short; what remains after the
last chunk is 116 bytes that are not a valid chunk header (the walk reads
`0x56d61794` / `0xb4e073cd` there), and the parser treats that trailing region as
an unknown chunk and returns 0. The "908 bytes short" statement above is wrong and
is superseded by this paragraph.

Restated cause (measured): framing — the buffer handed to the gate does not end on
a chunk boundary; 116 trailing bytes follow `LSNS` and are rejected. Whether those
bytes belong to the stream (padding the parser should accept) or are extra bytes
the loader should not have included is the open question.

## §18 Framing hypothesis killed by measurement (boot #208) — the missing 116 bytes are the trailing chunks

Boot #208 (`build_ga208`) handed the parser the walked framed extent instead of
the request total:

```
gate-probe walk size=9168400 framedEnd=9168284 chunks: [0]PSIN(len=0x3340c0) [3358912]LSCH(len=0x4c8) [3360136]LENM(len=0xb0) [3360312]LSNS(len=0x589f64) [9168284]<garbage>(0x56d61794,len=0xb4e073cd)
gate-probe return tid=804 ... size=9168284 fourcc='PSIN' beLen=0x3340c0 ... ret=0 al=0
veh: exception 0x80000003 at 00000001409DFC18 thread 804
```

Reducing the length did **not** help: the gate still returns 0. So the parse size
was not the defect and the trim is reverted (the raw size is passed through again).

The real reading of the numbers: the small streams are
`PSIN,LSCH,LENM,LSNS,LGID,LRSC,CHKS` and end on `CHKS`; the big stream stops at
`LSNS` and its trailing 116 bytes are garbage. `LGID` (8) + `LRSC` (0x58 = 88) +
`CHKS` (0x14 = 20) = **116** — exactly the shortfall. So the stream is meant to be
fully 9,168,400 bytes and its final three chunks arrive as garbage: the defect is
in what the read path wrote for that tail, not in the length handed to the gate.

Open: which write leaves the last 116 bytes unwritten/garbage.

## §19 Tail-writer probe (boot #210): this log site is NOT the one serving those requests

Instrumented the raw-copy branch at `Submit[%d] raw copy ...` to print the
destination range and pack. Boot #210's log still shows the requests in the OLD
format:

```
Submit[137] raw copy (hash 0x307c02cc9eb90000 offset=0x106929510 compSize=65536 destSize=65536)
Submit[138] raw copy (hash 0x307c02cc9eb90000 offset=0x106939510 compSize=58896 destSize=58896)
gate-probe walk size=9168400 framedEnd=9168284 chunks: [0]PSIN(len=0x3340c0) ... [9168284]<garbage>(0x56d61794,len=0xb4e073cd)
```

so the executed copy path is a *different* site than the branch that was edited
(the edited branch is not taken for this descriptor). Therefore the destination
range of these copies is still unmeasured, and no fix is claimed.

Measured state at the end of this pass: build_ga210 built (see build log), boot
#210 stops at 0x1409DFC18 as before, the gate still returns 0 for the full stream,
0x140004B5E remains cleared. Next step: log the destination in the branch that
actually executes (the one printing the old-format line), which is a distinct call
site in the same submit loop.

## §20 Tail writer located (boot #212): the full span IS written, from packs.rpf

Boot #212 (build_ga212: `BUILD_EXIT=0`, `B1 result: 149 checks, 0 failed -> PASS`,
`=== verify exit code: 0 ===`) with destination logging live (379 `dst=` lines):

```
Submit[137] raw copy (hash 0x307c02cc9eb90000 offset=0x106929510 compSize=65536 destSize=65536 dst=0x2098c402520..0x2098c412520 pack=D:\game\game\packs.rpf)
Submit[138] raw copy (hash 0x307c02cc9eb90000 offset=0x106939510 compSize=58896 destSize=58896 dst=0x2098c412520..0x2098c420b30 pack=D:\game\game\packs.rpf)
gate-probe walk size=9168400 framedEnd=9168284 chunks: [0]PSIN(len=0x3340c0) ... [9168284]<garbage>(0x56d61794,len=0xb4e073cd)
```

The writes are contiguous and cover the whole requested span, ending at
`0x2098c420b30`, i.e. the last 116 bytes *are* written. So the defect is neither
an under-copy nor a wrong destination offset: the bytes written there come from
**`D:\game\game\packs.rpf`**, and that source extent does not contain the
LGID/LRSC/CHKS chunks the stream declares.

That points at the third case — wrong pack member / source extent — and it is the
pack the deterministic attribution rule picks (first candidate in declared order
that covers the span). The rule made attribution order-independent as required,
but it is not yet evidence-correct for this id.

No fix landed. 0x1409DFC18 still fires; 0x140004B5E remains cleared.

## §21 The tail is not present in any pack extent (offline probe + boots #214/#215)

Offline probe of the pack bytes at the member base implied by the copies
(`base = 0x106939510 + 58896 - 9168400 = 0x106089510` in `packs.rpf`):

```
head: 42c2d0c760a301d4871e4920013ee674   <- not PSIN
walk  +0     -> [(0,'B..','0x60a301d4')]
      -116   -> [(0,'.4.9a L','0x4fa88b4e')]
      +116   -> [(0,'. ..C','0x73680ecd')]
first 'LGID' in +/-8 KiB + 9,168,400-byte window: none
```

So the extent the raw copies read from is **not** the stream member at all: no
shift in the probed window makes the head and the 116-byte `LGID+LRSC+CHKS` tail
frame, and `LGID` does not occur anywhere in that window.

Boot #214 added a guard refusing raw copies larger than 0x10000 that do not
frame; it refused unrelated audio requests but the PSIN tail page (58,896) was
below that threshold, so the walk was unchanged. Boot #215 gates every page-sized
raw copy (>= 0x1000): **90 refusals**, and the tail is now
`(0xffffffff,len=0x0)` — i.e. merely *unwritten* — instead of `0x56d61794`. The
gate still returns 0 and the boot still stops at 0x1409DFC18.

Measured conclusion: the wrong raw page was the only writer of that region; when
it is refused nothing supplies the real `LGID+LRSC+CHKS`. The tail is therefore
absent from the data this layer can reach, not misplaced by a base shift.

## §22 There is no member-resolution code to reuse: the container is RPF9

Measured from the container this layer reads:

```
D:\game\game\packs.rpf  size 4,798,213,264
first 16 bytes: 39 46 50 52 45 50 00 00 40 3f 00 00 90 1c 00 00
magic bytes = "RPF9"  (u32 LE 0x52504639)
next u32s: 0x00004550, 0x00003f40, 0x00001c90, 0x57fb6798, 0x706499ba, 0x00047a00, 0x00000150
'RPF0'..'RPF8' markers in the first 1 MiB: none
```

`grep -rn "RPF" src/*.cpp` shows **no TOC/directory parser anywhere** in this codebase:
the only RPF handling is the copy-coverage probing in `hle_registry.cpp`
(`ScDsResolvePack` / `ScDsPackByCoverage` / `ScDsReprobePack`). So the "real member
resolution" the previous pass proposed cannot be implemented by wiring up an
existing parser — RPF9 (the format this game uses) has no implementation here and
no public layout to copy.

That closes the loop on the tail question honestly: the trailing 116 bytes are not
missing because of a base or extent error that this layer can correct with its
current knowledge; they are missing because this layer does not know the member
layout of an RPF9 container, and therefore cannot fetch the member's true byte
range. Everything up to 9,168,284 arrives correctly because the guest's own page
requests happen to land inside the member.

No code changed in this pass. No claim is made beyond the hex and sizes quoted.

## §23 The size question answered by bytes: 9,168,400 is page-rounded, and the terminator is absent

Decisive test 1 run against the archived 9,168,400-byte PSIN buffer
(`D:\scarlett-dumps\psin_blob-195.bin`, exactly the size the gate is given):

```
len 9168400
search for b'LGID' anywhere in the blob : []        <- none
last 160 bytes: eac0c43f fe000000 00000000 ... 00000000   (~150 zero bytes)
```

So the buffer the gate parses contains **no LGID at all**, and its tail is
`eac0c43f fe000000` followed by zero padding. Two conclusions follow directly:

1. The full-stream buffer never contained the `LGID/LRSC/CHKS` terminator; it was
   not clobbered into garbage, it was simply never fetched (and the region reads
   as zeros / `0xffffffff` depending on the guard state).
2. `9,168,400 = 139*65536 + 58896` is a **page-rounded request total**, not the
   member's byte length. The member's real end is past what we copy; the honest
   fix is to fetch the member's true byte range, which needs the container
   directory (RPF9) — the same conclusion the earlier probes reached from the
   other side.

Measured: no terminator in the assembled stream; size is page-rounded. Inferred:
the member is longer than 9,168,400 and the guest's descriptors stop short of its
end for this variant.

No code changed in this pass; nothing invented, nothing trimmed, gate untouched.

## §24 Boot #216: the PSIN gate stop is gone; new stop localized at 0x144FC21F5

Boot #216 (build_ga216: `BUILD_EXIT=0`, `B1 result: 149 checks, 0 failed -> PASS`,
`=== verify exit code: 0 ===`) logged **zero** occurrences of `1409DFC18` — the PSIN
assert no longer fires. The gate takes complete streams that each end in a real
terminator:

```
gate-probe walk size=14448 … [14332]LGID(len=0x8) [14340]LRSC(len=0x58) [14428]CHKS(len=0x14)
gate-probe walk size=528   … [412]LGID(len=0x8) [420]LRSC(len=0x58) [508]CHKS(len=0x14)
gate-probe entry … size=632 / 14448 / 528, fourcc='PSIN'
```

Measured: **no `walk size=9168400` line exists in #216** — the 9,168,400-byte call
is no longer reached in this run (the guest now feeds 528/632/14,448-byte streams
to the same gate). Nothing was resolved from an RPF9 directory; the large call
simply does not occur any more, which is why the assert stopped firing. Inferred,
not proven: with the smaller streams accepted, the guest never takes the code path
that reassembled the large buffer.

New stop, localized:

```
exception code=0xc0000005 rip=0x144fc21f5 tid=5720
params[0]=0x0 (read), params[1]=0x1f4f7511f24 (faulting address)
rax=0x1f486206560 rcx=0x1f4f7511f24 (== faulting address) rdx=0x0 r8=0xffffffff00000000
0x144fc21f0: test rcx,rcx
0x144fc21f3: je   0x144fc226c
0x144fc21f5: movzx r8d, byte ptr [rcx]     <- fault, rcx is not mapped
```

So the guest dereferences the pointer `0x1f4f7511f24`, which is not mapped in the
process. That pointer is the value it was handed; the fix belongs where that
pointer is produced/committed by the layer, not at the fault.

No fix landed for 0x144FC21F5; one boot only; nothing forced or swallowed.

## §25 0x144FC21F5: the faulting operand is mapped nowhere

Dump evidence (minidump memory map, `F:\ScarlettCompat\out\scarlett_crash.dmp`
from boot #216):

```
0x1f4f7511f24   -> unmapped (no memory range in the dump contains it)
exception 0xc0000005 rip=0x144fc21f5 params=[0x0 (read), 0x1f4f7511f24]
rcx = 0x1f4f7511f24  (the pointer being dereferenced)
0x144fc21f5: movzx r8d, byte ptr [rcx]
```

So this is not a reserved-but-uncommitted region: the whole address is absent from
the process map. Our allocation paths in `src/era_to_gdk.cpp` all request
`MEM_RESERVE | MEM_COMMIT` (lines 288, 291, 368, 451, 539, 542), and
`EraXMemAllocDefault` (line 122) is the default allocator entry, so the pointer did
not come from a partially committed mapping of ours; it came from something that
handed the guest an address this process never backs (a guest-computed pointer into
a region we did not map, or an allocator answer we returned from a non-owning path).

No fix landed. One boot (#216). Nothing swallowed, nothing forced.

## §26 Boot #217: the faulting page is COMMITTED but PAGE_NOACCESS (measured)

New VEH probe logs VirtualQuery at the fault (`src/pe_loader.cpp`), boot #217
(`build_ga217`: `BUILD_EXIT=0`, `B1 result: 149 checks, 0 failed -> PASS`,
`=== verify exit code: 0 ===`, 0 × `1409DFC18`):

```
veh: av rip=0x144fc21f5 tid=19680 faultAddr=0x19b7784992b read=1 vq=48
     state=0x10000 protect=0x1 base=0x19b77849000 regionsize=0x3247000
```

- `state=0x10000` = `MEM_COMMIT`
- `protect=0x1` = `PAGE_NOACCESS`
- region base `0x19b77849000`, size `0x3247000` (~50 MB)

So this is **not** an unmapped address and **not** an uncommitted one — it is
committed memory whose protection was set to `PAGE_NOACCESS`. The access is a
*read* of one byte at `base+0x92b` inside a 50 MB committed region.

That changes the diagnosis: the block was protected (or decommitted-then-represented
as `PAGE_NOACCESS`) by a `VirtualProtect`/`MEM_DECOMMIT` path while the guest still
holds a live pointer into it. In this layer the guest's memory comes from
`EraXMemAllocDefault`/`HeapAlloc` (host heap), and the guest's own allocator can
`VirtualProtect`; the region size (50 MB) looks like an arena, not a single object.

Measured: committed + `PAGE_NOACCESS` read fault. Inferred: a free/decommit or
protect path ran on a region the guest still uses. No fix landed; one boot; no
fault swallowing, no protection changes made by us.

Earlier §25 said the address is "unmapped" — that was read from the *minidump* map,
which omits `PAGE_NOACCESS` ranges; §26's live VirtualQuery supersedes it.

## §27 Boots #218/#219: the reverted tree stops at 0x1409DFC18 again

Two fresh builds of the reverted sources, both green:

```
build_ga218  BUILD_EXIT=0  B1 result: 149 checks, 0 failed -> PASS  verify exit code: 0
build_ga219  BUILD_EXIT=0  B1 result: 149 checks, 0 failed -> PASS  verify exit code: 0
```

Boots:

```
#218  veh: exception 0x80000003 at 00000001409DFC18 thread 28008
#219  veh: exception 0x80000003 at 00000001409DFC18 thread 2736
```

#218 carried temporary `VirtualFree`-site logging (removed in #219, verified absent from
both sources); neither boot logged a single `VirtualFree RELEASE` line, so those three
`MEM_RELEASE` sites are not on the path that matters. `PAGE_NOACCESS` is produced by
`VirtualProtect`/`VirtualProtectEx`/`NtProtectVirtualMemory`/`MEM_DECOMMIT`, not by
`MEM_RELEASE` — the wrong API family was watched in #218.

Comparator: #216 and #217 (same source family, with the chunk-walk terminator parse)
got past `0x1409DFC18`. So across #216/#217 versus #218/#219 the gate's accept/reject
differs while the gate code is unchanged; the difference is not the probes (they do not
touch the gate). Measured: two stop kinds on effectively the same source.

Open: `0x144FC21F5`'s `PAGE_NOACCESS` producer on region base `0x19b77849000`
(size `0x3247000`) is still unnamed. #217's dump was deleted without archiving; from
here every dump is copied to `D:\scarlett-dumps` before any delete.

## §28 Boots #220/#221: deterministic size vs beLen mismatch

Two boots of `build_ga220` (`BUILD_EXIT=0`, `B1 result: 149 checks, 0 failed -> PASS`,
`=== verify exit code: 0 ===`), same binary:

```
#220 veh: exception 0x80000003 at 00000001409DFC18 thread 27192
#221 veh: exception 0x80000003 at 00000001409DFC18 (different thread)
size=9168400 flag=0 fourcc='PSIN' (0x5053494e) beLen=0x3340c0 head8=5053494e003340c0
     status=?4294967295 ret=0   (identical in both boots)
```

`head8` proves the buffer starts with `PSIN` and its declared BE length is
`0x3340c0` = 3,358,912, while the gate is handed `size = 0x8be2d0` = 9,168,400.
The extent is 5,809,488 bytes longer than the stream's own length. `ret=0`.

So (a) the 9,168,400-byte full-stream call IS reached, twice per boot; (b) the
mismatch is between the extent and the declared stream length, not a
not-yet-copied page set; (c) the stop is deterministic on this tree.

Next step: decide which number is wrong in the producer (over-extended extent /
page rounding / pack attribution, versus truncated stream header) and make the
extent equal the declared length. No fix landed.

## §29 Boot #222: beLen bounds one chunk, not the buffer (extent question closed)

Probe `atEnd8` (8 bytes at `buf + beLen`) added to `ScGateProbe`. Boot #222,
`build_ga222` (`BUILD_EXIT=0`, `B1 result: 149 checks, 0 failed -> PASS`,
`=== verify exit code: 0 ===`):

```
gate-probe ... size=976 fourcc='PSIN' beLen=0x150 ret=1 al=1
   head8=5053494e00000150  atEnd8=4c53434800000158
```

`4c 53 43 48` = `LSCH`, `00000158` = its length, sitting exactly at `buf + beLen`.
So `beLen` is the length of the **PSIN chunk only**, inside a buffer of concatenated
chunks (`PSIN` -> `LSCH` -> ...), which matches the chunk walk seen on the 528/632/
14,448-byte streams.

Verdict on the "which number is wrong" question: **neither.** `0x3340c0` bounds the
PSIN chunk; `0x8be2d0` is the concatenated buffer extent. Shrinking the buffer to
`beLen` would truncate the stream and rewriting `beLen` would corrupt pack data; both
are closed. The `0x80000003` assert at `0x1409DFC18` no longer fires in #222
(`ret=1 al=1`), and the run stops at `0xc0000005` in the `0x144...` family instead.

## §30 VEH lazy-commit for guest-reserved arenas (unexercised)

Boot #222 faulted at `0x144fc21f5` reading `base+0xa9d` inside a `MEM_RESERVE`
region (`state=0x2000`, `protect=0x0`, base `0x2588b3c8000`, size `0x3e5888000`):
the guest reserved an arena and touched it without an explicit commit. The VEH now
commits the single 64 KiB page containing a write/read fault whose region state is
`MEM_RESERVE`, then retries the instruction:

```
veh: lazy-commit page=%p in reserved region base=%p size=0x%llx -> %p (err %lu)
```

Scoped strictly to `mbi.State == MEM_RESERVE`; committed pages, the loader's own
mappings and every other fault keep their previous handling unchanged.

Status: `build_ga223` is green (`BUILD_EXIT=0`, `149 checks, 0 failed`,
`verify exit code: 0`) but the path has never fired — boot #223 logged 0
`lazy-commit` lines because the run stopped earlier at `0x1409DFC18`. Unexercised,
not a proven fix.

Gate instability, measured: #220 `ret=0`, #221 `ret=0`, #222 `ret=1 al=1`,
#223 `ret=0` — same gate code across those builds, so the verdict is not yet stable.

## §31 #224/#225: the flip is which call is reached, not a race

Same `build_ga224` binary, two boots:

```
#224 size=512 fourcc='PSIN' beLen=0x80 ret=1 al=1
     atEnd8=4c534348000000a8 resPages=1/1 firstNonRes=0x0 resExtent=0x1000
     -> 0xc0000005 at 0x144...
#225 size=9168400 fourcc='PSIN' beLen=0x3340c0 ret=0 al=0
     atEnd8=4c534348000004c8 resPages=2239/2239 firstNonRes=0x0 resExtent=0x8bf000
     -> 0x80000003 at 0x1409DFC18
```

Residency is refuted: the 9,168,400-byte buffer is fully committed
(`2239/2239`, `firstNonRes=0x0`, `resExtent=0x8bf000 >= 0x8be2d0`). Byte content is
identical across boots (`head8`/`atEnd8`/`fourcc` unchanged).

What differs is which gate call the boot reaches first: #224 ends on the small
512-byte stream (`ret=1`) and dies later in the arena; #225 reaches the
9,168,400-byte call and is rejected (`ret=0`), reproducibly (#220/#221/#225).

Also measured: `size != beLen` is **not** the reject rule — the 512-byte call has
`beLen=0x80` with the next chunk at `0x88` (`LSCH len=0xa8`) and is accepted. The
reject is specific to the large stream's tail. No fix landed in this commit.

## §32 Boot #226 (tail32 build): the large PSIN call was never reached

`build_ga226` (green: `BUILD_EXIT=0`, `149 checks, 0 failed`, `verify exit 0`) logs
`tail32` only on the `size == 9168400` branch. Boot #226's log is 16,972 lines and
contains **zero** occurrences of `9168400` — so the large call did not happen at all
that boot, and `tail32` never fired. The streams it did walk are small:

```
walk size=2456  framedEnd=2456  chunks: [0]PSIN(len=0x210) [528]LSCH(len=0x128) ...
walk size=74720 framedEnd=74720 chunks: [0]PSIN(len=0x6d70) [28016]LSCH(len=0x95b8) ...
```

Consequence for the "flip": across five boots of four builds the large call is
reached only sometimes (#220, #221, #225 yes; #222, #224, #226 no). The verdict on it
when reached has been `ret=0` every single time with identical bytes. What is
boot-order dependent is **whether the guest makes that call**, not what the gate
returns for it.

Still needed: the tail of the buffer on a boot that does reach it (`tail32`), then
cause (a) truncated assembly / (b) walk final-boundary — no fix landed.
