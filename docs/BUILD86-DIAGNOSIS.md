# Build 86 — XMemMapPhysicalPagesScatter ABI is wrong; the map is not the selector

This build replaces the assumed 4-argument prototype with the ABI the guest
actually uses, removes the synthetic mapping, and answers whether that
call's result steers the guest onto the submit at `0x1428C9A08`.

## A. The real ABI (measured, not assumed)

Build 86 first made the shim log `_ReturnAddress()` and the raw argument
registers. Boot #68 produced the site:

```
[xmem] XMemMapPhysicalPagesScatter(raw rcx=00000067282F2510 rdx=0000000000001A40
        r8=00000000282E5310 r9=00000000FD660000) from 0000000140F2CC43
```

`0x140F2CC43` is the instruction after `call 0x145F3FD20`, which is a
vtable thunk (`jmp qword [rip+0x853d22]` -> IAT `0x14E4D3A48`). The call
site itself:

```
0x140f2cc2e  test eax,eax
0x140f2cc30  je 0x140f2cc4c
0x140f2cc32  mov edx,eax          ; arg2 = count
0x140f2cc34  sub rsp,0x20
0x140f2cc38  mov rcx,r15          ; arg1 = page-descriptor array
0x140f2cc3b  mov r8,rbx           ; arg3 = second array (out)
0x140f2cc3e  call 0x145f3fd20     ; -> XMemMapPhysicalPagesScatter
0x140f2cc43  add rsp,0x20
0x140f2cc47  test eax,eax         ; result is a 32-bit STATUS
0x140f2cc49  jne 0x140f2cc4c
0x140f2cc4b  int3                 ; guest assert: status == 0 is fatal
```

The second and only other caller of the same thunk agrees exactly:

```
0x140b18dd4  lea rcx,[rip+0x66a7125]
0x140b18ddb  xor r8d,r8d
0x140b18dde  call 0x145f3fd20
0x140b18de3  test eax,eax
0x140b18de5  jne 0x140b18de8
0x140b18de7  int3
```

So the contract is **three arguments** — `rcx` = page array, `edx` = page
count, `r8` = second array — and the return value is a **status in `eax`**,
never a pointer. `r9` is not an argument: it held `0x337E0000` in boot #67
and `0xFD660000` in boot #68, i.e. leftover register garbage. The old
prototype's `attrs`/`protect` were therefore two non-arguments, and its
`void*` return was interpreted by the guest as a boolean.

In boot #68 the arrays are adjacent: `0x67282F2510 - 0x67282E5310 = 0xD200
= 6720 * 8` — two `count`-sized qword arrays, one in, one out.

## B. The implementation change

`HleXMemMapPhysicalPagesScatter` is now
`BOOL (const uint64_t* pages, uint32_t count, void* out)`. It logs the
three real arguments and the caller, and returns **FALSE**: the layer has
no physical-page pool (`XMemAllocatePhysicalPages` is a stub that returns
1), so the guest's page descriptors genuinely cannot be mapped. No
allocation, no synthetic 27 MB range, no fabricated success.

## C. Boot #69 — what the guest does with the corrected call

```
[xmem] XMemMapPhysicalPagesScatter(pages 000000C40BFF2980, count 6720,
        out 000000C40BFE5780) from 0000000140F2CC43
        -> FALSE (no physical-page pool; mapping not fabricated)
[loader] veh: exception 0x80000003 at 0000000140F2CC4B thread 24640
[loader] guest crashed (veh-breakpoint): exception 0x80000003 at 0000000140F2CC4B
child exit: 0x80000003
```

The guest hits **its own assert** at `0x140F2CC4B` immediately after the
call — the `int3` in the instruction pair above. `0x1428C9A08` is not
reached in this boot, because the process stops at the page-table
builder's assertion first. That is a clean failure at the documented
error path, not a fix: it shows the mapping is a required capability of
the emulated platform.

## D. Is the map result the selector of the submit? No — measured

Everything between the caller `0x1428C9713` and the fault:

```
0x1428c9960  push r15/r14/rsi/rdi/rbx ; sub rsp,0x80
0x1428c996e  cmp dword [rsp+0xe8],0        ; data check, not control flow
0x1428c999e  je 0x1428c99ab                ; both arms converge at 0x1428c99b6
0x1428c99a0  add rax,r8 / mov r14d,0x400000 ; (true arm)
0x1428c99ab  mov rax,[rax] / mov [rsp+0x60],r8 / xor r14d,r14d ; (false arm)
0x1428c99ce  call 0x14550db40              ; unconditional
0x1428c99d5  cmp byte [rbx+0x10],3         ; picks request.status only
0x1428c99df  sete cl / jne 0x1428c99e8     ; both arms merge at 0x1428c99e8
0x1428c99eb  lea rdx,[rsp+0x38]            ; request
0x1428c99f3  mov [rdx],r14
0x1428c99f6  mov [rdx+8],rdi
0x1428c99fa  mov [rdx+0x18],eax
0x1428c99fd  mov dword [rdx+0x34],0
0x1428c9a04  mov rcx,[rsi+0x18]            ; backend
0x1428c9a08  mov rax,[rcx]                 ; fault
0x1428c9a0b  call [rax+0x18]               ; slot 3
```

There is **no conditional branch** on the path: the two `cmp`/`je` pairs
choose the contents of the request, and both arms merge before the
submit. `0x1428C9960` is entered unconditionally from `0x1428C9713`
(slot 68), and the backend load/call at `0x1428c9a04` is straight-line
code. So no return value — not the map status, not slot 70's discarded
E_NOTIMPL, not slot 39's checked return — selects this submit. The only
gate the map status feeds is the assert at `0x140F2CC4B`, in the page-table
builder at `0x140F2CB4C`, which is a different subsystem from the
streamer (`0x1428C....`).

## E. Slot 66 (`call [rax+0x210]`) is guest code, not a layer vtable

* The owner is the guest static `0x1478F5E80` (inside the game image's
  `.data`), and its vtable `0x1462FC550` is in the game's `.rdata`.
* Slot 66 resolves to `0x1428C9460`, which contains the only store of the
  `"sdms"` magic in the image (`0x1428c949c`).
* Both factory call sites (`0x145cd3f8c`, `0x1428cb830`) invoke that guest
  function; the layer is not involved.

So there is no layer-supplied contract to implement at slot 66, and the
returned record's `+0x18` is a guest-internal member the guest never
initialises on either creation path (build 85).

## F. Conclusion

* The mis-parse is removed: the call now receives `(pages, count, out)`
  and returns a status, exactly as the two call sites require.
* With no fabricated mapping the guest stops at its own assertion
  (`0x140F2CC4B`), which proves the map call is a real platform
  requirement rather than the thing that steers the streamer.
* The submit at `0x1428C9A08` remains unconditional in the guest and its
  `+0x18` backend remains a member the guest never writes — the earlier
  6.2 GB watchpoint boot and the two creation paths still stand as that
  evidence. The map call is not the divergence.

## G. State

* Builds: working tree and `F:\ScarlettCompat` identical (MD5) and green —
  `build.bat` exit 0, era 21/21, ft 7/7, verify 0.
* Boot #68 dump (6,209,532,646 bytes) archived to
  `D:\scarlett-dumps\scarlett_crash_boot68.dmp`, head+tail verified,
  source deleted (F: 8.2 GB free).
* Instrumentation added: caller address + real ABI arguments on every
  scatter-map call. All build 82–85 instrumentation untouched.
