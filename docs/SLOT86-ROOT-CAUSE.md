# Slot 86 (CreatePipelineState) — why the forward crashed, and why it must not come back

**Status: root cause PROVEN by boots #183 and #184 (2026-11-25). Do not register
`g_xboxDeviceVtable[86]` as a "forward the stream unchanged" handler.**

Entry 33 (`0be2cc77`, Codex branch `ee9d3fd5`) added exactly that handler. Boots
#179/#180 died at 2,018 lines in `amdxc64.dll`. The guess at the time was "the
handler was never called" — that was wrong, and the reason is worth remembering:
**the entry-33 handler logged only after the forwarded call returned**
(entry-33 lines 3834–3847), so a fault *inside* the driver call produced exactly
zero log lines.

---

## The proof

Two probe builds, same base tree (`src/hle_registry.cpp` with the hash resolver, no
slot 86), each adding one handler at slot 86. Both compiled clean (`cl /c`, only the
pre-existing `getenv` C4996) and were booted once.

### Probe B — forward, but log *before* the call (`build_ga183.log`, boot #183)

```
1992: slot86 PROBE-B enter call#1 self=00000206373A4E80 desc=000000E090BFF310
      size=688 stream=000000E090BFEC60 riid={765A30F3-F624-4C6F-A828-ACE948622445}
      out=000000E090BFF338 ret=0x142479a8d dxbc=0 firstDxbc=-1
      head=00000000 00000000 6e9c6170 00000213 00000001 00000000 4688e13c 00000001
1993: slot86 PROBE-B call#1 -> forward to desktop CreatePipelineState
1994: veh: exception 0xc0000005 at 00007FFBA0D6FFD0 thread 30428
1995: veh: fault module amdxc64.dll +0x52ffd0 (params[0]=0x0 params[1]=0x0)
```

**No `-> returned` line.** The crash is inside `device2->CreatePipelineState(...)`
(entry-33 line 3825). `device2->Release()` (3826) — the only other unlogged call in
the handler — is cleared.

### Probe A — registered handler, entry log, `E_NOTIMPL`, no forward (boot #184)

```
1992: slot86 PROBE-A enter call#1 ... size=688 ... -> E_NOTIMPL (registered, called, NOT forwarded)
...
   4: 65,891 log lines, 50,467 slot-86 calls, ends `0x80000003` at 0x1443AD94E
```

Boot #182 (generic stub, throttled logging) reached **call #50,432** and ended at the
**same** `int3` `0x1443AD94E`. So probe A is behaviour-identical to the generic stub —
the line-count difference is only log volume (46,915 unthrottled lines).

**Two things are now settled:** registration, slot index, ABI, calling convention and
static init are all innocent (a registered handler ran 50,467 times without harm), and
the fault is strictly inside the forwarded driver call.

---

## What the guest actually hands the driver

Guest call site (capstone 5.0.3, `game_scarlett_master.exe`, base `0x140000000`):

```
0x142479a59  mov  rcx,[rip+0x51655a0]   ; this = proxy device
0x142479a60  lea  rdx,[rbp+0x630]       ; D3D12_PIPELINE_STATE_STREAM_DESC*
0x142479a67  mov  qword [rdx],0x2b0     ;   SizeInBytes = 688
0x142479a6e  mov  [rdx+8],rdi           ;   pSubobjectStream = rbp-0x80
0x142479a79  lea  r8,[rip+0x3d66c38]    ;   riid = 0x1461E06B8
0x142479a87  call qword [rax+0x2b0]     ; slot 86
0x142479a8d  add  rsp,0x20
```

- IID at `0x1461E06B8` = `{765A30F3-F624-4C6F-A828-ACE948622445}` = `IID_ID3D12PipelineState`
  — the desktop IID, and the same one the generic stub has printed since boot #154.
- `SizeInBytes = 0x2b0` (688). The probe's first 32 bytes read as plausible desktop
  subobjects: `Type=0` (ROOT_SIGNATURE) with a heap pointer at +8, then `Type=1` (VS)
  at +16 with a bytecode pointer at +24 — i.e. **the stream is desktop-shaped**, not a
  console-only struct.
- That VS bytecode pointer, `0x14688E13C`, resolves in the exe's `.data` to a **992-byte
  DXBC container** (`DXBC`, 7 chunks): `SFI0`, `ISG1`, `OSG1`, `PSV0`, `HASH`, `DXIL`,
  `PRIV`. So the payload is DXIL, carrying console-produced shader metadata.
- `dxbc=0` in the probe line is expected and not a contradiction: the container lives at
  the pointer, not inline in the first 512 bytes of the stream.

Post-call, the guest tolerates failure (`test edi,edi; js 0x142479adf` → skip storing the
PSO). That is why an `E_NOTIMPL` stub walks 50k PSO attempts and 16k log lines past this
point, while the forward kills the process on the very first one.

## Why the forward cannot work

`amdxc64.dll` is the AMD D3D12 UMD/compiler. Handed a console-produced DXIL container it
faults by reading `0x0` (#179, #183) or `0x28` (#180) — a null member dereference inside a
parser, **not** an `E_INVALIDARG` from stream validation. The entry-33 comment predicted
"the desktop runtime will reject the create"; the measured behaviour is worse: the UMD
takes the process down and there is no HRESULT to react to.

Which subobject/field it chokes on is **not** established — that needs the stream walked
subobject by subobject at runtime, not another blind boot.

---

## Rule for slot 86

1. **Never forward the guest stream unchanged.** It is not a desktop-valid PSO stream even
   though it is desktop-shaped: its shaders are console DXIL.
2. **Any slot-86 handler must log on entry** (before the forwarded call), not only after —
   the post-call-only logging is what made #179/#180 look like a registration crash.
3. **The real implementation is capture-and-translate**, which is what the shader subsystem
   was built for: parse the `0x2b0`-byte subobject stream, run each shader blob through the
   Phase 0 capture hook (`src/shader/sc_capture.cpp`), translate, rebuild a desktop stream,
   then call `CreatePipelineState`.
4. Until then the generic stub's `E_NOTIMPL` is the proven-safe answer: #182/#184 walked
   50k PSO attempts and ~16k log lines past it to the same later wall (`0x1443AD94E`).

## Evidence trail

| artefact | where |
|---|---|
| boot #183 (probe B, forward) | `scarlett_boot183.log` — probe at 1992–1993, fault at 1994 |
| boot #184 (probe A, stub) | `scarlett_boot184.log` — 65,891 lines, 50,467 calls, assert `0x1443AD94E` |
| probe sources + patches | `investigation/slot86/` (`hle_registry_probeB_forward.cpp`, `slot86-probeB.patch`, same for probe A) |
| full investigation write-up | `investigation/slot86/SLOT86-ROOT-CAUSE.md` |
| builds | `build_ga183.log` (374,272 B DLL), `build_ga184.log` (373,248 B), `build_ga185.log` (clean rebuild, 372,736 B) |

Both probe builds and the clean rebuild were green: B1 149/149, verify exit 0. The tree is
back to the no-slot-86 source afterwards (`md5 5345a1e041d605cb3993072e5066422f`).
