# Build 84 — the write watchpoint on [streaming record+0x18]

Decisive measurement, boot #67: **nothing writes `[record+0x18]` on the
crashing path.** The publish trapped, the watchpoint was re-armed on the
field inside that same exception context, and no second trap fired before
the crash. The field is read uninitialized.

## 1. The instrument

`src/pe_loader.cpp` (build 84) arms debug registers, observation only —
no guest byte is patched:

* **stage 1** — `DR0 = image global 0x79F6D70`, `DR7 = 0x000D0001`
  (L0, RW0 = 01 write, LEN0 = 11 → 8 bytes), armed on every thread of the
  process, re-scanned every 100 ms so threads the game creates later are
  covered;
* **stage 2** — when a publish traps, `DR0 = record+0x18` is written
  **into the trapping context** and the other threads are re-armed by the
  poller every 50 ms;
* every trap logs RIP (the instruction after the writer), thread id, the
  value the field now holds and the guest return addresses on the stack,
  then resumes with the watchpoint still armed;
* the watchdog thread is marked (`thread_local`) so its own SEH-guarded
  probes are never mistaken for a guest crash, and the watchpoint refuses
  to arm on images whose `SizeOfImage` does not map `0x79F6D70` (the ERA /
  guest verify children are `0xb000` bytes — arming there only probed
  unmapped memory and killed two verify runs before the gate).

`ScWatchRecordBackend()` publishes the watched value, and every
unimplemented device-slot log now ends with `bk=<value>` so the backend
state on both sides of any diverted guest branch is on record.

## 2. The trap log (boot #67)

```
[watch] watchdog start base=0x140000000 global=0x1479f6d70
[watch] stage1: DR0=0x1479f6d70 (record global) write/8 on 4 threads
[watch] trap #1 stage1 rip=0x145cd3f99 thread=24356 global value=0x1b41ad0b780 magic=0x6d736473
[watch] stack: 0x142240d57,0x1443a492c,0x14224070b,0x14169c64a,0x1471b9b10,...
[watch] stage2 (from trap): record 0x1b41ad0b780 owner=0x1478f5e80 -> DR0=0x1b41ad0b798 re-armed in-context
```

* `rip=0x145cd3f99` is the instruction after the publish store
  (`mov [rip+0x1d22dd7],rax` at `0x145cd3f92`), so the trap is exactly the
  record publish.
* The stack names its producer: `0x1443a492c`, the sole caller of the
  initializer `0x145CD3F50` — the same path established statically in
  docs/BUILD83-DIAGNOSIS.md.
* The record is the one that crashes: `ctx-probe` at fault time reports
  `rsi(ctx)=0x1b41ad0b780 ... registered=0x1b41ad0b780`.
* `grep -c '[watch] trap' = 1`: **no write to `+0x18` ever trapped**, on
  the thread that crashed (24356 published, 24356 faulted) for the whole
  publish → crash window. The `bk=` field is 0 at every slot call.

Crash outcome, unchanged: `0xC0000005` at `0x1428C9A08`,
`[record+0x18] = 0`, `fault-kind=sub-object-null`.

## 3. Build 82's `0x1461ECDB0` object: a register misattribution

Build 82 claimed `[context+0x18] = 0x1f08deb31f0` (vtable `0x1461ECDB0`)
was "held" there on the upload path. Boot #63's dump and log refute that,
and there is no contradiction with the 17 zero records:

* The slot-70 call at `0x14501F703` loads `r8 = [rsi+0x18]` at
  `0x14501F6ED`. The wrapper maps `r8` → slot `r9`, and the log shows
  `r9 = 0x1f1d6ee44e0`. That object is **not** a vtable object: it is an
  array of `{handle, pointer}` pairs (`0x1f10000001b → 0x1f1dbaf0340`,
  `0x1f10000001c → 0x1f1dbaf0360`).
* `0x1f08deb31f0` was the *fourth* logged value — a stack-spilled
  argument, not a register. Its `+0x00` is `0x1461ECDA0` and its
  container at `0x1f08deb31a0` has `0x1461ECDB0`; it is the tiled-resource
  record from the earlier texture copy, referenced only from that
  container (`+0x28`).
* The `rsi` at that call site is an upload-job object, not a `"sdms"`
  streaming record. Its `+0x18` (a descriptor array) and the streaming
  record's `+0x18` (the submission backend) are different fields of
  different classes.

So build 82 recorded the wrong register and the wrong object. The 17/17
zero census stands, and the watchpoint now supersedes the static claim.

## 4. Slot 16's E_NOTIMPL diversion: measured, and not this

The guest does branch on our failure at `0x140fa0b34` / `0x140fa0b48`
(`js` taken for `HRESULT < 0`), but the branch fires **before the
streaming record exists**:

```
1987  UNIMPLEMENTED slot 16 ... (call #1) bk=0
1989  UNIMPLEMENTED slot 16 ... (call #2) bk=0
2003  [watch] trap #1 ... record published
2006  XMemMapPhysicalPagesScatter(...) -> synthetic mapping
2029  ctx-probe ... sub[+0x18]=0x0
```

`bk=0` at both slot-16 calls means no record had been published yet
(`g_wpRecord` was still null). The record is created after that branch,
published at line 2003, and the watchpoint proves nothing wrote its
backend between publication and use. Slot 16's diversion therefore
cannot be the step that "skipped the attach" for this record; whatever it
breaks is upstream of the record's creation.

## 5. Confirmed separately: XMemMapPhysicalPagesScatter argument mis-parse

```
XMemMapPhysicalPagesScatter(pages 0x200F7F26C0, count 6720,
    attrs 0xf7e54c0, protect 0x337e0000) -> 0x1B708F90000
```

`attrs`/`protect` are stack values, i.e. our HLE signature does not match
the guest's. It still hands back a synthetic 27 MB mapping, so the boot
continues along a bogus path. This is a real defect, independent of the
null backend field, and is left for its own fix.

## 6. Verdict

No writer exists on this path. The record is constructed with `+0x18`
omitted (docs/BUILD83-DIAGNOSIS.md §1), published, and used milliseconds
later without anyone populating the field, on a single thread, with the
write watchpoint live across the whole window. It is not a missing device
slot, and it is not slot 16/86.

Per the mission, **no stand-in backend object was introduced**: the
interface contract (slot 3 request layout plus the other call sites
through the same field) is unknown, and a fabricated object would be a
dummy interface with fake success.

## 7. Verification

| check | working tree | F:\ScarlettCompat |
|---|---|---|
| `build.bat` exit | 0 | 0 |
| era stages | 21/21 | 21/21 |
| ft stages | 7/7 | 7/7 |
| verify exit | 0 | 0 |

Two intermediate runs were poisoned by the watchpoint itself and are
recorded honestly: boot #65 lost its watchdog to a self-suspend (fixed by
excluding the current thread), and one verify pass classified the
watchdog's own guarded probe as a guest crash (fixed by the `thread_local`
marker plus the `SizeOfImage` gate). `src/pe_loader.cpp` is MD5-identical
on both trees.

Boot #67: `child exit 0xC0000005`, same fault, black window,
`BOOT_EXIT=1`. Dump `6,209,471,190` bytes archived to
`D:\scarlett-dumps\scarlett_crash_boot67.dmp` (head+tail byte-identical),
source deleted, F: back to 8.2 GB free.
