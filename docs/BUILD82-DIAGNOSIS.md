# Build 82 — the null sub-object at 0x1428C9A08 (steps 1–2, evidence)

Boot #63 ran build 82 (boot-63 log, 2037 lines,
`BOOT_EXIT=1`). The crash is unchanged and unmodified: `0xC0000005` at
`0x1428C9A08`, `params[0]=0x0`. What is new is that **every unimplemented
device slot now reports its own number and its arguments**, which turns the two
anonymous call sites into identified interfaces and answers the A/B question
from GPT #13 with evidence.

No device slot was implemented: nothing was proven to create the missing
object, and the instructions say not to implement a slot because it happens to
run shortly before the crash.

## Step 1 — the three disassemblies

### Site A — `0x144ECC821`: device slot 70 (vtable `+0x230`)

```
0x144ecc800:  wrapper entry:  rcx=arg1, rdx=arg2, r8=arg3
0x144ecc805:  mov rsi, r8              ; keep arg3
0x144ecc808:  mov r8, rdx              ; slot arg2 = wrapper arg1... (see below)
0x144ecc80b:  mov rdx, rcx             ; slot arg1 = wrapper arg1
0x144ecc80e:  mov rcx, [rip+0x388d4eb] ; this = the global Xbox device
0x144ecc815:  mov r9, rsi              ; slot arg3 = wrapper arg3
0x144ecc818:  mov rax, [rcx]           ; vtable
0x144ecc81b:  call [rax + 0x230]       ; ABSOLUTE SLOT 70 (0x230/8), interface-relative slot 70
0x144ecc821:  mov rcx, rsi             ; return value (EAX) DISCARDED here
0x144ecc826:  ...
0x144ecc830:  ...  clwb loop over [r9, r9+len)   ; cache-line writeback
```

* Absolute COM slot **70**, interface-relative slot **70** (the console device
  vtable's own numbering; the layer's table indexes the same way — slot 25 is
  `GetResourceAllocationInfo` at `+0xc8`, which the game also calls).
* Arguments at the boot-#63 call from `0x14501F703`:
  `rdx=0xa198fff458` (descriptor), `r8=0xa198fff490` (output),
  `r9=0x1f08deb31f0` (see Step 2 — this is the caller's `[context+0x18]`).
* HRESULT: discarded by the wrapper; the wrapper then writebacks the cache
  lines for the range in `r9`'s sibling, so the operation is a memory-visibility
  / writeback step, not a creator.
* Callers of the wrapper: `0x1415E6D6D`, `0x144EC5B87`, `0x144EC5D06`,
  `0x144ECC658`, `0x14501F6FE`, `0x145BDBA67`. Two of them
  (`0x144ECC658`, `0x14501F6FE`) load `r8 = [rsi+0x18]` immediately before the
  call — i.e. the object under investigation is passed *into* slot 70.

### Site B — `0x142481B31`: device slot 39 (vtable `+0x138`)

```
0x142481aa0:  function entry; rsi = this (a heap/array owner)
0x142481aad:  movzx ecx, byte [rsi+0x3a]     ; mode
0x142481ab6:  mov dword [rsi+0x68], 2
0x142481abd:  mov ecx, [rsi+8] ; shl rcx,3 ; call 0x144f97130   ; alloc(count*8)
0x142481ac9:  mov [rsi+0x58], rax ; mov [rsi+0x50], rax ; mov [rsi+0x60], 0
0x142481afc:  lea rdx, [rsp+0x44]        ; {mode, [rsi+8], 1}
0x142481b01:  mov [rsi+0x68], eax
0x142481b04:  lea rdi, [rsi+0x58]        ; out array
0x142481b08:  lea r8, [rip+0x3d5ebe9]    ; static descriptor/format table
0x142481b11:  mov rcx, [rip+0x62d81e8]   ; this = global Xbox device
0x142481b18:  mov r9, rdi                ; slot arg3 = &obj->0x58
0x142481b28:  mov rax, [rcx]
0x142481b2b:  call [rax + 0x138]         ; ABSOLUTE SLOT 39 (0x138/8), interface-relative 39
0x142481b31:  mov ecx,eax / mov ebx,eax
0x142481b35:  call 0x144e9b660            ; HRESULT check
0x142481b3c:  js 0x142481bef              ; failure path (E_NOTIMPL takes it)
```

* Absolute COM slot **39**, interface-relative slot **39**.
* Boot-#63 arguments (4 identical calls from `0x142481B31`):
  `rdx=0xa198fff334` (stack struct `{u32 mode, u32 count, u32 1}`),
  `r8=0x1461e06f8` (static table in `.rdata`),
  `r9=0x1f08deb3188` (= `obj+0x58`, the array that was just allocated).
* It writes its results into the object's array at `+0x58`. It never touches
  `+0x18`, and the layer's `E_NOTIMPL` sends the caller down its documented
  failure path (`js 0x142481bef`), which is handled, not a crash.

### The crash site — `0x1428C9713` → `0x1428C9960`

Recovered in build 81; reproduced here for the record:

```
0x1428c96a8:  rdi = r9 ; 0x1428c96ab: rbx = r8 ; 0x1428c96ae: rsi = rdx   ; context
0x1428c96b5:  call 0x144ffd8c0 / 0x144ffd920 / 0x144f21b40              ; range + lock helpers
0x1428c96d8:  mov rax,[rsi+0x28] ; test rax,rax ; je ...                 ; cursor is null-checked
0x1428c96eb:  ... build [rsp+0x38] descriptor {flags, address, format, 0}
0x1428c970c:  rcx = rsi (the context)
0x1428c9713:  call 0x1428c9960                                          ; the crashing function
0x1428c9718:  inc dword [rsi+0x64]                                      ; per-context counter
```

Inside `0x1428C9960`: `0x1428c9a04  mov rcx,[rsi+0x18]` → **`rcx = 0`**;
`0x1428c9a08  mov rax,[rcx]` faults reading address `0x0`. `rdx` at that point
is `&[rsp+0x38]`, the descriptor built two instructions earlier, and the call
that follows is `call [rax+0x18]` — **absolute slot 3** of whatever interface
lives at `[context+0x18]` (for a COM interface that is the 4th entry; *not*
`Release`, which is `+0x10`). So: a **null sub-object method invocation**.

## Step 2 — where `[context+0x18]` comes from

The decisive measurement is not a watchpoint but the dump: the *working*
upload path passes a real object at that offset. Boot #63's slot-70 call from
`0x14501F703` carried `r9 = 0x1f08deb31f0`, taken from `[rsi+0x18]` by
`0x14501F6ED`. Reading that object out of the boot-#63 dump:

```
obj 0x1f08deb31f0:
  +0x00 = 0x00000001461ecda0   <- vtable, inside the game image's .rdata
  +0x08 = 0x000001f08deb31a0   <- self-related back pointer
  +0x10 = 0xffffffffffff0041
  +0x18 = 0x000001f1d6ee44e0
```

* The interface is a **game class**, not a layer wrapper and not a D3D12
  object: its vtable lives at `0x1461ECDA0`/`0x1461ECDB0` in the exe image
  (`0x140000000 + 0x61ECDB0`), and its constructor is game code — for example
  `0x14501F3F2` does `lea rax,[rip+0x11cd9b7]` (= `0x1461ECDB0`) and stores it
  to `[r14]`, then zeroes `+0x28`, `+0x58`, `+0x60`, `+0x68`.
* Ten `lea` sites reference that vtable (`0x1406B9AD5`, `0x140F7D479`,
  `0x141B4D93C`, `0x141F76DA3`, `0x141F76E69`, `0x142475110`, `0x1424754D0`,
  `0x144EC5552`, `0x144FAFECA`, `0x14501F40D`) — all in game code, several in
  the resource/upload modules that also call device slot 70 and slot 25.

Consequences, stated as findings rather than guesses:

1. **Hypothesis A (a missing device slot creates `[context+0x18]`) is not
   supported.** Slot 70 takes the object as an *input* (six call sites load it
   into the argument register before the call), and slot 39 fills the owner's
   `+0x58` array. Neither has an output parameter that could land at `+0x18`.
   A device-slot implementation therefore cannot produce this object: it is a
   game-side C++ object with a game-side vtable.
2. **Hypothesis B is the supported one**: the sub-object belongs to the
   context's own construction, and this context was built without it. The
   crashing context has `+0x18 == 0` while a context on the working upload path
   has a valid instance of that class there.
3. Which game function should have populated it, and why that path did not run
   or did not complete, is **not established** by this build. Everything
   measured so far says it is not reachable from a layer device slot.

## What build 82 changed

`src/hle_registry.cpp` only:

* The one shared `XboxDevice_Unimplemented` stub (which could only report the
  call site) is replaced by 192 per-slot stubs, instantiated through one pack
  expansion (`XboxDevice_SlotUnimplemented<Slot>` +
  `FillUnimplementedDeviceSlots(std::make_index_sequence<192>{})`). Each logs
  `slot N (vtable +0xN*8)`, the caller, and the six argument-register values,
  first four calls in full then every 64th, so a hot slot cannot drown the log.
  Every vtable entry remains a real function — never null.
* Nothing else changed. No slot-8 change, no texture-computer redesign, no
  mapping-cap, descriptor-heap, pool or dispatcher change, no change to the
  allocation-layout calculation.

## Boot #63 — slot inventory (new capability)

All unimplemented device slots reached before the crash, now named directly:

| slot | vtable | caller | args (rdx, r8, r9, stack…) |
| --- | --- | --- | --- |
| 70 | `+0x230` | `0x144ECC821` (wrapper `0x144ECC800`) | desc, out, ctx+0x18, …, caller |
| 20 | `+0xa0` | `0x140CC29EA` | obj, stack out, obj2, `0x40000001c`, 0, 0 |
| 39 | `+0x138` | `0x142481B31` | `{mode,count,1}`, static table, `obj+0x58` |
| 41 | `+0x148` | `0x142479554` | stack, 0, static, stack, … |
| 61 | `+0x1e8` | `0x1412B981E` | stack, ptr, 1, 1, … |
| 24 | `+0xc0` | `0x144ECC884` | 1, ptr, ptr, 1, … |
| 86 | `+0x2b0` | `0x142479A8D` | stack, static, stack, 0, 0, 0 |
| 16 | `+0x80` | `0x140FA0B3B` | 1, ptr, 0x74, … |

Slot 20 is newly revealed by this instrumentation; it had been invisible because
the old stub only printed the call site.

## Verification actually run

* Compile check of the two touched sources: exit 0.
* Full `build.bat`: `BUILD_EXIT=0`, `era stages: 21/21 pass`,
  `ft stages: 7/7 pass`, `=== verify exit code: 0 ===`.
  Two intermediate full-build runs failed on *different* GPU-dependent
  optimization stages (`opt_texcache` once with `entries=0`, `opt_flight` once
  with `hr=0x887a0005` = `DXGI_ERROR_DEVICE_REMOVED`); standalone reruns of
  `guest_opt.exe` report `opt=PASS` and `--- verdict: ALL PASS ---`. Those are
  this machine's GPU dropping under the back-to-back full-build load, not a
  code path: the failing stage changed between identical runs and the failure
  code is the driver's device-removal.
* Same sources built on `F:\ScarlettCompat`: `BUILD_EXIT=0`, era 21/21,
  ft 7/7, verify exit 0.
* Boot #63: same crash as #62 (`0x1428C9A08`), with the new slot inventory
  above. Window black, no frames.

## Not done, deliberately

No device slot was implemented. The evidence says the missing object is created
by game code, so implementing slot 70, 39 or 20 to manufacture a stand-in would
be a dummy interface with fake success — exactly what the instructions forbid.
