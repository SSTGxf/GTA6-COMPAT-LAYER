# Build 85 — no producer exists for [streaming record+0x18]

Build 85 adds no behaviour change: it closes the producer hunt with
disassembly of **both** creation paths, a whole-image scan for the attach
step, and the measured contract of the object the guest expects. The null
sub-object at `0x1428C9A08` is a member the guest's own record class never
initializes, on either path, and no code in the image attaches it.

## 1. Both record creation paths, disassembled

The class has exactly one constructor — the image contains a single store
of the `"sdms"` immediate (`0x1428c949c`), verified by a byte scan of
`.text`. It is reached through the owner's vtable slot 66 (`+0x210`),
owner = static `0x1478f5e80`. Two callers exercise it with different
`+0x20/+0x28` (aux buffer) modes:

**A — the initializer that publishes the global (the path that crashes)**

```
0x145cd3f55  mov rsi,rcx
0x145cd3f69  mov [global 0x1479f6d78],rsi        ; owner
0x145cd3f70  mov rcx,rsi
0x145cd3f73  mov edx,0x1000
0x145cd3f78  xor r8d,r8d                          ; arg3 = 0  (no aux)
0x145cd3f7b  mov r9d,2
0x145cd3f84  mov dword [rsp+0x20],0
0x145cd3f8c  call [rax+0x210]                     ; slot 66 -> record
0x145cd3f92  mov [global 0x1479f6d70],rax          ; publish
```

**B — the sibling path that builds the aux-bearing records**

```
0x1428cb7e7  mov ecx,0x80
0x1428cb7ec  call 0x144f97130                     ; 0x80 bookkeeping buffer
0x1428cb7f1  lea rsi,[rip+0x502a688]              ; = 0x1478f5e80 (owner)
0x1428cb810  mov rax,[rip+0x502a669]              ; owner
0x1428cb817  mov rcx,rsi
0x1428cb81a  mov edx,0x1000
0x1428cb81f  mov r8b,1                            ; arg3 = 1  (aux alloc)
0x1428cb822  mov r9d,2
0x1428cb828  mov dword [rsp+0x20],0
0x1428cb830  call [rax+0x210]                     ; slot 66 -> record
0x1428cb836  movzx ecx,byte [rip+0x7372882]
0x1428cb83d  mov rdx,[rip+0x7372844]              ; record table
0x1428cb844  mov r8d,ecx
0x1428cb847  shl r8d,3
0x1428cb84b  mov [rdx+r8],rax                     ; table[count] = record
```

Neither path writes `+0x18`; neither has the record's address in a
register that a following store could target. The constructor's own field
map (docs/BUILD83-DIAGNOSIS.md §1) writes
`+0x00, +0x08, +0x0c, +0x10, +0x20, +0x28, +0x38, +0x40, +0x48, +0x50,
+0x58, +0x68, +0x6c, +0x70` and omits `+0x18`.

## 2. The attach step does not exist anywhere in the image

* **238** `call [reg+0x210]` sites (the record factory). Byte-scan for a
  64-bit store into `[reg+0x18]` within the next `0x30` bytes of any of
  them: **0 matches**.
* The constructor is the only writer of the class magic, so there is no
  second constructor or magic-validating setter to find.
* Store census of the streaming module `0x1428C0000 … 0x1428D0000`:
  `0x1428c95bc`, `0x1428c99fa`, `0x1428c9c83`, `0x1428cde2b`,
  `0x1428cebe7`, `0x1428cec1f`, `0x1428cf104`, `0x1428cf2fa`,
  `0x1428cfb38`, `0x1428cffd6` — each targets a different structure
  (out-param flag, 0x20-byte map entry, 0x40-byte copy, 0x28-byte node),
  none a `0x80`-byte `"sdms"` record.
* All 8 references to the record global `0x1479f6d70` and all 8 references
  to the owner static `0x1478f5e80` were disassembled: the only `+0x18`
  store among them is `0x1428cb726  mov qword [rsi+0x18],0` — a
  **clear**, on a different structure.
* The DR0 watchpoint (build 84) confirmed it at run time: publish trapped
  on thread 24356, `+0x18` re-armed in that same context, **one trap
  total**, `bk=0` at every device slot call.

## 3. What the guest expects at `+0x18` (the contract)

The faulting consumer and its neighbours, with absolute and
interface-relative slots:

| site | instruction | `this` | args | slot (absolute / iface-relative) | return used |
|---|---|---|---|---|---|
| `0x1428c9a04` | `mov rcx,[rsi+0x18]`; `mov rax,[rcx]`; `call [rax+0x18]` | the record's backend | `rdx = &[rsp+0x38]` = `{qword flags, qword end, dword @+0x18, dword @+0x34=0}` | **3 / 3** | no (`nop` at `0x1428c9a0e`; the frame returns immediately) |
| `0x1428c9630` | `mov rcx,[rsi+0x18]`; `call [rax+0x40]` | same field, in the record destructor | — | **8 / 8** | no |

Two facts pin the shape of the expected object:

* the call is a **submission** — the descriptor is
  `{flags, end, status@+0x18, reserved@+0x34}` built at
  `0x1428c99eb`…`0x1428c99fd`, matching the `XMemMapPhysicalPagesScatter`
  page range that immediately precedes it;
* the same field is **null-checked** elsewhere in the module —
  `0x1428cb711 mov rcx,[rsi+0x18]; test rcx,rcx; je …` — so the guest
  itself treats the backend as optional on other paths, and only this
  submit path dereferences it unconditionally.

## 4. The layer's XMem shim is exonerated for this field

`XMemMapPhysicalPagesScatter` is called at log line 2006, **after** the
publish (2003) and with the watchpoint live on `+0x18`; no trap followed,
so the shim's write (or mis-write) never touches the field. Its signature
bug is real and separate:

```
XMemMapPhysicalPagesScatter(pages 0x200F7F26C0, count 6720,
    attrs 0xf7e54c0, protect 0x337e0000) -> 0x1B708F90000 (synthetic 27 MB)
```

`attrs`/`protect` are stack values, so our HLE signature is wrong — but
the field is null before that call and stays null after it, so correcting
that signature cannot populate `+0x18`.

## 5. Verdict

The object the guest needs at `[record+0x18]` has no producer in the
image: not in the constructor, not in either creation path, not in any of
the 238 factory call sites, not in the module that consumes it, and not
via any layer call (the watchpoint and the zero `bk=` values prove the
layer never even received that address).

Producing it from the layer would mean synthesising an undocumented Xbox
submission interface (slot 3 request layout, slot 8 teardown, plus every
other call site through the field) **and** writing it into guest memory
that the guest's own code never initialises — a stand-in backend with
invented semantics, which the mission forbids. It would also be
unreachable by contract: the guest's record is published and consumed
within the same millisecond by the same thread, with no layer call in
between.

So this is the fallback the mission allows: the object cannot be produced
without a subsystem/contract that does not exist in this layer, reported
with the evidence above. No dummy interface, no stand-in, no bypass, and
the faulting call is untouched.

## 6. State

* No behaviour change in this build; build 82/83/84 instrumentation
  (per-slot identity logs, fault-time ctx-probe, DR0 watchpoint, `bk=`
  field) all remain in place.
* Working tree and `F:\ScarlettCompat` were MD5-identical for every file
  changed in builds 83/84 and both passed `build.bat` (exit 0, era 21/21,
  ft 7/7, verify 0).
* Last boot (#67) still faults at `0x1428C9A08` with
  `fault-kind=sub-object-null`; no new boot was run for build 85 because
  the source is unchanged since #67 — an identical boot would only
  reproduce the same trap log.
