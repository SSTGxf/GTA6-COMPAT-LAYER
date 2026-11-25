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
