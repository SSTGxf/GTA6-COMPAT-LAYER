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
