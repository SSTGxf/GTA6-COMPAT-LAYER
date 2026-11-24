# Build 83 — the streaming context behind 0x1428C9A08

Boot #64 fault: `0xC0000005` reading `0x0` at `0x1428C9A08`
(`mov rax,[rcx]`, `rcx = [rsi+0x18]`). Same fault as #62/#63. What
changed is that the record behind it is now named by the layer itself:

```
[loader] ctx-probe: fault=00000001428C9A08 rsi(ctx)=0x2080191b780
         rcx(sub-object)=0x0 magic=0x6d736473 registered=0x2080191b780
[loader] ctx-probe: ctx=0x2080191b780 owner=0x1478f5e80 sub[+0x18]=0x0
         arg8=2 argc=0 aux[+0x20]=0x0 aux[+0x28]=0x0 out[+0x30]=0x0
         arr[+0x38]=0x208049c3530 used=0 cap=4096
[loader] ctx-probe: fault-kind=sub-object-null
```

`registered == ctx`: the faulting record is the one published to the
image global `0x79F6D70`, not a stray pointer.

## 1. What the record is

* Layout: `+0x00` magic `"sdms"` (`0x6d736473`), no vtable — this is a
  plain guest data record, not a COM object.
* Size `0x80`; boot #63's dump holds **17 live records** at
  `0x1f08b81ae80 … 0x1f08b81b780` (`0x80` stride), so they are a pool.
* Owner: every record stores the same `+0x10 = 0x1478f5e80`, a **static
  guest object** in the image whose vtable is `0x1462fc550`.
* Constructor: **`0x1428C9460`**, i.e. owner vtable slot 66
  (`+0x210`). It allocates `0x80`, writes the magic, then
  `+0x08 = r9d`, `+0x0c = arg5`, `+0x10 = this(owner)`,
  `+0x38/+0x40 = alloc(count*4)`, `+0x48 = base+count*4`,
  `+0x50 = 0`, `+0x58 = 0`, `+0x68 = count`, `+0x20/+0x28 = aux
  alloc (only when `r8b != 0`)`, and calls one external service.
* Publish: `0x145CD3F50` calls owner slot 66 with
  `(owner, 0x1000, 0, 2, 0)` and stores the result to the global
  `0x1479f6d70` (`0x145cd3f92`). Sole caller of that initializer:
  `0x1443a4927`.

### The constructor never writes `+0x18`

Field map of `0x1428C9460` (`0x1428c949c` … `0x1428c95e1`): it writes
`+0x00, +0x08, +0x0c, +0x10, +0x20, +0x28, +0x38, +0x40, +0x48,
+0x50, +0x58, +0x68, +0x6c, +0x70`. `+0x18` is **absent**.

The whole image was scanned for stores of the `"sdms"` immediate
(`73 64 73 6d` in `.text`): **exactly one hit**, `0x1428c949c`, the
constructor above. There is no second constructor and no
magic-validating setter.

### The one external call in the constructor does not fill `+0x18` either

```
0x1428c94f1  mov rcx,[rip+0x502c980]      ; global iface, VA 0x1478f5e78
0x1428c9507  lea rax,[rsi+0x30]           ; out pointer  = &ctx+0x30
0x1428c951d  mov [rsp+0x20],rax           ; 5th argument
0x1428c9522  call [r10+0x28]              ; slot 5 of that interface
```

The interface stored at `0x1478f5e78` lives in the layer DLL
(`ScarlettCompat.dll`, object `0x1e484389450` in boot #63) and the out
pointer is `&ctx+0x30` — not `+0x18`. In boot #63 **all 17 records have
`+0x30 = 0`**, i.e. that call never populated its out parameter for any
record, and the guest proceeds anyway.

## 2. Who reads `+0x18` (the faulting chain)

```
0x1428cfa70   lock(global 0x1479f6d80)
0x1428cfaa9   rdx = [0x1479f6d70]        ; the record
0x1428cfad1   rcx = [rdx+0x10]           ; owner
0x1428cfaf2   call [rax+0x230]           ; owner slot 70 = 0x1428c97c0
0x1428c9680   slot 68  (owns the loop)   ; ctx arrives in rdx -> rsi
0x1428c9713   call 0x1428c9960           ; rcx = ctx
0x1428c9979   mov rsi,rcx                ; entry snapshot
0x1428c9a04   mov rcx,[rsi+0x18]         ; NULL on this path
0x1428c9a08   mov rax,[rcx]              ; fault
0x1428c9a0b   call [rax+0x18]             ; slot 3, request at [rsp+0x38]
```

The request descriptor built just before the call (`0x1428c99eb` …)
holds `{flags, end, +0x18 = status dword, +0x34 = 0}` — a submission
request, so `+0x18` is the record's **backend/submission target**, not a
refcounted COM object being released.

## 3. Who writes `+0x18`: nobody, on this boot

* Store census of the whole streaming module `0x1428C0000 … 0x1428D0000`
  for `mov [reg+0x18], …`: `0x1428c95bc` (`[rdx+0x18]=1`, an out-param
  flag), `0x1428c99fa` (dword, in the submit function itself),
  `0x1428c9c83`, `0x1428cde2b` (0x20-byte map entry, `r15`-indexed),
  `0x1428cebe7`/`0x1428cec1f` (0x40-byte struct copy), `0x1428cf104`
  (0x28-byte map node, `+0x18 = 0`), `0x1428cf2fa`, `0x1428cfb38`,
  `0x1428cffd6`. **None targets a `+0x18` of a `0x80`-byte `"sdms"`
  record.**
* All 8 references to the record global `0x1479f6d70` were read
  individually; none writes `+0x18`:
  `0x1428cedd1` (owner slot 69), `0x1428cfaa9` (submit),
  `0x1428cfb97/0x1428cfbbf` (shutdown, stores `NULL` to the globals),
  `0x144f657b1/fc/f3` (owner slots 72/76/73), `0x145cd3f92` (publish).
* Boot #63 dump census: **17/17 records have `+0x18 = 0`**. The field was
  never populated for any record, not only the crashing one.
* `gta_boot63.log` contains **zero** occurrences of the record pointer or
  of `ctx+0x18` as an argument to any layer call, so no HLE entry point
  in this boot was positioned to write it.

## 4. Verdict

Hypothesis A (a missing device slot creates the object) is dead on a
second, sharper measurement: the record is built entirely by guest code,
its constructor's only external dependency targets `+0x30` (dead in all
17 records), the read sites are slot-3 submissions, and no layer call
ever received the record or its field.

Hypothesis B holds in a narrower form: **the `+0x18` backend of the
streaming record is never initialized in this boot by any executed or
in-module code.** It is not "an earlier device slot that we stubbed":
the guest constructor omits the field, the guest destructor
(`0x1428c9630`) dereferences it unguarded, and every live record carries
`NULL`.

Per the mission's rule ("no dummy interface, no fake success") **no
backend object was manufactured**. A stand-in would have to implement an
interface whose contract (slot 3 submission layout, dozens of other call
sites through the same field) is unknown.

Two observations for the next step, both measured, neither a fix:

* The stub diversions before the fault are real control flow: at
  `0x140fa0b34` the game calls device slot 16 (`+0x80`) through a global
  interface and at `0x140fa0b48` branches to its error path on
  `HRESULT < 0`; our `E_NOTIMPL` takes that branch. Device slot 86
  (`+0x2b0`, 4 calls) and slot 16 (2 calls) fire immediately before the
  submit.
* `XMemMapPhysicalPagesScatter` is called with a signature our HLE does
  not match (it logs `attrs 0x98fe5440`, `protect 0xa42f0000` — stack
  values), which is independent of the null field but is the next
  measured defect on this path.

## 5. What build 83 changed

One file, `src/pe_loader.cpp`: `LogStreamContextProbe`, called from the
VEH before the minidump. At fault time it reads the exception context
(`rsi`), validates the `"sdms"` magic, cross-checks the registered
record, and logs owner, `+0x18`, `+0x30`, aux pair, array, used/cap plus
a `fault-kind` verdict. It executes no guest code and modifies nothing.
Build 82's per-slot identity logging (192 slot stubs) is unchanged.

## 6. Verification

| check | working tree | F:\ScarlettCompat |
|---|---|---|
| `build.bat` exit | 0 | 0 |
| era stages | 21/21 | 21/21 |
| ft stages | 7/7 | 7/7 |
| verify exit | 0 | 0 |

The first F: run failed `ft_hitch` (`recovery pumps 1/3`, timing
assertion) with the same binary that passed 7/7 locally; the immediate
re-run passed 7/7. `src/pe_loader.cpp` is MD5-identical on both trees.

Boot #64: `child exit 0xC0000005`, same fault address, window black,
`BOOT_EXIT=1`. Dump `6,209,371,590` bytes archived to
`D:\scarlett-dumps\scarlett_crash_boot64.dmp` (head+tail byte-identical),
source deleted, F: back to 8.2 GB free.
